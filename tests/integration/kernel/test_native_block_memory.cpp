#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include "core/resource_observation.hpp"
#include "data/result_device_access.hpp"
#include "execution/block_memory.hpp"
#include "execution/diagnostics.hpp"
#include "execution/execution_test_hooks.hpp"
#include "execution/native_gpu.hpp"
#include "fixtures/native_vulkan_spirv.hpp"
#include "photospider/execution/execution.hpp"
#include "support/fixed_native_fixture.hpp"
#include "support/fixed_operation_fixture.hpp"
#include "support/physical_program_fixture.hpp"
#include "support/status_check.hpp"

namespace {
std::atomic<unsigned> capacity_queries{0};
unsigned failed_query = 0;
ps::ErrorCode capacity_error = ps::ErrorCode::Ok;
ps::ErrorCode fail_capacity() noexcept {
  return ++capacity_queries == failed_query ? capacity_error
                                            : ps::ErrorCode::Ok;
}
struct CapacityFault {
  ps::execution_testing::ExecutionTestHooks hooks;
  CapacityFault(ps::ErrorCode error, unsigned query) {
    capacity_queries = 0;
    failed_query = query;
    capacity_error = error;
    hooks.native_capacity_error = fail_capacity;
    ps::execution_testing::install_execution_test_hooks(&hooks);
  }
  ~CapacityFault() {
    ps::execution_testing::install_execution_test_hooks(nullptr);
  }
};
const ps::ResourceBudget* private_root = nullptr;
const ps::core_internal::PayloadObservation* private_observation = nullptr;
std::uint64_t private_capacity = 0;
bool private_freed_before_release = false;
void observe_private_free() noexcept {
  private_freed_before_release =
      private_root &&
      private_root->statistics().live[ps::ResourceKind::Device] ==
          private_capacity &&
      private_observation &&
      private_observation->live_bytes() ==
          ps::core_internal::PayloadObservation::Snapshot(private_capacity,
                                                          private_capacity);
}
int private_storage() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto probe = gpu_internal::Device::create();
  PS_CHECK(probe);
  PS_CHECK(!probe->private_allocation_capacity(0).ok());
  PS_CHECK(!probe->private_allocation_capacity(UINT64_MAX).ok());
  for (std::uint64_t bytes : {1U, 17U, 16385U}) {
    auto capacity = probe->private_allocation_capacity(bytes);
    PS_REQUIRE_OK(capacity);
    PS_CHECK(capacity.value() >= bytes);
    for (auto kind : {ResourceKind::Device, ResourceKind::Payload}) {
      ResourceLimits limits;
      limits.capacity[kind] = capacity.value() - 1;
      auto root = std::make_shared<ResourceBudget>(limits);
      auto device = gpu_internal::Device::create(root);
      PS_CHECK(device);
      auto refused = device->allocate_private(
          bytes, [root](std::uint64_t size) -> Result<std::shared_ptr<void>> {
            ResourceCapacity charge;
            charge[ResourceKind::Device] = size;
            charge[ResourceKind::Payload] = size;
            auto lease = root->reserve(charge);
            if (!lease.ok())
              return Result<std::shared_ptr<void>>(lease.status());
            return Result<std::shared_ptr<void>>(
                std::make_shared<ResourceLease>(lease.take_value()));
          });
      PS_CHECK(!refused.ok() &&
               refused.status().code == ErrorCode::ResourceExhausted);
      for (auto live : root->statistics().live.values)
        PS_CHECK(live == 0);
    }
    auto root = std::make_shared<ResourceBudget>();
    auto seen = std::make_shared<core_internal::PayloadObservation>();
    core_internal::ResourcePayloadScope scope(*root, {seen});
    auto device = gpu_internal::Device::create(root);
    PS_CHECK(device);
    const auto account = device->metadata();
    const auto reserve =
        [root, account](std::uint64_t size) -> Result<std::shared_ptr<void>> {
      ResourceCapacity charge;
      charge[ResourceKind::Device] = size;
      charge[ResourceKind::Payload] = size;
      charge[ResourceKind::Entries] = 1;
      auto lease = root->reserve(charge);
      if (!lease.ok())
        return Result<std::shared_ptr<void>>(lease.status());
      return Result<std::shared_ptr<void>>(std::allocate_shared<ResourceLease>(
          gpu_internal::NativeAllocator<ResourceLease>(account),
          lease.take_value()));
    };
    const auto commit = [](const std::shared_ptr<void>& lease,
                           std::uint64_t allocated) {
      core_internal::ResourcePayloadAccess::commit(
          *static_cast<ResourceLease*>(lease.get()), allocated);
    };
    for (bool allocation : {false, true}) {
      auto failed = device->allocate_private(
          bytes, reserve, [&](const auto& lease, auto allocated) {
            commit(lease, allocated);
            if (allocation)
              throw std::bad_alloc();
            throw std::runtime_error("private commit observer");
          });
      PS_CHECK(!failed.ok() && failed.status().code ==
                                   (allocation ? ErrorCode::ResourceExhausted
                                               : ErrorCode::OperationFailed));
      PS_CHECK(seen->live_bytes() ==
               core_internal::PayloadObservation::Snapshot(0, 0));
      for (auto live : root->statistics().live.values)
        PS_CHECK(live == 0);
    }
    if (device->backend() == PS_NATIVE_VULKAN_V2) {
      for (auto point : {1U, 3U}) {
        execution_testing::ExecutionTestHooks hooks;
        hooks.native_allocation_failure = point;
        hooks.native_memory_freed = observe_private_free;
        private_root = root.get();
        private_observation = seen.get();
        private_capacity = capacity.value();
        private_freed_before_release = false;
        execution_testing::install_execution_test_hooks(&hooks);
        auto failed = device->allocate_private(bytes, reserve, commit);
        execution_testing::install_execution_test_hooks(nullptr);
        private_root = nullptr;
        private_observation = nullptr;
        PS_CHECK(!failed.ok() &&
                 failed.status().code == ErrorCode::ResourceExhausted &&
                 private_freed_before_release);
        for (auto live : root->statistics().live.values)
          PS_CHECK(live == 0);
      }
    }
    auto first = device->allocate_private(bytes, reserve, commit);
    auto second = device->allocate_private(bytes, reserve, commit);
    PS_REQUIRE_OK(first);
    PS_REQUIRE_OK(second);
    auto a = first.take_value(), b = second.take_value();
    PS_CHECK(a.get() != b.get() && a->size() == bytes &&
             a->capacity() == capacity.value() && device->owns(*a) &&
             !probe->owns(*a));
    PS_CHECK(root->statistics().live[ResourceKind::Device] ==
                 2 * capacity.value() &&
             root->statistics().live[ResourceKind::Payload] ==
                 2 * capacity.value() &&
             root->statistics().live[ResourceKind::Shared] == 0);
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(2 * capacity.value(),
                                                         2 * capacity.value()));
    a.reset();
    std::weak_ptr<gpu_internal::Device> weak = device;
    device.reset();
    PS_CHECK(weak.expired() && b->size() == bytes);
    PS_CHECK(root->statistics().live[ResourceKind::Device] == capacity.value());
    b.reset();
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(0, 0));
    for (auto live : root->statistics().live.values)
      PS_CHECK(live == 0);
  }
  return 0;
}
int private_kernels() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto device = gpu_internal::Device::create();
  PS_CHECK(device);
  constexpr std::uint64_t bytes = 64;
  auto allocate = device->allocator(BufferAllocator{}).allocate(bytes);
  PS_REQUIRE_OK(allocate);
  auto input = allocate.take_value();
  for (std::uint32_t i = 0; i < bytes / 4; ++i)
    std::memcpy(input.data() + 4 * i, &i, 4);
  allocate = device->allocator(BufferAllocator{}).allocate(bytes);
  PS_REQUIRE_OK(allocate);
  auto output = allocate.take_value();
  auto allocation = device->allocate_private(bytes, {});
  PS_REQUIRE_OK(allocation);
  auto first = allocation.take_value();
  allocation = device->allocate_private(bytes, {});
  PS_REQUIRE_OK(allocation);
  auto second = allocation.take_value();
  auto other = gpu_internal::Device::create();
  PS_CHECK(other && !other->private_view(first->handle(), bytes, false).ok());
  PS_CHECK(!device->private_view(input.data(), bytes, false).ok());
  PS_CHECK(!device->private_view(first->handle(), bytes + 1, false).ok());
  PS_CHECK(!device->private_view(first->handle(), 0, false).ok());
  const std::uint32_t parameters[] = {16, 1, 1, 2, 1};
  const char shader[] =
      "#include <metal_stdlib>\nusing namespace metal;\n"
      "struct P {uint w,h,d,m,a;};\n"
      "kernel void transform(device const uint* a [[buffer(0)]],"
      "device uint* b [[buffer(1)]],constant P& p [[buffer(2)]],"
      "uint i [[thread_position_in_grid]]) {"
      "if(i<p.w) b[i]=a[i]*p.m+p.a;}\n";
  ps_native_binding_v2 bindings[] = {{0, PS_NATIVE_INPUT_V2, 0, 0, bytes},
                                     {1, PS_NATIVE_OUTPUT_V2, 0, 0, bytes}};
  ps_native_program_v2 program{};
  program.struct_size = sizeof(program);
  program.source = {shader, sizeof(shader) - 1};
  program.entry = {"transform", 9};
  if (device->backend() == PS_NATIVE_VULKAN_V2) {
    program.code_format = PS_NATIVE_SPIRV_V2;
    program.source = {reinterpret_cast<const char*>(kNativeVulkanSpirv),
                      sizeof(kNativeVulkanSpirv)};
  }
  program.bindings = bindings;
  program.binding_count = 2;
  program.constants = parameters;
  program.constant_bytes = sizeof(parameters);
  program.constant_index = 2;
  program.grid[0] = 16;
  program.grid[1] = program.grid[2] = 1;
  ps_kernel_view_spec_v8 read{}, write{};
  read.bytes = write.bytes = bytes;
  read.layout.rank = 1;
  read.layout.element_type = static_cast<std::uint32_t>(ElementType::UInt8);
  read.layout.shape[0] = bytes;
  read.layout.strides[0] = 1;
  write.layout = read.layout;
  for (unsigned step = 0; step < 3; ++step) {
    read.domain =
        step == 0 ? PS_KERNEL_DOMAIN_SHARED_V8 : PS_KERNEL_DOMAIN_DEVICE_V8;
    write.domain =
        step == 2 ? PS_KERNEL_DOMAIN_SHARED_V8 : PS_KERNEL_DOMAIN_DEVICE_V8;
    ps_kernel_preparation_v8 query{};
    query.struct_size = sizeof(query);
    query.placement.backend = PS_BACKEND_GPU_V8;
    query.placement.domain = write.domain;
    query.reads = &read;
    query.read_count = 1;
    query.writes = &write;
    query.write_count = 1;
    test::NativeKernel kernel;
    PS_REQUIRE_OK(kernel.prepare(device, program, query));
    ps_kernel_const_view_v8 in{};
    in.allocation = step == 0 ? static_cast<const void*>(input.data())
                              : (step == 1 ? first : second)->handle();
    in.bytes = bytes;
    in.domain = read.domain;
    in.layout = &read.layout;
    ps_kernel_mutable_view_v8 out{};
    out.allocation = step == 2 ? static_cast<void*>(output.data())
                               : (step == 0 ? first : second)->handle();
    out.bytes = bytes;
    out.domain = write.domain;
    out.layout = &write.layout;
    const std::uint64_t identity = step + 1;
    ps_kernel_invocation_v8 invocation{};
    invocation.inputs = &in;
    invocation.input_count = 1;
    invocation.input_object_ids = &identity;
    invocation.outputs = &out;
    invocation.output_count = 1;
    const auto original_allocation = in.allocation;
    if (step == 0) {
      in.domain = PS_KERNEL_DOMAIN_DEVICE_V8;
      in.allocation = first->handle();
    } else {
      in.domain = PS_KERNEL_DOMAIN_SHARED_V8;
      in.allocation = input.data();
    }
    PS_CHECK(kernel.run(invocation).code == ErrorCode::InvalidArgument);
    in.domain = read.domain;
    in.allocation = original_allocation;
    PS_REQUIRE_OK(kernel.run(invocation));
    if (step < 2) {
      auto owner = step == 0 ? first : second;
      owner->seal();
      PS_CHECK(device->private_view(owner->handle(), bytes, false).ok());
      PS_CHECK(!device->private_view(owner->handle(), bytes, true).ok());
      PS_CHECK(kernel.run(invocation).code == ErrorCode::InvalidArgument);
    }
  }
  for (std::uint32_t i = 0; i < bytes / 4; ++i) {
    std::uint32_t value = 0;
    std::memcpy(&value, output.data() + 4 * i, 4);
    PS_CHECK(value == 8 * i + 7);
  }
  const auto handle = first->handle();
  first.reset();
  PS_CHECK(!device->private_view(handle, bytes, false).ok());
  return 0;
}
int private_block() {
  using namespace ps;                      // NOLINT(build/namespaces)
  using namespace ps::physical;            // NOLINT(build/namespaces)
  using namespace ps::execution_internal;  // NOLINT(build/namespaces)
  auto program = test::physical_fixture::program(2);
  const auto private_tile = test::physical_fixture::output(
      &program, test::physical_fixture::task(&program, 0));
  const auto shared_tile = test::physical_fixture::output(
      &program, test::physical_fixture::task(&program, 1));
  program.placements[0] = {physical::Backend::Gpu, Domain::Device, "native",
                           ""};
  program.placements.push_back(
      {physical::Backend::Gpu, Domain::Shared, "native", ""});
  program.tasks[1].placement = 1;
  program.tiles[private_tile].domain = Domain::Device;
  program.tiles[shared_tile].domain = Domain::Shared;
  program.tasks[0].scratch = {11, 17, 19};
  program.return_schemas = {0};
  program.blocks[0].return_values = {program.tiles[private_tile].value};
  PS_REQUIRE_OK(finalize_program(&program));
  auto probe = gpu_internal::Device::create();
  PS_CHECK(probe);
  auto resolved = prepare_native_allocations(program, probe);
  PS_REQUIRE_OK(resolved);
  const auto& sizes = resolved.value();
  const auto private_bytes =
      sizes.tiles[private_tile] + 2 * sizes.scratch[0][1][1];
  const auto shared_bytes =
      sizes.tiles[shared_tile] + 2 * sizes.scratch[0][1][2];
  const auto payload = private_bytes + shared_bytes + 22;
  for (auto kind :
       {ResourceKind::Device, ResourceKind::Shared, ResourceKind::Payload}) {
    ResourceLimits limits;
    limits.capacity[kind] =
        (kind == ResourceKind::Payload  ? payload
         : kind == ResourceKind::Shared ? shared_bytes
                                        : private_bytes + shared_bytes) -
        1;
    auto root = std::make_shared<ResourceBudget>(limits);
    auto device = gpu_internal::Device::create(root);
    PS_CHECK(device);
    auto prepared = prepare_native_allocations(program, device);
    PS_REQUIRE_OK(prepared);
    const auto baseline = root->statistics().live;
    auto refused =
        BlockMemory::allocate(program, 0, 1, *root, 2, &prepared.value());
    PS_CHECK(!refused.ok() &&
             refused.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(root->statistics().live.values == baseline.values);
  }
  ResourceLimits limits;
  limits.capacity[ResourceKind::Payload] = payload;
  limits.capacity[ResourceKind::Device] = private_bytes + shared_bytes;
  limits.capacity[ResourceKind::Shared] = shared_bytes;
  auto root = std::make_shared<ResourceBudget>(limits);
  auto seen = std::make_shared<core_internal::PayloadObservation>();
  core_internal::ResourcePayloadScope scope(*root, {seen});
  auto device = gpu_internal::Device::create(root);
  PS_CHECK(device);
  auto prepared = prepare_native_allocations(program, device);
  PS_REQUIRE_OK(prepared);
  auto native = prepared.take_value();
  for (bool scratch : {false, true}) {
    auto changed_preparation = prepare_native_allocations(program, device);
    PS_REQUIRE_OK(changed_preparation);
    auto changed = changed_preparation.take_value();
    if (scratch)
      --changed.scratch[0][1][1];
    else
      --changed.tiles[private_tile];
    auto failed = BlockMemory::allocate(program, 0, 1, *root, 2, &changed);
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::BackendUnavailable);
    PS_CHECK(root->statistics().live[ResourceKind::Payload] == 0);
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(0, 0));
    device->collect_expired_allocations();
  }
  auto observed = std::make_shared<MemoryObservation>();
  std::uint64_t allocations = 0;
  std::shared_ptr<const gpu_internal::DeviceStorage> escaped;
  {
    auto allocated = BlockMemory::allocate(program, 0, 1, *root, 2, &native,
                                           nullptr, &allocations, observed);
    PS_REQUIRE_OK(allocated);
    auto block = allocated.take_value();
    const std::array<std::uint64_t, 3> expected{22, private_bytes,
                                                shared_bytes};
    PS_CHECK(allocations == 2);
    PS_CHECK(observed->snapshot().reserved_bytes_by_domain == expected &&
             observed->snapshot().live_bytes_by_domain == expected);
    PS_CHECK(root->statistics().live[ResourceKind::Payload] == payload);
    PS_CHECK(root->statistics().live[ResourceKind::Device] ==
             private_bytes + shared_bytes);
    PS_CHECK(root->statistics().live[ResourceKind::Shared] == shared_bytes);
    PS_CHECK(seen->live_bytes() ==
             core_internal::PayloadObservation::Snapshot(payload, payload));
    PS_CHECK(!block.device_scratch(0));
    PS_CHECK(block.device_scratch(1) && block.device_scratch(2));
    PS_CHECK(block.device_scratch(1)->handle() !=
             block.device_scratch(2)->handle());
    for (unsigned lane : {1U, 2U}) {
      PS_CHECK(block.scratch(lane).size() == 11);
      PS_CHECK(block.scratch(lane, Domain::Shared).size() == 19);
      auto owner = block.device_scratch(lane);
      PS_CHECK(owner->size() == 17 &&
               device->private_view(owner->handle(), 17, true).ok());
    }
    PS_CHECK(!block.tile(private_tile).data());
    PS_CHECK(!block.device_tile(shared_tile));
    auto envelope = block.unpublished_device(private_tile);
    PS_CHECK(envelope.get() == block.device_tile(private_tile));
    escaped = block.publish_device(private_tile);
    PS_CHECK(!block.device_tile(private_tile));
    PS_CHECK(!device->private_view(escaped->handle(), 4, true).ok());
    block.discard(shared_tile);
    PS_CHECK(observed->snapshot().live_bytes_by_domain[2] ==
             shared_bytes - sizes.tiles[shared_tile]);
    PS_CHECK(observed->snapshot().reserved_bytes_by_domain[2] == shared_bytes);
    // Releasing an allocation reduces committed bytes, but keeps the full
    // block reservation protected until the block boundary.
    PS_CHECK(root->statistics().live[ResourceKind::Payload] == payload);
    block = {};
    PS_CHECK(root->statistics().live[ResourceKind::Payload] ==
             sizes.tiles[private_tile]);
    PS_CHECK(root->statistics().live[ResourceKind::Shared] == 0);
  }
  native = {};
  device.reset();
  PS_CHECK(escaped->size() == 4);
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(
               sizes.tiles[private_tile], sizes.tiles[private_tile]));
  PS_CHECK(observed->snapshot().live_bytes_by_domain[0] == 0 &&
           observed->snapshot().live_bytes_by_domain[2] == 0 &&
           observed->snapshot().live_bytes_by_domain[1] ==
               sizes.tiles[private_tile]);
  escaped.reset();
  for (auto bytes : observed->snapshot().live_bytes_by_domain)
    PS_CHECK(bytes == 0);
  for (auto bytes : observed->snapshot().reserved_bytes_by_domain)
    PS_CHECK(bytes == 0);
  PS_CHECK(seen->live_bytes() ==
           core_internal::PayloadObservation::Snapshot(0, 0));
  for (auto live : root->statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int private_results() {
  using namespace ps;  // NOLINT(build/namespaces)
  using data_internal::ResultDeviceAccess;
  auto device = gpu_internal::Device::create();
  PS_CHECK(device);
  auto allocated = device->allocate_private(2, {});
  PS_REQUIRE_OK(allocated);
  auto storage = allocated.take_value();
  const auto capacity = storage->capacity();
  ResourceLimits limits;
  limits.capacity[ResourceKind::Referenced] = capacity;
  ResourceBudget root(limits);
  data_internal::DeviceTensorSpan retained;
  WeakResultRef weak;
  {
    auto started = ResultBuilder::start(root, test::fixed_fixture::schema(),
                                        "mixed-device-result");
    PS_REQUIRE_OK(started);
    auto builder = started.take_value();
    const std::uint8_t host[] = {7, 9};
    PS_REQUIRE_OK(builder.publish_tensor(0, Region({{0, 2}}),
                                         ByteView(host, sizeof(host))));
    auto old = builder.reference().capture();
    PS_REQUIRE_OK(old);
    auto old_facts = old.value().descriptor(false);
    PS_REQUIRE_OK(old_facts);
    PS_REQUIRE_OK(ResultDeviceAccess::publish(builder, 0, Region({{2, 2}}),
                                              {0, {1}, {2}}, storage));
    // Envelopes are built at admission. Publication must not revoke the
    // producer's write access before its actual completion.
    PS_CHECK(device->private_view(storage->handle(), 2, true).ok());
    storage->seal();
    auto sealed = builder.seal();
    PS_REQUIRE_OK(sealed);
    auto result = sealed.take_value();
    weak = result.weak();
    auto facts = result.descriptor();
    PS_REQUIRE_OK(facts);
    PS_CHECK(facts.value().tensor_coverage(0).contains({3}));
    std::uint8_t value = 0;
    PS_REQUIRE_OK(result.read_tensor(facts.value(), 0, {1}, &value, 1));
    PS_CHECK(value == 9);
    PS_CHECK(result.read_tensor(facts.value(), 0, {2}, &value, 1).code ==
             ErrorCode::BackendUnavailable);
    PS_CHECK(result.acquire_tensor(facts.value(), 0, Region::whole({4}))
                 .status()
                 .code == ErrorCode::BackendUnavailable);
    auto denied = ResultDeviceAccess::acquire(old.value(), old_facts.value(), 0,
                                              Region({{2, 2}}));
    PS_CHECK(!denied.ok() && denied.status().code == ErrorCode::NotFound);
    auto newer = ResultDeviceAccess::acquire(old.value(), facts.value(), 0,
                                             Region({{2, 2}}));
    PS_CHECK(!newer.ok() && newer.status().code == ErrorCode::Stale);
    auto view =
        ResultDeviceAccess::acquire(result, facts.value(), 0, Region({{3, 1}}));
    PS_REQUIRE_OK(view);
    retained = view.take_value();
    const std::uint64_t at = 3;
    PS_CHECK(retained.storage().get() == storage.get() &&
             retained.byte_address(&at, 1).value() == 1);
    PS_CHECK(root.statistics().live[ResourceKind::Referenced] == capacity);
    auto duplicate = ResultDeviceAccess::reference(root, storage);
    PS_REQUIRE_OK(duplicate);
    PS_CHECK(duplicate.value().get() == storage.get());
    PS_CHECK(root.statistics().live[ResourceKind::Referenced] == capacity);
    auto foreign = ResultBuilder::start(root, test::fixed_fixture::schema(),
                                        "foreign-descriptor");
    PS_REQUIRE_OK(foreign);
    auto other = foreign.value().reference().descriptor(false);
    PS_REQUIRE_OK(other);
    PS_CHECK(
        ResultDeviceAccess::acquire(result, other.value(), 0, Region({{2, 2}}))
            .status()
            .code == ErrorCode::Stale);
  }
  PS_CHECK(!weak.lock().valid());
  storage.reset();
  device.reset();
  PS_CHECK(retained.storage()->size() == 2);
  PS_CHECK(root.statistics().live[ResourceKind::Referenced] == capacity);
  retained = {};
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int shared_execution() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  auto produce = test::fixed_fixture::operation("shared", 0);
  produce.lower = [](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    physical::Placement placement;
    placement.domain = physical::Domain::Shared;
    auto task =
        builder.add_task("increment", "increment-v1", placement, {8, 0, 16});
    if (!task.ok())
      return Result<OperationProgram>(task.status());
    auto status = builder.write_output(task.value(), 0, 0, 0);
    if (!status.ok())
      return Result<OperationProgram>(status);
    auto result = builder.finish();
    if (!result.ok())
      return result;
    auto graph = result.take_value();
    graph.program.calls[0].returns = graph.outputs;
    status = physical::finalize_program(&graph.program);
    return status.ok() ? Result<OperationProgram>(std::move(graph))
                       : Result<OperationProgram>(status);
  };
  const auto prepare = produce.prepare_kernel;
  produce.prepare_kernel = [prepare](const auto& kernel, const auto& placement,
                                     const auto& query, const auto& resources,
                                     const ps_native_gpu_service_v2* service) {
    auto result = prepare(kernel, placement, query, resources, service);
    if (!result.ok())
      return result;
    auto raw = result.take_value();
    raw.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                        ps_kernel_status_v8* status) {
      if (invocation->input_count || invocation->output_count != 1 ||
          invocation->outputs[0].domain != PS_KERNEL_DOMAIN_SHARED_V8 ||
          invocation->scratch_count != 2 ||
          invocation->scratch[0].domain != PS_KERNEL_DOMAIN_HOST_V8 ||
          invocation->scratch[0].bytes != 8 ||
          invocation->scratch[1].domain != PS_KERNEL_DOMAIN_SHARED_V8 ||
          invocation->scratch[1].bytes != 16) {
        *status =
            kernel_failure(ErrorCode::InvalidArgument, "Shared invocation");
        return;
      }
      auto* output =
          static_cast<std::uint8_t*>(invocation->outputs[0].allocation);
      for (unsigned i = 0; i < 4; ++i)
        output[i] = i + 10;
      static_cast<std::uint8_t*>(invocation->scratch[0].allocation)[7] = 1;
      static_cast<std::uint8_t*>(invocation->scratch[1].allocation)[15] = 2;
      *status = {};
    };
    return Result<ps_prepared_kernel_v8>(raw);
  };
  auto copy = test::fixed_fixture::operation("download", 1);
  copy.prepare_kernel = {};
  copy.lower = [](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    auto task = builder.add_copy();
    if (!task.ok())
      return Result<OperationProgram>(task.status());
    auto status =
        builder.read_input(task.value(), 0, 0, 0, physical::Domain::Shared);
    if (!status.ok())
      return Result<OperationProgram>(status);
    status = builder.write_output(task.value(), 0, 0, 0);
    return status.ok() ? builder.finish() : Result<OperationProgram>(status);
  };
  PS_REQUIRE_OK(registry->register_operation(std::move(produce)));
  PS_REQUIRE_OK(registry->register_operation(std::move(copy)));
  auto external_copy = test::fixed_fixture::operation("external", 1);
  external_copy.prepare_kernel = {};
  external_copy.lower = [](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    auto task = builder.add_copy();
    if (!task.ok())
      return Result<OperationProgram>(task.status());
    auto status = builder.read_input(task.value(), 0, 0, 0);
    if (!status.ok())
      return Result<OperationProgram>(status);
    status = builder.write_output(task.value(), 0, 0, 0);
    return status.ok() ? builder.finish() : Result<OperationProgram>(status);
  };
  PS_REQUIRE_OK(registry->register_operation(std::move(external_copy)));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "shared", {}, {}},
                    {2, "download", {WorkflowNodeOutput{1, "out0"}}, {}}};
  document.outputs = {{"result", 2, "out0"}};
  GraphContext graph(document);
  PlanningOptions options;
  options.tile_extent = 4;
  auto compiled = Compiler(registry).compile(graph, options);
  PS_REQUIRE_OK(compiled);
  ResourceBudget budget;
  ResultRef exported;
  {
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    ExecutionContext context(registry, config);
    budget = context.resource_budget().take_value();
    {
      auto warm = context.prepare(compiled.value().plan.value());
      PS_REQUIRE_OK(warm);
    }
    const auto baseline = budget.statistics().live;
    // Output and scratch capacity queries both precede kernel preparation.
    for (auto error :
         {ErrorCode::BackendUnavailable, ErrorCode::ResourceExhausted,
          ErrorCode::OperationFailed}) {
      for (unsigned query : {1U, 2U}) {
        const auto kernels = test::fixed_fixture::preparations.load();
        CapacityFault fault(error, query);
        auto failed = context.prepare(compiled.value().plan.value());
        PS_CHECK(!failed.ok() && failed.status().code == error &&
                 failed.status().message == "injected native capacity query");
        PS_CHECK(capacity_queries == query &&
                 test::fixed_fixture::preparations == kernels);
        PS_CHECK(budget.statistics().live.values == baseline.values);
      }
    }
    auto prepared = context.prepare(compiled.value().plan.value());
    PS_REQUIRE_OK(prepared);
    ExecutionOptions run_options;
    run_options.on_call_retired = [&](const CallRecord& record) {
      if (record.source_call == 1)
        exported = record.values.begin()->second;
    };
    auto run = context.submit(prepared.value(), {}, {}, run_options);
    PS_REQUIRE_OK(run);
    auto result = run.value().join();
    PS_REQUIRE_OK(result);
    const auto output = result.value().results.at("result");
    const auto facts = output.descriptor();
    PS_REQUIRE_OK(facts);
    for (unsigned i = 0; i < 4; ++i) {
      std::uint8_t sample = 0;
      PS_REQUIRE_OK(output.read_tensor(facts.value(), 0, {i}, &sample, 1));
      PS_CHECK(sample == i + 10);
    }
    PS_CHECK(result.value().diagnostics.completed_tasks == 2);

    WorkflowDocument external;
    WorkflowInputDeclaration input;
    input.id = 1;
    input.name = "source";
    input.result_schema =
        std::make_shared<const SchemaTemplate>(test::fixed_fixture::schema());
    external.inputs = {input};
    external.nodes = {{1, "external", {WorkflowInputReference{1}}, {}}};
    external.outputs = {{"result", 1, "out0"}};
    GraphContext external_graph(external);
    // A named planner may explicitly require Shared external input storage.
    // The default static planner's external binding contract remains Host.
    auto planners = std::make_shared<PlannerRegistry>();
    auto shared_input = *make_default_planner_registry()->find("static-tiles");
    shared_input.name = "shared-input-test";
    const auto base = shared_input.plan;
    shared_input.plan = [base](const auto& ir, PlanBuilder& builder,
                               const auto& options) {
      auto status = base(ir, builder, options);
      if (!status.ok())
        return status;
      auto& program = builder.program();
      for (auto input : program.inputs)
        for (auto tile : program.values[input].tiles)
          program.tiles[tile].domain = physical::Domain::Shared;
      return physical::finalize_program(&program);
    };
    PS_REQUIRE_OK(planners->add(std::move(shared_input)));
    planners->freeze();
    auto shared_options = options;
    shared_options.planner = "shared-input-test";
    auto external_plan = Compiler(registry, {}, planners)
                             .compile(external_graph, shared_options);
    PS_REQUIRE_OK(external_plan);
    auto local = context.prepare(external_plan.value().plan.value());
    PS_REQUIRE_OK(local);
    auto reused = context.submit(local.value(), {{{"source", exported}}});
    PS_REQUIRE_OK(reused);
    auto reused_result = reused.value().join();
    PS_REQUIRE_OK(reused_result);
    auto reused_output = reused_result.value().results.at("result");
    std::uint8_t sample = 0;
    PS_REQUIRE_OK(reused_output.read_tensor(reused_output.descriptor().value(),
                                            0, {3}, &sample, 1));
    PS_CHECK(sample == 13);
    auto host = ResultBuilder::start(budget, test::fixed_fixture::schema(),
                                     "host.binding", {}, 4);
    PS_REQUIRE_OK(host);
    auto builder = host.take_value();
    const std::uint8_t bytes[4] = {10, 11, 12, 13};
    PS_REQUIRE_OK(builder.publish_tensor(0, Region::whole({4}), {bytes, 4}));
    auto host_result = builder.seal();
    PS_REQUIRE_OK(host_result);
    PS_CHECK(context.submit(local.value(), {{{"source", host_result.value()}}})
                 .status()
                 .code == ErrorCode::TypeMismatch);
    ExecutionContext other(registry, config);
    auto foreign = other.prepare(external_plan.value().plan.value());
    PS_REQUIRE_OK(foreign);
    PS_CHECK(
        other.submit(foreign.value(), {{{"source", exported}}}).status().code ==
        ErrorCode::TypeMismatch);
  }
  const auto facts = exported.descriptor();
  PS_REQUIRE_OK(facts);
  std::uint8_t sample = 0;
  PS_REQUIRE_OK(exported.read_tensor(facts.value(), 0, {3}, &sample, 1));
  PS_CHECK(sample == 13 && budget.statistics().live[ResourceKind::Shared] >= 4);
  exported = {};
  PS_CHECK(budget.statistics().live[ResourceKind::Shared] == 0);
  return 0;
}
}  // namespace

