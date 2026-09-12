#pragma once

#include "photospider/core/status.hpp"
#include "plugin/failure_latch.hpp"

namespace ps::execution_internal {
/** @brief Callback-local sticky fence for mandatory I/O protocol violations.
 * This is an execution contract for trusted code, not a process sandbox.
 */
inline thread_local ErrorCode* result_callback_failure = nullptr;
inline thread_local plugin_internal::FailureLatch* result_callback_latch{};
class ResultCallbackScope final {
 public:
  explicit ResultCallbackScope(
      ErrorCode* failure,
      plugin_internal::FailureLatch* latch = nullptr) noexcept
      : previous_(result_callback_failure),
        prior_latch_(result_callback_latch) {
    result_callback_failure = failure;
    result_callback_latch = latch;
  }
  ~ResultCallbackScope() {
    result_callback_failure = previous_;
    result_callback_latch = prior_latch_;
  }

 private:
  ErrorCode* previous_;
  plugin_internal::FailureLatch* prior_latch_;
};
inline Status result_io_allowed() {
  if (!result_callback_failure)
    return Status::success();
  const Status violation{ErrorCode::InvalidArgument,
                         {},
                         FailureReason::UnauthorizedRead,
                         {FailureOrigin::Protocol, FailureScope::Group}};
  if (result_callback_latch) {
    if (*result_callback_failure != ErrorCode::Ok)
      result_callback_latch->record(*result_callback_failure ==
                                            ErrorCode::InvalidArgument
                                        ? violation
                                        : Status{*result_callback_failure, {}});
    auto first = result_callback_latch->record(violation);
    if (*result_callback_failure == ErrorCode::Ok)
      *result_callback_failure = first.code;
    return first;
  }
  if (*result_callback_failure == ErrorCode::Ok)
    *result_callback_failure = ErrorCode::InvalidArgument;
  return *result_callback_failure == ErrorCode::InvalidArgument
             ? violation
             : Status{*result_callback_failure, {}};
}
}  // namespace ps::execution_internal
