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

// Original logical requests, including bit reversal, recursive child visits,
// codelet arithmetic and natural-order copies. Shape products are <= 2^40;
// these costs fit uint64_t, including the residual radix <= 31.
inline std::uint64_t fft_work(std::uint64_t n) {
  unsigned levels = 0;
  for (auto v = n; v > 1; v >>= 1)
    ++levels;
  return n - 1 + (levels & 1 ? n / 2 * 512 : 0) + (levels / 2) * (n / 4) * 2048;
}
inline std::uint64_t odd_work(std::uint64_t radix) {
  const auto pairs = radix / 2;
  // Common twiddles; pair sums/differences and DC; four enclosed real
  // products plus four Complex sums per output/input pair.
  return radix * 512 + pairs * 192 + pairs * pairs * 896;
}
inline std::uint64_t mixed_work(std::uint64_t n) {
  if (n == 1)
    return 0;
  auto radix = n;
  for (auto r : {2U, 3U, 5U})
    if (n % r == 0) {
      radix = r;
      break;
    }
  const auto m = n / radix;
  const auto combine = radix == 2                   ? 1152
                       : radix == 3                 ? 2944
                       : radix == 5                 ? 7680
                       : radix <= 31 && (radix & 1) ? odd_work(radix)
                                                    : radix * radix * 512;
  return radix * (1 + mixed_work(m)) + m * combine + n;
}
}  // namespace ps::plugin_internal::frequency
