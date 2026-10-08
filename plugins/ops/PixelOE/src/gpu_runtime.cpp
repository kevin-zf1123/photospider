#include "gpu_runtime.hpp"  // NOLINT(build/include_subdir)

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace px {
void dispatch_gpu(const ps_result_services_v2* services, const char* entry,
                  std::array<uint32_t, 3> grid, const Arguments& arguments) {
  if (services->consume_work(services->context, 16384) != 0)
    throw Failure(4, "PixelOE GPU metadata work budget exhausted");
  const GpuKernel* selected = nullptr;
  for (const auto& kernel : gpu_kernels()) {
    if (kernel.backend == services->gpu->backend &&
        std::strcmp(entry, kernel.name) == 0) {
      selected = &kernel;
      break;
    }
  }
  if (!selected)
    throw Failure(6, std::string("missing native kernel: ") + entry);
  const auto& kernel = *selected;
  if (services->consume_work(services->context,
                             gpu_work_bound(kernel, grid, arguments)) != 0)
    throw Failure(4, "PixelOE GPU work budget exhausted");
  std::array<uint8_t, 512> constants{};
  std::array<ps_gpu_buffer_binding_v1, 31> bindings{};
  uint32_t count = 0;
  for (uint32_t i = 0; i < kernel.parameter_count; ++i) {
    const auto& parameter = kernel.parameters[i];
    const auto& value = arguments.get(parameter.name);
    if (parameter.kind == GpuKind::Buffer) {
      if (!value.token || !value.count ||
          value.count > UINT64_MAX / parameter.size || count == bindings.size())
        throw Failure(6, "invalid native PixelOE buffer argument");
      bindings[count++] = {
          sizeof(ps_gpu_buffer_binding_v1), parameter.index,   value.token, 0,
          value.count * parameter.size,     parameter.writable};
    } else {
      if (parameter.offset > constants.size() ||
          parameter.size > constants.size() - parameter.offset)
        throw Failure(6, "invalid native PixelOE scalar layout");
      std::memcpy(constants.data() + parameter.offset,
                  parameter.kind == GpuKind::Float
                      ? static_cast<const void*>(&value.f)
                      : static_cast<const void*>(&value.u),
                  parameter.size);
    }
  }
  ps_gpu_dispatch_v1 dispatch{};
  dispatch.struct_size = sizeof(dispatch);
  dispatch.source = kernel.source;
  dispatch.source_size = kernel.source_size;
  dispatch.code_format = kernel.code_format;
  dispatch.entry = kernel.name;
  dispatch.entry_size = std::strlen(kernel.name);
  dispatch.buffers = bindings.data();
  dispatch.buffer_count = count;
  dispatch.constants = constants.data();
  dispatch.constant_size = kernel.constant_size;
  dispatch.constant_index = kernel.constant_index;
  for (unsigned axis = 0; axis < 3; ++axis) {
    dispatch.grid[axis] = grid[axis];
    dispatch.group[axis] = kernel.group[axis];
  }
  const int code = services->gpu->execute(services->gpu->context, &dispatch, 1);
  if (code)
    throw Failure(code,
                  std::string("PixelOE native dispatch failed: ") + entry);
}
void Context::copy(const Array& source, const Array& destination) {
  if (source.count != destination.count ||
      source.count > (UINT64_MAX - 4096) / 8)
    throw Failure(6, "invalid PixelOE image copy");
  if (!gpu_enabled()) {
    check();
    std::memcpy(destination.data, source.data, source.count * 4);
    return;
  }
  if (services_->gpu->backend == PS_GPU_BACKEND_VULKAN_V1) {
    if (source.count > UINT32_MAX)
      throw Failure(4, "PixelOE GPU copy grid exceeds uint32");
    dispatch_gpu(services_, "copy_words",
                 {static_cast<uint32_t>(source.count), 1, 1},
                 {{"source", source},
                  {"destination", destination},
                  {"count", static_cast<uint64_t>(source.count)}});
    return;
  }
  charge(source.count * 8 + 4096);
  const char shader[] =
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void copy_words(device const uint* a [[buffer(0)]], "
      "device uint* b [[buffer(1)]], constant ulong& n [[buffer(2)]], "
      "uint i [[thread_position_in_grid]]){if(i<n)b[i]=a[i];}";
  const ps_gpu_buffer_binding_v1 bindings[] = {
      {sizeof(ps_gpu_buffer_binding_v1), 0, source.token, 0, source.count * 4,
       0},
      {sizeof(ps_gpu_buffer_binding_v1), 1, destination.token, 0,
       destination.count * 4, 1}};
  const uint64_t count = source.count;
  ps_gpu_dispatch_v1 dispatch{};
  dispatch.struct_size = sizeof(dispatch);
  dispatch.source = shader;
  dispatch.source_size = sizeof(shader) - 1;
  dispatch.entry = "copy_words";
  dispatch.entry_size = 10;
  dispatch.buffers = bindings;
  dispatch.buffer_count = 2;
  dispatch.constants = &count;
  dispatch.constant_size = sizeof(count);
  dispatch.constant_index = 2;
  dispatch.grid[0] = count;
  dispatch.grid[1] = dispatch.grid[2] = 1;
  const int code =
      services_->gpu->execute(services_->gpu->context, &dispatch, 1);
  if (code)
    throw Failure(code, "PixelOE native copy failed");
}
}  // namespace px
