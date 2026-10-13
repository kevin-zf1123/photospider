#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <cstring>
#include <string>

#include "core/checked_math.hpp"
#include "core/numeric_bits.hpp"
#include "core/status_helpers.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps::core_internal;  // NOLINT(build/namespaces)
  PS_CHECK(binary64_order_key(0) ==
           binary64_order_key(UINT64_C(0x8000000000000000)));
  PS_CHECK(binary64_order_key(UINT64_C(0xbff0000000000000)) <
           binary64_order_key(0));
  PS_CHECK(binary64_order_key(0) <
           binary64_order_key(UINT64_C(0x3ff0000000000000)));
  std::uint64_t out = 17;
  PS_CHECK(!checked_add(UINT64_MAX, 1, &out) && out == 17);
  PS_CHECK(checked_add(UINT64_MAX - 1, 1, &out) && out == UINT64_MAX);
  PS_CHECK(checked_add(out, 0, &out) && out == UINT64_MAX);
  PS_CHECK(!checked_multiply(UINT64_MAX, 2, &out) && out == UINT64_MAX);
  PS_CHECK(checked_multiply(UINT64_MAX, 0, &out, INT64_MAX) && out == 0);
  PS_CHECK(checked_multiply(0, UINT64_MAX, &out, 0) && out == 0);
  PS_CHECK(checked_multiply(INT64_MAX, 1, &out, INT64_MAX) && out == INT64_MAX);
  PS_CHECK(!checked_multiply(static_cast<std::uint64_t>(INT64_MAX) + 1, 1, &out,
                             INT64_MAX) &&
           out == INT64_MAX);
  PS_CHECK(checked_multiply(1, 0, &out) && out == 0);
  PS_CHECK(saturating_multiply(UINT64_MAX, 0) == 0);
  PS_CHECK(saturating_multiply(0, UINT64_MAX) == 0);
  PS_CHECK(saturating_multiply(UINT64_MAX, 1) == UINT64_MAX);
  PS_CHECK(saturating_multiply(UINT64_MAX, 2) == UINT64_MAX);
  PS_CHECK(saturating_multiply(UINT64_MAX / 8, 8) == UINT64_MAX - 7);
  PS_CHECK(saturating_multiply(UINT64_MAX / 8 + 1, 8) == UINT64_MAX);
  PS_CHECK(saturating_multiply(UINT64_MAX, 0, 0) == 0);
  PS_CHECK(saturating_multiply(UINT64_MAX, 1, INT64_MAX) == INT64_MAX);
  PS_CHECK(saturating_multiply(3, 2, 7) == 6);
  PS_CHECK(saturating_multiply(4, 2, 7) == 7);
  PS_CHECK(!checked_add(UINT64_MAX, 0, &out, INT64_MAX) && out == 0);
  PS_CHECK(!checked_multiply_add(UINT64_MAX, 2, 1, &out) && out == 0);
  PS_CHECK(checked_multiply_add(UINT64_MAX, 0, 23, &out) && out == 23);
  PS_CHECK(!checked_multiply_add(0, 0, 24, &out, 23) && out == 23);
  PS_CHECK(checked_align_up(13, 6, &out) && out == 18);
  PS_CHECK(!checked_align_up(13, 0, &out) && out == 18);
  PS_CHECK(!checked_align_up(UINT64_MAX, 2, &out) && out == 18);
  PS_CHECK(checked_align_up(UINT64_MAX, 1, &out) && out == UINT64_MAX);
  PS_CHECK(checked_align_up(INT64_MAX - 4095, 4096, &out, INT64_MAX) &&
           out == INT64_MAX - 4095);
  PS_CHECK(!checked_align_up(INT64_MAX - 4094, 4096, &out, INT64_MAX) &&
           out == INT64_MAX - 4095);
  PS_CHECK(!checked_align_up(INT64_MAX, 4096, &out, INT64_MAX) &&
           out == INT64_MAX - 4095);
  // Include signaling NaNs: classification must leave the FP flags untouched.
  const std::uint64_t samples[] = {0,
                                   UINT64_C(0x8000000000000000),
                                   UINT64_C(0x7fefffffffffffff),
                                   UINT64_C(0x7ff0000000000000),
                                   UINT64_C(0xfff0000000000000),
                                   UINT64_C(0x7ff0000000000001),
                                   UINT64_C(0x7ff8000000000001)};
  std::fenv_t saved;
  PS_CHECK(std::fegetenv(&saved) == 0);
  bool valid = true;
  std::feclearexcept(FE_ALL_EXCEPT);
  for (unsigned i = 0; i < 7; ++i) {
    double value = 0;
    std::memcpy(&value, &samples[i], sizeof(value));
    valid = valid && binary64_bits(value) == samples[i];
    valid = valid && finite_binary64(value) == (i < 3);
  }
  const auto exceptions = std::fetestexcept(FE_ALL_EXCEPT);
  PS_CHECK(std::fesetenv(&saved) == 0);
  PS_CHECK(valid && exceptions == 0);
  const auto bounded = invalid_argument(std::string(5000, 'x').c_str());
  const auto complete = invalid(std::string(5000, 'x').c_str());
  PS_CHECK(bounded.message.size() == 4096 && complete.message.size() == 5000);
  PS_CHECK(complete.reason == ps::FailureReason::None);
  PS_CHECK(capacity_exhausted().reason == ps::FailureReason::CapacityLimit);
  PS_CHECK(exhausted(std::string(5000, 'x').c_str()).message.size() == 4096);
  PS_CHECK(resource_exhausted(std::string(5000, 'x')).message.size() == 5000);
  const auto quality = invalid_quality();
  PS_CHECK(quality.code == ps::ErrorCode::InvalidArgument &&
           quality.reason == ps::FailureReason::InvalidQuality &&
           quality.message == "invalid numerical quality report" &&
           quality.detail.origin == ps::FailureOrigin::Unspecified &&
           quality.detail.scope == ps::FailureScope::Unspecified);
  const auto schema = invalid_schema_domain("schema");
  PS_CHECK(schema.code == ps::ErrorCode::InvalidArgument &&
           schema.reason == ps::FailureReason::InvalidDomain &&
           schema.detail.origin == ps::FailureOrigin::Schema &&
           schema.detail.scope == ps::FailureScope::Unspecified);
  const auto overflow = arithmetic_overflow("overflow");
  PS_CHECK(overflow.code == ps::ErrorCode::OperationFailed &&
           overflow.reason == ps::FailureReason::ArithmeticOverflow &&
           overflow.detail.origin == ps::FailureOrigin::Domain &&
           overflow.detail.scope == ps::FailureScope::Group);
  return 0;
}
