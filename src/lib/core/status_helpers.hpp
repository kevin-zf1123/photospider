#pragma once

#include <string>
#include <utility>

#include "photospider/core/cancellation.hpp"
#include "photospider/core/status.hpp"

namespace ps::core_internal {
// Keep category-only errors distinct from domain/schema errors. The bounded
// form preserves Status::failure's diagnostic truncation contract.
inline Status invalid_argument(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
// Aggregate-form adapters retain the complete diagnostic, as their callers
// historically require. No origin, scope or failure reason is inferred.
inline Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message};
}
inline Status type_mismatch(std::string message) {
  return {ErrorCode::TypeMismatch, std::move(message)};
}
inline Status resource_exhausted(std::string message = {}) {
  return {ErrorCode::ResourceExhausted, std::move(message)};
}
inline Status exhausted(const char* message) {
  return Status::failure(ErrorCode::ResourceExhausted, message);
}
inline Status invalid_quality(
    std::string message = "invalid numerical quality report") {
  return {ErrorCode::InvalidArgument, std::move(message),
          FailureReason::InvalidQuality};
}
inline Status capacity_exhausted() {
  return {ErrorCode::ResourceExhausted, "managed resource capacity exhausted",
          FailureReason::CapacityLimit};
}
inline Status invalid_domain(std::string message) {
  return {ErrorCode::InvalidArgument, std::move(message),
          FailureReason::InvalidDomain};
}
inline Status invalid_schema_domain(std::string message) {
  return {ErrorCode::InvalidArgument,
          std::move(message),
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Status arithmetic_overflow(std::string message) {
  return {ErrorCode::OperationFailed,
          std::move(message),
          FailureReason::ArithmeticOverflow,
          {FailureOrigin::Domain, FailureScope::Group}};
}
inline Status cancelled(const char* message) {
  return Status::failure(ErrorCode::Cancelled, message);
}
inline Status cancellation_status(const CancellationToken& token,
                                  const char* message) {
  return token.cancelled() ? cancelled(message) : Status::success();
}
}  // namespace ps::core_internal
