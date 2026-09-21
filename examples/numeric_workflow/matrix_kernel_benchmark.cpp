// Internal candidate + certificate comparison. Public workflow timings live in
// matrix.cpp.
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

#include "01-numeric/accelerated_matrix.hpp"
#include "01-numeric/exact_dot.hpp"
namespace n = ps::plugin_internal::numeric_ops;
int main() {
  ps::input_internal::Float32Environment environment;
  n::MatrixBlock b;
  n::ExactDot exact(n::SequenceProfile::AppleSilicon);
  const std::function<ps::Status(std::uint64_t)> consume = [](auto) {
    return ps::Status::success();
  };
  for (unsigned o = 0; o < 4; ++o) {
    b.offsets[o] = (o + 1) / 16.;
    b.bias[o] = n::numeric_bits(b.offsets[o], true);
    for (unsigned j = 0; j < 4; ++j) {
      b.coefficients[o * 4 + j] = (o + j + 1) / 8.;
      b.matrix[o][j] = n::numeric_bits(b.coefficients[o * 4 + j], true);
    }
  }
  for (unsigned r = 0; r < 64; ++r)
    for (unsigned j = 0; j < 4; ++j) {
      b.x[r * 4 + j] = (r % 31 + j + 1) / 32.;
      b.raw[r][j] = n::numeric_bits(b.x[r * 4 + j], true);
    }
  volatile std::uint64_t checksum = 0;
  std::cout << "backend,rows,median_ns_per_output,max_ns_per_output,checksum\n";
  for (unsigned rows : {1U, 16U, 64U})
    for (unsigned backend = 0; backend < 4; ++backend) {
      if ((backend == 2 && !n::accelerate_matrix_available()) ||
          (backend == 3 && !n::sme_matrix_available()))
        continue;
      std::vector<double> times;
      const unsigned repeats = backend ? 1000 : 100;
      for (unsigned sample = 0; sample < 8; ++sample) {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned rep = 0; rep < repeats; ++rep) {
          if (backend == 1)
            n::scalar_matrix_candidates(&b, rows, 4, 4);
          if (backend == 2) {
            if (!n::accelerate_matrix_candidates(&b, rows, 4, 4))
              return 2;
          }
          if (backend == 3 && !n::sme_matrix_candidates(&b, rows, 4, 4))
            return 5;
          for (unsigned r = 0; r < rows; ++r)
            for (unsigned o = 0; o < 4; ++o) {
              if (backend) {
                auto result = n::certify_matrix_float32(b, r, o, 4, 4);
                if (!result)
                  return 3;
                checksum += *result;
              } else {
                auto result =
                    exact.evaluate(b.raw[r], b.matrix[o], b.bias[o], 4,
                                   ps::ElementType::Float32, consume);
                if (!result.ok())
                  return 4;
                checksum += result.value();
              }
            }
        }
        if (sample)
          times.push_back(std::chrono::duration<double, std::nano>(
                              std::chrono::steady_clock::now() - start)
                              .count() /
                          (repeats * rows * 4));
      }
      std::sort(times.begin(), times.end());
      std::cout << (backend == 0   ? "exact"
                    : backend == 1 ? "scalar-cert"
                    : backend == 2 ? "dgemm-cert"
                                   : "sme-cert")
                << ',' << rows << ',' << times[3] << ',' << times.back() << ','
                << checksum << '\n';
    }
}
