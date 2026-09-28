#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "05-filter/gaussian_coefficients.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using ps::plugin_internal::filter_ops::GaussianCoefficients;
}  // namespace
int main(int argc, char** argv) {
  for (unsigned stop : {0U, 1U, 128U, 129U}) {
    unsigned calls = 0;
    const auto construct = [&](std::uint64_t amount) {
      ++calls;
      if (amount != (calls <= 128 ? 193U : 971U))
        return Status{ErrorCode::Internal, "unexpected initialization bound"};
      return calls == stop ? Status{ErrorCode::Cancelled, "stop construction"}
                           : Status::success();
    };
    bool cancelled = false;
    try {
      auto constructed = std::make_unique<GaussianCoefficients>(construct);
      PS_CHECK(calls == 129);
    } catch (const Status& status) {
      PS_CHECK(status.code == ErrorCode::Cancelled);
      cancelled = true;
    }
    PS_CHECK(cancelled == (stop != 0));
    PS_CHECK(calls == (stop ? stop : 129));
  }
  auto math = std::make_unique<GaussianCoefficients>();
  const auto unlimited = [](std::uint64_t) { return Status::success(); };
  if (argc == 2 && std::string(argv[1]) == "--stdin") {
    std::uint64_t sigma, offset;
    while (std::cin >> std::hex >> sigma >> std::dec >> offset) {
      const auto value = math->coefficient(sigma, offset, unlimited);
      if (!value.ok()) {
        std::cerr << value.status().message << '\n';
        return 1;
      }
      std::cout << std::hex << value.value() << '\n';
    }
    return std::cin.eof() ? 0 : 2;
  }
  const std::array<std::uint64_t, 4> expected{
      UINT64_C(0x3ff0000000000000), UINT64_C(0x3fe368b2fc6f960a),
      UINT64_C(0x3fc152aaa3bf81cc), UINT64_C(0x3f86c0504695c417)};
  for (unsigned j = 0; j < expected.size(); ++j) {
    auto result = math->coefficient(UINT64_C(0x3ff0000000000000), j, unlimited);
    PS_CHECK(result.ok());
    PS_CHECK(result.value() == expected[j]);
  }
  auto tiny = math->coefficient(UINT64_C(0x3fc0000000000000), 4, unlimited);
  PS_CHECK(tiny.ok() && tiny.value() == UINT64_C(0x11c44109edb20931));
  auto zero = math->coefficient(UINT64_C(0x3fc0000000000000), 5, unlimited);
  PS_CHECK(zero.ok() && zero.value() == 0);
  for (auto sigma : {UINT64_C(0), UINT64_C(0x8000000000000000)}) {
    auto center = math->coefficient(sigma, 0, unlimited);
    PS_CHECK(center.ok() && center.value() == expected[0]);
    PS_CHECK(!math->coefficient(sigma, 1, unlimited).ok());
  }
  for (auto sigma : {UINT64_C(0xbff0000000000000), UINT64_C(0x7ff0000000000000),
                     UINT64_C(0x7ff0000000000001)})
    PS_CHECK(!math->coefficient(sigma, 0, unlimited).ok());
  auto smallest = math->coefficient(1, 1, unlimited);
  PS_CHECK(smallest.ok() && smallest.value() == 0);
  auto largest =
      math->coefficient(UINT64_C(0x7fefffffffffffff), INT64_MAX, unlimited);
  PS_CHECK(largest.ok() && largest.value() == expected[0]);
  unsigned calls = 0;
  const auto cancelled = [&](std::uint64_t) {
    return ++calls == 11 ? Status{ErrorCode::Cancelled, "cancel refinement"}
                         : Status::success();
  };
  auto interrupted =
      math->coefficient(UINT64_C(0x3ff0000000000000), 3, cancelled);
  PS_CHECK(!interrupted.ok() &&
           interrupted.status().code == ErrorCode::Cancelled);
  auto reused = math->coefficient(UINT64_C(0x3ff0000000000000), 3, unlimited);
  PS_CHECK(reused.ok() && reused.value() == expected[3]);
  const auto limited = [](std::uint64_t) {
    return Status{ErrorCode::ResourceExhausted, "work limit"};
  };
  auto exhausted = math->coefficient(UINT64_C(0x3ff0000000000000), 0, limited);
  PS_CHECK(!exhausted.ok() &&
           exhausted.status().code == ErrorCode::ResourceExhausted);
  // Force a 65-limb multiply independently of whether a particular
  // coefficient happens to need 4096-bit refinement. Cancellation must be
  // observed inside its first row, after the 32-word checkpoint.
  using Integer = ps::plugin_internal::numeric_ops::FixedInteger<192>;
  auto a = std::make_unique<Integer>();
  auto b = std::make_unique<Integer>();
  auto product = std::make_unique<Integer>();
  a->words[64] = b->words[64] = 1;
  unsigned in_row = 0;
  const auto stop_row = [&](std::uint64_t units) {
    if (!units) {
      ++in_row;
      return Status{ErrorCode::Cancelled, "cancel inside multiply row"};
    }
    return Status::success();
  };
  auto row = ps::plugin_internal::numeric_ops::multiply_fixed(
      *a, *b, product.get(), stop_row, 32);
  PS_CHECK(row.code == ErrorCode::Cancelled && in_row == 1);
  auto& interval = math->functions.math;
  interval.consume = unlimited;
  interval.precision = 4096;
  interval.used = 0;
  auto left = interval.interval(), right = interval.interval(),
       output = interval.interval();
  interval.integer(left, -1);
  interval.integer(right, 1);
  interval.consume = stop_row;
  bool stopped = false;
  try {
    interval.multiply(output, left, right);
  } catch (const Status& status) {
    stopped = status.code == ErrorCode::Cancelled;
  }
  interval.consume = nullptr;
  interval.used = 0;
  PS_CHECK(stopped && in_row == 2);
  return 0;
}
