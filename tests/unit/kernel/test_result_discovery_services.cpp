#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
void word(std::uint8_t* bytes, std::uint64_t value, unsigned width = 4) {
  for (unsigned i = 0; i < width; ++i)
    bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
}
struct State {
  State(unsigned mode, CancellationSource* cancellation)
      : cancellation(cancellation), mode(mode) {}
  CancellationSource* cancellation;
  unsigned mode;
  bool waiting = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (phase.gpu && phase.gpu->backend != PS_GPU_BACKEND_METAL_V1)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::BackendUnavailable, "fixture requires Metal"});
    if (!waiting) {
      waiting = true;
      auto status =
          phase.discover(2, 3, [&](const ResultGpuRequestTable& table) {
            if (mode == 14) {
              return phase
                  .discover(1, 1, [](const auto&) { return Status::success(); })
                  .status();
            }
            if (mode == 15) {
              auto incoming = multi_result::binding(
                                  phase.resources, "state", 0,
                                  multi_result::schema(ElementType::Float32))
                                  .result;
              return phase
                  .block(1, 0, 1, 1, incoming,
                         [&] { return Result<ResultRef>(incoming); })
                  .status();
            }
            if (mode == 16)
              throw std::runtime_error("discovery callback exception");
            std::array<std::uint8_t, 80> records{};
            auto* bytes = records.data();
            word(bytes, mode == 18 ? 0 : 2);
            for (unsigned i = 0; i < 2; ++i) {
              auto* row = bytes + 16 + i * 32;
              word(row, (mode == 20 || mode == 23) ? i : 0);
              word(row + 16,
                   mode == 24   ? 0
                   : mode == 27 ? 1
                   : mode == 25 ? (i ? UINT64_C(1) << 39 : 0)
                                : (i ? 9 : 1),
                   8);
              std::uint64_t begin = 0;
              for (unsigned b = 0; b < 8; ++b)
                begin |= static_cast<std::uint64_t>(row[16 + b]) << (8 * b);
              word(row + 24, begin + 1, 8);
            }
            if (mode == 1) {
              word(bytes, 3);
              word(bytes + 4, 1);
            }
            if (mode == 2)
              word(bytes, 3);
            if (mode == 3)
              word(bytes + 16 + 4, 3);
            if (mode == 4)
              word(bytes + 16 + 4, 1);
            if (mode == 5)
              word(bytes + 16 + 4, 2);
            if (mode == 6)
              word(bytes + 16, 9);
            if (mode == 7)
              word(bytes + 16 + 24, 17, 8);
            if (mode == 8)
              word(bytes + 8, 1);
            if (mode == 9)
              word(bytes, 4);  // Exceeds declared attempts.
            if (mode == 26)
              cancellation->cancel();
            const char shader[] =
                "#include <metal_stdlib>\nusing namespace metal;\n"
                "kernel void emit(device uint* t [[buffer(0)]],"
                "constant uint* r [[buffer(1)]], uint i "
                "[[thread_position_in_grid]])"
                "{if(i<20)t[i]=r[i];}";
            const auto* api = phase.gpu;
            std::uint64_t token = 0;
            if (api->buffer(api->context, table.bytes, table.byte_size, 1,
                            &token))
              return phase.gpu_status();
            const ps_gpu_buffer_binding_v1 binding{sizeof(binding), 0, token, 0,
                                                   table.byte_size, 1};
            ps_gpu_dispatch_v1 dispatch{};
            dispatch.struct_size = sizeof(dispatch);
            dispatch.source = shader;
            dispatch.source_size = sizeof(shader) - 1;
            dispatch.entry = "emit";
            dispatch.entry_size = 4;
            dispatch.buffers = &binding;
            dispatch.buffer_count = 1;
            dispatch.constants = records.data();
            dispatch.constant_size = records.size();
            dispatch.constant_index = 1;
            dispatch.grid[0] = 20;
            dispatch.grid[1] = dispatch.grid[2] = 1;
            if (api->execute(api->context, &dispatch, 1))
              return phase.gpu_status();
            return phase.gpu_status();
          });
      if ((mode == 15 || mode == 23 || mode == 24) && !status.ok())
        return Result<ResultProgramPoll>(status.status());
      // Ignored errors must still prevent a terminal value or Need publication.
      static_cast<void>(status);
      if (mode == 22) {
        ResultProgramNeed oversized;
        for (unsigned i = 0; i < 256; ++i)
          oversized.tiles.push_back({0, 0, false,
                                     multi_result::take(TileSet::from_indices(
                                         phase.resources, {1}))});
        return Result<ResultProgramPoll>(std::move(oversized));
      }
    }
    float sum = 0;
    if (mode != 10 && mode != 18) {
      if (mode == 24 &&
          phase.tensors->at({0, 0}).coverage().element_count().take_value() !=
              4)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::Internal, "channel tile coverage"});
      if (mode == 25 &&
          phase.tensors->at({0, 0}).coverage().element_count().take_value() !=
              2)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::Internal, "64-bit tile coverage"});
      for (unsigned i = 0; i < (mode == 24 ? 4U : 2U); ++i) {
        float value = 0;
        const TileIndex at =
            mode == 25 ? (i ? UINT64_C(1) << 39 : 0) : (i ? 9 : 1);
        const std::vector<std::uint64_t> coordinate =
            mode == 24 ? std::vector<std::uint64_t>{0, 0, i}
                       : std::vector<std::uint64_t>{0, at};
        const unsigned port = (mode == 20 || mode == 23) ? i : 0;
        auto status = phase.read_tensor(port, 0, coordinate, &value, 4);
        if (!status.ok())
          return Result<ResultProgramPoll>(status);
        if (i < 2)
          sum += value;
      }
    }
    auto builder = multi_result::take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));

    const auto box = multi_result::take(
        phase.query.output_tile_grids.at(phase.query.tensor_slot)
            .region(phase.query.tile_unit));
    multi_result::check(builder.publish_tensor(
        0, box, {reinterpret_cast<const std::uint8_t*>(&sum), 4}));
    return Result<ResultProgramPoll>(
        ResultPublication{multi_result::take(builder.seal()), true});
  }
};
struct MockDiscovery {
  bool throws;
  explicit MockDiscovery(bool throws) : throws(throws) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto ignored = phase.discover(2, 3, [&](const auto&) {
      if (throws)
        throw std::runtime_error("mock discovery callback exception");
      return Status::success();
    });
    static_cast<void>(ignored);
    return Result<ResultProgramPoll>(ResultProgramNeed{});
  }
};
int mock_callbacks() {
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  auto schema = multi_result::schema(ElementType::Float32);
  for (bool throws : {false, true}) {
    OperationRegistry registry;
    OperationDefinition op;
    op.key = "test.mock.discovery";
    op.traits.outputs[0] = multi_result::output("value", schema);
    op.traits.outputs[0].continuation_bytes = sizeof(MockDiscovery);
    op.start_result = [throws](const auto&, const auto& allocator) {
      return ResultContinuation::make<MockDiscovery>(allocator, throws);
    };
    multi_result::check(registry.register_operation(std::move(op)));
    ResultProgramMetadata metadata;
    metadata.output.result_schema =
        std::make_shared<const SchemaTemplate>(schema);
    const std::map<std::string, ParameterValue> parameters;
    ResultProgramQuery query(metadata, parameters);
    query.semantic_key = "test.mock.discovery";
    auto session = multi_result::take(
        registry.start_result("test.mock.discovery", query, root.allocator()));
    auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
    ResultObjectInputs inputs;
    const auto allocator = root.allocator();
    ResultProgramPhase phase{
        query,
        inputs,
        allocator,
        root,
        [&](std::uint64_t units) { return root.consume({units}); },
        failure};
    Status observed;
    phase.failure_observer = [&](const Status& status) { observed = status; };
    phase.discover = [&](std::uint32_t, std::uint32_t, const auto& compute)
        -> Result<std::shared_ptr<const ResultDiscoveryReceipt>> {
      // This intentionally mocked host exercises only callback fencing.
      // Native table decoding and static grant checks run below on real Metal.
      if (throws) {
        auto table = multi_result::take(allocator.allocate(48));
        static_cast<void>(compute({table.data(), table.size(), 2}));
      }
      failure->store(ErrorCode::ResourceExhausted);
      return Result<std::shared_ptr<const ResultDiscoveryReceipt>>(
          Status{ErrorCode::ResourceExhausted, "mock table admission"});
    };
    auto answer = session.poll(phase);
    const auto expected =
        throws ? ErrorCode::OperationFailed : ErrorCode::ResourceExhausted;
    PS_CHECK(answer.status().code == expected);
    if (throws)
      PS_CHECK(observed.reason == FailureReason::HostException);
    PS_CHECK(session.poll(phase).status().code == expected);
    PS_CHECK(root.statistics().live[ResourceKind::Payload] ==
             sizeof(MockDiscovery));
  }
  return 0;
}
ResultRef sparse_input(const ResourceBudget& root, const SchemaTemplate& schema,
                       float high = 21) {
  auto builder =
      multi_result::take(ResultBuilder::start(root, schema, "large"));

  for (unsigned i = 0; i < 2; ++i) {
    const float value = i ? high : 21;
    const Region region({{0, 1}, {i ? UINT64_C(1) << 39 : 0, 1}});
    auto bytes = multi_result::take(root.allocator().allocate(4));
    std::memcpy(bytes.data(), &value, 4);
    multi_result::check(builder.publish_tensor(
        0, region, StridedLayout{0, {4, 4}, {0, i ? UINT64_C(1) << 39 : 0}},
        std::move(bytes).freeze()));
  }
  return multi_result::take(builder.seal());
}
int run(unsigned mode, ErrorCode expected, std::uint64_t work = 1048576,
        std::uint32_t capacity = 65536) {
  auto registry = std::make_shared<OperationRegistry>();
  auto output_schema = multi_result::schema(ElementType::Float32, {1, 16});
  output_schema.tensors[0].layout.spatial = true;
  output_schema.tensors[0].layout.channel_axis.reset();
  auto input_schema = output_schema;
  if (mode == 24 || mode == 27) {
    auto& tensor = input_schema.tensors[0];
    tensor.descriptor.shape = {1, 1, 4};
    tensor.layout.spatial = true;
    tensor.layout.channel_axis = 2;
    tensor.atomic_trailing_axes = 1;
  }
  if (mode == 25)
    input_schema.tensors[0].descriptor.shape = {1, UINT64_C(1) << 40};
  CancellationSource cancellation;
  OperationDefinition definition;
  definition.key = "test.gpu.discovery";
  const bool two_inputs = mode == 20 || mode == 23;
  definition.traits.input_count = two_inputs ? 2 : 1;
  definition.traits.input_schema.resize(definition.traits.input_count);
  auto& input = definition.traits.input_schema[0];

  input.result_schema_id = input_schema.id;
  input.result_schema_version = input_schema.version;
  if (two_inputs)
    definition.traits.input_schema[1] = input;
  definition.traits.supports_cpu = false;
  definition.traits.supports_gpu = true;
  definition.traits.workspace_bytes = 16384;
  auto& output = definition.traits.outputs[0];
  output = multi_result::output("value", output_schema);
  output.continuation_bytes = sizeof(State);
  output.maximum_dependency_stages = 3;
  definition.map_dependencies = [mode, two_inputs](const ResultStaticQuery&) {
    ResultStaticMapping mapping;
    if (mode != 18) {
      for (std::uint32_t port = 0; port < (two_inputs ? 2U : 1U); ++port) {
        const auto tiles =
            mode == 25 ? std::vector<TileIndex>{0, UINT64_C(1) << 39}
            : mode == 24 || mode == 27 ? std::vector<TileIndex>{0}
            : two_inputs               ? std::vector<TileIndex>{port ? 9U : 1U}
                                       : std::vector<TileIndex>{1, 9};
        mapping.inputs.push_back({port, 0, false,
                                  multi_result::take(TileSet::from_indices(
                                      ResourceBudget{}, tiles))});
      }
    }
    return Result<ResultStaticMapping>(std::move(mapping));
  };
  definition.start_result = [&](const auto&, const auto& allocator) {
    return ResultContinuation::make<State>(allocator, mode, &cancellation);
  };
  multi_result::check(registry->register_operation(std::move(definition)));
  multi_result::check(registry->freeze());
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  if (!context.gpu_enabled())
    return 77;
  const auto root = context.resource_budget().take_value();
  ResultRef input_result;
  if (mode == 25) {
    input_result = sparse_input(root, input_schema);
  } else if (mode == 24 || mode == 27) {
    auto builder = multi_result::take(
        ResultBuilder::start(root, input_schema, "rgba.input"));
    const float rgba[] = {21, 21, 21, 1};
    multi_result::check(builder.publish_tensor(
        0, Region::whole({1, 1, 4}),
        {reinterpret_cast<const std::uint8_t*>(rgba), sizeof(rgba)}));
    input_result = multi_result::take(builder.seal());
  } else {
    input_result =
        multi_result::binding(root, "input", 21, input_schema).result;
  }
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", input_schema)};
  document.nodes = {{1, "test.gpu.discovery", {WorkflowInputReference{1}}, {}}};
  if (two_inputs) {
    document.inputs.push_back(
        multi_result::declaration(2, "input2", input_schema));
    document.nodes[0].inputs.push_back(WorkflowInputReference{2});
  }
  document.outputs = {{"out", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  planning.tile_extent = 1;
  auto compiled =
      multi_result::take(Compiler(registry).compile(graph, planning));
  ExecutionBindings bindings{{{"input", input_result}}};
  if (two_inputs)
    bindings.inputs.push_back({"input2", input_result});
  auto run_plan = compiled.plan.value();
  ExecutionBindings run_bindings = bindings;
  ExecutionOptions options;
  options.dependencies.maximum_work = work;
  options.dependencies.maximum_gpu_requests = capacity;
  if (mode == 22)
    options.dependencies.sets.maximum_boxes = 32;
  if (mode == 23)
    options.dependencies.sets.maximum_boxes = 4;
  const TileRequest demand{
      {{0, multi_result::take(TileSet::from_indices(
               root, mode == 28 ? std::vector<TileIndex>{0, 1}
                                : std::vector<TileIndex>{0}))}}};
  const auto before = root.statistics().live[ResourceKind::Payload];
  {
    auto progress =
        context.execute_fragments(run_plan, run_bindings, {{"out", demand}},
                                  cancellation.token(), options);
    if (progress.status().code == ErrorCode::BackendUnavailable &&
        progress.status().message == "fixture requires Metal")
      return 77;
    if (progress.status().code != expected)
      std::cerr << "mode=" << mode
                << ", code=" << static_cast<int>(progress.status().code)
                << ", message=" << progress.status().message << '\n';
    PS_CHECK(progress.status().code == expected);
    if (progress.ok()) {
      PS_CHECK(progress.value().diagnostics.native_dispatch_count ==
               (mode == 28 ? 2 : 1));
      const auto result = progress.value().results.at("out");
      float sum = 0;
      PS_CHECK(
          result
              .read_tensor(result.descriptor().take_value(), 0, {0, 0}, &sum, 4)
              .ok());
      PS_CHECK(sum == (mode == 10 || mode == 18 ? 0 : 42));
      for (TileIndex tile : demand.members.at(0)) {
        PS_CHECK(result
                     .read_tensor(multi_result::take(result.descriptor()), 0,
                                  {0, tile}, &sum, 4)
                     .ok());
        PS_CHECK(sum == (mode == 10 || mode == 18 ? 0 : 42));
        auto record =
            multi_result::take(progress.value().dependencies.tile_record(
                {compiled.plan.value()
                     .return_signature()
                     .at(compiled.plan.value().outputs().at("out"))
                     .record_index,
                 0, 0, tile}));
        PS_CHECK(record && record->inputs.size() == (mode == 18   ? 0
                                                     : two_inputs ? 2
                                                                  : 1));
        for (const auto& need : record->inputs) {
          const auto* source = std::get_if<PlanWorkflowInput>(&need.producer);
          PS_CHECK(source && source->declaration_index < (two_inputs ? 2 : 1));
          const auto expected_tiles =
              mode == 25   ? std::vector<TileIndex>{0, UINT64_C(1) << 39}
              : mode == 24 ? std::vector<TileIndex>{0}
              : two_inputs
                  ? std::vector<TileIndex>{source->declaration_index ? 9U : 1U}
                  : std::vector<TileIndex>{1, 9};
          PS_CHECK(need.producer_output == 0 &&
                   need.tiles == multi_result::take(TileSet::from_indices(
                                     root, expected_tiles)));
        }
      }
      if (mode == 25) {
        auto handle = multi_result::take(
            context.open_demand(compiled.plan.value(), bindings));
        auto observed = multi_result::take(handle.request({{"out", demand}}));
        auto updated = multi_result::take(handle.replace_bindings(
            {{{"input", sparse_input(root, input_schema, 22)}}}));
        PS_CHECK(updated.potential_dirty.at("out") == demand);
        auto changed = multi_result::take(handle.request({{"out", demand}}));
        float new_sum = 0;
        PS_CHECK(changed.results.at("out")
                     .read_tensor(multi_result::take(
                                      changed.results.at("out").descriptor()),
                                  0, {0, 0}, &new_sum, 4)
                     .ok() &&
                 new_sum == 43);
      }
      float outside = 0;
      PS_CHECK(!result
                    .read_tensor(multi_result::take(result.descriptor()), 0,
                                 {0, 2}, &outside, 4)
                    .ok());
    }
  }
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == before);
  return 0;
}
}  // namespace
int main(int argc, char** argv) try {
  if (argc == 1) {
    PS_CHECK(mock_callbacks() == 0);
    std::uint64_t used = UINT64_MAX;
    const std::vector<Region> boxes{Region({{1, 1}}), Region({{9, 1}})};
    PS_CHECK(Footprint::from_regions({16}, boxes, {}, &used).ok() && used > 1);
    FootprintLimits grant;
    grant.maximum_work = used - 1;
    std::uint64_t failed = UINT64_MAX;
    PS_CHECK(
        Footprint::from_regions({16}, boxes, grant, &failed).status().code ==
            ErrorCode::ResourceExhausted &&
        failed == used - 1);
    PS_CHECK(!Footprint::from_regions({0}, boxes, {}, &failed).ok() &&
             failed == 0);
    return 0;
  }
  PS_CHECK(argc == 2 && std::string(argv[1]) == "--native");
  const auto native = run(0, ErrorCode::Ok);
  if (native == 77) {
    std::cout << "Native Result discovery unavailable: skipped\n";
    return 77;
  }
  PS_CHECK(native == 0);
  PS_CHECK(run(1, ErrorCode::ResourceExhausted) == 0);
  for (unsigned mode : {2, 3, 4, 5, 6, 7, 8, 9, 14, 15})
    PS_CHECK(run(mode, ErrorCode::InvalidArgument) == 0);
  PS_CHECK(run(16, ErrorCode::OperationFailed) == 0);
  PS_CHECK(run(10, ErrorCode::Ok) == 0);
  PS_CHECK(run(18, ErrorCode::Ok) == 0);
  PS_CHECK(run(22, ErrorCode::ResourceExhausted) == 0);
  PS_CHECK(run(23, ErrorCode::ResourceExhausted) == 0);
  PS_CHECK(run(24, ErrorCode::Ok) == 0);
  PS_CHECK(run(27, ErrorCode::InvalidArgument) == 0);
  PS_CHECK(run(25, ErrorCode::Ok) == 0);
  PS_CHECK(run(26, ErrorCode::Cancelled) == 0);
  PS_CHECK(run(28, ErrorCode::Ok) == 0);
  PS_CHECK(run(0, ErrorCode::ResourceExhausted, 64) == 0);
  PS_CHECK(run(20, ErrorCode::Ok) == 0);
  // Three candidates, two table passes, and record/group normalization all
  // consume the same actor work grant; the second input cannot reset it.
  PS_CHECK(run(20, ErrorCode::ResourceExhausted, 168) == 0);
  PS_CHECK(run(0, ErrorCode::ResourceExhausted, 1048576, 0) == 0);
  std::cout
      << "Native Result discovery with GPU-emitted fixture records passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
