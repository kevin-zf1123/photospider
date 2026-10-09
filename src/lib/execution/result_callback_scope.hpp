#pragma once

#include "core/io_protocol_scope.hpp"
#include "plugin/failure_latch.hpp"

namespace ps::execution_internal {
/** @brief Callback-local sticky fence for mandatory I/O protocol violations.
 * This is an execution contract for trusted code, not a process sandbox.
 */
class ResultCallbackScope final {
 public:
  explicit ResultCallbackScope(
      ErrorCode* failure,
      plugin_internal::FailureLatch* latch = nullptr) noexcept
      : failure_(failure), latch_(latch), scope_(this, [](void* state) {
          return static_cast<ResultCallbackScope*>(state)->allowed();
        }) {}

 private:
  Status allowed() {
    if (!failure_)
      return Status::success();
    const Status violation{ErrorCode::InvalidArgument,
                           {},
                           FailureReason::UnauthorizedRead,
                           {FailureOrigin::Protocol, FailureScope::Group}};
    if (latch_) {
      if (*failure_ != ErrorCode::Ok)
        latch_->record(*failure_ == ErrorCode::InvalidArgument
                           ? violation
                           : Status{*failure_, {}});
      auto first = latch_->record(violation);
      if (*failure_ == ErrorCode::Ok)
        *failure_ = first.code;
      return first;
    }
    if (*failure_ == ErrorCode::Ok)
      *failure_ = ErrorCode::InvalidArgument;
    return *failure_ == ErrorCode::InvalidArgument ? violation
                                                   : Status{*failure_, {}};
  }
  ErrorCode* failure_;
  plugin_internal::FailureLatch* latch_;
  core_internal::IoProtocolScope scope_;
};
}  // namespace ps::execution_internal
