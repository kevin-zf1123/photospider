#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "core/numeric_bits.hpp"
#include "data/lut3d_bake_validation.hpp"
#include "support/test_support.hpp"

namespace {
double number(std::uint64_t bits) {
  double result = 0;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}
struct Fixture {
  ps::Lut3dBakeDescription description;
  ps::Lut3dBakeReport report;
  Fixture(std::uint64_t first, std::uint64_t last, std::uint64_t step,
          unsigned count) {
    description.shape = {count, 2, 2};
    report.axis = {number(first), number(last), number(step), 0, 1, 1, 0, 1, 1};
    report.validation_count = count - 1;
    for (unsigned i = 0; i < 3; ++i)
      report.max_error_point[3 * i] = number(first);
  }
  ps::Status validate(const std::function<ps::Status(std::uint64_t)>& work,
                      std::uint8_t passed = 1) {
    return ps::input_internal::validate_lut3d_bake_report_values(
        description, &report, passed, work);
  }
};
}  // namespace

int main() {
  constexpr std::uint64_t sign = UINT64_C(0x8000000000000000);
  constexpr std::uint64_t one = UINT64_C(0x3ff0000000000000);
  constexpr std::uint64_t half = UINT64_C(0x3fe0000000000000);
  constexpr std::uint64_t maximum = UINT64_C(0x7fefffffffffffff);
  constexpr std::uint64_t half_ulp = UINT64_C(0x3ca0000000000000);
  constexpr std::uint64_t infinity = UINT64_C(0x7ff0000000000000);
  struct AxisCase {
    std::uint64_t first, last, step;
    unsigned count;
    const char* failure;
  };
  const AxisCase cases[] = {
      {0, one, half, 3, nullptr},
      {one, 0, sign | half, 3, nullptr},
      {sign, one, one, 2, nullptr},
      {one, sign, sign | one, 2, nullptr},
      {sign | maximum, maximum, maximum, 3, nullptr},
      {maximum, sign | maximum, sign | maximum, 3, nullptr},
      {sign | 2, 1, 2, 3, nullptr},
      {UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000001), 1, 3,
       nullptr},
      {0, sign, 0, 2, "axis endpoints must differ"},
      {sign | maximum, maximum, maximum, 2,
       "axis component=2 inconsistent or unrepresentable step"},
      {0, 1, 0, 3, "axis component=2 inconsistent or unrepresentable step"},
      {sign | 1, 1, 1, 4, "non-strict reconstructed axis knot=2"},
      {one, one + 1, half_ulp, 3, "non-strict reconstructed axis knot=1"},
      {one + 1, one, sign | half_ulp, 3,
       "non-strict reconstructed axis knot=2"}};
  for (const auto& axis : cases) {
    Fixture fixture(axis.first, axis.last, axis.step, axis.count);
    auto status =
        fixture.validate([](std::uint64_t) { return ps::Status::success(); });
    if (axis.failure) {
      PS_CHECK(status.code == ps::ErrorCode::OperationFailed &&
               status.reason == ps::FailureReason::InvalidDomain &&
               status.detail.origin == ps::FailureOrigin::Domain &&
               status.detail.scope == ps::FailureScope::Group &&
               status.message == axis.failure);
    } else {
      PS_REQUIRE_OK(status);
      PS_CHECK(ps::core_internal::binary64_bits(fixture.report.axis[0]) ==
               axis.first);
      PS_CHECK(ps::core_internal::binary64_bits(fixture.report.axis[1]) ==
               axis.last);
    }
  }
  std::vector<std::uint64_t> charges;
  const auto work = [&](std::uint64_t count) {
    charges.push_back(count);
    return ps::Status::success();
  };
  Fixture valid(0, one, half, 3);
  PS_REQUIRE_OK(valid.validate(work));
  PS_CHECK(charges == (std::vector<std::uint64_t>{8192, 1, 1, 8192, 1, 8192, 1,
                                                  1, 8192, 1, 1}));

  const ps::Status sentinel{
      ps::ErrorCode::ResourceExhausted,
      "axis work sentinel",
      ps::FailureReason::WorkLimit,
      {ps::FailureOrigin::Resource, ps::FailureScope::Run}};
  for (unsigned fail_at : {1, 4}) {
    Fixture failing(0, one, half, 3);
    failing.report.axis[3] = number(infinity | 1);
    unsigned calls = 0;
    auto status = failing.validate([&](std::uint64_t) {
      return ++calls == fail_at ? sentinel : ps::Status::success();
    });
    PS_CHECK(status.code == sentinel.code && status.reason == sentinel.reason &&
             status.message == sentinel.message &&
             status.detail.origin == sentinel.detail.origin &&
             status.detail.scope == sentinel.detail.scope && calls == fail_at);
  }
  Fixture bad_step(0, one, one, 3);
  PS_CHECK(bad_step.validate([&](std::uint64_t) { return sentinel; }).message ==
           sentinel.message);
  Fixture nonfinite(0, 0, infinity, 2);
  charges.clear();
  PS_CHECK(nonfinite.validate(work).message == "nonfinite axis component=2");
  PS_CHECK(charges.empty());
  PS_CHECK(nonfinite.validate(work, 2).message ==
           "invalid measured LUT3D report values");
  PS_CHECK(charges.empty());
  Fixture domain_work(0, one, half, 3);
  auto domain = domain_work.validate([](std::uint64_t) {
    return ps::Status{ps::ErrorCode::OperationFailed,
                      "domain work",
                      ps::FailureReason::InvalidDomain,
                      {ps::FailureOrigin::Domain, ps::FailureScope::Atom}};
  });
  PS_CHECK(domain.message == "domain work" &&
           domain.detail.scope == ps::FailureScope::Group);

  // Exact reconstruction remains RN-even under a different ambient mode.
  std::fenv_t saved;
  PS_CHECK(std::fegetenv(&saved) == 0);
  PS_CHECK(std::fesetround(FE_UPWARD) == 0);
  std::feclearexcept(FE_ALL_EXCEPT);
  Fixture subnormal(sign | 2, 1, 2, 3);
  auto exact =
      subnormal.validate([](std::uint64_t) { return ps::Status::success(); });
  const auto rounding = std::fegetround();
  const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
  PS_CHECK(std::fesetenv(&saved) == 0);
  PS_REQUIRE_OK(exact);
  PS_CHECK(rounding == FE_UPWARD && flags == 0);
  return 0;
}
