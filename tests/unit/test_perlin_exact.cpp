#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "03-generation/perlin_exact.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using ps::plugin_internal::generation_ops::PerlinCoordinate;
using ps::plugin_internal::generation_ops::PerlinExact;
template <std::size_t Words>
Result<std::uint64_t> calculate(const std::array<PerlinCoordinate, 3>& p,
                                bool narrow) {
  auto math = std::make_unique<PerlinExact<Words>>();
  const auto consume = [](std::uint64_t) { return Status::success(); };
  return math->evaluate(p, narrow, consume);
}
Result<std::uint64_t> evaluate(const std::array<std::uint64_t, 3>& raw,
                               bool input_narrow, bool output_narrow) {
  std::array<PerlinCoordinate, 3> p;
  unsigned q = 0;
  for (unsigned i = 0; i < 3; ++i) {
    auto decoded = PerlinCoordinate::decode(raw[i], input_narrow);
    if (!decoded.ok())
      return Result<std::uint64_t>(decoded.status());
    p[i] = decoded.value();
    q = std::max(q, p[i].denominator_bits);
  }
  return q <= 31   ? calculate<8>(p, output_narrow)
         : q <= 63 ? calculate<16>(p, output_narrow)
                   : calculate<272>(p, output_narrow);
}
std::uint64_t bits(double value) {
  std::uint64_t raw;
  std::memcpy(&raw, &value, 8);
  return raw;
}
}  // namespace

int main(int argc, char** argv) {
  // Raw IEEE transport for the independent Fraction driver. No decimal
  // conversion or reference floating evaluation occurs in this executable.
  if (argc == 2 && std::string(argv[1]) == "--stdin") {
    unsigned input_narrow, output_narrow;
    std::array<std::uint64_t, 3> raw;
    while (std::cin >> std::dec >> input_narrow >> output_narrow >> std::hex >>
           raw[0] >> raw[1] >> raw[2]) {
      auto result = evaluate(raw, input_narrow != 0, output_narrow != 0);
      if (!result.ok())
        return 1;
      std::cout << std::hex << result.value() << '\n';
    }
    return std::cin.eof() ? 0 : 2;
  }
  for (double x : {0., -0., -1., 256., -257., 0x1p100, -0x1p100}) {
    auto result = evaluate({bits(x), bits(2.), bits(-3.)}, false, false);
    PS_CHECK(result.ok() && result.value() == 0);
  }
  const struct {
    unsigned q;
    std::uint64_t expected;
  } fixtures[] = {
      {2, UINT64_C(0xbfb6f40340000000)},  {31, UINT64_C(0x3fc8b06fff350000)},
      {32, UINT64_C(0x3fc8b06fff9a8000)}, {63, UINT64_C(0x3fc8b07000000000)},
      {64, UINT64_C(0x3fc8b07000000000)}, {1074, UINT64_C(0x3fc8b07000000000)}};
  auto reused = std::make_unique<PerlinExact<272>>();
  const auto success = [](std::uint64_t) { return Status::success(); };
  for (const auto& fixture : fixtures) {
    const auto x = fixture.q <= 1022
                       ? static_cast<std::uint64_t>(1023 - fixture.q) << 52
                       : UINT64_C(1) << (1074 - fixture.q);
    std::array<PerlinCoordinate, 3> coordinates{
        PerlinCoordinate::decode(x, false).value(),
        PerlinCoordinate::decode(bits(.25), false).value(),
        PerlinCoordinate::decode(bits(.75), false).value()};
    auto result = evaluate({x, bits(.25), bits(.75)}, false, false);
    PS_CHECK(result.ok() && result.value() == fixture.expected);
    auto credit = PerlinExact<272>::work_bound(fixture.q);
    const auto bounded = [&](std::uint64_t units) {
      if (units > credit)
        return Status{ErrorCode::Internal, "Perlin work bound exceeded"};
      credit -= units;
      return Status::success();
    };
    auto again = reused->evaluate(coordinates, false, bounded);
    PS_CHECK(again.ok() && again.value() == fixture.expected);
  }
  for (auto nonfinite :
       {UINT64_C(0x7f800000), UINT64_C(0xff800000), UINT64_C(0x7f800001)})
    PS_CHECK(!PerlinCoordinate::decode(nonfinite, true).ok());
  PS_CHECK(
      PerlinCoordinate::decode(UINT64_C(0x80000000), true).value().lattice ==
      0);
  const auto negative = PerlinCoordinate::decode(bits(-0.25), false);
  PS_CHECK(negative.ok());
  PS_CHECK(negative.value().lattice == 255);
  PS_CHECK(negative.value().complement);
  PS_CHECK(negative.value().remainder == 1);
  PS_CHECK(negative.value().denominator_bits == 2);
  for (auto nonfinite :
       {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff0000000000001)})
    PS_CHECK(!PerlinCoordinate::decode(nonfinite, false).ok());
  std::array<PerlinCoordinate, 3> p{};
  p[0] = PerlinCoordinate::decode(1, false).value();
  auto math = std::make_unique<PerlinExact<272>>();
  std::uint64_t calls = 0;
  const auto cancelled = [&](std::uint64_t) {
    return ++calls == 7 ? Status{ErrorCode::Cancelled, "cancel exact product"}
                        : Status::success();
  };
  const auto stopped = math->evaluate(p, false, cancelled);
  PS_CHECK(!stopped.ok() && stopped.status().code == ErrorCode::Cancelled);
  PS_CHECK(calls == 7);
  auto resumed = math->evaluate(p, false, success);
  PS_CHECK(resumed.ok() && resumed.value() == 1);
  const auto exhausted = [](std::uint64_t) {
    return Status{ErrorCode::ResourceExhausted, "work exhausted"};
  };
  PS_CHECK(math->evaluate(p, false, exhausted).status().code ==
           ErrorCode::ResourceExhausted);
  return 0;
}
