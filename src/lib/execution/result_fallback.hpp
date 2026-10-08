#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "execution/dependency_records.hpp"
#include "execution/structured_actor_lifecycle.hpp"
#include "plugin/failure_latch.hpp"

namespace ps::execution_internal {
// Owns one speculative backend attempt's payload-free ancestry. Retry drops
// old capabilities before restoring this owner; spent work is never restored.
// The coordinator borrows decisions, then drains and performs the transition.
struct ResultFallbackState {
  std::unique_ptr<DependencyRecords> records;
  bool taint = false, retry_safe = true;
  static bool can_retry_start(const OperationTraits&, Backend,
                              const StructuredStartPhase&,
                              const std::atomic<ErrorCode>&,
                              const plugin_internal::FailureLatch&);
  bool can_retry_poll(const OperationTraits&, Backend, const Status& failed,
                      const Status& host_failure,
                      const plugin_internal::FailureLatch& host_latch,
                      std::uint64_t native_dispatches, bool published,
                      ErrorCode sticky, ErrorCode operation_failure) const;
  static bool optional_joint_failure(const Status&);
};
}  // namespace ps::execution_internal
