#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/plugin/fragment_atlas_msl.h"
#include "support/gpu_result_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using gpu_result::check;
using gpu_result::take;
constexpr std::uint64_t length = 8192;
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr char shader[] = PS_FRAGMENT_ATLAS_MSL_V11
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
  return take(Footprint::from_regions({length}, {Region({{at, 1}})}));
}
Result<ResultProgramPoll> need(std::uint32_t input, std::uint32_t roles,
                               Footprint samples) {
  ResultProgramNeed need;
  need.tensors.push_back({input, 0, std::move(samples), roles});
  return Result<ResultProgramPoll>(std::move(need));
}
ResultRef block_state(const ResultProgramPhase& phase, std::uint64_t offset,
                      float sum = 0) {
  auto schema = gpu_result::schema(ElementType::UInt8, 12);
  schema.id = "example.gpu.block";
  auto builder =
      take(ResultBuilder::start(phase.resources, schema, "sparse.block"));
  check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(phase.resources, 1, {}))));
  auto memory = take(phase.allocator.allocate(12));
  std::memcpy(memory.data(), &offset, 8);
  std::memcpy(memory.data() + 8, &sum, 4);
  check(builder.publish_tensor(
      0, Region::whole({12}), {0, {1}}, std::move(memory).freeze(),
      take(ResultRelation::cartesian(phase.resources, 12, {})),
      {true, true, true, true}, phase.query.cancellation));
  return take(builder.seal());
}
std::array<std::uint8_t, 12> block_bytes(const ResultRef& state) {
  auto window = take(
      state.acquire_tensor(take(state.descriptor()), 0, Region::whole({12})));
  auto row = take(window.row_run({0}));
  if (row.samples != 12 || row.sample_stride_bytes != 1)
    throw std::runtime_error("invalid packed block state");
  std::array<std::uint8_t, 12> bytes{};
  std::memcpy(bytes.data(), row.data, bytes.size());
  return bytes;
}
Result<ResultRef> native_sum(const ResultProgramPhase& phase,
                             const ResultRef& incoming, unsigned mode) try {
  const auto bytes = block_bytes(incoming);
  std::uint64_t offset = 0;
  std::memcpy(&offset, bytes.data(), 8);
  const auto atlas = take(phase.acquire_native_atlas(0, 0));
  if (take(phase.acquire_native_atlas(0, 0)) != atlas)
    throw std::runtime_error("atlas not reused within poll");
  if (atlas->payload_bytes !=
          take(phase.tensors->at({0, 0}).coverage().element_count()) * 4 ||
      atlas->address({offset + 1}).status().code != ErrorCode::NotFound)
    throw std::runtime_error("atlas filled a sparse hole");
  auto memory = take(phase.allocator.allocate(8));
  const auto* api = phase.gpu;
  if (mode == 5) {
    std::uint64_t invalid = 0;
    static_cast<void>(api->buffer(api->context, atlas->payload.bytes().data(),
                                  atlas->payload.bytes().size(), 1, &invalid));
  }
  std::uint64_t data = 0, directory = 0, destination = 0;
  check(gpu_result::gpu_status(
      phase, api->buffer(api->context, atlas->payload.bytes().data(),
                         atlas->payload.bytes().size(), 0, &data)));
  check(gpu_result::gpu_status(
      phase, api->buffer(api->context, atlas->directory.bytes().data(),
                         atlas->directory.bytes().size(), 0, &directory)));
  check(gpu_result::gpu_status(
      phase, api->buffer(api->context, memory.data(), memory.size(), 1,
                         &destination)));
  ps_gpu_buffer_binding_v1 bindings[] = {
      {sizeof(ps_gpu_buffer_binding_v1), 0, data, 0,
       atlas->payload.bytes().size(), 0},
      {sizeof(ps_gpu_buffer_binding_v1), 1, directory, 0,
       atlas->directory.bytes().size(), 0},
      {sizeof(ps_gpu_buffer_binding_v1), 2, destination, 0, memory.size(), 1}};
  const std::uint64_t constants[] = {atlas->slot_count, length,
                                     atlas->tile_shape[0], atlas->payload_bytes,
                                     offset};
  ps_gpu_dispatch_v1 command{};
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
  check(gpu_result::gpu_status(phase, api->execute(api->context, &command, 1)));
  std::uint32_t missing = 0;
  float sum = 0;
  std::memcpy(&sum, memory.data(), 4);
  std::memcpy(&missing, memory.data() + 4, 4);
  if (missing)
    return Result<ResultRef>(
        Status{ErrorCode::OperationFailed, "unresolved native read"});
  return Result<ResultRef>(block_state(phase, offset, sum));
} catch (const gpu_result::Failure& failed) {
  return Result<ResultRef>(failed.status);
}
struct SparseState {
  ResourceVector<std::uint64_t> outputs, offsets;
  MutableBuffer sums;
  std::uint64_t cursor = 0, offset = 0;
  unsigned stage = 0, mode = 0;
  explicit SparseState(unsigned mode = 0) : mode(mode) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    if (mode >= 8 && mode <= 11) {
      if (phase.gpu->buffer(nullptr, nullptr, 1, 0, nullptr) !=
              PS_RESULT_STATUS_FAILURE_V2 ||
          phase.gpu->execute(nullptr, nullptr, 0) !=
              PS_RESULT_STATUS_FAILURE_V2 ||
          phase.gpu->release(nullptr, 0) != PS_RESULT_STATUS_FAILURE_V2)
        throw std::runtime_error("null native service context accepted");
      if (mode == 9)
        static_cast<void>(phase.consume_work(UINT64_MAX));
      std::uint64_t token = 0;
      if (mode == 10)
        static_cast<void>(phase.gpu->execute(phase.gpu->context, nullptr, 0));
      else if (mode == 11)
        static_cast<void>(phase.gpu->release(phase.gpu->context, 0));
      else
        static_cast<void>(
            phase.gpu->buffer(phase.gpu->context, nullptr, 1, 0, &token));
      if (mode != 9)
        static_cast<void>(phase.consume_work(UINT64_MAX));
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "ignored first native failure"});
    }
    if (mode == 7) {
      if (!stage++)
        return need(0, 9, take(Footprint::none({length})));
      const auto atlas = take(phase.acquire_native_atlas(0, 0));
      if (atlas->payload_bytes || atlas->payload.bytes().size() != 1 ||
          atlas->slot_count != 2 ||
          atlas->address({0}).status().code != ErrorCode::NotFound)
        throw std::runtime_error("empty atlas invented a sample");
      auto builder = gpu_result::builder(phase);
      return Result<ResultProgramPoll>(
          ResultPublication{take(builder.seal()), true});
    }
    if (!stage) {
      const auto demand = gpu_result::demand(phase);
      check(phase.consume_work(take(demand.element_count())));
      check(demand.visit(
          [&](const auto& at) {
            outputs.push_back(at[0]);
            return Status::success();
          },
          length, phase.query.cancellation));
      if (!outputs.empty()) {
        sums = take(phase.allocator.allocate(outputs.size() * 4));
        stage = 1;
        return need(1, 10, point(outputs[0]));
      }
    } else if (stage == 1) {
      std::int64_t control = 0;
      check(phase.read_tensor(1, 0, {outputs[cursor]}, &control, 8));
      if (control < 0 || control >= 64)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::OperationFailed, "invalid control"});
      offset = control;
      offsets.push_back(offset);
      check(phase.consume_work(65));
      std::vector<Region> boxes;
      for (std::uint64_t i = 0; i < (mode == 1 ? 64 : 65); ++i)
        boxes.push_back(Region({{offset + i * 64, 1}}));
      FootprintLimits limits;
      limits.cancellation = phase.query.cancellation;
      limits.consume_work = phase.consume_work;
      stage = 2;
      return need(0, mode == 3 ? 8 : 9,
                  take(Footprint::from_regions({length}, boxes, limits)));
    } else {
      float sum = 0;
      if (mode == 2)
        static_cast<void>(phase.acquire_native_atlas(0, 0));
      if (mode == 4)
        static_cast<void>(phase.acquire_native_atlas(99, 0));
      if (phase.query.backend == Backend::Cpu) {
        check(phase.consume_work(65));
        for (std::uint64_t i = 0; i < 65; ++i) {
          float sample = 0;
          check(phase.read_tensor(0, 0, {offset + i * 64}, &sample, 4));
          sum += sample;
        }
      } else {
        const auto incoming = block_state(phase, offset);
        const auto evaluated = take(phase.block(1, 0, 65, 1, incoming, [&] {
          return mode == 6 ? Result<ResultRef>(block_state(phase, offset, 2145))
                           : native_sum(phase, incoming, mode);
        }));
        const auto bytes = block_bytes(evaluated);
        std::memcpy(&sum, bytes.data() + 8, 4);
      }
      std::memcpy(sums.data() + cursor * 4, &sum, 4);
      if (++cursor < outputs.size()) {
        stage = 1;
        return need(1, 10, point(outputs[cursor]));
      }
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    auto basis = take(ResultRelation::cartesian(phase.resources, 1, {}));
    if (!outputs.empty()) {
      basis = take(ResultRelation::unite(
          phase.resources,
          {take(ResultRelation::cartesian(
               phase.resources, 1,
               {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0})),
           take(ResultRelation::cartesian(
               phase.resources, 1,
               {1, 8, 0, 1, ResultSupportTarget::Descriptor, 0}))}));
    }
    check(builder.bind_descriptor_relation(basis));
    check(phase.consume_work(outputs.size() * 66));
    auto relation =
        outputs.empty()
            ? take(ResultRelation::cartesian(phase.resources, length, {}))
            : take(ResultRelation::sample_rows(
                  phase.resources, length, outputs.size() * 66,
                  [&](std::uint64_t row) {
                    const auto observation = row / 66, sample = row % 66;
                    ResultSupport support =
                        sample == 65
                            ? ResultSupport{1,
                                            2,
                                            outputs[observation],
                                            1,
                                            ResultSupportTarget::Tensor,
                                            0}
                            : ResultSupport{0,
                                            1,
                                            offsets[observation] + sample * 64,
                                            1,
                                            ResultSupportTarget::Tensor,
                                            0};
                    return Result<ResultRelationRow>(
                        ResultRelationRow{outputs[observation], support});
                  }));
    auto storage = std::move(sums).freeze();
    for (std::size_t i = 0; i < outputs.size(); ++i)
      check(builder.publish_tensor(
          0, Region({{outputs[i], 1}}), {i * 4, {4}, {outputs[i]}}, storage,
          relation, {true, true, true, true}, phase.query.cancellation));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const gpu_result::Failure& failed) {
    return Result<ResultProgramPoll>(failed.status);
  }
};
int require(bool valid, const char* message);

