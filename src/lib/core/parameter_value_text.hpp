#pragma once

#include <string>

#include "photospider/plugin/node_view.hpp"

namespace ps::core {

/**
 * @brief Formats one detached parameter value for Host inspection display.
 *
 * @param value Recursive parameter value to format.
 * @return Deterministic dependency-neutral display text.
 * @throws std::bad_alloc when output or stream allocation is exhausted.
 * @throws std::logic_error if the value reports an unknown parameter kind.
 * @note Top-level strings remain unquoted for compatibility. Recursive arrays
 * and objects use a stable JSON-like display grammar; this text is not a
 * persisted JSON contract and is never parsed as cache or graph input.
 */
std::string format_parameter_value_for_inspection(
    const plugin::ParameterValue& value);

}  // namespace ps::core
