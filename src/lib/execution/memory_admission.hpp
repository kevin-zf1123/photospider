#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "data/memory_budget.hpp"

namespace ps::execution_internal {
// A driver-owned admission capability. Allocator copies may retain State, but
// closing the driver fences callbacks before borrowed Run/context state
// retires. The lock serializes close with admission; payload leases do not
// retain State.
class ScopedMemoryAdmission final {
 public:
  using Admit =
      std::function<Result<std::shared_ptr<data_internal::MemoryReservation>>(
          std::uint64_t)>;
  explicit ScopedMemoryAdmission(Admit admit)
      : state_(std::make_shared<State>(std::move(admit))) {}
  ~ScopedMemoryAdmission() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->admit = {};
  }
  ScopedMemoryAdmission(const ScopedMemoryAdmission&) = delete;
  ScopedMemoryAdmission& operator=(const ScopedMemoryAdmission&) = delete;
  Admit callback() const {
    return [state = state_](std::uint64_t bytes)
               -> Result<std::shared_ptr<data_internal::MemoryReservation>> {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->admit)
        return Result<std::shared_ptr<data_internal::MemoryReservation>>(
            Status{ErrorCode::OperationFailed, "retired allocation admission"});
      return state->admit(bytes);
    };
  }

 private:
  struct State {
    std::mutex mutex;
    Admit admit;
    explicit State(Admit callback) : admit(std::move(callback)) {}
  };
  std::shared_ptr<State> state_;
};

}  // namespace ps::execution_internal
