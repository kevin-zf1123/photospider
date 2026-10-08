#pragma once
#include <cstdint>
#include <memory>

#include "photospider/execution/execution.hpp"
namespace ps::execution_internal {
struct DemandCoordinator;
std::shared_ptr<DemandCoordinator> make_demand_coordinator(std::uint32_t,
                                                           std::uint64_t);
// Bind only during construction. Close cancels calls and waits outside the
// context's shared registry lock; no caller may borrow context after close.
void bind_demand_context(const std::shared_ptr<DemandCoordinator>&,
                         ExecutionContext*) noexcept;
void close_demand_coordinator(
    const std::shared_ptr<DemandCoordinator>&) noexcept;
}  // namespace ps::execution_internal
