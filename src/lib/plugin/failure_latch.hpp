#pragma once

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
  ErrorCode load() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_.code();
  }
  Status snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return copy();
  }
  Status record(const Status& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failure_.ok()) {
      if (status.ok())
        failure_.record(Status{ErrorCode::Internal, {}});
      else
        failure_.record(status);
    }
    return copy();
  }
  void enrich(const Status& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    failure_.enrich(status);
  }
  bool compare_exchange_strong(ErrorCode& expected, ErrorCode desired) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failure_.code() != expected) {
      expected = failure_.code();
      return false;
    }
    failure_.record(Status{desired, {}});
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
  mutable std::mutex mutex_;
  core_internal::StoredFailure failure_;
};
}  // namespace ps::plugin_internal
