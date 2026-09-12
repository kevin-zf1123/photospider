#pragma once

#include <functional>
#include <optional>

#include "photospider/plugin/dependency_program.hpp"

namespace ps::plugin_internal {
// Address-stable records in one admitted buffer. Every reference in phase
// refers to this record or its owning DependencySession, never a nested stack.
struct JointMemberPhase final {
  BufferAllocator allocator;
  std::function<Status(std::uint64_t)> consume;
  std::function<Status(Status)> report;
  std::optional<DependencyPhase> phase;
};
}  // namespace ps::plugin_internal
