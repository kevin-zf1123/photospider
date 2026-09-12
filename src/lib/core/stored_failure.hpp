#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "photospider/core/status.hpp"

namespace ps::core_internal {
// Inline storage is charged with its owning runtime object. Failure recording
// cannot allocate, and therefore cannot lose a reason during resource failure.
class StoredFailure final {
 public:
  bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  ErrorCode code() const noexcept { return code_; }
  FailureOrigin origin() const noexcept { return detail_.origin; }
  void bind_producer(std::uint64_t node) noexcept {
    if (!producer_)
      producer_ = node;
    if (!detail_.node_id && !detail_.input_id)
      detail_.node_id = producer_;
  }
  void enrich(const Status& status) noexcept {
    if (code_ != status.code || reason_ != status.reason)
      return;
    if (detail_.origin == FailureOrigin::Unspecified)
      detail_.origin = status.detail.origin;
    if (detail_.scope == FailureScope::Unspecified) {
      detail_.scope = status.detail.scope;
      detail_.atom = status.detail.atom;
      detail_.domain = status.detail.domain;
      detail_.association = status.detail.association;
    }
    if (!detail_.node_id && !detail_.input_id) {
      detail_.node_id = status.detail.node_id;
      detail_.input_id = status.detail.input_id;
    }
  }
  void record(const Status& status) noexcept {
    if (!ok())
      return;
    code_ = status.ok() ? ErrorCode::Internal : status.code;
    reason_ = status.reason;
    detail_ = status.detail;
    bind_producer(producer_);
    size_ = std::min(status.message.size(), message_.size());
    if (size_)
      std::memcpy(message_.data(), status.message.data(), size_);
  }
  Status fixed_status() const noexcept {
    return Status{code_, {}, reason_, detail_};
  }
  Status status() const noexcept {
    try {
      return Status{code_, std::string(message_.data(), size_), reason_,
                    detail_};
    } catch (...) {
      return fixed_status();
    }
  }

 private:
  ErrorCode code_ = ErrorCode::Ok;
  FailureReason reason_ = FailureReason::None;
  FailureDetail detail_;
  std::array<char, 256> message_{};
  std::size_t size_ = 0;
  std::uint64_t producer_ = 0;
};
}  // namespace ps::core_internal
