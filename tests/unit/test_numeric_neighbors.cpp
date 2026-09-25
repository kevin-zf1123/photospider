#include <cerrno>
#include <cfenv>  // NOLINT(build/c++11): project requires C++17.
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "01-numeric/accelerated_math.hpp"

namespace {
namespace n = ps::plugin_internal::numeric_ops;
struct Observation final {
  std::uint64_t bits;
  int exceptions;
  int error;
};
Observation observe(double value, bool up, bool reference) {
  std::feclearexcept(FE_ALL_EXCEPT);
  std::feraiseexcept(FE_DIVBYZERO);
  errno = EDOM;
  // Indirect volatile call prevents substitution of the reference with our
  // integer implementation. Compare exception flags and errno as well as bits.
  double (*volatile next)(double, double) = std::nextafter;
  const double result =
      reference ? next(value, up ? std::numeric_limits<double>::infinity()
                                 : -std::numeric_limits<double>::infinity())
                : (up ? n::numeric_up(value) : n::numeric_down(value));
  return {n::numeric_bits(result), std::fetestexcept(FE_ALL_EXCEPT), errno};
}
}  // namespace

int main() {
  std::fenv_t saved{};
  if (std::fegetenv(&saved))
    return 1;
  try {
    std::vector<std::uint64_t> inputs;
    for (std::uint64_t exponent = 0; exponent < 2048; ++exponent)
      for (const auto fraction :
           {UINT64_C(0), UINT64_C(1), UINT64_C(0x000fffffffffffff)})
        for (const auto sign : {UINT64_C(0), UINT64_C(0x8000000000000000)})
          inputs.push_back(sign | (exponent << 52) | fraction);
    for (const auto special :
         {UINT64_C(0x7ff0000000000001), UINT64_C(0x7ff8123456789abc),
          UINT64_C(0xfff0000000000042), UINT64_C(0xfff8abcdef123456)})
      inputs.push_back(special);
    std::uint64_t state = UINT64_C(0x123456789abcdef0);
    for (unsigned i = 0; i < 50000; ++i) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      inputs.push_back(state);
    }
    std::uint64_t checked = 0;
    for (const int rounding :
         {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
      if (std::fesetround(rounding))
        throw std::runtime_error("cannot set rounding mode");
      for (const auto bits : inputs)
        for (const bool upward : {false, true}) {
          const auto expected = observe(n::numeric_double(bits), upward, true);
          const auto actual = observe(n::numeric_double(bits), upward, false);
          if (actual.bits != expected.bits ||
              actual.exceptions != expected.exceptions ||
              actual.error != expected.error)
            throw std::runtime_error("numeric neighbor mismatch at raw bits " +
                                     std::to_string(bits));
          ++checked;
        }
    }
    std::fesetenv(&saved);
    std::cout << checked << " nextafter bit/errno/fenv comparisons passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::fesetenv(&saved);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
