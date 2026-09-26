#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11): C++17 floating-environment regression.
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "02-format-color/alpha_math.hpp"
#include "support/alpha_golden.hpp"

namespace {
using ps::FailureReason;
using ps::Status;
using ps::plugin_internal::alpha_ops::Action;
using ps::plugin_internal::alpha_ops::math_span;
using ps::plugin_internal::alpha_ops::MathFailure;
using ps::plugin_internal::alpha_ops::simd_available;
void require(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}
struct Answer {
  MathFailure failure;
  std::vector<std::uint64_t> bits;
};
Answer run(const std::vector<std::uint64_t>& color,
           const std::vector<std::uint64_t>& alpha, bool narrow, Action action,
           bool raw, bool simd, bool reference, unsigned offset = 0) {
  require(color.size() == alpha.size(), "test input counts");
  const auto width = narrow ? 4U : 8U;
  const auto bytes = color.size() * width;
  // No readable suffix: ASan detects even a one-lane read past the authorized
  // extent. Non-natural byte offsets also exercise alignment independence.
  auto x = std::make_unique<std::uint8_t[]>(bytes + offset + (bytes ? 0 : 1));
  auto a = std::make_unique<std::uint8_t[]>(bytes + offset + (bytes ? 0 : 1));
  std::vector<std::uint8_t> y(bytes + offset + 16, 0xa5);
  for (std::size_t i = 0; i < color.size(); ++i) {
    std::memcpy(x.get() + offset + i * width, &color[i], width);
    std::memcpy(a.get() + offset + i * width, &alpha[i], width);
  }
  ps::input_internal::Float32Environment environment;
  require(environment.active(), "test floating environment");
  auto failure =
      math_span(x.get() + offset, a.get() + offset, y.data() + offset,
                color.size(), narrow, action, raw, simd, reference, environment,
                [](std::uint64_t) { return Status::success(); });
  require(failure.status.ok(), "unexpected arithmetic resource failure");
  require(std::all_of(y.begin(), y.begin() + offset,
                      [](auto c) { return c == 0xa5; }) &&
              std::all_of(y.begin() + offset + bytes, y.end(),
                          [](auto c) { return c == 0xa5; }),
          "write beyond span");
  Answer result{failure, std::vector<std::uint64_t>(color.size())};
  for (std::size_t i = 0; i < color.size(); ++i) {
    std::memcpy(&result.bits[i], y.data() + offset + i * width, width);
  }
  return result;
}
void same(const Answer& expected, const Answer& actual, const char* message) {
  require(expected.failure.reason == actual.failure.reason &&
              expected.failure.lane == actual.failure.lane &&
              std::string(expected.failure.operand) == actual.failure.operand,
          std::string(message) + ": failure order");
  if (expected.failure.reason == FailureReason::None) {
    require(expected.bits == actual.bits,
            std::string(message) + ": result bits");
  }
}
void golden_spans() {
  for (bool narrow : {true, false}) {
    for (bool divide : {false, true}) {
      const auto& cases =
          narrow ? alpha_golden::binary32 : alpha_golden::binary64;
      const auto action = divide ? Action::Unassociate : Action::Associate;
      for (unsigned count :
           {0U,  1U,  2U,  3U,  4U,  7U,   8U,   9U,   15U,  16U,
            17U, 31U, 32U, 63U, 64U, 127U, 128U, 129U, 130U, 257U}) {
        for (unsigned offset : {0U, 1U, 3U, 7U, 15U, 31U}) {
          std::vector<std::uint64_t> x, a, expected;
          for (unsigned i = 0; i < count; ++i) {
            const auto& c =
                cases[(i * 137 + count * 19 + offset * 31) % cases.size()];
            x.push_back(c.x);
            a.push_back(c.alpha);
            expected.push_back(divide ? c.divide : c.multiply);
          }
          for (bool simd : {false, true}) {
            if (simd && !simd_available()) {
              continue;
            }
            const auto got =
                run(x, a, narrow, action, true, simd, false, offset);
            require(got.failure.reason == FailureReason::None &&
                        got.bits == expected,
                    "independent golden, offset and tail");
          }
        }
      }
    }
  }
}
void exceptional_lanes() {
  for (bool narrow : {true, false}) {
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    const auto inf =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto one =
        narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
    const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
    for (const auto action :
         {Action::Associate, Action::Unassociate, Action::Set}) {
      std::vector<std::uint64_t> x(130, one), a(130, one);
      // Exercise every possible SIMD position, both sides of 128 and the tail.
      for (unsigned lane : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 15U, 31U, 63U,
                            127U, 128U, 129U}) {
        for (unsigned kind = 0; kind < 9; ++kind) {
          x.assign(130, one);
          a.assign(130, one);
          switch (kind) {
            case 0:
              a[lane] = sign;
              x[lane] = sign;
              break;
            case 1:
              a[lane] = 0;
              break;
            case 2:
              a[lane] = one + 1;
              break;
            case 3:
              a[lane] = inf | 123;
              break;
            case 4:
              x[lane] = inf | quiet | 456;
              break;
            case 5:
              a[lane] = inf;
              x[lane] = 0;
              break;
            case 6:
              a[lane] = 1;
              x[lane] = inf - 1;
              break;
            case 7:
              a[lane] = sign | one;
              break;
            case 8:
              x[lane] = sign;
              break;
          }
          for (bool raw : {false, true}) {
            if (raw && action == Action::Set) {
              continue;
            }
            const auto expected = run(x, a, narrow, action, raw, false, true);
            same(expected, run(x, a, narrow, action, raw, false, false, 1),
                 "scalar exceptional lane");
            if (simd_available()) {
              same(expected, run(x, a, narrow, action, raw, true, false, 3),
                   "SIMD exceptional lane");
            }
          }
        }
      }
    }
    std::vector<std::uint64_t> x(130, one), a(130, one);
    x[0] = inf - 1;
    a[0] = 1;
    x.back() = inf | quiet | 7;
    for (bool simd : {false, true}) {
      if (simd && !simd_available()) {
        continue;
      }
      const auto got =
          run(x, a, narrow, Action::Unassociate, false, simd, false);
      require(got.failure.reason == FailureReason::InvalidDomain &&
                  got.failure.lane == 129,
              "later domain error must precede earlier overflow");
    }
    // Both NaNs: first operand payload/sign wins, independent of ISA selection.
    x.assign(17, inf | 42);
    a.assign(17, sign | inf | quiet | 99);
    const auto expected =
        run(x, a, narrow, Action::Associate, true, false, true);
    if (simd_available()) {
      same(expected, run(x, a, narrow, Action::Associate, true, true, false),
           "NaN precedence");
    }
  }
}
void semantic_random() {
  std::uint64_t seed = UINT64_C(0x7e54935a12fd);
  auto random = [&] {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
  };
  for (bool narrow : {true, false}) {
    const auto one =
        narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
    const auto mask = narrow ? UINT64_C(0xffffffff) : UINT64_MAX;
    const auto inf =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    for (const auto action : {Action::Associate, Action::Unassociate}) {
      for (unsigned batch = 0; batch < 8; ++batch) {
        std::vector<std::uint64_t> x(129), a(129);
        for (std::size_t i = 0; i < x.size(); ++i) {
          x[i] = random() & mask;
          if ((x[i] & inf) == inf) {
            x[i] &= ~inf;
          }
          a[i] = random() % (one + 1);
          if (action == Action::Unassociate && !a[i]) {
            x[i] = 0;
          }
        }
        const auto expected = run(x, a, narrow, action, false, false, true);
        same(expected, run(x, a, narrow, action, false, false, false),
             "random scalar");
        if (simd_available()) {
          same(expected, run(x, a, narrow, action, false, true, false),
               "random SIMD");
        }
      }
    }
  }
}
void environment_restoration() {
  std::fenv_t original;
  require(std::fegetenv(&original) == 0, "save fenv");
  require(std::fesetround(FE_UPWARD) == 0, "set rounding");
  std::feclearexcept(FE_ALL_EXCEPT);
  std::feraiseexcept(FE_DIVBYZERO);
#if defined(__x86_64__)
  const auto csr = _mm_getcsr();
  _mm_setcsr(csr | (1U << 15) | (1U << 6));
  const auto changed = _mm_getcsr();
#endif
  const auto got = run(std::vector<std::uint64_t>(17, 1),
                       std::vector<std::uint64_t>(17, 0x3f800000), true,
                       Action::Associate, false, simd_available(), false, 1);
  const bool restored =
      std::fegetround() == FE_UPWARD && std::fetestexcept(FE_DIVBYZERO) != 0;
#if defined(__x86_64__)
  const bool controls = _mm_getcsr() == changed;
  _mm_setcsr(csr);
#else
  const bool controls = true;
#endif
  std::fesetenv(&original);
  require(restored && controls,
          "restore rounding, flags and denormal controls");
  require(got.failure.reason == FailureReason::None &&
              got.bits == std::vector<std::uint64_t>(17, 1),
          "gradual underflow");
}
}  // namespace
int main() try {
  golden_spans();
  exceptional_lanes();
  semantic_random();
  environment_restoration();
  std::cout << "alpha math: golden/tails/unaligned/exception-order/random/fenv "
               "passed; SIMD="
            << simd_available() << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << "alpha math: " << e.what() << '\n';
  return 1;
}
