#include "01-numeric/accelerated_matrix.hpp"

#if defined(PHOTOSPIDER_HAS_ACCELERATE) && defined(__APPLE__) && \
    defined(__aarch64__)
// Select the current LP64 BLAS interface; dimensions never exceed 64.
#define ACCELERATE_NEW_LAPACK
#include <Accelerate/Accelerate.h>
#endif

#if defined(PHOTOSPIDER_HAS_MATRIX_SME)
#include <sys/sysctl.h>
#endif

namespace ps::plugin_internal::numeric_ops {
#if defined(PHOTOSPIDER_HAS_MATRIX_SME)
void sme_matrix_kernel(const double*, const double*, const double*, double*,
                       unsigned, unsigned, unsigned);
#endif
bool sme_matrix_available() {
#if defined(PHOTOSPIDER_HAS_MATRIX_SME)
  static const bool available = [] {
    int sme = 0, fp64 = 0;
    std::size_t size = sizeof(int);
    if (sysctlbyname("hw.optional.arm.FEAT_SME", &sme, &size, nullptr, 0) !=
            0 ||
        !sme)
      return false;
    size = sizeof(int);
    return sysctlbyname("hw.optional.arm.FEAT_SME_F64F64", &fp64, &size,
                        nullptr, 0) == 0 &&
           fp64;
  }();
  return available;
#else
  return false;
#endif
}
bool sme_matrix_candidates(MatrixBlock* block, unsigned rows,
                           unsigned input_components,
                           unsigned output_components) {
#if defined(PHOTOSPIDER_HAS_MATRIX_SME)
  if (!sme_matrix_available() || !rows || rows > MatrixBlock::kRows ||
      input_components < 2 || input_components > 4 || !output_components ||
      output_components > 4)
    return false;
  // SME streaming loads are contiguous; transpose into admitted fixed scratch
  // instead of requiring the optional streaming gather (SME_FA64) feature.
  for (unsigned j = 0; j < input_components; ++j) {
    for (unsigned r = 0; r < rows; ++r)
      block->sme_x[j * MatrixBlock::kRows + r] =
          block->x[r * input_components + j];
    for (unsigned o = 0; o < output_components; ++o)
      block->sme_coefficients[j * 4 + o] =
          block->coefficients[o * input_components + j];
  }
  sme_matrix_kernel(block->sme_x.data(), block->sme_coefficients.data(),
                    block->offsets.data(), block->y.data(), rows,
                    input_components, output_components);
  return true;
#else
  (void)block;
  (void)rows;
  (void)input_components;
  (void)output_components;
  return false;
#endif
}

bool accelerate_matrix_available() {
#if defined(PHOTOSPIDER_HAS_ACCELERATE) && defined(__APPLE__) && \
    defined(__aarch64__) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (__builtin_available(macOS 15.0, *))
    return true;
#endif
  return false;
}
bool accelerate_matrix_candidates(MatrixBlock* block, unsigned rows,
                                  unsigned input_components,
                                  unsigned output_components) {
#if defined(PHOTOSPIDER_HAS_ACCELERATE) && defined(__APPLE__) && \
    defined(__aarch64__) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (__builtin_available(macOS 15.0, *)) {
    if (!rows || rows > MatrixBlock::kRows || input_components < 2 ||
        input_components > 4 || !output_components || output_components > 4)
      return false;
    const auto previous = BLASGetThreading();
    if (BLASSetThreading(BLAS_THREADING_SINGLE_THREADED) != 0)
      return false;
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasTrans, rows,
                output_components, input_components, 1., block->x.data(),
                input_components, block->coefficients.data(), input_components,
                0., block->y.data(), output_components);
    // Both enum values are supported on an admitted platform. Restore before
    // returning to host code, including before certification/exact replay.
    const auto restored = BLASSetThreading(previous);
    if (restored != 0)
      throw Status{ErrorCode::Internal, "cannot restore BLAS threading"};
    for (unsigned r = 0; r < rows; ++r)
      for (unsigned o = 0; o < output_components; ++o)
        block->y[r * output_components + o] += block->offsets[o];
    return true;
  }
#else
  (void)block;
  (void)rows;
  (void)input_components;
  (void)output_components;
#endif
  return false;
}
}  // namespace ps::plugin_internal::numeric_ops
