#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void word(std::uint8_t* bytes, std::uint64_t value, unsigned width = 4) {
  for (unsigned i = 0; i < width; ++i)
    bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
}
Footprint point(std::uint64_t at) {
  return Footprint::from_regions({16}, {Region({{at, 1}})}).take_value();
}
struct State {
  explicit State(unsigned mode) : mode(mode) {}
  unsigned mode;
  bool waiting = false;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!waiting) {
      waiting = true;
      auto status =
          phase.discover(2, 3, [&](const DependencyGpuRequestTable& table) {
            if (mode == 14)
              return phase.discover(
                  1, 1, [](const auto&) { return Status::success(); });
            if (mode == 15) {
              auto writer =
                  MutableValue::allocate({ElementType::UInt8, {1}},
                                         Region::whole({1}), phase.allocator)
                      .take_value();
              writer.data()[0] = 0;
              const auto incoming = std::move(writer).publish().take_value();
              auto result = phase.block(1, 0, 1, 1, incoming, [&] {
                return Result<Value>(incoming);
              });
              return result.status();
            }
            if (mode == 16)
              throw std::runtime_error("discovery callback exception");
            word(table.bytes, mode == 18 ? 0 : 2);
            for (unsigned i = 0; i < 2; ++i) {
              auto* row = table.bytes + 16 + i * 144;
              word(row + 4, 1);
              word(row + 8, 1);
              word(row + 16, i == 0 ? 1 : 9, 8);
              word(row + 80, 1, 8);
            }
            if (mode == 1) {
              word(table.bytes, 3);
              word(table.bytes + 4, 1);
            }
            if (mode == 2)
              word(table.bytes, 3);
            if (mode == 3)
              word(table.bytes + 16 + 24, 1, 8);
            if (mode == 4)
              word(table.bytes + 16 + 8, 0);
            if (mode == 5)
              word(table.bytes + 16 + 4, 16);
            if (mode == 6)
              word(table.bytes + 16 + 80, 0, 8);
            if (mode == 7)
              word(table.bytes + 16 + 16, 16, 8);
            if (mode == 8)
              word(table.bytes + 8, 1);
            if (mode == 20 || mode == 23)
              word(table.bytes + 16 + 144 + 4, 2);
            if (mode == 24 || mode == 25) {
              for (unsigned i = 0; i < 2; ++i) {
                auto* row = table.bytes + 16 + i * 144;
                std::memset(row, 0, 144);
                word(row + 4, 1);
                word(row + 8, mode == 24 ? 3 : 8);
                for (unsigned axis = 0; axis < (mode == 24 ? 3U : 8U); ++axis) {
                  word(row + 16 + axis * 8,
                       mode == 24 ? (axis == 2 ? i * 2 : 0)
                                  : (i ? UINT64_C(1) << 39 : 0),
                       8);
                  word(row + 80 + axis * 8, mode == 24 && axis == 2 ? 2 : 1, 8);
                }
              }
            }
            if (mode == 9)
              word(table.bytes, 4);  // Exceeds declared attempts.
            return Status::success();
          });
      if ((mode == 15 || mode == 23 || mode == 24) && !status.ok())
        return Result<DependencyPoll>(status);
      // Ignored errors must still prevent a terminal value or Need publication.
      static_cast<void>(status);
      if (mode == 28)
        return Result<DependencyPoll>(DependencyNeedBatch{});
      if (mode == 22)
        return Result<DependencyPoll>(
            DependencyNeedBatch{std::vector<AtomCertificate>(256, {{1}, {}}),
                                {}});
      if (mode != 10 && mode != 18)
        return Result<DependencyPoll>(DependencyNeedBatch{{{{0}, {}}}, {}});
    }
    float sum = 0;
    if (mode != 10 && mode != 18) {
      for (const auto at : {1, 9}) {
        float value = 0;
        auto status =
            phase.read(0, {static_cast<std::uint64_t>(at)}, &value, 4);
        if (!status.ok())
          return Result<DependencyPoll>(status);
        sum += value;
      }
    }
    auto writer =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator)
            .take_value();
    for (std::size_t offset = 0; offset < writer.size(); offset += 4)
      std::memcpy(writer.data() + offset, &sum, 4);
    auto value = std::move(writer).publish().take_value();
    return Result<DependencyPoll>(
        ValueFragments::create(phase.query.output.descriptor, {},
                               phase.query.outputs, {value})
            .take_value());
  }
};
int run(unsigned mode, ErrorCode expected, std::uint64_t work = 1048576,
        std::uint32_t capacity = 65536) {
  OperationRegistry registry;
  OperationDefinition definition;
  definition.key = "test.gpu.discovery";
  auto& traits = definition.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.output_element_type = ElementType::Float32;
  traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  if (mode == 24 || mode == 25) {
    traits.shape_rule = OperationShapeRule::Fixed;
    traits.fixed_output_shape = {16};
  }
  traits.region_rule = OperationRegionRule::Dependency;
  traits.dependency_version = 1;
  traits.supports_gpu = true;
  traits.continuation_bytes = sizeof(State);
  traits.maximum_dependency_stages = 3;
  if (mode == 28)
    traits.observation_kind = ObservationKind::RequestRecord;
  definition.start_dependency = [mode](const DependencyQuery&,
                                       const BufferAllocator& allocator) {
    return DependencyContinuation::make<State>(allocator, mode);
  };
  PS_CHECK(registry.register_operation(definition).ok());
  DependencyRequest request;
  request.inputs = {{{ElementType::Float32, {16}}, {}}};
  if (mode == 24)
    request.inputs = {{{ElementType::Float32, {1, 1, 4}},
                       {encode_semantic(rgba_semantics()).take_value()}}};
  if (mode == 25)
    request.inputs = {{{ElementType::Float32,
                        std::vector<std::uint64_t>(8, UINT64_C(1) << 40)},
                       {}}};
  request.outputs = point(0);
  if (mode == 28)
    request.outputs = point(0).unite(point(1)).take_value();
  CancellationSource auxiliary;
  request.limits.sets.cancellation = auxiliary.token();
  request.snapshot_identity = "discovery-mock";
  request.backend = Backend::Gpu;
  request.limits.maximum_work = work;
  request.limits.maximum_gpu_requests = capacity;
  if (mode == 22)
    request.limits.sets.maximum_boxes = 32;
  if (mode == 23)
    request.limits.sets.maximum_boxes = 4;
  auto session =
      registry.start_dependency(definition.key, request).take_value();
  unsigned allocations = 0, live = 0;
  BufferAllocator allocator([&](std::uint64_t) {
    ++live;
    return Result<std::shared_ptr<void>>(
        std::shared_ptr<void>(new int(0), [&](void* pointer) {
          --live;
          delete static_cast<int*>(pointer);
        }));
  });
  DependencyGpuServices gpu;
  gpu.allocation_capacity = [](std::uint64_t bytes) { return bytes; };
  gpu.materialize = [](const FragmentAtlasPlan&, const ValueFragments&,
                       const FootprintLimits&) {
    return Result<FragmentAtlas>(Status{ErrorCode::BackendUnavailable, {}});
  };
  gpu.buffer = [](const std::uint8_t*, std::uint64_t, bool) {
    return Result<std::uint64_t>(Status{ErrorCode::BackendUnavailable, {}});
  };
  gpu.execute = [](const ps_gpu_dispatch_v8*, std::uint32_t) {
    return Status{ErrorCode::BackendUnavailable, {}};
  };
  gpu.allocate_discovery = [&](std::uint64_t bytes) {
    ++allocations;
    if (mode == 26)
      auxiliary.cancel();
    return allocator.allocate(bytes - (mode == 13 ? 1 : 0));
  };
  auto progress = session->poll(BufferAllocator{}, {}, {}, gpu);
  if (progress.status().code != expected || live != 0)
    std::cerr << "mode=" << mode
              << ", code=" << static_cast<int>(progress.status().code)
              << ", work=" << session->consumed_work() << ", live=" << live
              << '\n';
  PS_CHECK(progress.status().code == expected && live == 0);
  if (work == 64 || capacity == 0)
    PS_CHECK(allocations == 0);
  if (mode == 23)
    PS_CHECK(progress.status().message == "GPU discovery metadata limit");
  if (mode == 24)
    PS_CHECK(progress.status().message ==
             "GPU discovery omits image channel closure");
  if (expected != ErrorCode::Ok)
    return 0;
  if (mode == 25) {
    auto reads = session->pending_reads().take_value();
    PS_CHECK(reads.size() == 1 &&
             reads[0].samples.element_count().value() == 2 &&
             reads[0].samples.contains(
                 std::vector<std::uint64_t>(8, UINT64_C(1) << 39)));
    return 0;
  }
  if (mode == 18) {
    PS_CHECK(std::holds_alternative<DependencyResult>(progress.value()));
    return 0;
  }
  PS_CHECK(std::holds_alternative<DependencyNeedBatch>(progress.value()));
  auto reads = session->pending_reads().take_value();
  PS_CHECK(reads.size() == 1 &&
           reads[0].samples == point(1).unite(point(9)).value());
  auto writer = MutableValue::allocate(request.inputs[0].descriptor,
                                       Region::whole({16}), BufferAllocator{})
                    .take_value();
  std::memset(writer.data(), 0, writer.size());
  const float value = 21;
  std::memcpy(writer.data() + 4, &value, 4);
  std::memcpy(writer.data() + 36, &value, 4);
  auto input = std::move(writer).publish().take_value();
  PS_CHECK(session
               ->supply({ValueFragments::create(input.descriptor(), {},
                                                reads[0].samples, {input})
                             .take_value()},
                        request.snapshot_identity)
               .ok());
  auto complete = session->poll(BufferAllocator{}, {}, {}, gpu);
  PS_CHECK(complete.ok());
  const auto& result = std::get<DependencyResult>(complete.value());
  float sum = 0;
  PS_CHECK(result.value.read({0}, &sum, 4).ok() && sum == 42);
  if (mode == 28) {
    PS_CHECK(result.kind == ObservationKind::RequestRecord &&
             !result.certificate &&
             result.original_outputs == request.outputs &&
             result.value.read({1}, &sum, 4).ok() && sum == 42);
    return 0;
  }
  auto evidence = result.certificate->backward(point(0)).take_value();
  auto support = Footprint::none({16}).take_value();
  for (const auto& need : evidence)
    support = support.unite(need.samples).take_value();
  PS_CHECK(support == reads[0].samples);
  return 0;
}
}  // namespace
int main() {
  std::uint64_t used = UINT64_MAX;
  const std::vector<Region> boxes{Region({{1, 1}}), Region({{9, 1}})};
  PS_CHECK(Footprint::from_regions({16}, boxes, {}, &used).ok() && used > 1);
  FootprintLimits grant;
  grant.maximum_work = used - 1;
  std::uint64_t failed = UINT64_MAX;
  PS_CHECK(Footprint::from_regions({16}, boxes, grant, &failed).status().code ==
               ErrorCode::ResourceExhausted &&
           failed == used - 1);
  PS_CHECK(!Footprint::from_regions({0}, boxes, {}, &failed).ok() &&
           failed == 0);
  PS_CHECK(run(0, ErrorCode::Ok) == 0);
  PS_CHECK(run(1, ErrorCode::ResourceExhausted) == 0);
  for (unsigned mode : {2, 3, 4, 5, 6, 7, 8, 9, 10, 13, 14, 15})
    PS_CHECK(run(mode, ErrorCode::InvalidArgument) == 0);
  PS_CHECK(run(16, ErrorCode::OperationFailed) == 0);
  PS_CHECK(run(18, ErrorCode::Ok) == 0);
  PS_CHECK(run(22, ErrorCode::ResourceExhausted) == 0);
  PS_CHECK(run(23, ErrorCode::ResourceExhausted) == 0);
  PS_CHECK(run(24, ErrorCode::InvalidArgument) == 0);
  PS_CHECK(run(25, ErrorCode::Ok) == 0);
  PS_CHECK(run(26, ErrorCode::Cancelled) == 0);
  PS_CHECK(run(28, ErrorCode::Ok) == 0);
  PS_CHECK(run(0, ErrorCode::ResourceExhausted, 64) == 0);
  std::uint64_t single_group = 0;
  PS_CHECK(Footprint::from_regions({16}, {Region({{1, 1}})}, {}, &single_group)
               .ok());
  // Start + poll + discover, three candidates, and table zero/decode leave
  // exactly one group's normalization grant. Two groups cannot reuse it.
  const auto one_group_budget = 3 + 3 + 2 * (16 + 2 * 144) + single_group;
  PS_CHECK(run(20, ErrorCode::ResourceExhausted, one_group_budget) == 0);
  PS_CHECK(run(0, ErrorCode::ResourceExhausted, 1048576, 0) == 0);
  return 0;
}
