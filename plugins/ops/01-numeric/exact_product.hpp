#pragma once

#include <cstdint>
#include <functional>

#include "01-numeric/exact_ratio.hpp"

namespace ps::plugin_internal::numeric_ops {
// Inputs may alias each other; output must be distinct. Every stored word and
// carry is bounded, with cancellation/work checks before each multiplication
// row.
template <std::size_t Words>
Status multiply_fixed(const FixedInteger<Words>& a,
                      const FixedInteger<Words>& b, FixedInteger<Words>* output,
                      const std::function<Status(std::uint64_t)>& consume) {
  auto status = consume(4 * Words);
  if (!status.ok())
    return status;
  output->words.fill(0);
  const auto a_size = (ExactRatioWorkspace<Words>::top(a) + 64) / 64;
  const auto b_size = (ExactRatioWorkspace<Words>::top(b) + 64) / 64;
  for (int i = 0; i < a_size; ++i) {
    status = consume(16 * b_size + 1);
    if (!status.ok())
      return status;
    unsigned __int128 carry = 0;
    for (int j = 0; j < b_size; ++j) {
      const auto slot = static_cast<std::size_t>(i + j);
      if (slot >= Words)
        return Status{ErrorCode::ResourceExhausted, "fixed product capacity",
                      FailureReason::CapacityLimit};
      const auto product =
          static_cast<unsigned __int128>(a.words[i]) * b.words[j] +
          output->words[slot] + carry;
      output->words[slot] = static_cast<std::uint64_t>(product);
      carry = product >> 64;
    }
    const auto slot = static_cast<std::size_t>(i + b_size);
    if (carry) {
      if (slot >= Words)
        return Status{ErrorCode::ResourceExhausted, "fixed carry capacity",
                      FailureReason::CapacityLimit};
      output->words[slot] = static_cast<std::uint64_t>(carry);
    }
  }
  return Status::success();
}
// Exact up-to-128-bit coefficient at bit zero, without truncating to an IEEE
// significand. The destination always has at least 68 words.
template <std::size_t Words>
void integer_coefficient(unsigned __int128 value, FixedInteger<Words>* output) {
  output->words.fill(0);
  output->words[0] = static_cast<std::uint64_t>(value);
  output->words[1] = static_cast<std::uint64_t>(value >> 64);
}
}  // namespace ps::plugin_internal::numeric_ops
