#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "01-numeric/exact_product.hpp"

namespace ps::plugin_internal::generation_ops {
// Decode floor(x) mod 256 and |fraction| without floating instructions or
// converting an unbounded finite coordinate to a machine integer.
struct PerlinCoordinate final {
  unsigned lattice = 0, denominator_bits = 0;
  std::uint64_t remainder = 0;
  bool complement = false;
  static Result<PerlinCoordinate> decode(std::uint64_t raw, bool narrow) {
    const auto p = numeric_ops::BinaryParts::decode(raw, narrow);
    if (p.nan || p.infinite)
      return Result<PerlinCoordinate>(
          Status{ErrorCode::InvalidArgument, "nonfinite Perlin coordinate"});
    PerlinCoordinate result;
    unsigned integral = 0;
    if (p.exponent >= 0) {
      if (p.exponent < 8)
        integral = static_cast<unsigned>(p.significand << p.exponent) & 255;
    } else {
      const auto bits = static_cast<unsigned>(-p.exponent);
      integral =
          bits < 64 ? static_cast<unsigned>(p.significand >> bits) & 255 : 0;
      result.remainder = bits < 64 ? p.significand & ((UINT64_C(1) << bits) - 1)
                                   : p.significand;
      if (result.remainder) {
        const auto zeros =
            static_cast<unsigned>(__builtin_ctzll(result.remainder));
        result.remainder >>= zeros;
        result.denominator_bits = bits - zeros;
        result.complement = p.negative;
      }
    }
    result.lattice =
        p.negative ? (0U - integral - (result.remainder != 0)) & 255 : integral;
    return Result<PerlinCoordinate>(result);
  }
};

inline constexpr std::array<unsigned, 256> kPerlinPermutation = {
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,
    225, 140, 36,  103, 30,  69,  142, 8,   99,  37,  240, 21,  10,  23,  190,
    6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203, 117,
    35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136,
    171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158,
    231, 83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,  55,  46,
    245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209,
    76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,
    164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124, 123, 5,
    202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,
    58,  17,  182, 189, 28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,
    154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253,
    19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,
    228, 251, 34,  242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,  51,
    145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157, 184,
    84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,
    222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156,
    180};  // NOLINT(whitespace/indent_namespace)

// All numerators use a common D=2^q. Fade has denominator D^5;
// weight*dot has denominator D^16. Before sign cancellation the sum of eight
// magnitudes is <16*D^16. Intermediate polynomial terms are <=31*D^2.
// Thus 16*q+5 bits suffice, including rounding's dyadic extraction (no shift
// workspace enlargement). q<=31/63/1074 fits 8/16/272 uint64 limbs.
// The owner allocates this fixed workspace through the execution allocator.
template <std::size_t Words>
struct PerlinExact final {
  using Integer = numeric_ops::FixedInteger<Words>;
  using Ratio = numeric_ops::ExactRatioWorkspace<Words>;
  Ratio ratio{numeric_ops::SequenceProfile::Strict};
  std::array<Integer, 3> fractions;
  std::array<std::array<Integer, 2>, 3> fades;
  Integer denominator, a, b, c, weight, dot;

  // Conservative declared work, reserved before evaluating one sample. The
  // local consumer still checks every multiplication row and never exceeds
  // this credit. Unused credit is not refunded. This avoids a shared atomic
  // admission update for every inner arithmetic row.
  static std::uint64_t work_bound(unsigned q) {
    if (!q)
      return 32 * Words;
    const auto limbs = [](unsigned bits) -> std::uint64_t {
      return (bits + 63) / 64;
    };
    const auto product = [&](unsigned a_bits, unsigned b_bits) {
      return 4 * Words + limbs(a_bits) * (16 * limbs(b_bits) + 1);
    };
    return 32 * Words +
           3 * (product(q, q) + product(2 * q, q) + product(2 * q + 4, 3 * q)) +
           8 * (product(5 * q + 1, 5 * q + 1) + product(10 * q + 1, 5 * q + 1) +
                product(15 * q + 1, q + 2)) +
           512;
  }