int main() {
  using namespace ps;                      // NOLINT(build/namespaces)
  using namespace ps::physical;            // NOLINT(build/namespaces)
  using namespace ps::execution_internal;  // NOLINT(build/namespaces)
  auto device = gpu_internal::Device::create();
  if (!device) {
    std::cout << "Native device unavailable\n";
    return 77;
  }
  std::cout << device->identity() << '\n';
  PS_CHECK(private_storage() == 0);
  PS_CHECK(private_kernels() == 0);
  PS_CHECK(private_block() == 0);
  PS_CHECK(private_results() == 0);
  auto program = test::physical_fixture::program();
  const auto tile = test::physical_fixture::output(
      &program, test::physical_fixture::task(&program, 0));
  auto schema = std::make_shared<SchemaTemplate>(*program.schemas[0]);
  schema->tensors[0].descriptor.shape[0] = 16385;
  program.schemas[0] = std::move(schema);
  program.tile_extent = 32768;
  program.layouts[0].shape[0] = 16385;
  program.tiles[tile].bytes = 16385;
  program.tiles[tile].domain = Domain::Shared;
  program.placements[0].backend = physical::Backend::Gpu;
  program.placements[0].domain = Domain::Shared;
  program.tasks[0].scratch = {17, 0, 8193};
  program.return_schemas = {0};
  program.blocks[0].return_values = {program.tiles[tile].value};
  PS_REQUIRE_OK(finalize_program(&program));
  auto prepared = prepare_native_allocations(program, device);
  PS_REQUIRE_OK(prepared);
  auto native = prepared.take_value();
  auto tile_capacity = device->allocation_capacity(16385);
  auto scratch_capacity = device->allocation_capacity(8193);
  PS_REQUIRE_OK(tile_capacity);
  PS_REQUIRE_OK(scratch_capacity);
  PS_CHECK(native.tiles[tile] == tile_capacity.value() &&
           native.scratch[0][1][2] == scratch_capacity.value());
  const auto shared = tile_capacity.value() + 2 * scratch_capacity.value();
  const auto payload = shared + 2 * 17;
  for (auto kind :
       {ResourceKind::Device, ResourceKind::Shared, ResourceKind::Payload}) {
    ResourceLimits limits;
    limits.capacity[kind] =
        (kind == ResourceKind::Payload ? payload : shared) - 1;
    ResourceBudget small(limits);
    auto refused = BlockMemory::allocate(program, 0, 1, small, 2, &native);
    PS_CHECK(!refused.ok() &&
             refused.status().code == ErrorCode::ResourceExhausted);
    for (auto value : small.statistics().live.values)
      PS_CHECK(value == 0);
  }
  ResourceLimits limits;
  limits.capacity[ResourceKind::Payload] = payload;
  limits.capacity[ResourceKind::Device] = shared;
  limits.capacity[ResourceKind::Shared] = shared;
  ResourceBudget root(limits);
  // The prepared capacity is a per-allocation contract, not a pool fallback.
  for (bool scratch : {false, true}) {
    auto changed_preparation = prepare_native_allocations(program, device);
    PS_REQUIRE_OK(changed_preparation);
    auto changed = changed_preparation.take_value();
    if (scratch)
      ++changed.scratch[0][1][2];
    else
      ++changed.tiles[tile];
    ResourceBudget mismatch_budget;
    auto mismatch =
        BlockMemory::allocate(program, 0, 1, mismatch_budget, 2, &changed);
    PS_CHECK(!mismatch.ok() &&
             mismatch.status().code == ErrorCode::BackendUnavailable);
    // Scratch rejection occurs after a real Shared output was allocated.
    for (auto value : mismatch_budget.statistics().live.values)
      PS_CHECK(value == 0);
  }
  std::shared_ptr<const CpuStorage> escaped;
  {
    auto allocation = BlockMemory::allocate(program, 0, 1, root, 2, &native);
    PS_REQUIRE_OK(allocation);
    auto block = allocation.take_value();
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == payload);
    PS_CHECK(root.statistics().live[ResourceKind::Device] == shared);
    PS_CHECK(root.statistics().live[ResourceKind::Shared] == shared);
    PS_CHECK(block.scratch(0).size() == 0);
    for (unsigned lane = 1; lane < 3; ++lane) {
      PS_CHECK(block.scratch(lane).size() == 17);
      PS_CHECK(block.scratch(lane, Domain::Shared).size() == 8193);
      auto view =
          device->view(block.scratch(lane, Domain::Shared).data(), 8193, true);
      PS_REQUIRE_OK(view);
      PS_CHECK(device->owns(*view.value().storage));
    }
    PS_CHECK(block.scratch(1, Domain::Shared).data() !=
             block.scratch(2, Domain::Shared).data());
    PS_CHECK(block.tile(tile).size() == 16385);
    for (unsigned i = 0; i < 16385; ++i)
      PS_CHECK(block.tile(tile).data()[i] == 0);
    block.tile(tile).data()[16384] = 71;
    escaped = block.publish(tile);
    PS_CHECK(device->owns(*escaped) && root.allocator().owns(*escaped));
    PS_CHECK(escaped->capacity() == tile_capacity.value());
    auto writable = device->view(escaped->bytes().data(), 16385, true);
    PS_CHECK(!writable.ok());
    // The block continues protecting the entire envelope after publication.
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == payload);
  }
  native = {};
  device.reset();
  PS_CHECK(escaped->bytes()[16384] == 71);
  for (auto kind :
       {ResourceKind::Payload, ResourceKind::Device, ResourceKind::Shared})
    PS_CHECK(root.statistics().live[kind] == tile_capacity.value());
  escaped.reset();
  for (auto value : root.statistics().live.values)
    PS_CHECK(value == 0);
  PS_CHECK(shared_execution() == 0);
  return 0;
}
