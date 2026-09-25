#include <array>
#include <cfenv>  // NOLINT(build/c++11): project requires C++17.
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>

#include "02-format-color/model_math.hpp"
#include "02-format-color/model_simd.hpp"
#include "fixtures/fmt11/finite_oracles.hpp"
#include "support/test_support.hpp"

namespace m = ps::plugin_internal::model_ops;
namespace n = ps::plugin_internal::numeric_ops;
using ps::Status;
struct Engine {
  m::MathConfig config;
  m::RationalMath::Work work;
  std::shared_ptr<const m::Constants> constants;
  std::unique_ptr<m::ModelMath> math;
  std::uint64_t charged = 0;
  explicit Engine(m::MathConfig cfg) : config(cfg) {
    work = [&](std::uint64_t value) {
      charged += value;
      return Status::success();
    };
    auto prepared = m::prepare_constants(config);
    if (!prepared.ok())
      throw std::runtime_error(prepared.status().message);
    constants = prepared.take_value();
    math = std::make_unique<m::ModelMath>(config, *constants, work);
  }
  ps::Result<std::uint64_t> eval(std::array<double, 3> values,
                                 unsigned component) {
    std::array<std::uint64_t, 3> bits{};
    for (unsigned i = 0; i < 3; ++i)
      bits[i] = n::numeric_bits(values[i], config.narrow);
    return math->evaluate(bits, component);
  }
};
int main() {
  ps::input_internal::Float32Environment environment;
  PS_CHECK(environment.active());
  std::unique_ptr<Engine> engine;
  unsigned passed = 0;
  for (const auto& fixture : kFmt11Oracles) {
    if (!engine || static_cast<unsigned>(engine->config.kind) != fixture.kind ||
        engine->config.narrow != fixture.narrow) {
      m::MathConfig config;
      config.kind = static_cast<m::Kind>(fixture.kind);
      config.narrow = fixture.narrow;
      config.algorithm = m::Algorithm::Reference;
      config.semantic = false;
      config.white = {.25, .25};
      config.ncl = {.25, .25};
      config.input_pi = config.output_pi = true;
      engine = std::make_unique<Engine>(config);
    }
    auto result = engine->math->evaluate(fixture.input, fixture.component);
    if (!result.ok() || result.value() != fixture.expected) {
      std::cerr << "oracle #" << passed << " kind=" << fixture.kind
                << " narrow=" << fixture.narrow
                << " component=" << fixture.component << " input=" << std::hex
                << fixture.input[0] << ',' << fixture.input[1] << ','
                << fixture.input[2] << " expected=" << fixture.expected
                << " actual=";
      if (result.ok())
        std::cerr << result.value();
      else
        std::cerr << result.status().message;
      std::cerr << std::dec << '\n';
      return 1;
    }
    ++passed;
  }
  std::cout << "independent finite oracle components: " << passed << '\n';
  for (const bool narrow : {false, true}) {
    const auto sign = UINT64_C(1) << (narrow ? 31 : 63);
    const auto one = n::numeric_bits(1, narrow);
    const auto nan =
        narrow ? UINT64_C(0x7fc00000) : UINT64_C(0x7ff8000000000000);
    const auto snan =
        narrow ? UINT64_C(0xff800123) : UINT64_C(0xfff0000000000123);
    const auto inf =
        narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
    m::MathConfig c;
    c.narrow = narrow;
    c.semantic = false;
    c.white = {.25, .25};
    c.ncl = {.25, .25};
    c.input_pi = c.output_pi = true;
    c.algorithm = m::Algorithm::Reference;
    c.kind = m::Kind::XyzToLab;
    Engine lab(c);
    PS_CHECK(lab.eval({1, 1, 2}, 0).value() == one);
    PS_CHECK(lab.eval({1, 1, 2}, 1).value() == 0);
    PS_CHECK(lab.eval({1, 1, 2}, 2).value() == 0);
    PS_CHECK(lab.math->evaluate({snan, one, inf}, 0).value() == one);
    PS_CHECK(lab.math->evaluate({snan, one, inf}, 1).value() ==
             (snan | (narrow ? UINT64_C(1) << 22 : UINT64_C(1) << 51)));
    c.kind = m::Kind::LabToLch;
    Engine polar(c);
    PS_CHECK(polar.math->evaluate({snan, 0, 0}, 0).value() == snan);
    PS_CHECK(polar.math->evaluate({0, sign, sign}, 2).value() == 0);
    PS_CHECK(polar.math->evaluate({0, one | sign, sign}, 2).value() ==
             (one | sign));
    c.kind = m::Kind::LchToLab;
    Engine cart(c);
    PS_CHECK(cart.eval({0, 1, .5}, 1).value() == 0);
    PS_CHECK(cart.eval({0, 1, .5}, 2).value() == one);
    PS_CHECK(cart.eval({0, 1, -1}, 2).value() == sign);
    PS_CHECK(cart.math->evaluate({0, inf, 0}, 2).value() == nan);
    PS_CHECK(cart.math->evaluate({0, 0, inf}, 1).value() == nan);
    c.semantic = true;
    Engine safe_cart(c);
    PS_CHECK(!safe_cart.math->evaluate({0, 0, inf}, 2).ok());
    PS_CHECK(!safe_cart.eval({0, -1, 0}, 1).ok());
    PS_CHECK(safe_cart.eval({0, -0., -1}, 2).value() == 0);
    c.kind = m::Kind::RgbToHsl;
    Engine hsl(c);
    PS_CHECK(!hsl.eval({1, 0, -1}, 1).ok());
    PS_CHECK(hsl.eval({1, 0, -1}, 0).ok());
    PS_CHECK(hsl.eval({1, 0, -1}, 2).value() == 0);
    c.kind = m::Kind::XyyToXyz;
    Engine xyy(c);
    PS_CHECK(!xyy.eval({.5, 0, 0}, 0).ok());
    PS_CHECK(xyy.eval({.5, 0, -0.}, 1).value() == sign);
    c.kind = m::Kind::GrayToColor;
    c.gray = m::GrayKind::CielabL;
    Engine gray(c);
    PS_CHECK(gray.math->evaluate({snan, 0, 0}, 1).value() == 0);
    PS_CHECK(!gray.math->evaluate({snan, 0, 0}, 0).ok());
    c.kind = m::Kind::Threshold;
    c.semantic = false;
    c.threshold = std::nextafter(1., 2.);
    Engine threshold(c);
    PS_CHECK(threshold.math->evaluate({one, 0, 0}, 0).value() == 0);
    PS_CHECK(threshold.math->evaluate({one + 1, 0, 0}, 0).value() == one);
    PS_CHECK(threshold.math->evaluate({snan, 0, 0}, 0).value() == 0);
    PS_CHECK(threshold.math->evaluate({inf, 0, 0}, 0).value() == one);
    c.kind = m::Kind::BinaryToGray;
    c.levels = {sign, sign};
    Engine binary(c);
    PS_CHECK(binary.math->evaluate({0, 0, 0}, 0).value() == sign);
    PS_CHECK(binary.math->evaluate({sign, 0, 0}, 0).value() == sign);
    PS_CHECK(binary.math->evaluate({one, 0, 0}, 0).value() == sign);
    PS_CHECK(!binary.math->evaluate({one + 1, 0, 0}, 0).ok());
    PS_CHECK(!binary.math->evaluate({snan, 0, 0}, 0).ok());
    c.kind = m::Kind::RgbToYcbcr;
    Engine ncl(c);
    PS_CHECK(ncl.math->evaluate({0, 0, inf}, 1).value() == inf);
    PS_CHECK(ncl.math->evaluate({0, 0, inf}, 2).value() == (inf | sign));
    c.kind = m::Kind::YcbcrToRgb;
    Engine inverse(c);
    PS_CHECK(inverse.math->evaluate({one, snan, 0}, 0).value() == one);
    c.kind = m::Kind::XyzToXyy;
    c.semantic = true;
    Engine ratios(c);
    PS_CHECK(ratios.eval({0, 0, 0}, 0).value() == n::numeric_bits(.25, narrow));
    const double high = narrow ? std::numeric_limits<float>::max()
                               : std::numeric_limits<double>::max();
    PS_CHECK(ratios.eval({high, high, high}, 0).value() ==
             n::numeric_bits(1. / 3, narrow));
  }
  // Compare the independently certified scalar/automatic filters to strict.
  for (const auto profile :
       {n::SequenceProfile::Strict, n::SequenceProfile::X86Avx2,
        n::SequenceProfile::AppleSilicon}) {
    if (!n::sequence_profile_available(profile).ok())
      continue;
    for (const bool narrow : {false, true}) {
      m::MathConfig c;
      c.kind = m::Kind::RgbToYcbcr;
      c.narrow = narrow;
      c.profile = profile;
      Engine fast(c);
      c.algorithm = m::Algorithm::Reference;
      Engine reference(c);
      for (unsigned i = 1; i < 33; ++i) {
        const std::array<double, 3> values{static_cast<double>(i) / 32,
                                           static_cast<double>(i + 3) / 64,
                                           static_cast<double>(i + 5) / 128};
        for (unsigned component = 0; component < 3; ++component) {
          auto exact = reference.eval(values, component),
               candidate = fast.eval(values, component);
          PS_CHECK(exact.ok() && candidate.ok());
          if (profile == n::SequenceProfile::Strict) {
            PS_CHECK(exact.value() == candidate.value());
          } else {
            const auto a =
                static_cast<float>(n::numeric_double(exact.value(), narrow));
            const auto b = n::numeric_double(candidate.value(), narrow);
            const auto ulp = std::abs(std::nextafter(a, INFINITY) - a);
            PS_CHECK(std::abs(b - n::numeric_double(exact.value(), narrow)) <=
                     4 * ulp);
          }
        }
      }
    }
  }
  // All independent fixtures also exercise the automatic certificates. Profile
  // validation is separate from the reference oracle; no self-roundtrip oracle.
  unsigned filtered = 0;
  for (const auto profile :
       {n::SequenceProfile::Strict, n::SequenceProfile::X86Avx2,
        n::SequenceProfile::AppleSilicon}) {
    if (!n::sequence_profile_available(profile).ok())
      continue;
    engine.reset();
    for (const auto& fixture : kFmt11Oracles) {
      if (!engine ||
          static_cast<unsigned>(engine->config.kind) != fixture.kind ||
          engine->config.narrow != fixture.narrow) {
        m::MathConfig c;
        c.kind = static_cast<m::Kind>(fixture.kind);
        c.narrow = fixture.narrow;
        c.profile = profile;
        c.semantic = false;
        c.input_pi = c.output_pi = true;
        c.white = {.25, .25};
        c.ncl = {.25, .25};
        engine = std::make_unique<Engine>(c);
      }
      const auto result =
          engine->math->evaluate(fixture.input, fixture.component);
      PS_CHECK(result.ok());
      const auto expected = n::numeric_double(fixture.expected, fixture.narrow);
      if (profile == n::SequenceProfile::Strict || expected == 0 ||
          std::abs(expected) < std::numeric_limits<float>::min() ||
          std::abs(expected) > std::numeric_limits<float>::max()) {
        PS_CHECK(result.value() == fixture.expected);
      } else {
        const float rounded = static_cast<float>(expected);
        const double ulp =
            static_cast<double>(std::nextafter(std::abs(rounded), INFINITY)) -
            std::abs(rounded);
        PS_CHECK(std::abs(n::numeric_double(result.value(), fixture.narrow) -
                          expected) <= 4 * ulp);
      }
      ++filtered;
    }
  }
  std::cout << "automatic/profile oracle checks: " << filtered << '\n';
  // Proof fuel exhaustion is observable, never a reduced-precision success.
  m::MathConfig config;
  config.kind = m::Kind::XyzToOklab;
  config.algorithm = m::Algorithm::Reference;
  auto constants = m::prepare_constants(config);
  PS_CHECK(constants.ok());
  const m::RationalMath::Work exhausted = [](std::uint64_t) {
    return Status{ps::ErrorCode::ResourceExhausted, "test fuel",
                  ps::FailureReason::WorkLimit};
  };
  auto math =
      std::make_unique<m::ModelMath>(config, *constants.value(), exhausted);
  auto rejected = math->evaluate(
      {n::numeric_bits(.3), n::numeric_bits(.4), n::numeric_bits(.5)}, 0);
  PS_CHECK(!rejected.ok() &&
           rejected.status().code == ps::ErrorCode::ResourceExhausted);
  // Explicit cancellation during exact work is preserved, not remapped into
  // a numerical domain failure. Preparation restores the caller's FP state.
  std::uint64_t issued = 0;
  const m::RationalMath::Work cancelled = [&](std::uint64_t amount) {
    issued += amount;
    return issued > 1000 ? Status{ps::ErrorCode::Cancelled, "test cancellation",
                                  ps::FailureReason::Cancelled}
                         : Status::success();
  };
  auto cancel_math =
      std::make_unique<m::ModelMath>(config, *constants.value(), cancelled);
  auto stop = cancel_math->evaluate(
      {n::numeric_bits(.3), n::numeric_bits(.4), n::numeric_bits(.5)}, 0);
  PS_CHECK(!stop.ok() && stop.status().code == ps::ErrorCode::Cancelled);
  std::fenv_t saved{};
  PS_CHECK(std::fegetenv(&saved) == 0);
  PS_CHECK(std::fesetround(FE_UPWARD) == 0);
  std::feclearexcept(FE_ALL_EXCEPT);
  std::feraiseexcept(FE_DIVBYZERO);
  auto under_changed_environment = m::prepare_constants(config);
  PS_CHECK(under_changed_environment.ok());
  PS_CHECK(std::fegetround() == FE_UPWARD &&
           std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
  PS_CHECK(std::fesetenv(&saved) == 0);
  std::cout
      << "special values, sparse support, filters and work exhaustion passed\n";
}
