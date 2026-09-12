#pragma once

#include "photospider/core/status.hpp"

namespace ps::execution_internal {
/** @brief Callback-local sticky fence for mandatory I/O protocol violations.
 * This is an execution contract for trusted code, not a process sandbox.
 */
inline thread_local ErrorCode* result_callback_failure = nullptr;
class ResultCallbackScope final {
 public:
  explicit ResultCallbackScope(ErrorCode* failure) noexcept
      : previous_(result_callback_failure) {
    result_callback_failure = failure;
  }
  ~ResultCallbackScope() { result_callback_failure = previous_; }

 private:
  ErrorCode* previous_;
};
inline Status result_io_allowed() {
  if (!result_callback_failure)
    return Status::success();
  if (*result_callback_failure == ErrorCode::Ok)
    *result_callback_failure = ErrorCode::InvalidArgument;
  return Status{*result_callback_failure,
                "structured callback I/O requires a coordinator request"};
}
}  // namespace ps::execution_internal
