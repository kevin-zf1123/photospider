#pragma once

#include "photospider/core/status.hpp"

namespace ps::core_internal {
// Keep category-only argument errors distinct from domain/schema errors.
// The bounded form preserves Status::failure's diagnostic truncation contract.
inline Status invalid_argument(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
inline Status invalid_domain(const char* message) {
  return {ErrorCode::InvalidArgument, message, FailureReason::InvalidDomain};
}
inline Status invalid_schema_domain(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
}  // namespace ps::core_internal
