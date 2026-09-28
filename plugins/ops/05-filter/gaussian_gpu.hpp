#pragma once

#include <cstdint>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal {
struct GaussianGpuKernel final {
  const std::uint8_t* coefficients;
  std::uint64_t bytes, x_offset, y_offset, nx, ny, cval;
  std::uint32_t x, y, boundary, identity;
};
inline constexpr std::uint64_t kGaussianGpuWorkspace = 64 * (8 + 10 * 136) * 4;
// Coefficients are borrowed through synchronous completion. All sample math,
// normalization and final IEEE rounding execute on device.
Result<Value> execute_gaussian_gpu(const OperationInvocation& call,
                                   const GaussianGpuKernel& kernel);
}  // namespace ps::plugin_internal
