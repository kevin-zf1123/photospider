#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <tuple>
#include <vector>

#include "02-format-color/transfer_fast.hpp"
#include "02-format-color/transfer_math.hpp"
#include "fixtures/fmt09_division.hpp"
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
  {
    ps::plugin_internal::numeric_ops::DirectedIntervalStorage<8> division(
        SequenceProfile::Strict);
    division.consume = &consume;
    for (const auto& row : fmt09_test::division) {
      decltype(division)::Integer n{}, d{}, q{}, r{};
      std::copy_n(row[0], 8, n.words.begin());
      std::copy_n(row[1], 8, d.words.begin());
      division.divide_unsigned(q, r, n, d);
      PS_CHECK(std::equal(q.words.begin(), q.words.end(), row[2]));
      PS_CHECK(std::equal(r.words.begin(), r.words.end(), row[3]));
    }
  }
  for (const auto code :
       {ErrorCode::Cancelled, ErrorCode::Stale, ErrorCode::ResourceExhausted}) {
    ps::plugin_internal::numeric_ops::DirectedIntervalStorage<8> divider(
        SequenceProfile::Strict);
    unsigned digits = 0;
    const std::function<Status(std::uint64_t)> stop =
        [&](std::uint64_t amount) {
          if (amount == 32 && ++digits == 2)
            return Status{code, "word division stop"};
          return Status::success();
        };
    divider.consume = &stop;
    decltype(divider)::Integer n{}, d{}, q{}, r{};
    n.words.fill(UINT64_MAX);
    d.words[2] = 1;
    d.words[0] = 7;
    bool failed = false;
    try {
      divider.divide_unsigned(q, r, n, d);
    } catch (const Status& status) {
      failed = status.code == code;
    }
    PS_CHECK(failed && digits == 2);
  }
  // Direct integer oracle covers aliasing/carry in the reused remainder.
  std::mt19937_64 random(90926);
  for (unsigned i = 0; i < 2000; ++i) {
    const auto n = random(), d = random() | 1;
    const auto qr = Natural::divide(Natural(n), Natural(d));
    PS_CHECK(qr.first.low64() == n / d);
    PS_CHECK(qr.second.low64() == n % d);
    Natural shifted(n);
    for (unsigned bits : {0U, 1U, 31U, 32U, 33U, 64U, 1074U}) {
      auto value = shifted;
      value.shift_left(bits);
      value.shift_right(bits);
      PS_CHECK(value.compare(shifted) == 0);
    }
  }
  {
    Natural d(13), q(1), rem(7);
    d.shift_left(1074);
    q.shift_left(127);
    const auto n = Natural::add(Natural::multiply(d, q), rem);
    const auto divided = Natural::divide(n, d);
    PS_CHECK(divided.first.compare(q) == 0);
    PS_CHECK(divided.second.compare(rem) == 0);
    Natural boundary(1);
    boundary.shift_left(32 * 254);
    PS_CHECK(boundary.words.size() == 255);
    bool limited = false;
    try {
      boundary.shift_left(32);
    } catch (const std::bad_alloc&) {
      limited = true;
    }
    PS_CHECK(limited);
  }
  for (const auto code :
       {ErrorCode::Cancelled, ErrorCode::Stale, ErrorCode::ResourceExhausted}) {
    unsigned polls = 0;
    const std::function<Status(std::uint64_t)> stop = [&](std::uint64_t) {
      if (++polls == 3)
        return Status{code, "division stop"};
      return Status::success();
    };
    bool failed = false;
    try {
      data_internal::format_numeric::ExactWorkScope inner(&stop, nullptr);
      Natural n(1), d(1);
      n.shift_left(4000);
      d = Natural(3);
      d.set_bit(64);
      static_cast<void>(Natural::divide(n, d));
    } catch (const data_internal::format_numeric::ExactWorkFailure& e) {
      failed = e.status.code == code;
    }
    PS_CHECK(failed);
    PS_CHECK(polls == 3);
  }
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
