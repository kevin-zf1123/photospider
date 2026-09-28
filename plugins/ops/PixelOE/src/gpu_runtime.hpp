#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "runtime.hpp"  // NOLINT(build/include_subdir)
namespace px {
enum class GpuKind { Buffer, Float, Integer };
struct GpuParameter {
  const char* name;
  GpuKind kind;
  uint32_t index, offset, size, writable;
};
struct GpuKernel {
  const char* name;
  const char* source;
  uint32_t source_size;
  std::array<uint32_t, 3> group;
  uint32_t constant_index, constant_size;
  const GpuParameter* parameters;
  uint32_t parameter_count;
  uint32_t backend, code_format;
};
const std::vector<GpuKernel>& gpu_kernels();
uint64_t gpu_work_bound(const GpuKernel&, std::array<uint32_t, 3>,
                        const Arguments&);
void dispatch_gpu(const ps_planar_services_v3*, const char*,
                  std::array<uint32_t, 3>, const Arguments&);
}  // namespace px
