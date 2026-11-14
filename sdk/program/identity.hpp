#pragma once

#include <map>
#include <string>
#include <vector>

#include "photospider/compiler/plan.hpp"

namespace ps::compiler_internal {
std::string ir_digest(const IRProgram& program);
std::string physical_digest(const ExecutionPlan& plan);
std::string operation_digest(const OperationTraits& traits);
}  // namespace ps::compiler_internal
