#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct State {
  explicit State(unsigned mode) : mode(mode) {}
  unsigned mode;
  bool supplied = false;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!supplied) {
      supplied = true;
      return Result<DependencyPoll>(
          DependencyNeedBatch{{{{0}, {{0, 1, phase.query.outputs, {}}}}}, {}});
    }
    if (mode == 3) {
      static_cast<void>(phase.gpu_buffer(nullptr, 0, false));
    } else if (mode == 4) {
      static_cast<void>(phase.gpu_execute(nullptr, 0));
    } else {
      auto atlas = phase.atlas(mode == 1 ? 1 : 0);
      if (atlas.ok()) {
        auto again = phase.atlas(0);
        if (!again.ok() ||
            again.value().payload.storage() != atlas.value().payload.storage())
          return Result<DependencyPoll>(
              Status{ErrorCode::Internal, "atlas repeat differs"});
      }
      // Deliberately ignore service failures to verify host fencing.
    }
    auto writer =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator)
            .take_value();
    const float value = 7;
    std::memcpy(writer.data(), &value, 4);
    auto output = std::move(writer).publish().take_value();
    return Result<DependencyPoll>(
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs, {output})
            .take_value());
  }
};
int run(unsigned mode, Backend backend, ErrorCode expected,
        std::uint64_t work = 1048576) {
  OperationRegistry registry;
  OperationDefinition definition;
  definition.key = "test.native.services";
  auto& traits = definition.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.output_element_type = ElementType::Float32;
  traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.region_rule = OperationRegionRule::Dependency;
  traits.dependency_version = 1;
  traits.supports_gpu = true;
  traits.continuation_bytes = sizeof(State);
  traits.maximum_dependency_stages = 2;
  definition.start_dependency = [mode](const DependencyQuery&,
                                       const BufferAllocator& allocator) {
    return DependencyContinuation::make<State>(allocator, mode);
  };
  PS_CHECK(registry.register_operation(definition).ok());
  const ValueDescriptor descriptor{ElementType::Float32, {1}};
  auto writer =
      MutableValue::allocate(descriptor, Region::whole({1}), BufferAllocator{})
          .take_value();
  std::memset(writer.data(), 0, 4);
  auto input = std::move(writer).publish().take_value();
  DependencyRequest request;
  request.inputs = {{descriptor, {}}};
  request.outputs = Footprint::all({1}).take_value();
  request.snapshot_identity = "native-contract";
  request.backend = backend;
  request.limits.maximum_work = work;
  auto session =
      registry.start_dependency(definition.key, request).take_value();
  unsigned materializations = 0;
  std::weak_ptr<const CpuStorage> payload;
  DependencyGpuServices gpu;
  gpu.allocation_capacity = [](std::uint64_t bytes) { return bytes; };
  gpu.materialize = [&](const FragmentAtlasPlan& plan,
                        const ValueFragments& value,
                        const FootprintLimits& limits) {
    ++materializations;
    if (mode == 2)
      throw std::runtime_error("host materialization exception");
    auto packed = plan.materialize(value, BufferAllocator{}, limits);
    if (packed.ok())
      payload = packed.value().payload.storage();
    return packed;
  };
  gpu.buffer = [](const std::uint8_t*, std::uint64_t, bool) {
    return Result<std::uint64_t>(
        Status{ErrorCode::InvalidArgument, "mock buffer failure"});
  };
  gpu.execute = [](const ps_gpu_dispatch_v8*, std::uint32_t) {
    return Status{ErrorCode::OperationFailed, "mock execute failure"};
  };
  if (mode == 6)
    gpu = {};
  auto first = session->poll(BufferAllocator{}, {}, {}, gpu);
  if (mode == 6) {
    PS_CHECK(first.status().code == expected && materializations == 0);
    return 0;
  }
  PS_CHECK(first.ok() &&
           std::holds_alternative<DependencyNeedBatch>(first.value()));
  PS_CHECK(session
               ->supply({ValueFragments::create(descriptor, {}, request.outputs,
                                                {input})
                             .take_value()},
                        request.snapshot_identity)
               .ok());
  auto second = session->poll(BufferAllocator{}, {}, {}, gpu);
  PS_CHECK(second.status().code == expected);
  PS_CHECK(payload.expired());
  if (expected == ErrorCode::Ok)
    PS_CHECK(materializations == 1);
  if (expected == ErrorCode::ResourceExhausted || backend == Backend::Cpu)
    PS_CHECK(materializations == 0);
  PS_CHECK(session->poll().status().code == ErrorCode::InvalidArgument);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(run(0, Backend::Gpu, ErrorCode::Ok) == 0);
  PS_CHECK(run(1, Backend::Gpu, ErrorCode::InvalidArgument) == 0);
  PS_CHECK(run(2, Backend::Gpu, ErrorCode::OperationFailed) == 0);
  PS_CHECK(run(3, Backend::Gpu, ErrorCode::InvalidArgument) == 0);
  PS_CHECK(run(4, Backend::Gpu, ErrorCode::OperationFailed) == 0);
  PS_CHECK(run(0, Backend::Cpu, ErrorCode::InvalidArgument) == 0);
  PS_CHECK(run(6, Backend::Gpu, ErrorCode::BackendUnavailable) == 0);
  PS_CHECK(run(0, Backend::Gpu, ErrorCode::ResourceExhausted, 64) == 0);
  return 0;
}
