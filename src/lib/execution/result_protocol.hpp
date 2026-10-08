#pragma once

#include "photospider/core/status.hpp"

namespace ps::execution_internal {
inline Status protocol_failure(const char* message) {
  return Status{ErrorCode::InvalidArgument,
                message,
                FailureReason::MalformedEnvelope,
                {FailureOrigin::Protocol, FailureScope::Group}};
}
}  // namespace ps::execution_internal
