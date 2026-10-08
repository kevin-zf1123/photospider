#pragma once

#include <exception>
#include <new>

#include "photospider/core/status.hpp"

namespace ps::plugin_internal {
// Called only from an active exception handler. Diagnostic allocation failure
// must remain a typed failure rather than escaping the operation boundary.
inline Status current_operation_exception() noexcept {
  try {
    throw;
  } catch (const std::bad_alloc&) {
    return Status{ErrorCode::ResourceExhausted, {}};
  } catch (const std::exception& error) {
    try {
      const auto* message = error.what();
      return Status{ErrorCode::OperationFailed, message ? message : "",
                    FailureReason::HostException};
    } catch (...) {
      return Status{ErrorCode::ResourceExhausted, {}};
    }
  } catch (...) {
    try {
      return Status{ErrorCode::OperationFailed,
                    "operation raised a nonstandard exception",
                    FailureReason::HostException};
    } catch (...) {
      return Status{ErrorCode::ResourceExhausted, {}};
    }
  }
}
}  // namespace ps::plugin_internal
