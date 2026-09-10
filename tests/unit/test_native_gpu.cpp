#include <cstring>
#include <iostream>
#include <memory>
#include <utility>

#include "execution/native_gpu.hpp"
#include "support/test_support.hpp"

int main() {
  using ps::BufferAllocator;
  using ps::CpuStorage;
  using ps::ErrorCode;
  using ps::gpu_internal::allocation_capacity;
  using ps::gpu_internal::Device;
  using ps::gpu_internal::Invocation;
  PS_CHECK(allocation_capacity(16385) == 32768);
  PS_CHECK(allocation_capacity(UINT64_MAX) == 0);
  auto device = Device::create();
  if (!device) {
    std::cout << "Metal unavailable: native hardware tests skipped\n";
    return 77;
  }
  auto allocator = device->allocator(BufferAllocator());
  auto input = allocator.allocate(16).take_value();
  const float data[4] = {0, .25F, .5F, 1};
  std::memcpy(input.data(), data, sizeof(data));
  auto frozen = std::move(input).freeze();
  auto output = allocator.allocate(16).take_value();
  PS_CHECK(device->owns(*frozen));
  PS_CHECK(!device->view(frozen->bytes().data(), 16, true).ok());
  PS_CHECK(!device->view(reinterpret_cast<const std::uint8_t*>(data), 16, false)
                .ok());
  std::shared_ptr<const CpuStorage> retained;
  {
    Invocation invocation(device, {});
    const auto* api = invocation.service();
    std::uint64_t source = 0, destination = 0;
    PS_CHECK(
        api->buffer(api->context, frozen->bytes().data(), 16, 0, &source) == 0);
    PS_CHECK(api->buffer(api->context, output.data(), 16, 1, &destination) ==
             0);
    const char shader[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void scale(device const float* a [[buffer(0)]], "
        "device float* b [[buffer(1)]], uint i [[thread_position_in_grid]])"
        "{b[i]=a[i]*.5f;}";
    ps_gpu_buffer_binding_v7 buffers[] = {
        {sizeof(ps_gpu_buffer_binding_v7), 0, source, 0, 16, 0},
        {sizeof(ps_gpu_buffer_binding_v7), 1, destination, 0, 16, 1}};
    ps_gpu_dispatch_v7 command{};
    command.struct_size = sizeof(command);
    command.source = shader;
    command.source_size = sizeof(shader) - 1;
    command.entry = "scale";
    command.entry_size = 5;
    command.buffers = buffers;
    command.buffer_count = 2;
    command.grid[0] = 4;
    command.grid[1] = command.grid[2] = 1;
    PS_CHECK(api->execute(api->context, &command, 1) == 0);
    PS_CHECK(invocation.statistics().dispatches == 1);
    float actual[4];
    std::memcpy(actual, output.data(), sizeof(actual));
    for (int i = 0; i < 4; ++i)
      PS_CHECK(actual[i] == data[i] * .5F);
    // This shader supplies no pragma: the host must disable contraction.
    auto rounding = allocator.allocate(16).take_value();
    const float operands[4] = {0x1.000002p0F, 1, 1, 1};
    std::memcpy(rounding.data(), operands, sizeof(operands));
    std::uint64_t rounding_token = 0;
    PS_CHECK(api->buffer(api->context, rounding.data(), 16, 0,
                         &rounding_token) == 0);
    const char arithmetic[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void scale(device const float* a [[buffer(0)]], "
        "device float* b [[buffer(1)]], uint i [[thread_position_in_grid]])"
        "{b[i]=a[i]*0x1.fffffcp-1f-1.0f;}";
    buffers[0].token = rounding_token;
    command.source = arithmetic;
    command.source_size = sizeof(arithmetic) - 1;
    PS_CHECK(api->execute(api->context, &command, 1) == 0);
    float rounded = 1;
    std::memcpy(&rounded, output.data(), 4);
    PS_CHECK(rounded == 0);  // FMA would produce -2^-46.
    buffers[0].token = source;
    command.source = shader;
    command.source_size = sizeof(shader) - 1;
    PS_CHECK(api->execute(api->context, &command, 1) == 0);
    retained = std::move(output).freeze();
    buffers[0].byte_size = 17;
    PS_CHECK(api->execute(api->context, &command, 1) != 0);
    PS_CHECK(invocation.status().code == ErrorCode::InvalidArgument);
    PS_CHECK(invocation.statistics().dispatches == 3);
  }
  allocator = BufferAllocator();
  device.reset();
  float last = 0;
  std::memcpy(&last, retained->bytes().data() + 12, 4);
  PS_CHECK(last == .5F);
  std::cout << "Metal native allocation, dispatch, bounds and retained "
               "lifetime passed\n";
  return 0;
}
