#include "kernel/services/compute-service/dirty_sibling_commit_gate.hpp"

namespace ps::compute {

void DirtySiblingCommitGate::wait_for_rt_commit_or_throw() {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] { return rt_committed_ || aborted_; });
  if (aborted_ && !rt_committed_) {
    throw GraphError(GraphErrc::ComputeError,
                     "HP dirty commit aborted because RT proxy commit failed.");
  }
}

void DirtySiblingCommitGate::mark_rt_committed() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    rt_committed_ = true;
  }
  condition_.notify_all();
}

void DirtySiblingCommitGate::abort_hp_commit() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
  }
  condition_.notify_all();
}

}  // namespace ps::compute
