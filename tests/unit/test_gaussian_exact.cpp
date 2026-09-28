#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

#include "05-filter/gaussian_exact.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using ps::plugin_internal::filter_ops::GaussianExact;
std::uint64_t bits(double value) {
  std::uint64_t raw;
  std::memcpy(&raw, &value, 8);
  return raw;
}
std::uint64_t bits(float value) {
  std::uint32_t raw;
  std::memcpy(&raw, &value, 4);
  return raw;
}
}  // namespace
int main() {
  auto math = std::make_unique<GaussianExact>();
  const auto consume = [](std::uint64_t) { return Status::success(); };
  const std::array<std::uint64_t, 3> weights{UINT64_C(0x3fe368b2fc6f960a),
                                             UINT64_C(0x3ff0000000000000),
                                             UINT64_C(0x3fe368b2fc6f960a)};
  const auto evaluate = [&](const std::array<std::uint64_t, 9>& samples,
                            bool narrow) -> Result<std::uint64_t> {
    const auto maximum = GaussianExact::work_bound(3, 3).value();
    std::uint64_t consumed = 0;
    const auto bounded = [&](std::uint64_t units) {
      if (units > maximum - consumed)
        return Status{ErrorCode::Internal, "Gaussian work bound exceeded"};
      consumed += units;
      return Status::success();
    };
    auto status =
        math->begin(weights.data(), 3, weights.data(), 3, narrow, bounded);
    if (!status.ok())
      return Result<std::uint64_t>(status);
    for (unsigned i = 0; i < 9; ++i) {
      status = math->add(weights[i % 3], weights[i / 3], samples[i], narrow,
                         consume);
      if (!status.ok())
        return Result<std::uint64_t>(status);
    }
    return math->finish(bounded);
  };
  for (bool narrow : {false, true}) {
    std::array<std::uint64_t, 9> samples{};
    samples[4] = narrow ? bits(1.f) : bits(1.);
    auto impulse = evaluate(samples, narrow);
    PS_CHECK(impulse.ok());
    PS_CHECK(impulse.value() ==
             (narrow ? UINT64_C(0x3e51148d) : UINT64_C(0x3fca22919bd6e99f)));
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    samples.fill(sign);
    auto negative_zero = evaluate(samples, narrow);
    PS_CHECK(negative_zero.ok() && negative_zero.value() == sign);
    samples[4] = 0;
    auto mixed_zero = evaluate(samples, narrow);
    PS_CHECK(mixed_zero.ok() && mixed_zero.value() == 0);
    const auto inf =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    const auto quiet = UINT64_C(1) << (narrow ? 22 : 51);
    samples[0] = inf;
    samples[1] = inf | sign;
    auto invalid = evaluate(samples, narrow);
    PS_CHECK(invalid.ok() && invalid.value() == (inf | quiet));
    samples[7] = inf | sign | 123;
    samples[8] = inf | 456;
    auto payload = evaluate(samples, narrow);
    PS_CHECK(payload.ok() && payload.value() == (inf | sign | quiet | 123));
    samples.fill(narrow ? bits(-3.f) : bits(-3.));
    auto constant = evaluate(samples, narrow);
    PS_CHECK(constant.ok() && constant.value() == samples[0]);
    samples.fill(0);
    samples[3] = narrow ? bits(1.f) : bits(1.);
    samples[5] = narrow ? bits(-1.f) : bits(-1.);
    auto cancellation = evaluate(samples, narrow);
    PS_CHECK(cancellation.ok() && cancellation.value() == 0);
  }
  // A rounded horizontal Float32 intermediate gives 0x40c18f28 here.
  const std::array<float, 9> counterexample{96.71428680419922f,
                                            -104.57142639160156f,
                                            -125.71428680419922f,
                                            115.28571319580078f,
                                            39.f,
                                            82.28571319580078f,
                                            -79.14286041259766f,
                                            -76.57142639160156f,
                                            55.57143020629883f};
  std::array<std::uint64_t, 9> samples;
  for (unsigned i = 0; i < 9; ++i)
    samples[i] = bits(counterexample[i]);
  auto exact = evaluate(samples, true);
  PS_CHECK(exact.ok() && exact.value() == UINT64_C(0x40c18f2a));
  const std::array<std::uint64_t, 2> ones{UINT64_C(0x3ff0000000000000),
                                          UINT64_C(0x3ff0000000000000)};
  PS_CHECK(math->begin(ones.data(), 2, ones.data(), 1, true, consume).ok());
  // A Float64 boundary constant may exceed Float32 range while its weighted
  // output is finite. Narrowing the constant before summation loses this case.
  PS_CHECK(
      math->add(ones[0], ones[0], bits(0x1.fffffep128), false, consume).ok());
  PS_CHECK(math->add(ones[0], ones[0], bits(0.f), true, consume).ok());
  auto wide_constant = math->finish(consume);
  PS_CHECK(wide_constant.ok() && wide_constant.value() == UINT64_C(0x7f7fffff));
  const std::uint64_t zero_weight = 0;
  PS_CHECK(math->begin(&zero_weight, 1, ones.data(), 1, false, consume).code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(
      math->begin(weights.data(), UINT64_MAX, weights.data(), 3, false, consume)
          .code == ErrorCode::ResourceExhausted);
  const auto cancelled = [](std::uint64_t) {
    return Status{ErrorCode::Cancelled, "cancel Gaussian sum"};
  };
  PS_CHECK(math->begin(weights.data(), 3, weights.data(), 3, false, cancelled)
               .code == ErrorCode::Cancelled);
  auto reused = evaluate(samples, true);
  PS_CHECK(reused.ok() && reused.value() == exact.value());
  PS_CHECK(!GaussianExact::work_bound(0, 1).ok());
  PS_CHECK(!GaussianExact::work_bound(UINT64_MAX, 2).ok());
  PS_CHECK(!GaussianExact::work_bound(UINT64_MAX, 1).ok());
  PS_CHECK(!GaussianExact::work_bound(1000000000, 1000000000).ok());
  return 0;
}
