#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <cstring>

#include "plugin/expression.hpp"
#include "support/test_support.hpp"

int main() {
  const struct {
    const char* text;
    std::uint64_t bits;
  } cases[] = {{"0.1", UINT64_C(0x3fb999999999999a)},
               {"5e-324", 1},
               {"2.2250738585072014e-308", UINT64_C(0x0010000000000000)},
               {"1.7976931348623157e308", UINT64_C(0x7fefffffffffffff)},
               {"0e-99999", 0}};
  fenv_t previous;
  PS_CHECK(fegetenv(&previous) == 0);
  for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    PS_CHECK(fesetround(rounding) == 0);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_DIVBYZERO);
    for (const auto& test : cases) {
      auto parsed = ps::expression_internal::parse(test.text, 1);
      PS_CHECK(parsed.ok());
      PS_CHECK(parsed.value().nodes.size() == 1);
      std::uint64_t actual;
      std::memcpy(&actual, &parsed.value().nodes[0].number, sizeof(actual));
      PS_CHECK(actual == test.bits);
      PS_CHECK(fegetround() == rounding);
      PS_CHECK(fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
    }
    for (const char* invalid : {"1e9999", "1e-9999", "1e", "1,5"})
      PS_CHECK(!ps::expression_internal::parse(invalid, 1).ok());
  }
  PS_CHECK(fesetenv(&previous) == 0);
  return 0;
}
