#pragma once

#include <map>
#include <string>
#include <vector>

#include "photospider/compiler/compiler.hpp"

namespace ps::compiler_internal {
std::string semantic_digest(const IRProgram& program);
std::string optimized_digest(const std::string& semantic,
                             const IRProgram& program);
std::string physical_digest(const ExecutionPlan& plan);
std::string plan_cache_key(const std::string& plan);
std::string operation_digest(const OperationTraits& traits);
}  // namespace ps::compiler_internal
