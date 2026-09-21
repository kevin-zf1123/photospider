#include <array>
#include <cstdint>
#include <limits>

#include "01-numeric/accelerated_matrix.hpp"
#include "support/test_support.hpp"

namespace n = ps::plugin_internal::numeric_ops;
int main() {
  ps::input_internal::Float32Environment environment;
  PS_CHECK(environment.active());
  struct Case {
    std::array<double, 4> x, m;
    double bias;
    std::uint32_t expected;
    bool accepted;
  };
  const Case cases[] = {
      {{2, 3, 0, 0}, {1, 2, 0, 0}, 4, 0x41400000, true},
      {{1, 0x1p-24, 0, 0}, {1, 1, 0, 0}, 0, 0x3f800000, false},
      {{1 + 0x1p-23, 0x1p-24, 0, 0}, {1, 1, 0, 0}, 0, 0x3f800002, false},
      {{1, 0x1p-24, 0, 0}, {1, 1, 0, 0}, 0x1p-48, 0x3f800001, true},
      {{1, 0x1p-24, 0, 0}, {1, 1, 0, 0}, -0x1p-48, 0x3f800000, true},
      {{1, 0x1p-24, 0, 0}, {1, 1, 0, 0}, 0x1p-52, 0x3f800001, false},
      {{-1, -0x1p-24, 0, 0}, {1, 1, 0, 0}, -0x1p-48, 0xbf800001, true},
      {{0x1p-126, 0x1p-149, 0, 0}, {1, -.5, 0, 0}, 0, 0x00800000, false},
      {{0x1.fffffep127, 0x1p103, 0, 0}, {1, 1, 0, 0}, 0, 0x7f800000, false},
      {{0x1p-149, 0, 0, 0}, {0x1p-149, 0, 0, 0}, 0, 0, false},
      {{0x1.fffffep127, 0x1.fffffep127, 0, 0},
       {0x1.fffffep127, -0x1.fffffep127, 0, 0},
       1,
       0x3f800000,
       false},
      {{-0., -0., -0., -0.}, {1, 1, 1, 1}, -0., 0x80000000, false}};
  for (const auto& c : cases) {
    n::MatrixBlock block;
    block.offsets[0] = c.bias;
    for (unsigned j = 0; j < 4; ++j) {
      block.x[j] = c.x[j];
      block.coefficients[j] = c.m[j];
    }
    block.y[0] = n::numeric_double(c.expected, true);
    auto answer = n::certify_matrix_float32(block, 0, 0, 4, 2);
    PS_CHECK(answer.has_value() == c.accepted);
    if (answer)
      PS_CHECK(*answer == c.expected);
    // The gate must not trust candidate arithmetic: wrong neighbors and
    // nonfinite candidates are rejected even when the true result is easy.
    for (auto wrong :
         {c.expected ^ 1U, 0x7fc00042U, 0x7f800000U, 0xff800000U}) {
      block.y[0] = n::numeric_double(wrong, true);
      PS_CHECK(!n::certify_matrix_float32(block, 0, 0, 4, 2));
    }
  }
}
