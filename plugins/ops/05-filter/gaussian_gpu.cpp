#include "05-filter/gaussian_gpu.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "05-filter/gaussian_spirv.hpp"
#include "execution/result_native.hpp"
#include "gaussian_shader.hpp"  // NOLINT(build/include_subdir)

namespace ps::plugin_internal {
namespace {
struct Arguments final {
  std::uint64_t begin, tap_begin, tap_end, offset, nx, ny, cval;
  std::uint64_t shape[8], strides[8], origin[8];
  std::uint32_t count, rank, x, y, boundary, narrow, identity, initialize,
      finish;
};
static_assert(sizeof(Arguments) == 288);
struct alignas(16) VulkanSlot final {
  std::uint64_t value, padding;
};
struct alignas(16) VulkanArguments final {
  std::uint64_t begin, tap_begin, tap_end, offset, nx, ny, cval;
  std::uint64_t x_offset, y_offset, padding;
  VulkanSlot shape[8], strides[8], origin[8];
  std::uint32_t count, rank, x, y, boundary, narrow, identity, initialize,
      finish;
};
static_assert(sizeof(VulkanArguments) == 512);
static_assert(offsetof(Arguments, shape) == 56);
static_assert(offsetof(Arguments, count) == 248);
static_assert(offsetof(VulkanArguments, shape) == 80);
static_assert(offsetof(VulkanArguments, strides) == 208);
static_assert(offsetof(VulkanArguments, origin) == 336);
static_assert(offsetof(VulkanArguments, count) == 464);
static_assert(offsetof(VulkanArguments, finish) == 496);
VulkanArguments pack_vulkan(const Arguments& args,
                            const GaussianGpuKernel& kernel) {
  VulkanArguments result{};
  std::memcpy(&result, &args, offsetof(Arguments, shape));
  result.x_offset = kernel.x_offset / 8;
  result.y_offset = kernel.y_offset / 8;
  for (unsigned i = 0; i < 8; ++i) {
    result.shape[i].value = args.shape[i];
    result.strides[i].value = args.strides[i];
    result.origin[i].value = args.origin[i];
  }
  std::memcpy(&result.count, &args.count, 9 * sizeof(std::uint32_t));
  return result;
}
// Trimming exact low zero words limits significand spans to 3 words per
// binary64 operand and 5 per weight product. Including scans and row control,
// both products cost <= 2*(6*136) + (3+5)*(16*3+6) = 2064 units. Remaining
// decoding, addressing and signed accumulation fit 8200. Keep the conservative
// 100000-unit admission, independent of input bits and coefficient exponents.
constexpr std::uint64_t tap_work = 100000;
// At most 53 quotient bits, each with shift/compare/subtract of 136 words,
// plus exponent alignment and final remainder doubling fit this bound.
constexpr std::uint64_t finish_work = 131072;
constexpr std::uint64_t initialize_work = 4096;
}  // namespace
Result<MutableBuffer> execute_gaussian_gpu(const ResultProgramPhase& phase,
                                           const ResultTensorReadWindow& window,
                                           const GaussianGpuKernel& kernel) {
  using Answer = Result<MutableBuffer>;
  const auto* api = phase.gpu;
  if (!api || phase.query.backend != Backend::Gpu)
    return Answer(Status{ErrorCode::BackendUnavailable,
                         "Gaussian requires native GPU services"});
  const bool vulkan = api->backend == PS_GPU_BACKEND_VULKAN_V1;
  if (!vulkan && api->backend != PS_GPU_BACKEND_METAL_V1)
    return Answer(Status{ErrorCode::BackendUnavailable,
                         "Gaussian native backend is unsupported"});
  const auto consume = [&](std::uint64_t amount) {
    if (phase.query.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, "Gaussian GPU cancelled"};
    return phase.consume_work(amount);
  };
  auto uploaded = execution_internal::ResultNativeScope::input(
      window, phase.consume_work, phase.query.cancellation);
  if (!uploaded.ok())
    return Answer(uploaded.status());
  const auto input = uploaded.take_value();
  const auto count = input.region().element_count().value();
  const auto width = Value::element_size(input.descriptor().element_type);
  auto charged = consume(count * width);
  if (!charged.ok())
    return Answer(charged);
  auto allocated = execution_internal::ResultNativeScope::output(count * width);
  if (!allocated.ok())
    return Answer(allocated.status());
  auto output = allocated.take_value();
  if (!count)
    return Answer(std::move(output));
  const auto lanes =
      std::min<std::uint64_t>(vulkan ? 64 : kGaussianGpuMaximumLanes, count);
  auto status = consume(lanes * (8 + 10 * 136));
  if (!status.ok())
    return Answer(status);
  auto allocated_scratch = phase.allocator.allocate(lanes * (8 + 10 * 136) * 4);
  if (!allocated_scratch.ok())
    return Answer(allocated_scratch.status());
  auto scratch = allocated_scratch.take_value();
  std::uint64_t tokens[4]{};
  if (api->buffer(api->context, input.bytes().data(), input.bytes().size(), 0,
                  &tokens[0]) ||
      api->buffer(api->context, output.data(), output.size(), 1, &tokens[1]) ||
      api->buffer(api->context, kernel.coefficients, kernel.bytes, 0,
                  &tokens[2]) ||
      api->buffer(api->context, scratch.data(), scratch.size(), 1, &tokens[3]))
    return Answer(
        Status{ErrorCode::OperationFailed, "Gaussian GPU binding failed"});
  const ps_gpu_buffer_binding_v1 bindings[] = {
      {sizeof(ps_gpu_buffer_binding_v1), 0, tokens[0], 0, input.bytes().size(),
       0},
      {sizeof(ps_gpu_buffer_binding_v1), 1, tokens[1], 0, output.size(), 1},
      {sizeof(ps_gpu_buffer_binding_v1), 2, tokens[2],
       vulkan ? 0 : kernel.x_offset, vulkan ? kernel.bytes : kernel.nx * 8, 0},
      {sizeof(ps_gpu_buffer_binding_v1), 3, tokens[2],
       vulkan ? 0 : kernel.y_offset, vulkan ? kernel.bytes : kernel.ny * 8, 0},
      {sizeof(ps_gpu_buffer_binding_v1), 4, tokens[3], 0, scratch.size(), 1}};
  Arguments args{};
  args.offset = input.layout().byte_offset;
  args.nx = kernel.nx;
  args.ny = kernel.ny;
  args.cval = kernel.cval;
  args.rank = static_cast<std::uint32_t>(input.descriptor().shape.size());
  args.x = kernel.x;
  args.y = kernel.y;
  args.boundary = kernel.boundary;
  args.identity = kernel.identity;
  args.narrow = input.descriptor().element_type == ElementType::Float32;
  for (unsigned axis = 0; axis < args.rank; ++axis) {
    args.shape[axis] = input.descriptor().shape[axis];
    args.strides[axis] =
        static_cast<std::uint64_t>(input.layout().byte_strides[axis]);
    args.origin[axis] =
        input.layout().origin.empty() ? 0 : input.layout().origin[axis];
  }
  ps_gpu_dispatch_v1 command{};
  command.struct_size = sizeof(command);
  command.source =
      vulkan ? reinterpret_cast<const char*>(filter_ops::kGaussianSpirv)
             : filter_ops::kGaussianShader;
  command.source_size = vulkan ? sizeof(filter_ops::kGaussianSpirv)
                               : sizeof(filter_ops::kGaussianShader) - 1;
  command.code_format = vulkan ? PS_GPU_CODE_SPIRV_V1 : PS_GPU_CODE_MSL_V1;
  command.entry = "gaussian_exact";
  command.entry_size = 14;
  command.buffers = bindings;
  command.buffer_count = 5;
  command.constants = &args;
  command.constant_size = vulkan ? sizeof(VulkanArguments) : sizeof(args);
  command.constant_index = 5;
  command.grid[1] = command.grid[2] = 1;
  command.group[0] = 64;
  command.group[1] = command.group[2] = 1;
  const auto taps = kernel.nx * kernel.ny;
  // Two ordered dispatches share scratch across the backend's device barrier.
  // Bound each submission to at most 32 taps per lane for cancellation drain.
  constexpr unsigned maximum_cohort = 2;
  const auto record_work = (sizeof(Arguments) + sizeof(command) + 7) / 8 +
                           (vulkan ? sizeof(VulkanArguments) / 8 + 64 : 0);
  status = consume(maximum_cohort * record_work);
  if (!status.ok())
    return Answer(status);
  std::array<Arguments, maximum_cohort> arguments{};
  std::array<VulkanArguments, maximum_cohort> vulkan_arguments;
  std::array<ps_gpu_dispatch_v1, maximum_cohort> commands{};
  for (std::uint64_t begin = 0; begin < count;) {
    args.begin = begin;
    args.count = static_cast<std::uint32_t>(std::min(lanes, count - begin));
    command.grid[0] = args.count;
    for (std::uint64_t tap = 0; tap < taps;) {
      unsigned issued = 0;
      for (; issued < maximum_cohort && tap < taps; ++issued) {
        const auto chunk = std::min<std::uint64_t>(16, taps - tap);
        args.tap_begin = tap;
        args.tap_end = tap + chunk;
        args.initialize = tap == 0;
        args.finish = args.tap_end == taps;
        status = consume(args.count * (chunk * tap_work +
                                       args.initialize * initialize_work +
                                       args.finish * finish_work) +
                         record_work);
        if (!status.ok())
          return Answer(status);
        arguments[issued] = args;
        commands[issued] = command;
        commands[issued].constants = &arguments[issued];
        if (vulkan) {
          vulkan_arguments[issued] = pack_vulkan(args, kernel);
          commands[issued].constants = &vulkan_arguments[issued];
        }
        tap = args.tap_end;
      }
      if (api->execute(api->context, commands.data(), issued))
        return Answer(Status{ErrorCode::OperationFailed,
                             "Gaussian native GPU dispatch failed"});
    }
    begin += args.count;
  }
  status = consume(0);
  return status.ok() ? Answer(std::move(output)) : Answer(status);
}
}  // namespace ps::plugin_internal
