#pragma once

#include "04-mask-morphology/common.hpp"

namespace ps::plugin_internal::mask {
// Charge every submitted estimate immediately to the invocation's existing
// Work counter. Only cancellation callbacks are batched; other Work users see
// the same used count, and no pending budget can be hidden from their checks.
// Quantum is in caller-defined work units; it is not an elapsed-time bound.
template <std::uint64_t Quantum>
struct BatchedWork final {
  static_assert(Quantum > 0, "work quantum must be positive");
  const Work& work;
  mutable std::uint64_t pending = 0;
  Status operator()(std::uint64_t amount) const {
    if (amount > work.limit - work.used)
      return work(amount);
    if (!amount || amount >= Quantum - pending) {
      pending = 0;
      return work(amount);
    }
    work.used += amount;
    pending += amount;
    return Status::success();
  }
  void poll(std::uint64_t amount = 1) const { check((*this)(amount)); }
  void flush() const {
    pending = 0;
    work.poll(0);
  }
};
using ArithmeticWork = BatchedWork<8192>;
}  // namespace ps::plugin_internal::mask
