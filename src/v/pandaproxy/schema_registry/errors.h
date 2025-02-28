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

#include "outcome.h"
#include "pandaproxy/schema_registry/error.h"
#include "pandaproxy/schema_registry/exceptions.h"
#include "pandaproxy/schema_registry/types.h"

#include <fmt/format.h>

#include <type_traits>

namespace pandaproxy::schema_registry {

class error_info {
public:
    error_info() = default;
    error_info(error_code ec, std::string msg)
      : _ec{ec}
      , _msg{std::move(msg)} {}

    const error_code& code() const noexcept { return _ec; }
    const std::string& message() const noexcept { return _msg; }

private:
    error_code _ec;
    std::string _msg;
};

inline exception as_exception(const error_info& ei) {
    return exception(ei.code(), ei.message());
}

///\brief Integrate error_info with outcome
inline std::error_code make_error_code(const error_info& ei) {
    return make_error_code(ei.code());
}

///\brief Integrate error_info with outcome
inline void outcome_throw_as_system_error_with_payload(const error_info& ei) {
    throw as_exception(ei);
}

template<typename T>
using result = result<T, error_info>;

inline error_info not_found(schema_id id) {
    return error_info{
      error_code::schema_id_not_found,
      fmt::format("Schema {} not found", id())};
}

inline error_info not_found(const subject& sub) {
    return error_info{
      error_code::subject_not_found,
      fmt::format("Subject '{}' not found.", sub())};
}

inline error_info not_found(const subject& sub, schema_version id) {
    return error_info{
      error_code::subject_version_not_found,
      fmt::format("Subject '{}' Version {} not found.", sub(), id())};
}

inline error_info soft_deleted(const subject& sub) {
    return error_info{
      error_code::subject_soft_deleted,
      fmt::format(
        "Subject '{}' was soft deleted.Set permanent=true to delete "
        "permanently",
        sub())};
}

inline error_info not_deleted(const subject& sub) {
    return error_info{
      error_code::subject_not_deleted,
      fmt::format(
        "Subject '{}' was not deleted first before being permanently deleted",
        sub())};
}

inline error_info soft_deleted(const subject& sub, schema_version version) {
    return error_info{
      error_code::subject_version_soft_deleted,
      fmt::format(
        "Subject '{}' Version {} was soft deleted.Set permanent=true to "
        "delete permanently",
        sub(),
        version())};
}

inline error_info not_deleted(const subject& sub, schema_version version) {
    return error_info{
      error_code::subject_version_not_deleted,
      fmt::format(
        "Subject '{}' Version {} was not deleted first before being "
        "permanently deleted",
        sub(),
        version())};
}

inline error_info invalid_schema_type(schema_type type) {
    return {
      error_code::schema_invalid,
      fmt::format("Invalid schema type {}", to_string_view(type))};
}

} // namespace pandaproxy::schema_registry