// NOLINTBEGIN(whitespace/indent_namespace)
constexpr char batch_shader[] = PS_FRAGMENT_ATLAS_MSL_V11
    "kernel void atlas_one(device const uchar* data [[buffer(0)]],"
    "device const ulong* directory [[buffer(1)]], device uint* out "
    "[[buffer(2)]],"
    "constant ulong* c [[buffer(3)]]) { ulong address=0; ulong "
    "at[8]={1,1,1,1,0,0,0,0};"
    "bool found=ps_atlas_address(directory,c[0],c+1,c+9,4,at,c[17],4,address);"
    "uint bits=0; if(found)for(uint "
    "b=0;b<4;++b)bits|=uint(data[address+b])<<(8*b);"
    "out[0]=bits;out[1]=!found;}";
// NOLINTEND
struct AtlasProbe {
  bool supplied = false;
  std::shared_ptr<const FragmentAtlas>* survivor;
  explicit AtlasProbe(std::shared_ptr<const FragmentAtlas>* survivor)
      : survivor(survivor) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    if (!supplied) {
      supplied = true;
      std::vector<Region> boxes;
      for (std::uint64_t n = 0; n < 2; ++n)
        for (std::uint64_t l = 0; l < 2; ++l)
          boxes.push_back(Region({{n, 1}, {l, 1}, {0, 2}, {1, 1}}));
      return need(0, 9, take(Footprint::from_regions({2, 2, 2, 4}, boxes)));
    }
    const auto atlas = take(phase.acquire_native_atlas(0, 0));
    if (atlas->payload_bytes != 32 ||
        atlas->address({0, 0, 0, 0}).status().code != ErrorCode::NotFound)
      throw std::runtime_error("spatial batch atlas coverage");
    *survivor = atlas;
    const auto* api = phase.gpu;
    auto output = take(phase.allocator.allocate(8));
    std::uint64_t data = 0, directory = 0, destination = 0;
    check(gpu_result::gpu_status(
        phase, api->buffer(api->context, atlas->payload.bytes().data(),
                           atlas->payload.bytes().size(), 0, &data)));
    check(gpu_result::gpu_status(
        phase, api->buffer(api->context, atlas->directory.bytes().data(),
                           atlas->directory.bytes().size(), 0, &directory)));
    check(gpu_result::gpu_status(
        phase, api->buffer(api->context, output.data(), output.size(), 1,
                           &destination)));
    ps_gpu_buffer_binding_v1 bindings[] = {
        {sizeof(ps_gpu_buffer_binding_v1), 0, data, 0,
         atlas->payload.bytes().size(), 0},
        {sizeof(ps_gpu_buffer_binding_v1), 1, directory, 0,
         atlas->directory.bytes().size(), 0},
        {sizeof(ps_gpu_buffer_binding_v1), 2, destination, 0, output.size(),
         1}};
    std::array<std::uint64_t, 18> constants{};
    constants[0] = atlas->slot_count;
    constants[17] = atlas->payload_bytes;
    for (std::size_t i = 0; i < 4; ++i) {
      constants[1 + i] = atlas->descriptor.shape[i];
      constants[9 + i] = atlas->tile_shape[i];
    }
    ps_gpu_dispatch_v1 command{};
    command.struct_size = sizeof(command);
    command.source = batch_shader;
    command.source_size = sizeof(batch_shader) - 1;
    command.entry = "atlas_one";
    command.entry_size = 9;
    command.buffers = bindings;
    command.buffer_count = 3;
    command.constants = constants.data();
    command.constant_size = sizeof(constants);
    command.constant_index = 3;
    command.grid[0] = command.grid[1] = command.grid[2] = 1;
    check(
        gpu_result::gpu_status(phase, api->execute(api->context, &command, 1)));
    std::uint32_t missing = 0;
    std::memcpy(&missing, output.data() + 4, 4);
    if (missing)
      throw std::runtime_error("spatial batch shader lookup");
    auto builder = gpu_result::builder(phase);
    check(builder.publish_tensor(
        0, Region::whole({1}), {0, {4}}, std::move(output).freeze(),
        take(ResultRelation::cartesian(
            phase.resources, 1, {0, 1, 29, 1, ResultSupportTarget::Tensor, 0})),
        {true, true, true, true}, phase.query.cancellation));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const gpu_result::Failure& failed) {
    return Result<ResultProgramPoll>(failed.status);
  }
};
int spatial_owner() {
  auto input_schema = gpu_result::schema(ElementType::Float32, 1);
  input_schema.id = "example.atlas.batch";
  auto& tensor = input_schema.tensors[0];
  tensor.batch_axes = {2, 2};
  tensor.descriptor.shape = {2, 4};
  tensor.layout.spatial = true;
  tensor.layout.channel_axis = {};
  std::shared_ptr<const FragmentAtlas> survivor;
  WeakResultRef source;
  ResourceBudget root;
  {
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition op;
    op.key = "example.atlas_batch";
    op.traits.input_count = 1;
    op.traits.input_schema.resize(1);
    op.traits.input_schema[0].kind = OperationPortKind::Result;
    op.traits.input_schema[0].result_schema_id = input_schema.id;
    op.traits.input_schema[0].result_schema_version = 1;
    op.traits.outputs = {
        gpu_result::output(gpu_result::schema(ElementType::Float32, 1))};
    op.traits.outputs[0].continuation_bytes = sizeof(AtlasProbe);
    op.traits.supports_gpu = true;
    op.traits.workspace_bytes = 8;
    op.start_result = [&survivor](const ResultProgramQuery&,
                                  const BufferAllocator& allocator) {
      return ResultContinuation::make<AtlasProbe>(allocator, &survivor);
    };
    check(registry->register_operation(op));
    check(registry->freeze());
    ExecutionContext context(registry, {1, true, 8, 1 << 20, 0});
    root = take(context.resource_budget());
    auto builder =
        take(ResultBuilder::start(root, input_schema, "batch.input"));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(root, 1, {}))));
    auto memory = take(root.allocator().allocate(128));
    for (std::uint32_t i = 0; i < 32; ++i) {
      const float value = static_cast<float>(i + 1);
      std::memcpy(memory.data() + i * 4, &value, 4);
    }
    check(builder.publish_tensor(0, Region::whole({2, 2, 2, 4}),
                                 {0, {64, 32, 16, 4}},
                                 std::move(memory).freeze(),
                                 take(ResultRelation::cartesian(root, 32, {})),
                                 {true, true, true, true}));
    ExecutionBinding binding;
    binding.name = "data";
    binding.result = take(builder.seal());
    source = binding.result.weak();
    WorkflowDocument doc;
    doc.inputs = {gpu_result::declaration(1, "data", input_schema)};
    doc.nodes = {{1, op.key, {WorkflowInputReference{1}}, {}}};
    doc.outputs = {{"out", 1, "value"}};
    GraphContext graph(doc);
    PlanningOptions planning;
    planning.execution_mode = ExecutionMode::NativeGpu;
    auto plan = take(Compiler(registry).compile(graph, planning)).plan;
    auto output = take(context.execute(plan, {{binding}}));
    if (require(gpu_result::number(output.results.at("out"), 0) == 30 &&
                    output.diagnostics.native_dispatch_count > 0,
                "spatial batch native lookup"))
      return 1;
  }
  float value = 0;
  const auto address = take(survivor->address({1, 1, 1, 1}));
  std::memcpy(&value, survivor->payload.bytes().data() + address, 4);
  if (require(!source.lock().valid() && value == 30 &&
                  root.statistics().live[ResourceKind::Payload] ==
                      survivor->payload.storage()->capacity() +
                          survivor->directory.storage()->capacity(),
              "atlas owns native bytes independently of source and context"))
    return 1;
  survivor.reset();
  return require(root.statistics().live[ResourceKind::Payload] == 0,
                 "atlas final owner release");
}
struct CancelState {
  CancellationSource cancellation;
  bool protocol;
  CancelState(CancellationSource source, bool protocol)
      : cancellation(std::move(source)), protocol(protocol) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (protocol) {
      std::uint64_t token = 0;
      static_cast<void>(
          phase.gpu->buffer(phase.gpu->context, nullptr, 1, 0, &token));
    }
    cancellation.cancel();
    return Result<ResultProgramPoll>(
        Status{ErrorCode::InvalidArgument, "callback cancelled"});
  }
};
int require(bool valid, const char* message) {
  if (!valid)
    std::cerr << message << '\n';
  return valid ? 0 : 1;
}
ExecutionBindings inputs(const ResourceBudget& root, unsigned layout = 0) {
  ExecutionBindings bindings;
  for (bool control : {false, true}) {
    auto schema = gpu_result::schema(
        control ? ElementType::Int64 : ElementType::Float32, length);
    auto builder =
        take(ResultBuilder::start(root, schema, control ? "control" : "data"));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(root, 1, {}))));
    if (!control && layout == 4) {
      ExecutionBinding binding;
      binding.name = "data";
      binding.result = take(builder.seal());
      bindings.inputs.push_back(std::move(binding));
      continue;
    }
    const auto width = control ? 8U : 4U;
    const auto count = !control && layout == 2 ? 1 : length;
    auto memory = take(root.allocator().allocate(count * width));
    for (std::uint64_t i = 0; i < count; ++i) {
      if (control) {
        const std::int64_t value = i == 1 ? 1 : 0;
        std::memcpy(memory.data() + i * 8, &value, 8);
      } else {
        const auto coordinate = layout == 1 ? length - 1 - i : i;
        const auto value =
            static_cast<float>((coordinate / 64 + 1) * (coordinate % 64 + 1));
        std::memcpy(memory.data() + i * 4, &value, 4);
      }
    }
    auto storage = std::move(memory).freeze();
    auto relation = take(ResultRelation::cartesian(root, length, {}));
    if (!control && layout == 3) {
      for (std::uint64_t i = 0; i < 65; ++i)
        check(builder.publish_tensor(0, Region({{i * 64, 1}}), {0, {4}},
                                     storage, relation,
                                     {true, true, true, true}));
    } else {
      const auto stride = control ? 8 : layout == 1 ? -4 : layout == 2 ? 0 : 4;
      const auto offset = !control && layout == 1 ? (length - 1) * 4 : 0;
      check(builder.publish_tensor(0, Region::whole({length}),
                                   {offset, {stride}}, storage, relation,
                                   {true, true, true, true}));
    }
    ExecutionBinding binding;
    binding.name = control ? "control" : "data";
    binding.result = take(builder.seal());
    bindings.inputs.push_back(std::move(binding));
  }
  return bindings;
}
}  // namespace
int main() try {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition definition;
  definition.key = "example.sparse_native_sum";
  auto& traits = definition.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& port : traits.input_schema) {
    port.kind = OperationPortKind::Result;
    port.result_schema_id = "example.gpu.tensor";
    port.result_schema_version = 1;
  }
  traits.outputs = {
      gpu_result::output(gpu_result::schema(ElementType::Float32, length))};
  traits.supports_gpu = true;
  traits.outputs[0].continuation_bytes = sizeof(SparseState);
  traits.workspace_bytes = length * 4 + 32;
  traits.outputs[0].maximum_dependency_stages = length * 2 + 1;
  definition.start_result = [](const ResultProgramQuery&,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<SparseState>(allocator);
  };
  check(registry->register_operation(definition));
  for (unsigned mode = 1; mode <= 11; ++mode) {
    auto op = definition;
    op.key = "example.atlas_failure_" + std::to_string(mode);
    op.start_result = [mode](const ResultProgramQuery&,
                             const BufferAllocator& allocator) {
      return ResultContinuation::make<SparseState>(allocator, mode);
    };
    check(registry->register_operation(op));
  }
  CancellationSource cancellation, protocol_cancellation;
  for (bool protocol : {false, true}) {
    auto op = definition;
    op.key =
        protocol ? "example.native_protocol_cancel" : "example.native_cancel";
    op.traits.outputs[0].continuation_bytes = sizeof(CancelState);
    op.start_result = [source = protocol ? protocol_cancellation : cancellation,
                       protocol](const ResultProgramQuery&,
                                 const BufferAllocator& allocator) {
      return ResultContinuation::make<CancelState>(allocator, source, protocol);
    };
    check(registry->register_operation(op));
  }
  check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {
      gpu_result::declaration(1, "data",
                              gpu_result::schema(ElementType::Float32, length)),
      gpu_result::declaration(2, "control",
                              gpu_result::schema(ElementType::Int64, length))};
  document.nodes = {{1,
                     definition.key,
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"sum", 1, "value"}};
  GraphContext graph(document);
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.result_cache_bytes = 1048576;
  ExecutionContext context(registry, config);
  const auto bindings = inputs(take(context.resource_budget()));
  for (bool gpu : {false, true}) {
    if (gpu && !context.gpu_enabled()) {
      std::cout << "CPU oracle passed; native workflow skipped\n";
      return 77;
    }
    PlanningOptions planning;
    planning.execution_mode =
        gpu ? ExecutionMode::NativeGpu : ExecutionMode::CpuExact;
    const auto plan = take(Compiler(registry).compile(graph, planning)).plan;
    const auto frozen = take(context.freeze(plan, bindings));
    const auto query = take(take(point(0).unite(point(1))).unite(point(2)));
    auto result = take(context.execute_fragments(frozen, {{"sum", query}}));
    for (std::uint64_t i = 0; i < 3; ++i)
      if (require(gpu_result::number(result.results.at("sum"), i) ==
                      2145 * (i == 1 ? 2 : 1),
                  "independent arithmetic oracle failed"))
        return 1;
    auto support = take(take(result.dependencies.restrict({{"sum", point(0)}}))
                            .source_support());
    auto reused = take(take(result.dependencies.restrict({{"sum", point(2)}}))
                           .source_support());
    if (require(take(support.at("data").element_count()) == 65 &&
                    support.at("control") == point(0) &&
                    take(support.at("data").intersect(point(1))).empty() &&
                    reused.at("control") == point(2) &&
                    reused.at("data") == support.at("data"),
                "sparse dependency evidence or current block witness failed"))
      return 1;
    const auto& diagnostics = result.diagnostics;
    std::uint64_t dispatches = 0;
    for (const auto& timing : diagnostics.operation_timings) {
      if (require(timing.backend == (gpu ? Backend::Gpu : Backend::Cpu),
                  "timing backend"))
        return 1;
      dispatches += timing.native_dispatch_count;
    }
    if (require((gpu ? diagnostics.native_dispatch_count > 0
                     : diagnostics.native_dispatch_count == 0) &&
                    diagnostics.selected_backends.at({1, 0}) ==
                        (gpu ? Backend::Gpu : Backend::Cpu) &&
                    dispatches == diagnostics.native_dispatch_count,
                "native dispatch/block/backend diagnostics"))
      return 1;
    std::cout << (gpu ? "Metal" : "CPU") << ": sums=2145,4290,2145; dispatches="
              << diagnostics.native_dispatch_count
              << "; block_hits=" << diagnostics.block_cache_hits << '\n';
    if (gpu) {
      auto fresh = take(context.freeze(plan, bindings));
      auto hit = take(context.execute_fragments(fresh, {{"sum", point(2)}}));
      if (require(gpu_result::number(hit.results.at("sum"), 2) == 2145,
                  "native result across repeated Result requests"))
        return 1;
    }
  }
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  const auto plan = take(Compiler(registry).compile(graph, planning)).plan;
  constexpr std::uint64_t source_bytes = length * 12;
  constexpr std::uint64_t minimum =
      source_bytes + sizeof(SparseState) + 4 + 12 + 12 + 8 + 65 * 4 + 32768;
  for (const auto bytes : {minimum - 1, minimum}) {
    ExecutionContextConfig bounded;
    bounded.gpu_enabled = true;
    bounded.maximum_live_bytes = bytes;
    ExecutionContext small(registry, bounded);
    const auto root = take(small.resource_budget());
    const auto bound = inputs(root);
    const auto frozen = take(small.freeze(plan, bound));
    auto result = small.execute_fragments(frozen, {{"sum", point(0)}});
    if (require(bytes == minimum
                    ? result.ok()
                    : result.status().code == ErrorCode::ResourceExhausted,
                "native atlas admission frontier"))
      return 1;
    if (bytes == minimum) {
      if (require(result.value().diagnostics.native_dispatch_count > 0 &&
                      root.statistics().peak[ResourceKind::Payload] == minimum,
                  "native allocation capacity"))
        return 1;
      result = Result<DemandResult>(Status{ErrorCode::Cancelled, {}});
      if (require(root.statistics().live[ResourceKind::Payload] == source_bytes,
                  "native owner retirement"))
        return 1;
      auto retry = take(small.execute_fragments(frozen, {{"sum", point(0)}}));
      if (require(retry.diagnostics.native_dispatch_count > 0 &&
                      gpu_result::number(retry.results.at("sum"), 0) == 2145,
                  "native retry after owner retirement"))
        return 1;
    }
  }
  std::cout << "Result atlas admission: " << minimum - 1 << " rejected, "
            << minimum << " passed and reusable\n";
  for (unsigned layout : {1U, 2U, 3U}) {
    ExecutionContext arranged(registry, config);
    const auto bound = inputs(take(arranged.resource_budget()), layout);
    const auto frozen = take(arranged.freeze(plan, bound));
    auto result = take(arranged.execute_fragments(frozen, {{"sum", point(0)}}));
    if (require(gpu_result::number(result.results.at("sum"), 0) ==
                        (layout == 2 ? 65 : 2145) &&
                    result.diagnostics.native_dispatch_count > 0,
                "negative, broadcast, or fragmented Result atlas"))
      return 1;
  }
  for (unsigned mode : {1U, 2U, 3U, 4U, 5U, 6U, 8U, 9U, 10U, 11U}) {
    auto bad_document = document;
    bad_document.nodes[0].operation =
        "example.atlas_failure_" + std::to_string(mode);
    GraphContext bad_graph(bad_document);
    auto selected = planning;
    if (mode == 2)
      selected.execution_mode = ExecutionMode::CpuExact;
    const auto bad_plan =
        take(Compiler(registry).compile(bad_graph, selected)).plan;
    ExecutionContext isolated(registry, config);
    const auto root = take(isolated.resource_budget());
    const auto bound = inputs(root);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
      const auto frozen = take(isolated.freeze(bad_plan, bound));
      auto failed = isolated.execute_fragments(frozen, {{"sum", point(0)}});
      const auto expected = mode == 9 ? ErrorCode::ResourceExhausted
                            : mode == 1 || mode == 6
                                ? ErrorCode::OperationFailed
                                : ErrorCode::InvalidArgument;
      if (require(
              failed.status().code == expected &&
                  (mode < 8 || mode == 9 ||
                   (failed.status().reason == FailureReason::UnauthorizedRead &&
                    failed.status().detail.origin == FailureOrigin::Protocol &&
                    failed.status().detail.scope == FailureScope::Group)) &&
                  isolated.cache_statistics().entries == 0 &&
                  root.statistics().live[ResourceKind::Payload] == source_bytes,
              "atlas protocol failure, cache isolation, or owner retirement")) {
        std::cerr << "atlas mode=" << mode
                  << " status=" << static_cast<int>(failed.status().code) << " "
                  << failed.status().message << '\n';
        return 1;
      }
    }
    const auto frozen = take(isolated.freeze(plan, bound));
    auto recovered =
        take(isolated.execute_fragments(frozen, {{"sum", point(0)}}));
    if (require(recovered.diagnostics.native_dispatch_count > 0 &&
                    gpu_result::number(recovered.results.at("sum"), 0) == 2145,
                "fresh native retry after atlas failure"))
      return 1;
  }
  {
    auto empty_document = document;
    empty_document.nodes[0].operation = "example.atlas_failure_7";
    GraphContext empty_graph(empty_document);
    const auto empty_plan =
        take(Compiler(registry).compile(empty_graph, planning)).plan;
    ExecutionContext empty_context(registry, config);
    const auto root = take(empty_context.resource_budget());
    const auto bound = inputs(root, 4);
    const auto frozen = take(empty_context.freeze(empty_plan, bound));
    auto output = take(empty_context.execute_fragments(
        frozen, {{"sum", take(Footprint::none({length}))}}));
    if (require(take(output.results.at("sum").descriptor())
                        .tensor_coverage(0)
                        .empty() &&
                    output.diagnostics.native_dispatch_count == 0 &&
                    root.statistics().live[ResourceKind::Payload] == length * 8,
                "empty atlas reads no unavailable source samples and retires "
                "native buffers"))
      return 1;
  }
  for (bool protocol : {false, true}) {
    document.nodes[0].operation =
        protocol ? "example.native_protocol_cancel" : "example.native_cancel";
    GraphContext cancelled_graph(document);
    const auto cancelled_plan =
        take(Compiler(registry).compile(cancelled_graph, planning)).plan;
    ExecutionOptions options;
    options.dependencies.sets.cancellation =
        (protocol ? protocol_cancellation : cancellation).token();
    unsigned delivered = 0;
    options.result_publication = [&](ValueRef, const ResultRef&) {
      ++delivered;
      return Status::success();
    };
    auto result = context.execute(cancelled_plan, bindings, {}, options);
    const auto& status = result.status();
    if (require(
            delivered == 0 &&
                (protocol
                     ? status.code == ErrorCode::InvalidArgument &&
                           status.reason == FailureReason::UnauthorizedRead &&
                           status.detail.origin == FailureOrigin::Protocol
                     : status.code == ErrorCode::Cancelled),
            "native cancellation/protocol priority or publication"))
      return 1;
  }
  if (spatial_owner())
    return 1;
  return 0;
} catch (const gpu_result::Failure& failed) {
  std::cerr << static_cast<int>(failed.status.code) << ": "
            << failed.status.message << '\n';
  return 1;
} catch (const std::exception& failed) {
  std::cerr << failed.what() << '\n';
  return 1;
}
