#pragma once

#include <atomic>
#include <mutex>
#include <utility>

#include "core/stored_failure.hpp"

namespace ps::plugin_internal {
/** First host failure wins, including its reason/scope. Escaped allocator
 * observers remain synchronized and cannot replace a protocol failure with a
 * later resource or execution error. Diagnostic-copy failure preserves all
 * fixed-size machine-readable fields without allocating a replacement string.
 */
class FailureLatch final {
 public:
  static bool retryable_backend_failure(const Status& status) noexcept {
    return status.code == ErrorCode::BackendUnavailable &&
           status.reason == FailureReason::None &&
           (status.detail.origin == FailureOrigin::Unspecified ||
            status.detail.origin == FailureOrigin::Backend) &&
           (status.detail.scope == FailureScope::Unspecified ||
            status.detail.scope == FailureScope::Group);
  }
  bool backend_retry_allowed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !backend_retry_veto_;
  }
  void veto_backend_retry() {
    std::lock_guard<std::mutex> lock(mutex_);
    backend_retry_veto_ = true;
  }
  ErrorCode load() const { return code_.load(std::memory_order_acquire); }
  Status snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return copy();
  }
  Status record(const Status& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!retryable_backend_failure(status))
      backend_retry_veto_ = true;
    if (failure_.ok()) {
      if (status.ok())
        failure_.record(Status{ErrorCode::Internal, {}});
      else
        failure_.record(status);
    }
    code_.store(failure_.code(), std::memory_order_release);
    return copy();
  }
  void enrich(const Status& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    failure_.enrich(status);
  }
  bool compare_exchange_strong(ErrorCode& expected, ErrorCode desired) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (desired != ErrorCode::BackendUnavailable)
      backend_retry_veto_ = true;
    if (failure_.code() != expected) {
      expected = failure_.code();
      return false;
    }
    failure_.record(Status{desired, {}});
    code_.store(failure_.code(), std::memory_order_release);
    return true;
  }

 private:
  Status copy() const {
    try {
      return failure_.status();
    } catch (...) {
      return failure_.fixed_status();
    }
  }
  // Publish only after the locked diagnostic record is complete.
  std::atomic<ErrorCode> code_{ErrorCode::Ok};
  mutable std::mutex mutex_;
  core_internal::StoredFailure failure_;
  // A later host error cannot replace the first cause, but it still forbids
  // replaying an attempt that initially failed on an unavailable backend.
  bool backend_retry_veto_ = false;
};
}  // namespace ps::plugin_internal
