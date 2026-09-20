#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "01-numeric/exact_ratio.hpp"

namespace ps::plugin_internal::numeric_ops {
// Compile-time decimal conversion. The grammar has already validated the token.
// <=4096 source bytes bound the exact significand by 13607 bits. For a result
// in the binary64 range, the largest power-of-ten denominator is below 14690
// bits; the rounding shifts add at most 1074 numerator / 53 denominator bits.
// 16384 bits therefore cover the complete token, including an arbitrarily late
// nonzero digit at a midpoint. Plan preparation owns this bounded scratch;
// it is never allocated per runtime sample.
inline Result<std::uint64_t> decimal_binary64(std::string_view token) {
  using Answer = Result<std::uint64_t>;
  const auto invalid = [] {
    return Answer(Status{ErrorCode::InvalidArgument,
                         "overflowing expression literal",
                         FailureReason::InvalidDomain,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  if (token.empty() || token.size() > 4096)
    return invalid();
  ExactRatioWorkspace<256> ratio(SequenceProfile::Strict);
  auto multiply_ten = [](auto* integer, unsigned digit) {
    unsigned __int128 carry = digit;
    for (auto& word : integer->words) {
      carry += static_cast<unsigned __int128>(word) * 10;
      word = static_cast<std::uint64_t>(carry);
      carry >>= 64;
    }
    return carry == 0;
  };
  int fractional = 0, significant = 0;
  bool dot = false, nonzero = false;
  std::size_t i = 0;
  for (; i < token.size() && token[i] != 'e' && token[i] != 'E'; ++i) {
    if (token[i] == '.') {
      dot = true;
      continue;
    }
    const auto digit = static_cast<unsigned>(token[i] - '0');
    if (dot)
      ++fractional;
    if (digit || nonzero) {
      nonzero = true;
      ++significant;
    }
    if (!multiply_ten(&ratio.numerator, digit))
      return invalid();
  }
  int exponent = 0;
  if (i < token.size()) {
    ++i;
    bool negative = false;
    if (token[i] == '+' || token[i] == '-')
      negative = token[i++] == '-';
    for (; i < token.size(); ++i)
      exponent = std::min(100000, exponent * 10 + token[i] - '0');
    if (negative)
      exponent = -exponent;
  }
  if (!nonzero)
    return Answer(UINT64_C(0));
  exponent -= fractional;
  const auto decimal_top = significant - 1 + exponent;
  if (decimal_top > 308)
    return invalid();
  if (decimal_top < -324)
    return Answer(UINT64_C(0));
  ratio.denominator.words[0] = 1;
  auto* scaled = exponent < 0 ? &ratio.denominator : &ratio.numerator;
  for (int j = 0; j < (exponent < 0 ? -exponent : exponent); ++j)
    if (!multiply_ten(scaled, 0))
      return invalid();
  auto result =
      ratio.round(false, [](std::uint64_t) { return Status::success(); }, 0);
  if (result.ok() && (result.value() & UINT64_C(0x7ff0000000000000)) ==
                         UINT64_C(0x7ff0000000000000))
    return invalid();
  return result;
}
}  // namespace ps::plugin_internal::numeric_ops
