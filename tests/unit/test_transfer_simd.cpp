#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

#include "01-numeric/accelerated_math.hpp"
#include "02-format-color/transfer_simd.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;                   // NOLINT(build/namespaces)
using namespace ps::plugin_internal;  // NOLINT(build/namespaces)
std::uint64_t reference(std::uint64_t bits, bool narrow, bool encode) {
  const auto sign =
      narrow ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
  const auto inf = narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  const auto quiet = narrow ? UINT64_C(0x400000) : UINT64_C(0x8000000000000);
  const auto magnitude = bits & ~sign;
  if (magnitude > inf)
    return bits | quiet;
  const double x = numeric_ops::numeric_double(magnitude, narrow);
  if (narrow) {
    volatile float f = static_cast<float>(x);
    const float y = encode ? std::sqrt(f) : f * f;
    return numeric_ops::numeric_bits(y, true) | (bits & sign);
  }
  volatile double d = x;
  const double y = encode ? std::sqrt(d) : d * d;
  return numeric_ops::numeric_bits(y) | (bits & sign);
}
}  // namespace
int main() {
  if (!numeric_ops::accelerated_math_available())
    return 77;
  input_internal::Float32Environment environment;
  PS_CHECK(environment.active());
  std::mt19937_64 rng(9092026);
  std::uint64_t checks = 0;
  for (bool narrow : {false, true}) {
    const auto width = narrow ? 4U : 8U;
    const auto sign =
        narrow ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
    const auto inf =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const std::vector<std::uint64_t> special = {
        0,
        sign,
        1,
        sign | 1,
        inf,
        inf | sign,
        inf + 1,
        inf + 5 + sign,
        inf - 1,
        (inf - 1) | sign,
        narrow ? UINT64_C(0x00800000) : UINT64_C(0x0010000000000000)};
    for (bool encode : {false, true})
      for (unsigned count = 0; count <= 65; ++count) {
        // Four distinct misalignments; exact-size input allocations let ASan
        // catch even one padded vector lane past the authorized span.
        for (unsigned offset : {0U, 1U, 3U, 7U}) {
          std::vector<std::uint8_t> input(offset + count * width);
          std::vector<std::uint8_t> output(offset + count * width + 13, 0xa7);
          std::vector<std::uint64_t> expected;
          for (unsigned i = 0; i < count; ++i) {
            auto b = i < special.size() ? special[i] : rng();
            if (narrow)
              b &= UINT64_C(0xffffffff);
            std::memcpy(input.data() + offset + i * width, &b, width);
            expected.push_back(reference(b, narrow, encode));
          }
          transfer_ops::gamma2_simd(
              input.empty() ? nullptr : input.data() + offset,
              output.data() + offset, count, narrow, encode);
          for (unsigned i = 0; i < count; ++i) {
            std::uint64_t got = 0;
            std::memcpy(&got, output.data() + offset + i * width, width);
            PS_CHECK(got == expected[i]);
            ++checks;
          }
          PS_CHECK(std::all_of(output.begin(), output.begin() + offset,
                               [](auto b) { return b == 0xa7; }));
          PS_CHECK(std::all_of(output.begin() + offset + count * width,
                               output.end(), [](auto b) { return b == 0xa7; }));
        }
      }
  }
  std::cout << "FMT-09 gamma2 SIMD: " << checks
            << " exact scalar comparisons; lengths 0..65, unaligned, "
               "subnormal/overflow/NaN payload/tails PASS\n";
}
