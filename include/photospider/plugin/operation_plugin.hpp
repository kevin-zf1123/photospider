#pragma once

#include <cstdint>

#include "photospider/plugin/operation_plugin_api.h"

namespace ps::plugin {

/**
 * @brief Converts a C ABI element enum to its numeric representation.
 * @param value C ABI enum value.
 * @return Unsigned wire/record value.
 * @throws Nothing.
 * @note This header-only helper adds no second ABI.
 */
constexpr std::uint32_t element_type_value(
    ps_operation_element_type_v2 value) noexcept {
  return static_cast<std::uint32_t>(value);
}

}  // namespace ps::plugin