  static void power(unsigned exponent, Integer* target) {
    target->words.fill(0);
    target->words[exponent / 64] = UINT64_C(1) << (exponent % 64);
  }
  static void times(unsigned factor, Integer* target) {
    unsigned __int128 carry = 0;
    for (auto& word : target->words) {
      const auto value = static_cast<unsigned __int128>(word) * factor + carry;
      word = static_cast<std::uint64_t>(value);
      carry = value >> 64;
    }
  }
  Result<std::uint64_t> evaluate(
      const std::array<PerlinCoordinate, 3>& coordinates, bool output_narrow,
      const execution_internal::WorkConsumer& consume) {
    using Answer = Result<std::uint64_t>;
    unsigned q = 0;
    for (const auto& coordinate : coordinates)
      q = std::max(q, coordinate.denominator_bits);
    if (16 * q + 5 > Words * 64)
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "Perlin exact capacity",
                           FailureReason::CapacityLimit});
    auto status = consume(32 * Words);
    if (!status.ok())
      return Answer(status);
    if (!q)
      return Answer(UINT64_C(0));
    power(q, &denominator);
    for (unsigned axis = 0; axis < 3; ++axis) {
      const auto& coordinate = coordinates[axis];
      numeric_ops::integer_coefficient(coordinate.remainder, &a);
      Ratio::shift(a, q - coordinate.denominator_bits, &fractions[axis]);
      if (coordinate.complement) {
        a = denominator;
        a.subtract(fractions[axis]);
        fractions[axis] = a;
      }
      const auto& n = fractions[axis];
      // n^3 * (6*n^2 - 15*n*D + 10*D^2); the parenthesis is positive.
      status = numeric_ops::multiply_fixed(n, n, &a, consume);
      if (!status.ok())
        return Answer(status);
      status = numeric_ops::multiply_fixed(a, n, &b, consume);
      if (!status.ok())
        return Answer(status);
      times(6, &a);
      power(2 * q, &c);
      times(10, &c);
      a.add(c);
      Ratio::shift(n, q, &c);
      times(15, &c);
      a.subtract(c);
      status = numeric_ops::multiply_fixed(a, b, &fades[axis][1], consume);
      if (!status.ok())
        return Answer(status);
      power(5 * q, &fades[axis][0]);
      fades[axis][0].subtract(fades[axis][1]);
    }
    ratio.numerator.words.fill(0);
    ratio.negative = false;
    for (unsigned corner = 0; corner < 8; ++corner) {
      const unsigned x = corner & 1, y = (corner >> 1) & 1, z = corner >> 2;
      const auto p = [](unsigned index) {
        return kPerlinPermutation[index & 255];
      };
      const auto hash =
          p(p(p(coordinates[0].lattice + x) + coordinates[1].lattice + y) +
            coordinates[2].lattice + z) &
          15;
      const unsigned u = hash < 8 ? 0 : 1;
      const unsigned v = hash < 4 ? 1 : (hash == 12 || hash == 14) ? 0 : 2;
      const auto component = [&](unsigned axis, Integer* target) {
        const bool upper = (corner >> axis) & 1;
        *target = upper ? denominator : fractions[axis];
        if (upper)
          target->subtract(fractions[axis]);
        return upper;
      };
      bool sign = component(u, &dot) != ((hash & 1) != 0);
      const bool other_sign = component(v, &a) != ((hash & 2) != 0);
      if (sign == other_sign) {
        dot.add(a);
      } else if (ratio.compare(dot, a) >= 0) {
        dot.subtract(a);
      } else {
        a.subtract(dot);
        dot = a;
        sign = other_sign;
      }
      status =
          numeric_ops::multiply_fixed(fades[0][x], fades[1][y], &a, consume);
      if (!status.ok())
        return Answer(status);
      status = numeric_ops::multiply_fixed(a, fades[2][z], &weight, consume);
      if (!status.ok())
        return Answer(status);
      status = numeric_ops::multiply_fixed(weight, dot, &ratio.term, consume);
      if (!status.ok())
        return Answer(status);
      ratio.add_term(sign);
    }
    power(16 * q, &ratio.denominator);
    return ratio.round(output_narrow, consume, 0);
  }
};
}  // namespace ps::plugin_internal::generation_ops
