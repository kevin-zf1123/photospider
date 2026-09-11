#pragma once

#include <functional>
#include <string>

#include "photospider/plugin/dependency_program.hpp"

namespace ps::plugin_internal {
/** @brief Validates, keys and evaluates one explicit pure internal transition.
 */
Result<Value> evaluate_dependency_block(
    const std::string& contract, const DependencyPhase& phase,
    const DependencyBlockServices& services, std::uint32_t kind,
    std::uint64_t begin, std::uint64_t end, std::uint64_t mode,
    const Value& incoming, const std::function<Result<Value>()>& compute);
}  // namespace ps::plugin_internal
