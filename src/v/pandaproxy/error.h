/*
 * Copyright 2021 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once

#include "kafka/protocol/errors.h"
#include "seastarx.h"

#include <cstdint>
#include <system_error>

namespace pandaproxy {

// reply_error_code is a std::error_condition through which errors are returned
// in the reply.
//
// This is the default_error_condition for the proxy; to map other
// error_codes to this, override its error_category::default_error_condition().
enum class reply_error_code : uint16_t {
    not_acceptable = 406,
    conflict = 409,
    unsupported_media_type = 415,
    unprocessable_entity = 422,
    kafka_bad_request = 40002,
    kafka_authentication_error = 40101,
    kafka_authorization_error = 40301,
    topic_not_found = 40401,
    partition_not_found = 40402,
    consumer_instance_not_found = 40403,
    subject_soft_deleted = 40404,
    subject_not_deleted = 40405,
    subject_version_soft_deleted = 40406,
    subject_version_not_deleted = 40407,
    write_collision = 50301,
    zookeeper_error = 50001,
    kafka_error = 50002,
    kafka_retriable_error = 50003,
    ssl_unavailable = 50101,
    broker_not_available = 50302,
};

std::error_condition make_error_condition(reply_error_code);
const std::error_category& reply_category() noexcept;

} // namespace pandaproxy

namespace kafka {

std::error_code make_error_code(error_code);

}

namespace std {

template<>
struct is_error_condition_enum<pandaproxy::reply_error_code> : true_type {};

template<>
struct is_error_code_enum<kafka::error_code> : true_type {};

} // namespace std
