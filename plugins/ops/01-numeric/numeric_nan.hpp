#pragma once

#include <cstdint>

#include "photospider/data/value.hpp"

namespace ps::plugin_internal::numeric_ops {
// Numeric reducers preserve the first NaN sign and payload. Conversion excludes
// the quiet bit; narrowing a nonzero payload never erases it completely.
inline std::uint64_t converted_nan(std::uint64_t bits, ElementType source,
                                   ElementType destination) {
  const bool from32 = source == ElementType::Float32;
  const bool to32 = destination == ElementType::Float32;
  const auto quiet = UINT64_C(1) << (from32 ? 22 : 51);
  auto payload = bits & (quiet - 1);
  if (from32 && !to32)
    payload <<= 29;
  if (!from32 && to32) {
    const auto original = payload;
    payload >>= 29;
    if (original && !payload)
      payload = 1;
  }
  const auto sign = (bits >> (from32 ? 31 : 63)) << (to32 ? 31 : 63);
  return sign | payload |
         (to32 ? UINT64_C(0x7fc00000) : UINT64_C(0x7ff8000000000000));
}
}  // namespace ps::plugin_internal::numeric_ops
