#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <utility>

#include "execution/execution_test_hooks.hpp"
#include "execution/native_gpu.hpp"
#include "fixtures/native_vulkan_spirv.hpp"
#include "photospider/execution/resources.hpp"
#include "photospider/photospider.hpp"
#include "support/native_allocation_quota.hpp"
#include "support/native_atlas_budget.hpp"
#include "support/native_metadata_budget.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using gpu_internal::Device;
using gpu_internal::Invocation;
struct Parameters {
  std::uint32_t width, height, depth, multiplier, increment;
};
std::uint32_t expected_value(std::uint32_t value, const Parameters& p) {
  const std::uint64_t wide = std::uint64_t{value} * p.multiplier + p.increment;
  return static_cast<std::uint32_t>(wide ^ (wide >> 32));
}
ps_gpu_dispatch_v11 command(const ps_gpu_buffer_binding_v11* binding,
                            const Parameters* parameters) {
  ps_gpu_dispatch_v11 result{};
  result.struct_size = sizeof(result);
  result.source = reinterpret_cast<const char*>(kNativeVulkanSpirv);
  result.source_size = sizeof(kNativeVulkanSpirv);
  result.entry = "transform";
  result.entry_size = 9;
  result.buffers = binding;
  result.buffer_count = 1;
  result.constants = parameters;
  result.constant_size = sizeof(*parameters);
  result.constant_index = 1;
  result.grid[0] = parameters->width;
  result.grid[1] = parameters->height;
  result.grid[2] = parameters->depth;
  result.code_format = PS_GPU_CODE_SPIRV_V11;
  return result;
}
struct DependencyCompute {
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    auto made =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!made.ok())
      return Result<DependencyPoll>(made.status());
    auto output = made.take_value();
    auto token = phase.gpu_buffer(output.data(), output.size(), true);
    if (!token.ok())
      return Result<DependencyPoll>(token.status());
    const ps_gpu_buffer_binding_v11 binding{
        sizeof(binding), 0, token.value(), 0, output.size(), 1};
    const Parameters parameters{1, 1, 1, 3, 7};
    auto dispatch = command(&binding, &parameters);
    auto status = phase.gpu_execute(&dispatch, 1);
    if (!status.ok())
      return Result<DependencyPoll>(status);
    auto value = std::move(output).publish();
    if (!value.ok())
      return Result<DependencyPoll>(value.status());
    auto fragments =
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs, {value.take_value()});
    return fragments.ok() ? Result<DependencyPoll>(fragments.take_value())
                          : Result<DependencyPoll>(fragments.status());
  }
};
int dependency_workflow(const std::shared_ptr<Device>& device) {
  execution_testing::ExecutionTestHooks hooks;
  hooks.native_device = true;
  execution_testing::install_execution_test_hooks(&hooks);
  struct Clear {
    ~Clear() { execution_testing::install_execution_test_hooks(nullptr); }
  } clear;
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition definition;
  definition.key = "test.vulkan.dependency";
  definition.traits.supports_cpu = false;
  definition.traits.supports_gpu = true;
  auto& output = definition.traits.outputs[0];
  output.output_element_type = ElementType::Int64;
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {1};
  output.region_rule = OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(DependencyCompute);
  output.maximum_dependency_stages = 1;
  definition.start_dependency = [](const DependencyQuery&,
                                   const BufferAllocator& allocator) {
    return DependencyContinuation::make<DependencyCompute>(allocator);
  };
  auto registered = registry->register_operation(std::move(definition));
  if (!registered.ok())
    std::cerr << registered.message << '\n';
  PS_CHECK(registered.ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, "test.vulkan.dependency", {}, {}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_CHECK(compiled.ok() && compiled.value().plan.dependency_network());
  const auto payload = device->allocation_capacity(8).value();
  const auto constants =
      device->allocation_capacity(sizeof(Parameters)).value();
  for (auto kind : {ResourceKind::Device, ResourceKind::Shared}) {
    for (const bool reject : {true, false}) {
      Value retained;
      {
        ExecutionContextConfig config;
        config.gpu_enabled = true;
        config.cpu_workers = 2;
        config.managed_resources = ResourceLimits{};
        config.managed_resources->capacity[kind] = payload + constants - reject;
        ExecutionContext context(registry, config);
        auto result = context.execute(compiled.value().plan);
        if (result.ok() == reject)
          std::cerr << result.status().message << '\n';
        PS_CHECK(result.ok() == !reject);
        if (reject) {
          PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
          PS_CHECK(context.resource_budget().value().statistics().live[kind] ==
                   0);
        } else {
          PS_CHECK(result.value().diagnostics.native_dispatch_count == 1);
          PS_CHECK(result.value().diagnostics.fallback_reasons.empty());
          retained = result.value().values.at("result");
          // execute() collects dependency fragments into host result storage.
          const auto stats = context.resource_budget().value().statistics();
          PS_CHECK(stats.live[kind] == 0);
          PS_CHECK(stats.peak[kind] == payload + constants);
        }
      }
      if (!reject) {
        std::uint64_t value = 0;
        std::memcpy(&value, retained.bytes().data(), 8);
        PS_CHECK(value == 7);
      }
    }
  }
  return 0;
}
int capacity(const std::shared_ptr<Device>& device) {
  const auto queried = device->allocation_capacity(4);
  PS_CHECK(queried.ok() && queried.value() >= 4);
  const auto bytes = queried.value();
  const auto metadata =
      sizeof(CpuStorage) + ResourceBudget::lease_metadata_bytes();
  for (const auto kind : {ResourceKind::Host, ResourceKind::Device,
                          ResourceKind::Shared, ResourceKind::Payload}) {
    ResourceLimits limits;
    const auto exact_limit =
        bytes + (kind == ResourceKind::Host ? metadata : 0);
    limits.capacity[kind] = exact_limit - 1;
    ResourceBudget rejected(limits);
    auto failure = device->allocator(rejected.allocator()).allocate(4);
    PS_CHECK(!failure.ok() &&
             failure.status().code == ErrorCode::ResourceExhausted);
    for (auto live : rejected.statistics().live.values)
      PS_CHECK(live == 0);
    limits.capacity[kind] = exact_limit;
    ResourceBudget exact(limits);
    {
      auto value = device->allocator(exact.allocator()).allocate(4);
      PS_CHECK(value.ok() && value.value().size() == 4);
      for (const auto charged : {ResourceKind::Host, ResourceKind::Device,
                                 ResourceKind::Shared, ResourceKind::Payload})
        PS_CHECK(exact.statistics().live[charged] ==
                 bytes + (charged == ResourceKind::Host ? metadata : 0));
    }
    for (auto live : exact.statistics().live.values)
      PS_CHECK(live == 0);
  }
  PS_CHECK(!device->allocation_capacity(UINT64_MAX).ok());
  return 0;
}
int compute(const std::shared_ptr<Device>& device) {
  const Parameters parameters{17, 9, 3, 0x1234567U, 71};
  constexpr std::size_t count = 17 * 9 * 3;
  const auto bytes = device->allocation_capacity(count * 4).value();
  const auto constants =
      device->allocation_capacity(sizeof(parameters)).value();
  for (bool reject : {false, true}) {
    ResourceLimits limits;
    limits.capacity[ResourceKind::Device] = bytes + 2 * constants - reject;
    ResourceBudget budget(limits);
    auto storage =
        device->allocator(budget.allocator()).allocate(count * 4).take_value();
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto value = 0xF0000000U + i;
      std::memcpy(storage.data() + 4 * i, &value, 4);
    }
    {
      Invocation invocation(device, {}, budget.allocator());
      const auto* api = invocation.service();
      PS_CHECK(api->backend == PS_GPU_BACKEND_VULKAN_V11);
      PS_CHECK(api->minimum_buffer_offset_alignment >= 4);
      std::uint64_t token = 0;
      PS_CHECK(
          api->buffer(api->context, storage.data(), count * 4, 1, &token) == 0);
      const ps_gpu_buffer_binding_v11 binding{sizeof(binding), 0, token, 0,
                                              count * 4,       1};
      std::array<ps_gpu_dispatch_v11, 2> commands{
          command(&binding, &parameters), command(&binding, &parameters)};
      // One dispatch selects the module's shape; the other checks it
      // explicitly.
      commands[1].group[0] = 8;
      commands[1].group[1] = 4;
      commands[1].group[2] = 2;
      const auto result =
          api->execute(api->context, commands.data(), commands.size());
      PS_CHECK((result == 0) == !reject);
      if (reject)
        PS_CHECK(invocation.status().code == ErrorCode::ResourceExhausted);
      PS_CHECK(invocation.statistics().dispatches == (reject ? 0 : 2));
      PS_CHECK(invocation.statistics().submissions == (reject ? 0 : 1));
      PS_CHECK(invocation.statistics().constant_bytes ==
               (reject ? 0 : 2 * sizeof(parameters)));
      PS_CHECK(budget.statistics().live[ResourceKind::Device] == bytes);
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t value = 0;
        std::memcpy(&value, storage.data() + 4 * i, 4);
        const auto initial = 0xF0000000U + i;
        PS_CHECK(value ==
                 (reject ? initial
                         : expected_value(expected_value(initial, parameters),
                                          parameters)));
      }
    }
    storage = MutableBuffer();
    for (auto live : budget.statistics().live.values)
      PS_CHECK(live == 0);
  }
  return 0;
}
int validation(const std::shared_ptr<Device>& device) {
  const auto alignment = device->minimum_buffer_offset_alignment();
  PS_CHECK(alignment <= UINT64_MAX - 4);
  auto storage = device->allocator(BufferAllocator())
                     .allocate(std::max<std::uint64_t>(256, alignment + 4))
                     .take_value();
  const Parameters parameters{1, 1, 1, 3, 7};
  for (unsigned mode = 0; mode < 8; ++mode) {
    Invocation invocation(device, {});
    const auto* api = invocation.service();
    std::uint64_t token = 0;
    PS_CHECK(api->buffer(api->context, storage.data(), storage.size(), 1,
                         &token) == 0);
    ps_gpu_buffer_binding_v11 binding{sizeof(binding), 0, token, 0,
                                      storage.size(),  1};
    auto dispatch = command(&binding, &parameters);
    if (mode == 0) {
      binding.offset = 1;
      binding.byte_size = storage.size() - 1;
    }
    if (mode == 1)
      binding.byte_size = storage.size() + 1;
    if (mode == 2)
      dispatch.group[0] = 8;
    if (mode == 3)
      dispatch.code_format = PS_GPU_CODE_MSL_V11;
    if (mode == 4)
      dispatch.source_size = 7;
    if (mode == 5) {
      PS_CHECK(api->release(api->context, token) == 0);
      std::uint64_t replacement = 0;
      PS_CHECK(api->buffer(api->context, storage.data(), storage.size(), 1,
                           &replacement) == 0);
      PS_CHECK(replacement != token);
    }
    if (mode == 6)
      dispatch.grid[0] = UINT64_MAX;
    if (mode == 7)
      dispatch.code_format = 99;
    PS_CHECK(api->execute(api->context, &dispatch, 1) != 0);
    PS_CHECK(invocation.status().code == (mode == 3
                                              ? ErrorCode::BackendUnavailable
                                              : ErrorCode::InvalidArgument));
    PS_CHECK(invocation.statistics().submissions == 0);
  }
  // A descriptor-aligned subview keeps all writes inside that view.
  Invocation invocation(device, {});
  const auto* api = invocation.service();
  const auto offset = api->minimum_buffer_offset_alignment;
  std::uint64_t token = 0;
  PS_CHECK(api->buffer(api->context, storage.data() + offset, 4, 1, &token) ==
           0);
  const ps_gpu_buffer_binding_v11 binding{sizeof(binding), 0, token, 0, 4, 1};
  auto dispatch = command(&binding, &parameters);
  PS_CHECK(api->execute(api->context, &dispatch, 1) == 0);
  for (std::size_t i = 0; i < storage.size(); i += 4) {
    std::uint32_t value = 0;
    std::memcpy(&value, storage.data() + i, 4);
    PS_CHECK(value == (i == offset ? 7 : 0));
  }
  return 0;
}
CancellationSource* active_cancellation = nullptr;
bool memory_freed = false;
void observe_free() noexcept {
  memory_freed = true;
}
int allocation_failure(const std::shared_ptr<Device>& device) {
  for (std::uint32_t point : {1, 2, 3}) {
    memory_freed = false;
    bool retired = false, ordered = false;
    struct Lease {
      bool& retired;
      bool& ordered;
      ~Lease() {
        retired = true;
        ordered = memory_freed;
      }
    };
    BufferAllocator host([&](std::uint64_t) {
      return Result<std::shared_ptr<void>>(
          std::shared_ptr<void>(new Lease{retired, ordered}));
    });
    host = host.limited_requested(4);
    execution_testing::ExecutionTestHooks hooks;
    hooks.native_allocation_failure = point;
    hooks.native_memory_freed = observe_free;
    execution_testing::install_execution_test_hooks(&hooks);
    auto failed = device->allocator(host).allocate(4);
    execution_testing::install_execution_test_hooks(nullptr);
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(retired && ordered && memory_freed);
    PS_CHECK(device->allocator(host).allocate(4).ok());
  }
  execution_testing::ExecutionTestHooks hooks;
  hooks.native_allocation_limit = 1;
  execution_testing::install_execution_test_hooks(&hooks);
  auto first = device->allocator(BufferAllocator()).allocate(4);
  const bool first_ok = first.ok();
  auto blocked = device->allocator(BufferAllocator()).allocate(4);
  first = Result<MutableBuffer>(MutableBuffer());
  auto reused = device->allocator(BufferAllocator()).allocate(4);
  execution_testing::install_execution_test_hooks(nullptr);
  PS_CHECK(first_ok && !blocked.ok() &&
           blocked.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(reused.ok());
  return 0;
}
void cancel_submitted() noexcept {
  active_cancellation->cancel();
}
int cancellation(const std::shared_ptr<Device>& device) {
  ResourceBudget root;
  auto storage = device->allocator(root.allocator()).allocate(4).take_value();
  CancellationSource source;
  Invocation invocation(device, source.token(), root.allocator());
  const auto* api = invocation.service();
  std::uint64_t token = 0;
  PS_CHECK(api->buffer(api->context, storage.data(), 4, 1, &token) == 0);
  const ps_gpu_buffer_binding_v11 binding{sizeof(binding), 0, token, 0, 4, 1};
  const Parameters parameters{1, 1, 1, 3, 7};
  auto dispatch = command(&binding, &parameters);
  execution_testing::ExecutionTestHooks hooks;
  hooks.native_submitted = cancel_submitted;
  active_cancellation = &source;
  execution_testing::install_execution_test_hooks(&hooks);
  const auto code = api->execute(api->context, &dispatch, 1);
  execution_testing::install_execution_test_hooks(nullptr);
  active_cancellation = nullptr;
  PS_CHECK(code != 0 && invocation.status().code == ErrorCode::Cancelled);
  PS_CHECK(invocation.statistics().submissions == 1);
  std::uint32_t value = 0;
  std::memcpy(&value, storage.data(), 4);
  PS_CHECK(value == 7);
  return 0;
}
int numeric_probe(const std::shared_ptr<Device>& device) {
  auto buffer = device->allocator(BufferAllocator()).allocate(28).take_value();
  std::array<std::uint32_t, 7> values{1, 0x00800000};
  std::memcpy(buffer.data(), values.data(), sizeof(values));
  Invocation invocation(device, {});
  const auto* api = invocation.service();
  std::uint64_t token = 0;
  PS_CHECK(api->buffer(api->context, buffer.data(), buffer.size(), 1, &token) ==
           0);
  const ps_gpu_buffer_binding_v11 binding{sizeof(binding), 0, token, 0,
                                          buffer.size(),   1};
  ps_gpu_dispatch_v11 dispatch{};
  dispatch.struct_size = sizeof(dispatch);
  dispatch.source =
      reinterpret_cast<const char*>(kNativeVulkanNumericProbeSpirv);
  dispatch.source_size = sizeof(kNativeVulkanNumericProbeSpirv);
  dispatch.entry = "numeric_probe";
  dispatch.entry_size = 13;
  dispatch.buffers = &binding;
  dispatch.buffer_count = 1;
  dispatch.grid[0] = dispatch.grid[1] = dispatch.grid[2] = 1;
  dispatch.code_format = PS_GPU_CODE_SPIRV_V11;
  PS_CHECK(api->execute(api->context, &dispatch, 1) == 0);
  std::memcpy(values.data(), buffer.data(), sizeof(values));
  PS_CHECK(values[2] == values[0]);
  // Record native arithmetic separately from the exact integer/copy contract.
  // These observations alone do not establish an operator numerical profile.
  std::cout << "fp32_probe_hex copy=" << std::hex << values[2]
            << " normal_half=" << values[3] << " tiny_add=" << values[4]
            << " sqrt_normal=" << values[5] << " sqrt_tiny=" << values[6]
            << std::dec << '\n';
  return 0;
}
}  // namespace
int main() {
  auto device = Device::create();
  if (!device || device->backend() != PS_GPU_BACKEND_VULKAN_V11) {
    std::cout << "Vulkan unavailable: native hardware tests skipped\n";
    return 77;
  }
  std::cout << device->identity() << '\n';
  const Parameters metadata_parameters{1, 1, 1, 3, 7};
  PS_CHECK(native_metadata_testing::check(
               command(nullptr, &metadata_parameters)) == 0);
  PS_CHECK(capacity(device) == 0);
  PS_CHECK(native_requested_quota(device) == 0);
  PS_CHECK(native_atlas_budget(device) == 0);
  PS_CHECK(allocation_failure(device) == 0);
  PS_CHECK(compute(device) == 0);
  PS_CHECK(validation(device) == 0);
  PS_CHECK(cancellation(device) == 0);
  PS_CHECK(numeric_probe(device) == 0);
  PS_CHECK(dependency_workflow(device) == 0);
  auto buffer = device->allocator(BufferAllocator()).allocate(4).take_value();
  const std::uint32_t bits = 0x1234abcd;
  std::memcpy(buffer.data(), &bits, 4);
  auto retained = std::move(buffer).freeze();
  device.reset();
  std::uint32_t actual = 0;
  std::memcpy(&actual, retained->bytes().data(), 4);
  PS_CHECK(actual == bits);
  retained.reset();
  std::cout << "PASS: native Vulkan exact capacity, GPU Int64 computation, "
               "barriers, constants budget, bounds, cancellation drain and "
               "owner lifetime\n";
  return 0;
}
