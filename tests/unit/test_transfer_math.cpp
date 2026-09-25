#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <tuple>
#include <vector>

#include "02-format-color/transfer_fast.hpp"
#include "02-format-color/transfer_math.hpp"
#include "fixtures/fmt09_sweep.hpp"
#include "support/test_support.hpp"

using namespace ps;                                 // NOLINT(build/namespaces)
using namespace ps::plugin_internal::transfer_ops;  // NOLINT(build/namespaces)

int main() {
  input_internal::Float32Environment environment;
  PS_CHECK(environment.active());
  std::uint64_t work = 0, proved = 0, accelerated = 0;
  const std::function<Status(std::uint64_t)> consume = [&](std::uint64_t n) {
    work += n;
    return Status::success();
  };
  data_internal::format_numeric::ExactWorkScope scope(&consume, nullptr);
  std::unique_ptr<CurveProgram> program;
  std::unique_ptr<StrictMath> math;
  std::unique_ptr<CompactMath> compact;
  unsigned compact_proved = 0, compact_retry = 0;
  std::unique_ptr<FastMath> fast;
  using Key = std::tuple<unsigned, bool, double, unsigned, double, double>;
  std::optional<Key> previous;
  unsigned tested = 0, failures = 0;
  std::vector<fmt09_test::Case> tests(std::begin(fmt09_test::golden),
                                      std::end(fmt09_test::golden));
  tests.insert(tests.end(), std::begin(fmt09_test::sweep),
               std::end(fmt09_test::sweep));
  for (const auto& test : tests) {
    Key key{test.curve,   test.encode, test.gamma,
            test.variant, test.black,  test.white};
    if (!previous || *previous != key) {
      previous = key;
      TransferDefinition d;
      d.curve = static_cast<TransferCurve>(test.curve);
      if (d.curve == TransferCurve::PowerGamma) {
        d.gamma = test.gamma;
      }
      if (d.curve == TransferCurve::Bt2020) {
        d.coefficient_variant = static_cast<Bt2020Coefficients>(test.variant);
      }
      if (d.curve == TransferCurve::Bt1886) {
        d.black_luminance = test.black;
        d.white_luminance = test.white;
      }
      program = std::make_unique<CurveProgram>(make_program(d, test.encode));
      math = std::make_unique<StrictMath>(SequenceProfile::Strict);
      compact = std::make_unique<CompactMath>(SequenceProfile::Strict);
      fast = std::make_unique<FastMath>();
    }
    const auto& c = *program;
    const double x = numeric_double(test.input, test.narrow);
    const double u = c.signed_curve ? std::abs(x) : x;
    auto a = anchor(c, test.input, test.narrow);
    auto computed =
        a ? Result<std::uint64_t>(*a)
          : math->evaluate(c.branches[c.branch(u)], u, test.narrow, consume);
    if (computed.ok() && !a && c.signed_curve) {
      computed = Result<std::uint64_t>(computed.value() |
                                       (test.input & sign_mask(test.narrow)));
    }
    if (!computed.ok() || computed.value() != test.expected) {
      std::cerr << "case=" << tested << " curve=" << test.curve
                << " encode=" << test.encode << " narrow=" << test.narrow
                << " input=0x" << std::hex << test.input << " expected=0x"
                << test.expected;
      if (computed.ok()) {
        std::cerr << " got=0x" << computed.value();
      } else {
        std::cerr << " error=" << computed.status().message;
      }
      std::cerr << std::dec << '\n';
      if (++failures == 20) {
        return 1;
      }
    }
    if (!a) {
      try {
        auto small =
            compact->evaluate(c.branches[c.branch(u)], u, test.narrow, consume);
        PS_CHECK(small.ok());
        auto bits = small.value();
        if (c.signed_curve) {
          bits |= test.input & sign_mask(test.narrow);
        }
        if (bits != test.expected) {
          std::cerr << "compact mismatch case=" << tested << '\n';
          return 1;
        }
        ++compact_proved;
      } catch (const ps::plugin_internal::numeric_ops::DirectedCapacityRetry&) {
        ++compact_retry;
      }
    }
    for (bool strict : {true, false}) {
      std::uint64_t result = 0;
      bool accepted = false;
      auto s = fast->evaluate(c.branches[c.branch(u)], c.branch(u), &u, 1,
                              test.narrow, strict, &result, &accepted, consume,
                              true);
      PS_CHECK(s.ok());
      if (!accepted || a) {
        continue;
      }
      if (c.signed_curve) {
        result |= test.input & sign_mask(test.narrow);
      }
      if (strict) {
        PS_CHECK(result == test.expected);
        ++proved;
      } else {
        const double ref = numeric_double(test.expected, test.narrow),
                     got = numeric_double(result, test.narrow);
        if (ref == 0 || std::abs(ref) < 0x1p-126 ||
            std::abs(ref) > 0x1.fffffep127) {
          PS_CHECK(result == test.expected);
        } else if (test.narrow) {
          const auto delta = result > test.expected ? result - test.expected
                                                    : test.expected - result;
          PS_CHECK(delta <= 4);
        } else {
          PS_CHECK(std::abs(got - ref) <=
                   std::ldexp(1.0, std::ilogb(std::abs(ref)) - 21));
        }
        ++accelerated;
      }
    }
    ++tested;
  }
  // A caller failure must never be mistaken for compact-capacity fallback.
  TransferDefinition pq;
  pq.curve = TransferCurve::Pq;
  auto program_pq = make_program(pq, false);
  for (auto code :
       {ErrorCode::ResourceExhausted, ErrorCode::Cancelled, ErrorCode::Stale}) {
    CompactMath small(SequenceProfile::Strict);
    unsigned calls = 0;
    const std::function<Status(std::uint64_t)> stopped = [&](std::uint64_t) {
      return ++calls == 10 ? Status{code, "injected work failure"}
                           : Status::success();
    };
    const auto result =
        small.evaluate(program_pq.branches[0], .5, false, stopped);
    PS_CHECK(!result.ok() && result.status().code == code && calls == 10);
  }
  PS_CHECK(compact_proved > 0 && compact_retry > 0);
  std::cout << "compact proved=" << compact_proved
            << "; wide retries=" << compact_retry
            << "; workspace bytes compact=" << sizeof(CompactMath)
            << "; wide=" << sizeof(StrictMath) << '\n';
  std::cout << "FMT-09 math: " << tested
            << " independent golden cases; strict filter=" << proved
            << "; accelerated filter=" << accelerated << "; work=" << work
            << "; failures=" << failures << '\n';
  return failures ? 1 : 0;
}
