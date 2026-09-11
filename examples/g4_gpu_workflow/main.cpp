#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "photospider/plugin/fragment_atlas_msl.h"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
constexpr std::uint64_t length = 8192;
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr char shader[] = PS_FRAGMENT_ATLAS_MSL_V9
    "kernel void sparse_sum(device const uchar* data [[buffer(0)]],"
    " device const ulong* directory [[buffer(1)]],"
    " device uint* output [[buffer(2)]], constant ulong* c [[buffer(3)]]) {"
    " float sum=0; uint missing=0; ulong at[8]={0}, address=0;"
    " for (uint i=0; i<65; ++i) { at[0]=c[4]+i*64;"
    " if (!ps_atlas_address(directory,c[0],c+1,c+2,1,at,c[3],4,address))"
    " {missing=1; continue;}"
    " uint bits=0; for (uint b=0;b<4;++b) bits|=uint(data[address+b])<<(8*b);"
    " sum+=as_type<float>(bits); }"
    " output[0]=as_type<uint>(sum); output[1]=missing; }";
// NOLINTEND
Footprint point(std::uint64_t at) {
  return Footprint::from_regions({length}, {Region({{at, 1}})}).take_value();
}
struct SparseState {
  std::uint64_t offset = 0;
  unsigned stage = 0;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const auto output = phase.query.outputs.boxes()[0].dimensions()[0].offset;
    const auto need = [&](std::uint32_t port, std::uint32_t role,
                          Footprint samples) {
      return Result<DependencyPoll>(DependencyNeedBatch{
          {{{output}, {{port, role, std::move(samples), {}}}}},
          {}});
    };
    if (stage++ == 0)
      return need(1, 2, point(output));
    if (stage == 2) {
      std::int64_t control = 0;
      auto status = phase.read(1, {output}, &control, 8);
      if (!status.ok())
        return Result<DependencyPoll>(status);
      if (control < 0 || control >= 64)
        return Result<DependencyPoll>(
            Status{ErrorCode::OperationFailed, "invalid control"});
      offset = control;
      status = phase.consume_work(65);
      if (!status.ok())
        return Result<DependencyPoll>(status);
      std::vector<Region> boxes;
      for (std::uint64_t i = 0; i < 65; ++i)
        boxes.push_back(Region({{offset + i * 64, 1}}));
      return need(
          0, 1,
          Footprint::from_regions({length}, boxes, phase.sets).take_value());
    }
    float sum = 0;
    if (phase.query.backend == Backend::Cpu) {
      for (std::uint64_t i = 0; i < 65; ++i) {
        float sample = 0;
        auto status = phase.read(0, {offset + i * 64}, &sample, 4);
        if (!status.ok())
          return Result<DependencyPoll>(status);
        sum += sample;
      }
    } else {
      auto state = MutableValue::allocate({ElementType::UInt8, {12}},
                                          Region::whole({12}), phase.allocator);
      if (!state.ok())
        return Result<DependencyPoll>(state.status());
      auto bytes = state.take_value();
      std::memset(bytes.data(), 0, bytes.size());
      std::memcpy(bytes.data(), &offset, 8);
      const auto incoming = std::move(bytes).publish().take_value();
      auto evaluated =
          phase.block(1, 0, 65, 1, incoming, [&]() -> Result<Value> {
            std::uint64_t block_offset = 0;
            std::memcpy(&block_offset, incoming.bytes().data(), 8);
            auto packed = phase.atlas(0);
            if (!packed.ok())
              return Result<Value>(packed.status());
            auto atlas = packed.take_value();
            auto same = phase.atlas(0);
            if (!same.ok() ||
                same.value().payload.storage() != atlas.payload.storage())
              return Result<Value>(
                  Status{ErrorCode::Internal, "atlas not reused"});
            auto scratch = phase.allocator.allocate(8);
            if (!scratch.ok())
              return Result<Value>(scratch.status());
            auto memory = scratch.take_value();
            auto data = phase.gpu_buffer(atlas.payload.bytes().data(),
                                         atlas.payload.bytes().size(), false);
            auto directory =
                phase.gpu_buffer(atlas.directory.bytes().data(),
                                 atlas.directory.bytes().size(), false);
            auto destination =
                phase.gpu_buffer(memory.data(), memory.size(), true);
            if (!data.ok() || !directory.ok() || !destination.ok())
              return Result<Value>(
                  Status{ErrorCode::OperationFailed, "native view failed"});
            ps_gpu_buffer_binding_v9 bindings[] = {
                {sizeof(ps_gpu_buffer_binding_v9), 0, data.value(), 0,
                 atlas.payload.bytes().size(), 0},
                {sizeof(ps_gpu_buffer_binding_v9), 1, directory.value(), 0,
                 atlas.directory.bytes().size(), 0},
                {sizeof(ps_gpu_buffer_binding_v9), 2, destination.value(), 0,
                 memory.size(), 1}};
            const std::uint64_t constants[] = {
                atlas.slot_count, length, atlas.tile_shape[0],
                atlas.payload_bytes, block_offset};
            ps_gpu_dispatch_v9 command{};
            command.struct_size = sizeof(command);
            command.source = shader;
            command.source_size = sizeof(shader) - 1;
            command.entry = "sparse_sum";
            command.entry_size = 10;
            command.buffers = bindings;
            command.buffer_count = 3;
            command.constants = constants;
            command.constant_size = sizeof(constants);
            command.constant_index = 3;
            command.grid[0] = command.grid[1] = command.grid[2] = 1;
            auto status = phase.gpu_execute(&command, 1);
            if (!status.ok())
              return Result<Value>(status);
            std::uint32_t missing = 0;
            std::memcpy(&missing, memory.data() + 4, 4);
            if (missing)
              return Result<Value>(
                  Status{ErrorCode::OperationFailed, "unresolved native read"});
            auto outgoing = MutableValue::allocate(
                incoming.descriptor(), incoming.region(), phase.allocator);
            if (!outgoing.ok())
              return Result<Value>(outgoing.status());
            auto writer = outgoing.take_value();
            std::memcpy(writer.data(), incoming.bytes().data(),
                        incoming.bytes().size());
            std::memcpy(writer.data() + 8, memory.data(), 4);
            return std::move(writer).publish();
          });
      if (!evaluated.ok())
        return Result<DependencyPoll>(evaluated.status());
      std::memcpy(&sum, evaluated.value().bytes().data() + 8, 4);
    }
    auto allocated =
        MutableValue::allocate(phase.query.output.descriptor,
                               phase.query.outputs.boxes()[0], phase.allocator);
    if (!allocated.ok())
      return Result<DependencyPoll>(allocated.status());
    auto writer = allocated.take_value();
    std::memcpy(writer.data(), &sum, 4);
    auto value = std::move(writer).publish().take_value();
    auto result = ValueFragments::create(phase.query.output.descriptor, {},
                                         phase.query.outputs, {value});
    if (!result.ok())
      return Result<DependencyPoll>(result.status());
    return Result<DependencyPoll>(result.take_value());
  }
};
struct CancelState {
  explicit CancelState(CancellationSource cancellation)
      : cancellation(std::move(cancellation)) {}
  CancellationSource cancellation;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    static_cast<void>(phase.gpu_buffer(nullptr, 1, false));
    cancellation.cancel();
    return Result<DependencyPoll>(
        Status{ErrorCode::InvalidArgument, "ignored native failure"});
  }
};
int check(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition ? 0 : 1;
}
}  // namespace
int main() {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition definition;
  definition.key = "example.sparse_native_sum";
  auto& traits = definition.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  traits.outputs[0].output_element_type = ElementType::Float32;
  traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.outputs[0].region_rule = OperationRegionRule::Dependency;
  traits.outputs[0].dependency_version = 1;
  traits.supports_gpu = true;
  traits.outputs[0].continuation_bytes = sizeof(SparseState);
  traits.workspace_bytes = 32;
  traits.outputs[0].maximum_dependency_stages = 4;
  definition.start_dependency = [](const DependencyQuery&,
                                   const BufferAllocator& allocator) {
    return DependencyContinuation::make<SparseState>(allocator);
  };
  if (check(registry->register_operation(definition).ok(),
            "registration failed"))
    return 1;
  CancellationSource auxiliary;
  auto cancel_definition = definition;
  cancel_definition.key = "example.native_cancel";
  cancel_definition.traits.outputs[0].continuation_bytes = sizeof(CancelState);
  cancel_definition.start_dependency =
      [auxiliary](const DependencyQuery&, const BufferAllocator& allocator) {
        return DependencyContinuation::make<CancelState>(allocator, auxiliary);
      };
  if (check(registry->register_operation(cancel_definition).ok(),
            "cancel registration failed"))
    return 1;
  if (check(registry->freeze().ok(), "registry freeze failed"))
    return 1;
  WorkflowDocument document;
  document.inputs = {{1,
                      "data",
                      {ElementType::Float32, {length}},
                      Region::whole({length}),
                      {0, {4}},
                      {}},
                     {2,
                      "control",
                      {ElementType::Int64, {length}},
                      Region::whole({length}),
                      {0, {8}},
                      {}}};
  document.nodes = {{1,
                     definition.key,
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"sum", 1, "value"}};
  auto data = MutableValue::allocate(document.inputs[0].descriptor,
                                     Region::whole({length}), BufferAllocator{})
                  .take_value();
  auto control =
      MutableValue::allocate(document.inputs[1].descriptor,
                             Region::whole({length}), BufferAllocator{})
          .take_value();
  std::memset(control.data(), 0, control.size());
  const std::int64_t one = 1;
  std::memcpy(control.data() + 8, &one, 8);
  for (std::uint64_t i = 0; i < length; ++i) {
    const float number = static_cast<float>((i / 64 + 1) * (i % 64 + 1));
    std::memcpy(data.data() + i * 4, &number, 4);
  }
  ExecutionBindings bindings{
      {{"data", std::move(data).publish().take_value()},
       {"control", std::move(control).publish().take_value()}}};
  GraphContext graph(document);
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.result_cache_bytes = 1048576;
  ExecutionContext context(registry, config);
  const bool native = context.gpu_enabled();
  for (bool gpu : {false, true}) {
    if (gpu && !native) {
      std::cout << "CPU oracle passed; native workflow skipped\n";
      return 77;
    }
    PlanningOptions planning;
    planning.execution_mode =
        gpu ? ExecutionMode::MetalFp32 : ExecutionMode::CpuExact;
    auto compiled = Compiler(registry).compile(graph, planning);
    if (check(compiled.ok(), "compile failed"))
      return 1;
    auto frozen = context.freeze(compiled.value().plan, bindings);
    if (check(frozen.ok(), "freeze failed"))
      return 1;
    auto query =
        point(0).unite(point(1)).take_value().unite(point(2)).take_value();
    auto result = context.execute_fragments(frozen.value(), {{"sum", query}});
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    if (check(result.ok(), "workflow failed"))
      return 1;
    for (std::uint64_t i = 0; i < 3; ++i) {
      float actual = 0;
      if (check(result.value().values.at("sum").read({i}, &actual, 4).ok() &&
                    actual == 2145 * (i == 1 ? 2 : 1),
                "independent arithmetic oracle failed"))
        return 1;
    }
    auto proof =
        result.value().dependencies.restrict({{"sum", point(0)}}).take_value();
    auto support = proof.source_support().take_value();
    if (check(support.at("data").element_count().value() == 65 &&
                  support.at("control") == point(0) &&
                  !support.at("data")
                       .intersect(point(1))
                       .value()
                       .element_count()
                       .value(),
              "sparse certificate failed"))
      return 1;
    auto reused = result.value()
                      .dependencies.restrict({{"sum", point(2)}})
                      .take_value()
                      .source_support()
                      .take_value();
    if (check(reused.at("control") == point(2) &&
                  reused.at("data") == support.at("data"),
              "block reused obsolete witness"))
      return 1;
    const auto& diagnostics = result.value().diagnostics;
    if (check(diagnostics.native_dispatch_count == (gpu ? 2 : 0),
              "native dispatch count failed"))
      return 1;
    if (check(diagnostics.selected_backends.at({1, 0}) ==
                      (gpu ? Backend::Gpu : Backend::Cpu) &&
                  diagnostics.block_cache_hits == (gpu ? 1 : 0),
              "backend or block cache diagnostics failed"))
      return 1;
    std::uint64_t dispatches = 0;
    for (const auto& timing : diagnostics.operation_timings) {
      if (check(timing.backend == (gpu ? Backend::Gpu : Backend::Cpu),
                "timing backend failed"))
        return 1;
      dispatches += timing.native_dispatch_count;
    }
    if (check(dispatches == diagnostics.native_dispatch_count,
              "timing dispatches failed"))
      return 1;
    if (gpu && check(context.cache_statistics().native_retained_bytes == 36,
                     "native cached output/state capacity failed"))
      return 1;
    std::cout << (gpu ? "Metal" : "CPU")
              << ": sums=2145,4290,2145; unique_data_samples=130; dispatches="
              << diagnostics.native_dispatch_count
              << "; peak=" << diagnostics.peak_live_bytes << '\n';
  }
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::MetalFp32;
  auto compiled = Compiler(registry).compile(graph, planning).take_value();
  // Actual device directory capacity is 32768 for 256 eighty-byte slots.
  // One live continuation, 4 output + 32 workspace, and both atlas buffers
  // form this finite stage's complete reservation.
  constexpr std::uint64_t minimum =
      sizeof(SparseState) + 4 + 32 + 65 * 4 + 32768;
  for (const auto bytes : {minimum - 1, minimum}) {
    ExecutionContextConfig bounded;
    bounded.gpu_enabled = true;
    bounded.maximum_live_bytes = bytes;
    ExecutionContext small(registry, bounded);
    auto frozen = small.freeze(compiled.plan, bindings).take_value();
    auto result = small.execute_fragments(frozen, {{"sum", point(0)}});
    if (check(bytes == minimum
                  ? result.ok()
                  : result.status().code == ErrorCode::ResourceExhausted,
              "native stage admission frontier failed"))
      return 1;
    if (bytes == minimum) {
      if (check(result.value().diagnostics.native_dispatch_count == 1,
                "bounded native dispatch failed"))
        return 1;
      result = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
      if (check(small.execute_fragments(frozen, {{"sum", point(0)}}).ok(),
                "native owners did not retire"))
        return 1;
    }
  }
  std::cout << "native stage admission: " << minimum - 1 << " rejected, "
            << minimum << " passed and reusable\n";
  document.nodes[0].operation = cancel_definition.key;
  GraphContext cancel_graph(document);
  auto cancel_plan =
      Compiler(registry).compile(cancel_graph, planning).take_value();
  ExecutionOptions cancelled_options;
  cancelled_options.dependencies.sets.cancellation = auxiliary.token();
  unsigned delivered = 0;
  auto cancelled = context.execute_stream(
      cancel_plan.plan, bindings,
      [&](const std::string&, ValueView) {
        ++delivered;
        return Status::success();
      },
      {}, cancelled_options);
  if (check(cancelled.status().code == ErrorCode::Cancelled && delivered == 0,
            "auxiliary cancellation lost to native service error"))
    return 1;
  return 0;
}
