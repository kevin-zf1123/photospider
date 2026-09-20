// Only this translation unit is compiled for SME. Runtime feature admission is
// in accelerated_matrix_apple.cpp, compiled for the ordinary host target.
#include <arm_sme.h>

namespace ps::plugin_internal::numeric_ops {
// ACLE manages streaming-mode and ZA state at this private function boundary.
// Every active ZA lane accumulates the same Cin double products in order.
// Predicates exclude all tails; the kernel never reads beyond packed scratch.
__arm_new("za") __arm_locally_streaming void sme_matrix_kernel(
    const double* x, const double* matrix, const double* bias, double* y,
    unsigned rows, unsigned input_components, unsigned output_components) {
  const auto lanes = static_cast<unsigned>(svcntd());
  for (unsigned begin = 0; begin < rows; begin += lanes) {
    const unsigned count = rows - begin < lanes ? rows - begin : lanes;
    const auto pr = svwhilelt_b64_u64(0, count);
    for (unsigned first = 0; first < output_components; first += lanes) {
      const unsigned columns =
          output_components - first < lanes ? output_components - first : lanes;
      const auto pc = svwhilelt_b64_u64(0, columns);
      svzero_za();
      for (unsigned j = 0; j < input_components; ++j) {
        const auto a = svld1_f64(pr, x + j * 64 + begin);
        const auto b = svld1_f64(pc, matrix + j * 4 + first);
        svmopa_za64_f64_m(0, pr, pc, a, b);
      }
      const auto offset = svld1_f64(pc, bias + first);
      for (unsigned r = 0; r < count; ++r) {
        const auto value = svread_hor_za64_f64_m(svdup_f64(0), pc, 0, r);
        svst1_f64(pc, y + (begin + r) * output_components + first,
                  svadd_f64_x(pc, value, offset));
      }
    }
  }
}
}  // namespace ps::plugin_internal::numeric_ops
