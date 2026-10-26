#pragma once

#include "05-filter/common.hpp"

namespace ps::plugin_internal::frequency {
// A deterministic block owns exactly its declared work, admitted before any
// payload arithmetic. No speculative credit, retry, refund or deferred charge.
// Dynamic refinement must keep using Fuel/Work, never this cancel-only cursor.
class TransformBlock final {
 public:
  TransformBlock(const Work& work, std::uint64_t cost) : work_(work) {
    check(work_(cost));
  }
  void poll(std::uint64_t count = 1) const {
    // Every caller's indivisible arithmetic segment is at most 1024 units.
    // Poll before crossing the bound; the admission itself polls initially.
    if (count > 1024 - since_poll_) {
      const auto& cancel = work_.invocation.cancellation;
      if (cancel.is_cancelled && cancel.is_cancelled(cancel.context))
        check(work_.fail(ErrorCode::Cancelled, FailureReason::Cancelled));
      since_poll_ = 0;
    }
    since_poll_ += count;
  }

 private:
  const Work& work_;
  mutable std::uint64_t since_poll_ = 0;
};

}  // namespace ps::plugin_internal::frequency
