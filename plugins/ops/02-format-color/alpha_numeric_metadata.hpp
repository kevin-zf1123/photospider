#pragma once

#include <cstring>

#include "02-format-color/alpha_common.hpp"
#include "data/exact_numeric.hpp"

namespace ps::plugin_internal::alpha_ops {
inline data_internal::format_numeric::Rational exact_endpoint(
    const TensorEndpoint& e) {
  using data_internal::format_numeric::Rational;
  if (const auto* n = std::get_if<std::int64_t>(&e)) {
    return Rational::integer(*n);
  }
  if (const auto* f = std::get_if<double>(&e)) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, f, 8);
    return Rational::binary(bits, false);
  }
  const auto& exact = std::get<TensorRationalEndpoint>(e);
  Rational r;
  r.n.words.assign(exact.numerator.begin(), exact.numerator.end());
  r.d.words.assign(exact.denominator.begin(), exact.denominator.end());
  r.n.trim();
  r.d.trim();
  r.negative = exact.negative;
  return r;
}
// Descriptor arithmetic, never a sample scan or implicit decode. Test affine
// identity mathematically: equivalent integer/dyadic/rational endpoints and
// signed zeros must not produce false conflicts. Validation already bounds the
// descriptor's endpoint representation before this function is called.
inline Status normalized_alpha_encoding(
    const std::optional<TensorEncoding>& encoding) {
  if (!encoding) {
    return Status::success();
  }
  std::uint64_t fuel = 20000000;
  const std::function<Status(std::uint64_t)> consume = [&fuel](
                                                           std::uint64_t n) {
    if (n > fuel) {
      return Status{ErrorCode::ResourceExhausted, "alpha encoding work limit"};
    }
    fuel -= n;
    if (const auto* budget = resource_internal::metadata_budget()) {
      return budget->consume({n});
    }
    return Status::success();
  };
  data_internal::format_numeric::ExactWorkScope work(&consume, nullptr);
  try {
    if (exact_endpoint(encoding->stored[0])
            .compare(exact_endpoint(encoding->decoded[0])) ||
        exact_endpoint(encoding->stored[1])
            .compare(exact_endpoint(encoding->decoded[1]))) {
      return invalid(
          "normalized alpha requires explicit decoding of conflicting "
          "encoding");
    }
    return Status::success();
  } catch (const data_internal::format_numeric::ExactWorkFailure& failure) {
    return failure.status;
  } catch (const std::overflow_error&) {
    return invalid("alpha encoding exceeds exact arithmetic capacity");
  }
}
}  // namespace ps::plugin_internal::alpha_ops
