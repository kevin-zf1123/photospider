#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11): required floating environment API.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <tuple>
#include <vector>

#include "02-format-color/rgb_basis_limb.hpp"
#include "02-format-color/rgb_basis_math.hpp"
#include "fixtures/fmt10_oracles.hpp"
#include "plugin/port_validation.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps::plugin_internal::basis_ops;  // NOLINT(build/namespaces)

Matrix matrix(const fmt10_oracle::Case& c) {
  if (c.member == 2)
    return adaptation(c.basis ? std::array<double, 2>{.3127, .3290}
                              : std::array<double, 2>{.25, .25},
                      c.basis ? std::array<double, 2>{.3457, .3585}
                              : std::array<double, 2>{.5, .25},
                      c.method);
  const auto& v = fmt10_oracle::geometry[c.basis];
  auto m = rgb_matrix({v[0], v[1], v[2], v[3], v[4], v[5]}, {v[6], v[7]});
  return c.member ? inverse(m) : m;
}
double unpack(std::uint64_t value, bool narrow) {
  if (narrow) {
    auto word = static_cast<std::uint32_t>(value);
    float f;
    std::memcpy(&f, &word, 4);
    return f;
  }
  double d;
  std::memcpy(&d, &value, 8);
  return d;
}
int oracle_cases() {
  std::size_t exact_count = 0, certificates = 0;
  std::map<std::tuple<unsigned, unsigned, unsigned>, std::array<ExactRow, 3>>
      rows;
  for (const auto& c : fmt10_oracle::cases) {
    auto key = std::make_tuple(c.member, c.basis, c.method);
    auto it = rows.find(key);
    if (it == rows.end()) {
      auto m = matrix(c);
      it = rows.emplace(key,
                        std::array<ExactRow, 3>{ExactRow(m, 0), ExactRow(m, 1),
                                                ExactRow(m, 2)})
               .first;
    }
    for (unsigned r = 0; r < 3; ++r) {
      const auto& row = it->second[r];
      const auto actual = row.evaluate(c.input, c.narrow);
      if (actual != c.expected[r]) {
        std::cerr << "oracle mismatch case " << exact_count << " member "
                  << c.member << " basis " << c.basis << " row " << r
                  << " actual " << std::hex << actual << " expected "
                  << c.expected[r] << std::dec << '\n';
      }
      PS_CHECK(actual == c.expected[r]);
      PS_CHECK(row.evaluate_reference(c.input, c.narrow) == actual);
      ++exact_count;
      CandidateBlock block;
      for (unsigned j = 0; j < 3; ++j)
        block.input[j][0] = unpack(c.input[j], c.narrow);
      candidates(&block, 1, row, SequenceProfile::Strict);
      std::uint64_t certified = 0;
      if (certify(row, block, 0, c.narrow, SequenceProfile::Strict,
                  &certified)) {
        PS_CHECK(certified == actual);
        ++certificates;
      }
      if (certify(row, block, 0, c.narrow, SequenceProfile::X86Avx2,
                  &certified)) {
        if (c.narrow) {
          PS_CHECK(certified == actual);
        } else {
          const double x = unpack(certified, false),
                       ref = unpack(actual, false);
          const float f = static_cast<float>(ref);
          const double up =
              std::nextafter(f, INFINITY) - static_cast<double>(f);
          const double down =
              static_cast<double>(f) - std::nextafter(f, -INFINITY);
          PS_CHECK(std::isfinite(x) &&
                   std::abs(x - ref) <= 4 * std::max(up, down));
        }
      }
    }
  }
  std::cout << "independent exact row checks=" << exact_count
            << " strict certified checks=" << certificates << '\n';
  return 0;
}
int specials() {
  Matrix m;
  for (unsigned i = 0; i < 9; ++i)
    m[i] = Rational::integer(i % 4 == 0 ? 2 : 0);
  ExactRow row(m, 0);
  PS_CHECK(row.evaluate({0xff800123, 0x7fc00077, 0}, true) == 0xffc00123);
  PS_CHECK(row.evaluate({0, 0x7f800000, 0}, true) == 0x7fc00000);
  PS_CHECK(row.evaluate({0x7f800000, 0, 0}, true) == 0x7f800000);
  PS_CHECK(row.evaluate({0xff800000, 0, 0}, true) == 0xff800000);
  PS_CHECK(row.evaluate({0x80000000, 0, 0}, true) == 0);
  m[0] = number(.5);
  row = ExactRow(m, 0);
  PS_CHECK(row.evaluate({0x80000001, 0, 0}, true) == 0x80000000);
  PS_CHECK(row.evaluate({3, 0, 0}, true) == 2);  // ties-to-even
  m[0] = number(1);
  m[1] = number(-1);
  row = ExactRow(m, 0);
  PS_CHECK(row.evaluate({0x7f800000, 0x7f800000, 0}, true) == 0x7fc00000);
  PS_CHECK(row.evaluate({0xfff0000000000001ULL, 0, 0}, false) ==
           0xfff8000000000001ULL);
  PS_CHECK(identity(adaptation({.25, .25}, {.25, .25}, 1)));
  bool rejected = false;
  try {
    (void)adaptation({.05, .05}, {.05, .05}, 1);
  } catch (const GeometryError&) {
    rejected = true;
  }
  PS_CHECK(rejected);
  rejected = false;
  try {
    (void)rgb_matrix({1, 0, 1, 0, 0, 0}, {.25, .25});
  } catch (const GeometryError&) {
    rejected = true;
  }
  PS_CHECK(rejected);
  return 0;
}
std::uint64_t random_word(std::uint64_t* state) {
  *state ^= *state << 13;
  *state ^= *state >> 7;
  *state ^= *state << 17;
  return *state;
}
int simd_and_random() {
  const auto m = rgb_matrix({.64, .33, .30, .60, .15, .06}, {.3127, .3290});
  std::vector<SequenceProfile> profiles{SequenceProfile::Strict};
  for (auto p : {SequenceProfile::X86Avx2, SequenceProfile::AppleSilicon})
    if (ps::plugin_internal::numeric_ops::sequence_profile_available(p).ok())
      profiles.push_back(p);
  std::uint64_t rng = 0x1977af09531ULL;
  std::uint64_t checked = 0, accepted = 0;
  for (unsigned r = 0; r < 3; ++r) {
    ExactRow row(m, r);
    for (unsigned trial = 0; trial < 24; ++trial) {
      CandidateBlock base;
      std::array<std::array<std::uint64_t, 3>, block_size> raw{};
      for (unsigned lane = 0; lane < block_size; ++lane)
        for (unsigned j = 0; j < 3; ++j) {
          auto word = static_cast<std::uint32_t>(random_word(&rng));
          if ((word & 0x7f800000) == 0x7f800000)
            word ^= 1U << 23;
          raw[lane][j] = word;
          base.input[j][lane] = unpack(word, true);
        }
      candidates(&base, block_size, row, SequenceProfile::Strict);
      for (auto profile : profiles) {
        auto block = base;
        candidates(&block, block_size, row, profile);
        PS_CHECK(block.value == base.value);
        PS_CHECK(block.magnitude == base.magnitude);
        // Short tails exercise every possible SIMD remainder without OOB.
        for (unsigned tail = 0; tail < 8; ++tail)
          candidates(&block, tail, row, profile);
        for (unsigned lane = 0; lane < block_size; ++lane) {
          auto exact = row.evaluate(raw[lane], true);
          std::uint64_t result = 0;
          if (certify(row, block, lane, true, profile, &result)) {
            PS_CHECK(result == exact);
            ++accepted;
          }
          ++checked;
        }
      }
    }
  }
  std::cout << "random certificate/SIMD checks=" << checked
            << " accepted=" << accepted << " profiles=" << profiles.size()
            << '\n';
  return 0;
}
int outward_neighbors() {
  const std::array<std::uint64_t, 12> edges{0,
                                            UINT64_C(0x8000000000000000),
                                            1,
                                            UINT64_C(0x8000000000000001),
                                            UINT64_C(0x000fffffffffffff),
                                            UINT64_C(0x0010000000000000),
                                            UINT64_C(0x7fefffffffffffff),
                                            UINT64_C(0xffefffffffffffff),
                                            UINT64_C(0x7ff0000000000000),
                                            UINT64_C(0xfff0000000000000),
                                            UINT64_C(0x3ff0000000000000),
                                            UINT64_C(0xbff0000000000000)};
  std::uint64_t seed = UINT64_C(0xdecaf107099), checked = 0;
  for (unsigned iteration = 0; iteration < 100000; ++iteration) {
    const auto word =
        iteration < edges.size() ? edges[iteration] : random_word(&seed);
    const double value = unpack(word, false);
    if ((word & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x7ff0000000000000)) {
      PS_CHECK(bits(outward(value, true)) == word);
      PS_CHECK(bits(outward(value, false)) == word);
      continue;
    }
    for (bool up : {false, true}) {
      PS_CHECK(bits(outward(value, up)) ==
               bits(std::nextafter(value, up ? INFINITY : -INFINITY)));
      ++checked;
    }
  }
  std::cout << "outward neighbor comparisons=" << checked << '\n';
  return 0;
}
bool same_integer(const RowUnsigned& a, const Natural& b) {
  return a.size == b.words.size() &&
         std::equal(b.words.begin(), b.words.end(), a.words.begin());
}
int bounded_limb_arithmetic() {
  std::uint64_t seed = UINT64_C(0xabc123731);
  for (unsigned trial = 0; trial < 512; ++trial) {
    Natural n;
    n.words.resize(trial % 79);
    for (auto& word : n.words)
      word = static_cast<std::uint32_t>(random_word(&seed));
    n.trim();
    const auto factor = random_word(&seed);
    RowUnsigned a(n);
    a.multiply(factor);
    PS_CHECK(same_integer(a, Natural::multiply(n, Natural(factor))));
    for (unsigned shift : {0U, 1U, 31U, 32U, 33U, 63U, 64U, 129U, 2047U}) {
      RowUnsigned shifted(n);
      shifted.shift_left(shift);
      PS_CHECK(same_integer(shifted, n.shift(shift)));
      shifted.shift_right(shift);
      PS_CHECK(same_integer(shifted, n));
    }
    RowUnsigned doubled(n);
    doubled.add(doubled);  // alias-safe carry propagation
    PS_CHECK(same_integer(doubled, Natural::add(n, n)));
    doubled.subtract(RowUnsigned(n));
    PS_CHECK(same_integer(doubled, n));
  }
  // Long borrow/carry chains and explicit 8192-bit capacity fence.
  auto n = Natural(1).shift(7900);
  RowUnsigned a(n);
  a.subtract(RowUnsigned(Natural(1)));
  PS_CHECK(same_integer(a, Natural::subtract(n, Natural(1))));
  bool exhausted = false;
  try {
    a.shift_left(1024);
  } catch (const std::bad_alloc&) {
    exhausted = true;
  }
  PS_CHECK(exhausted);
  std::cout << "bounded limb arithmetic: 512 randomized sets and capacity "
               "boundary passed\n";
  return 0;
}
int exact_differential() {
  std::uint64_t seed = UINT64_C(0xf1070ddcafe), checked = 0;
  for (unsigned basis = 0; basis < 10; ++basis) {
    const auto& v = fmt10_oracle::geometry[basis];
    const auto forward =
        rgb_matrix({v[0], v[1], v[2], v[3], v[4], v[5]}, {v[6], v[7]});
    for (const auto& m : {forward, inverse(forward)}) {
      for (unsigned r = 0; r < 3; ++r) {
        ExactRow row(m, r);
        for (bool narrow : {true, false}) {
          for (unsigned trial = 0; trial < 64; ++trial) {
            std::array<std::uint64_t, 3> words;
            for (auto& word : words) {
              word = random_word(&seed);
              if (narrow)
                word &= UINT64_C(0xffffffff);
            }
            // Cancellation across signs and wide exponent gaps; retain raw
            // nonfinites too, so payload propagation is compared as bits.
            if (trial % 4 == 0)
              words[1] = words[0] ^ (UINT64_C(1) << (narrow ? 31 : 63));
            PS_CHECK(row.evaluate(words, narrow) ==
                     row.evaluate_reference(words, narrow));
            ++checked;
          }
        }
      }
    }
  }
  std::cout << "exact limb/reference differential rows=" << checked << '\n';
  return 0;
}
}  // namespace
int main() {
  ps::input_internal::Float32Environment environment;
  PS_CHECK(environment.active());
  PS_CHECK(oracle_cases() == 0);
  PS_CHECK(specials() == 0);
  PS_CHECK(simd_and_random() == 0);
  PS_CHECK(outward_neighbors() == 0);
  PS_CHECK(bounded_limb_arithmetic() == 0);
  PS_CHECK(exact_differential() == 0);
  return 0;
}
