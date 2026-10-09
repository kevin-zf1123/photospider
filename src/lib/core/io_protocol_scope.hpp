#pragma once

#include "photospider/core/status.hpp"

namespace ps::core_internal {
// A synchronous thread-local I/O fence. The owner supplies protocol policy;
// data storage needs no knowledge of execution Actors or plugin failure
// latches. Nested scopes restore the previous capability. State is borrowed
// until exit.
class IoProtocolScope final {
 public:
  using Check = Status (*)(void*);
  IoProtocolScope(void* state, Check check) noexcept
      : state_(state), check_(check), previous_(current_) {
    current_ = this;
  }
  ~IoProtocolScope() { current_ = previous_; }
  IoProtocolScope(const IoProtocolScope&) = delete;
  IoProtocolScope& operator=(const IoProtocolScope&) = delete;
  static Status allowed() {
    return current_ ? current_->check_(current_->state_) : Status::success();
  }

 private:
  void* state_;
  Check check_;
  IoProtocolScope* previous_;
  inline static thread_local IoProtocolScope* current_ = nullptr;
};
}  // namespace ps::core_internal
