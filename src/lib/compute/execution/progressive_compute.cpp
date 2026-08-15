/**
 * @file progressive_compute.cpp
 * @brief Implements the private progressive preview/final submission gate.
 */
#include "compute/execution/progressive_compute.hpp"

namespace ps::compute {

/** @copydoc ProgressiveFinalGate::arm */
bool ProgressiveFinalGate::arm() noexcept {
  State expected = State::Pending;
  return state_.compare_exchange_strong(expected, State::Armed,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
}

/** @copydoc ProgressiveFinalGate::deny */
bool ProgressiveFinalGate::deny() noexcept {
  State observed = state_.load(std::memory_order_acquire);
  while (observed == State::Pending || observed == State::Armed) {
    if (state_.compare_exchange_weak(observed, State::Denied,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

/** @copydoc ProgressiveFinalGate::try_trigger */
bool ProgressiveFinalGate::try_trigger() noexcept {
  State expected = State::Armed;
  return state_.compare_exchange_strong(expected, State::Triggered,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
}

/** @copydoc ProgressiveFinalGate::state */
ProgressiveFinalGate::State ProgressiveFinalGate::state() const noexcept {
  return state_.load(std::memory_order_acquire);
}

}  // namespace ps::compute
