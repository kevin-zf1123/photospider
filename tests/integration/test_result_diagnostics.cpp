#include <cstdint>

#include "support/result_diagnostics_fixture.hpp"

int main() {
  using result_diagnostics::verify_cpp_fixed_broadcast;
  PS_CHECK(verify_cpp_fixed_broadcast({4}) == 0);
  PS_CHECK(verify_cpp_fixed_broadcast({UINT64_MAX}) == 0);
  PS_CHECK(verify_cpp_fixed_broadcast({UINT64_MAX, 2}) == 0);
  PS_CHECK(verify_cpp_fixed_broadcast({UINT64_MAX}, true) == 0);
  return 0;
}
