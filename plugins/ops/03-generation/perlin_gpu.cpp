#include "03-generation/perlin_gpu.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "03-generation/perlin_exact.hpp"
#include "03-generation/perlin_spirv.hpp"
#include "execution/result_native.hpp"
#include "perlin_shader.hpp"  // NOLINT(build/include_subdir)

namespace ps::plugin_internal {
namespace {
std::uint64_t work_bound(unsigned q, unsigned words) {
  const auto limbs = [](unsigned bits) -> std::uint64_t {
    return (bits + 31) / 32;
  };
  const auto multiply = [&](unsigned a, unsigned b) {
    return 4 * words + limbs(a) * (16 * limbs(b) + 4);
  };
  // All clearing, scans, add/subtract, shifts, signs, address arithmetic and
  // IEEE extraction fit 512*words. The 33 schoolbook products use the same
  // mathematical degree bounds as the CPU implementation, in 32-bit limbs.
  return 512 * words +
         3 * (multiply(q, q) + multiply(2 * q, q) +
              multiply(2 * q + 4, 3 * q)) +
         8 * (multiply(5 * q + 1, 5 * q + 1) + multiply(10 * q + 1, 5 * q + 1) +
              multiply(15 * q + 1, q + 2));
}
struct Arguments final {
  std::uint64_t begin;
  std::uint32_t count, words, input_narrow, output_narrow;
  std::uint64_t offset, rank, shape[8], strides[8], origin[8];
};
static_assert(sizeof(Arguments) == 232);
struct alignas(16) VulkanSlot final {
  std::uint64_t value, padding;
};
struct alignas(16) VulkanArguments final {
  std::uint64_t begin;
  std::uint32_t count, words, input_narrow, output_narrow;
  std::uint64_t offset, rank, padding;
  VulkanSlot shape[8], strides[8], origin[8];
};
static_assert(sizeof(VulkanSlot) == 16 && sizeof(VulkanArguments) == 432);
static_assert(offsetof(Arguments, shape) == 40);
static_assert(offsetof(VulkanArguments, shape) == 48);
static_assert(offsetof(VulkanArguments, strides) == 176);
static_assert(offsetof(VulkanArguments, origin) == 304);
VulkanArguments pack_vulkan(const Arguments& args) {
  VulkanArguments result{};
  std::memcpy(&result, &args, offsetof(Arguments, shape));
  for (unsigned i = 0; i < 8; ++i) {
    result.shape[i].value = args.shape[i];
    result.strides[i].value = args.strides[i];
    result.origin[i].value = args.origin[i];
  }
  return result;
}
}  // namespace
Result<MutableBuffer> execute_perlin_gpu(const ResultProgramPhase& phase,
                                         const ResultTensorReadWindow& window) {
  using Answer = Result<MutableBuffer>;
  const auto* api = phase.gpu;
  if (!api || phase.query.backend != Backend::Gpu)
    return Answer(Status{ErrorCode::BackendUnavailable,
                         "Perlin requires native GPU services"});
  const bool vulkan = api->backend == PS_GPU_BACKEND_VULKAN_V1;
  if (!vulkan && api->backend != PS_GPU_BACKEND_METAL_V1)
    return Answer(Status{ErrorCode::BackendUnavailable,
                         "Perlin native backend is unsupported"});
  auto uploaded = execution_internal::ResultNativeScope::input(
      window, phase.consume_work, phase.query.cancellation);
  if (!uploaded.ok())
    return Answer(uploaded.status());
  const auto input = uploaded.take_value();
  const auto width = Value::element_size(input.descriptor().element_type);
  Arguments args{};
  args.input_narrow = width == 4;
  args.offset = input.layout().byte_offset;
  args.rank = input.descriptor().shape.size();
  for (std::size_t axis = 0; axis < args.rank; ++axis) {
    args.shape[axis] = input.descriptor().shape[axis];
    args.strides[axis] =
        static_cast<std::uint64_t>(input.layout().byte_strides[axis]);
    args.origin[axis] =
        input.layout().origin.empty() ? 0 : input.layout().origin[axis];
  }
  const auto consume = [&](std::uint64_t amount) {
    if (phase.query.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, "Perlin GPU cancelled"};
    return phase.consume_work(amount);
  };
  const auto count = input.region().element_count().value() / 3;
  unsigned q = 0;
  // Pure admission: decode the immutable input bits to choose limb capacity
  // and bound device work. All polynomial and output rounding runs on device.
  for (std::uint64_t i = 0; i < count; ++i) {
    auto status = consume(128 + 8 * args.rank);
    if (!status.ok())
      return Answer(status);
    // Unsigned arithmetic computes the exact validated affine address modulo
    // 2^64. Even huge signed intermediate terms cancel to the proven in-buffer
    // address, avoiding signed overflow without needing device int128.
    auto linear = i;
    auto address =
        args.offset - args.origin[args.rank - 1] * args.strides[args.rank - 1];
    for (std::size_t axis = args.rank - 1; axis-- > 0;) {
      const auto coordinate = linear % args.shape[axis];
      linear /= args.shape[axis];
      address += (coordinate - args.origin[axis]) * args.strides[axis];
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
      std::uint64_t raw = 0;
      const auto at = address + axis * args.strides[args.rank - 1];
      std::memcpy(&raw, input.bytes().data() + at, width);
      auto decoded = generation_ops::PerlinCoordinate::decode(raw, width == 4);
      if (!decoded.ok())
        return Answer(decoded.status());
      q = std::max(q, decoded.value().denominator_bits);
    }
  }
  const unsigned words = q <= 31 ? 16 : q <= 63 ? 32 : 544;
  const auto batch = std::min<std::uint64_t>(count, q <= 63 ? 256 : 4);
  const bool narrow =
      phase.query.output.result_schema->tensors[0].descriptor.element_type ==
      ElementType::Float32;
  auto charged = consume(count * (narrow ? 4 : 8));
  if (!charged.ok())
    return Answer(charged);
  auto allocated =
      execution_internal::ResultNativeScope::output(count * (narrow ? 4 : 8));
  if (!allocated.ok())
    return Answer(allocated.status());
  auto output = allocated.take_value();
  if (!count)
    return Answer(std::move(output));
  auto status = consume(batch * 17 * words);
  if (!status.ok())
    return Answer(status);
  auto made = phase.allocator.allocate(batch * 17 * words * 4);
  if (!made.ok())
    return Answer(made.status());
  auto scratch = made.take_value();
  std::uint64_t tokens[3]{};
  if (api->buffer(api->context, input.bytes().data(), input.bytes().size(), 0,
                  &tokens[0]) ||
      api->buffer(api->context, output.data(), output.size(), 1, &tokens[1]) ||
      api->buffer(api->context, scratch.data(), scratch.size(), 1, &tokens[2]))
    return Answer(
        Status{ErrorCode::OperationFailed, "Perlin GPU binding failed"});
  const ps_gpu_buffer_binding_v1 bindings[] = {
      {sizeof(ps_gpu_buffer_binding_v1), 0, tokens[0], 0, input.bytes().size(),
       0},
      {sizeof(ps_gpu_buffer_binding_v1), 1, tokens[1], 0, output.size(), 1},
      {sizeof(ps_gpu_buffer_binding_v1), 2, tokens[2], 0, scratch.size(), 1}};
  args.words = words;
  args.output_narrow = narrow;
  ps_gpu_dispatch_v1 command{};
  command.struct_size = sizeof(command);
  command.source =
      vulkan ? reinterpret_cast<const char*>(generation_ops::kPerlinSpirv)
             : generation_ops::kPerlinShader;
  command.source_size = vulkan ? sizeof(generation_ops::kPerlinSpirv)
                               : sizeof(generation_ops::kPerlinShader) - 1;
  command.code_format = vulkan ? PS_GPU_CODE_SPIRV_V1 : PS_GPU_CODE_MSL_V1;
  command.entry = "perlin_exact";
  command.entry_size = 12;
  command.buffers = bindings;
  command.buffer_count = 3;
  command.constants = &args;
  command.constant_size = vulkan ? sizeof(VulkanArguments) : sizeof(args);
  command.constant_index = 3;
  command.grid[1] = command.grid[2] = 1;
  // Sequential encoders in one synchronous submission reuse the same scratch
  // after each device barrier. Limit cohorts to bound cancellation
  // interference; expensive full-denominator batches retain a single-dispatch
  // cohort.
  constexpr unsigned maximum_cohort = 8;
  const auto record_work = (sizeof(Arguments) + sizeof(command) + 7) / 8 +
                           (vulkan ? sizeof(VulkanArguments) / 8 + 64 : 0);
  status = consume(maximum_cohort * record_work);
  if (!status.ok())
    return Answer(status);
  std::array<Arguments, maximum_cohort> arguments{};
  std::array<VulkanArguments, maximum_cohort> vulkan_arguments;
  std::array<ps_gpu_dispatch_v1, maximum_cohort> commands{};
  for (std::uint64_t begin = 0; begin < count;) {
    unsigned issued = 0;
    const unsigned cohort = q <= 63 ? maximum_cohort : 1;
    for (; issued < cohort && begin < count; ++issued) {
      const auto size = std::min(batch, count - begin);
      status = consume(size * work_bound(q, words) + record_work);
      if (!status.ok())
        return Answer(status);
      arguments[issued] = args;
      arguments[issued].begin = begin;
      arguments[issued].count = static_cast<std::uint32_t>(size);
      commands[issued] = command;
      commands[issued].constants = &arguments[issued];
      if (vulkan) {
        vulkan_arguments[issued] = pack_vulkan(arguments[issued]);
        commands[issued].constants = &vulkan_arguments[issued];
      }
      commands[issued].grid[0] = size;
      begin += size;
    }
    if (api->execute(api->context, commands.data(), issued))
      return Answer(Status{ErrorCode::OperationFailed,
                           "Perlin native GPU dispatch failed"});
  }
  status = consume(0);
  return status.ok() ? Answer(std::move(output)) : Answer(status);
}
}  // namespace ps::plugin_internal
