#include "compute/dirty_sibling_commit_gate.hpp"

#include <exception>

namespace ps::compute {

/** @copydoc DirtySiblingCommitGate::wait_for_rt_commit_or_throw */
void DirtySiblingCommitGate::wait_for_rt_commit_or_throw() {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] { return state_ != State::Pending; });
  if (state_ == State::Denied) {
    throw GraphError(GraphErrc::ComputeError,
                     "HP dirty commit aborted because RT proxy commit failed.");
  }
}

/** @copydoc DirtySiblingCommitGate::mark_rt_committed */
void DirtySiblingCommitGate::mark_rt_committed() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Pending) {
      state_ = State::Committed;
    }
  }
  condition_.notify_all();
}

/** @copydoc DirtySiblingCommitGate::abort_hp_commit */
void DirtySiblingCommitGate::abort_hp_commit() noexcept {
  try {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_ == State::Pending) {
        state_ = State::Denied;
      }
    }
    condition_.notify_all();
  } catch (...) {
    std::terminate();
  }
}

}  // namespace ps::compute
