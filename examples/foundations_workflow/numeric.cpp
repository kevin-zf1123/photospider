#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "workflow.hpp"  // NOLINT(build/include_subdir)

namespace foundations {
void numeric() {
  exact<float>(output(operation("numeric.subtract", {array<float>({3, 2, 1}),
                                                     array<float>({4, 4, 4})})),
               {-1, -2, -3});
  exact<double>(output(operation("numeric.mean", {array<float>({1, 2, 3})})),
                {2});
  auto variance =
      output(operation("numeric.variance", {array<float>({1, 2, 3})}));
  double v;
  std::memcpy(&v, variance.bytes().data(), 8);
  require(std::abs(v - 2. / 3) < 1e-15, "variance oracle");
  std::cout << "numeric negative_ramp=[-1,-2,-3] mean=2 variance=2/3 "
               "oracle=passed\n";
}
}  // namespace foundations
