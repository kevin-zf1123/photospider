#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "execution/structured_actor_lifecycle.hpp"
#include "plugin/failure_latch.hpp"

namespace ps::execution_internal {
// Records retry eligibility for one speculative backend attempt. Retry drops
// old capabilities; spent work and completed producer tiles remain intact.
// The coordinator borrows decisions, then drains and performs the transition.
struct ResultFallbackState {
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
};
}  // namespace ps::execution_internal
