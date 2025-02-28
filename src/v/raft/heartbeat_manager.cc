// Copyright 2020 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "raft/heartbeat_manager.h"

#include "config/configuration.h"
#include "model/metadata.h"
#include "model/timeout_clock.h"
#include "outcome_future_utils.h"
#include "raft/consensus_client_protocol.h"
#include "raft/errc.h"
#include "raft/group_configuration.h"
#include "raft/raftgen_service.h"
#include "raft/types.h"
#include "rpc/reconnect_transport.h"
#include "rpc/types.h"
#include "vlog.h"

#include <seastar/core/coroutine.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/timed_out_error.hh>
#include <seastar/core/with_timeout.hh>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <bits/stdint-uintn.h>
#include <boost/range/iterator_range.hpp>

namespace raft {
ss::logger hbeatlog{"r/heartbeat"};
using consensus_ptr = heartbeat_manager::consensus_ptr;
using consensus_set = heartbeat_manager::consensus_set;

struct heartbeat_requests {
    /// Requests to dispatch.  Can include request to self.
    std::vector<heartbeat_manager::node_heartbeat> requests;

    /// These nodes' heartbeat status indicates they need
    /// a transport reconnection before sending next heartbeat
    absl::flat_hash_set<model::node_id> reconnect_nodes;
};

static heartbeat_requests requests_for_range(
  const consensus_set& c, clock_type::duration heartbeat_interval) {
    absl::flat_hash_map<
      model::node_id,
      std::vector<std::pair<heartbeat_metadata, follower_req_seq>>>
      pending_beats;
    if (c.empty()) {
        return {};
    }

    // Set of follower nodes whose heartbeat_failed status indicates
    // that we should tear down their TCP connection before next heartbeat
    absl::flat_hash_set<model::node_id> reconnect_nodes;

    auto last_heartbeat = clock_type::now() - heartbeat_interval;
    for (auto& ptr : c) {
        if (!ptr->is_leader()) {
            continue;
        }

        auto maybe_create_follower_request = [ptr,
                                              last_heartbeat,
                                              &pending_beats,
                                              &reconnect_nodes](
                                               const vnode& rni) mutable {
            // special case self beat
            // self beat is used to make sure that the protocol will make
            // progress when there is only on node
            if (rni == ptr->self()) {
                pending_beats[rni.id()].emplace_back(
                  heartbeat_metadata{ptr->meta(), rni}, 0);
                return;
            }

            if (ptr->are_heartbeats_suppressed(rni)) {
                return;
            }
            auto last_append_timestamp = ptr->last_append_timestamp(rni);

            if (last_append_timestamp > last_heartbeat) {
                vlog(
                  hbeatlog.trace,
                  "Skipping sending beat to {} gr: {} last hb {}, last append "
                  "{}",
                  rni,
                  ptr->group(),
                  last_heartbeat.time_since_epoch().count(),
                  last_append_timestamp.time_since_epoch().count());
                // we already sent heartbeat, skip it
                return;
            }

            auto seq_id = ptr->next_follower_sequence(rni);
            ptr->update_suppress_heartbeats(
              rni, seq_id, heartbeats_suppressed::yes);
            pending_beats[rni.id()].emplace_back(
              heartbeat_metadata{ptr->meta(), ptr->self(), rni}, seq_id);

            if (ptr->should_reconnect_follower(rni)) {
                reconnect_nodes.insert(rni.id());
            }
        };

        auto group = ptr->config();
        // collect voters
        group.for_each_broker_id(maybe_create_follower_request);
    }

    std::vector<heartbeat_manager::node_heartbeat> reqs;
    reqs.reserve(pending_beats.size());
    for (auto& p : pending_beats) {
        std::vector<heartbeat_metadata> requests;
        absl::flat_hash_map<
          raft::group_id,
          heartbeat_manager::follower_request_meta>
          meta_map;
        requests.reserve(p.second.size());
        meta_map.reserve(p.second.size());
        for (auto& [hb, seq] : p.second) {
            meta_map.emplace(
              hb.meta.group,
              heartbeat_manager::follower_request_meta{
                seq, hb.meta.prev_log_index, hb.target_node_id});
            requests.push_back(std::move(hb));
        }
        reqs.emplace_back(
          p.first, heartbeat_request{std::move(requests)}, std::move(meta_map));
    }

    return heartbeat_requests{
      .requests{reqs}, .reconnect_nodes{reconnect_nodes}};
}

heartbeat_manager::heartbeat_manager(
  duration_type interval,
  consensus_client_protocol proto,
  model::node_id self,
  duration_type heartbeat_timeout)
  : _heartbeat_interval(interval)
  , _heartbeat_timeout(heartbeat_timeout)
  , _client_protocol(std::move(proto))
  , _self(self) {
    _heartbeat_timer.set_callback([this] { dispatch_heartbeats(); });
}

ss::future<>
heartbeat_manager::send_heartbeats(std::vector<node_heartbeat> reqs) {
    return ss::do_with(
      std::move(reqs), [this](std::vector<node_heartbeat>& reqs) mutable {
          std::vector<ss::future<>> futures;
          futures.reserve(reqs.size());
          for (auto& r : reqs) {
              // self heartbeat
              if (r.target == _self) {
                  futures.push_back(do_self_heartbeat(std::move(r)));
                  continue;
              }

              futures.push_back(do_heartbeat(std::move(r)));
          }
          return ss::when_all_succeed(futures.begin(), futures.end());
      });
}

ss::future<> heartbeat_manager::do_dispatch_heartbeats() {
    auto reqs = requests_for_range(_consensus_groups, _heartbeat_interval);

    for (const auto& node_id : reqs.reconnect_nodes) {
        if (co_await _client_protocol.ensure_disconnect(node_id)) {
            vlog(
              hbeatlog.info, "Closed unresponsive connection to {}", node_id);
        };
    }

    co_await send_heartbeats(std::move(reqs.requests));
}

ss::future<> heartbeat_manager::do_self_heartbeat(node_heartbeat&& r) {
    heartbeat_reply reply;
    reply.meta.reserve(r.request.heartbeats.size());
    std::transform(
      std::begin(r.request.heartbeats),
      std::end(r.request.heartbeats),
      std::back_inserter(reply.meta),
      [](heartbeat_metadata& hb) {
          return append_entries_reply{
            .target_node_id = hb.target_node_id,
            .node_id = hb.target_node_id,
            .group = hb.meta.group,
            .result = append_entries_reply::status::success};
      });
    process_reply(r.target, std::move(r.meta_map), std::move(reply));
    return ss::now();
}

ss::future<> heartbeat_manager::do_heartbeat(node_heartbeat&& r) {
    auto f = _client_protocol
               .heartbeat(
                 r.target,
                 std::move(r.request),
                 rpc::client_opts(
                   clock_type::now() + _heartbeat_timeout,
                   rpc::compression_type::zstd,
                   512))
               .then([node = r.target, groups = std::move(r.meta_map), this](
                       result<heartbeat_reply> ret) mutable {
                   // this will happen after RPC client will return and resume
                   // sending heartbeats to follower
                   process_reply(node, std::move(groups), std::move(ret));
               });
    // fail fast to make sure that not lagging nodes will be able to receive
    // hearteats
    return ss::with_timeout(next_heartbeat_timeout(), std::move(f))
      .handle_exception_type([](const ss::timed_out_error&) {
          // we just ignore this exception since it is the timeout so we do not
          // have to update consensus instances with results
      })
      .handle_exception_type([](const ss::gate_closed_exception&) {});
}

void heartbeat_manager::process_reply(
  model::node_id n,
  absl::flat_hash_map<raft::group_id, follower_request_meta> groups,
  result<heartbeat_reply> r) {
    if (!r) {
        vlog(
          hbeatlog.trace,
          "Could not send hearbeats to node:{}, reason:{}, message:{}",
          n,
          r,
          r.error().message());
        for (auto& [g, req_meta] : groups) {
            auto it = _consensus_groups.find(g);
            if (it == _consensus_groups.end()) {
                vlog(hbeatlog.error, "cannot find consensus group:{}", g);
                continue;
            }

            (*it)->update_heartbeat_status(req_meta.follower_vnode, false);

            (*it)->update_suppress_heartbeats(
              req_meta.follower_vnode, req_meta.seq, heartbeats_suppressed::no);
            // propagate error
            (*it)->process_append_entries_reply(
              n,
              result<append_entries_reply>(r.error()),
              req_meta.seq,
              req_meta.dirty_offset);
            (*it)->get_probe().heartbeat_request_error();
        }
        return;
    }
    for (auto& m : r.value().meta) {
        auto it = _consensus_groups.find(m.group);
        if (it == _consensus_groups.end()) {
            vlog(
              hbeatlog.error, "Could not find consensus for group:{}", m.group);
            continue;
        }
        auto meta = groups.find(m.group)->second;
        (*it)->update_heartbeat_status(meta.follower_vnode, true);
        (*it)->update_suppress_heartbeats(
          meta.follower_vnode, meta.seq, heartbeats_suppressed::no);
        (*it)->process_append_entries_reply(
          n,
          result<append_entries_reply>(std::move(m)),
          meta.seq,
          meta.dirty_offset);
    }
}

void heartbeat_manager::dispatch_heartbeats() {
    (void)with_gate(_bghbeats, [this] {
        return _lock.with([this] {
            return do_dispatch_heartbeats().finally([this] {
                if (!_bghbeats.is_closed()) {
                    _heartbeat_timer.arm(next_heartbeat_timeout());
                }
            });
        });
    }).handle_exception([](const std::exception_ptr& e) {
        vlog(hbeatlog.warn, "Error dispatching hearbeats - {}", e);
    });
    // update last
    _hbeat = clock_type::now();
}

ss::future<> heartbeat_manager::deregister_group(group_id g) {
    return _lock.with([this, g] {
        auto it = _consensus_groups.find(g);
        vassert(it != _consensus_groups.end(), "group not found: {}", g);
        _consensus_groups.erase(it);
    });
}

ss::future<>
heartbeat_manager::register_group(ss::lw_shared_ptr<consensus> ptr) {
    return _lock.with([this, ptr = std::move(ptr)] {
        auto ret = _consensus_groups.insert(ptr);
        vassert(
          ret.second,
          "double registration of group: {}:{}",
          ptr->ntp(),
          ptr->group());
    });
}

ss::future<> heartbeat_manager::start() {
    dispatch_heartbeats();
    return ss::make_ready_future<>();
}
ss::future<> heartbeat_manager::stop() {
    _heartbeat_timer.cancel();
    return _bghbeats.close();
}

clock_type::time_point heartbeat_manager::next_heartbeat_timeout() {
    return clock_type::now() + _heartbeat_interval;
}

} // namespace raft
