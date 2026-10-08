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
Footprint point(std::uint64_t at) {
  return Footprint::from_regions({16}, {Region({{at, 1}})}).take_value();
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
            std::array<std::uint8_t, 304> records{};
            auto* bytes = records.data();
            word(bytes, mode == 18 ? 0 : 2);
            for (unsigned i = 0; i < 2; ++i) {
              auto* row = bytes + 16 + i * 144;
              word(row + 4, 1);
              word(row + 8, 1);
              word(row + 16, i == 0 ? 1 : 9, 8);
              word(row + 80, 1, 8);
            }
            if (mode == 1) {
              word(bytes, 3);
              word(bytes + 4, 1);
            }
            if (mode == 2)
              word(bytes, 3);
            if (mode == 3)
              word(bytes + 16 + 24, 1, 8);
            if (mode == 4)
              word(bytes + 16 + 8, 0);
            if (mode == 5)
              word(bytes + 16 + 4, 16);
            if (mode == 6)
              word(bytes + 16 + 80, 0, 8);
            if (mode == 7)
              word(bytes + 16 + 16, 16, 8);
            if (mode == 8)
              word(bytes + 8, 1);
            if (mode == 20 || mode == 23)
              word(bytes + 16 + 144 + 4, 2);
            if (mode == 24 || mode == 25) {
              for (unsigned i = 0; i < 2; ++i) {
                auto* row = bytes + 16 + i * 144;
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
              word(bytes, 4);  // Exceeds declared attempts.
            if (mode == 26)
              cancellation->cancel();
            const char shader[] =
                "#include <metal_stdlib>\nusing namespace metal;\n"
                "kernel void emit(device uint* t [[buffer(0)]],"
                "constant uint* r [[buffer(1)]], uint i "
                "[[thread_position_in_grid]])"
                "{if(i<76)t[i]=r[i];}";
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
            dispatch.grid[0] = 76;
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
          oversized.tensors.push_back({0, 0, point(1), 1});
        return Result<ResultProgramPoll>(std::move(oversized));
      }
      if (mode != 10 && mode != 18)
        return Result<ResultProgramPoll>(ResultProgramNeed{});
    }
    float sum = 0;
    if (mode == 25) {
      if (phase.tensors->at({0, 0}).coverage().element_count().take_value() !=
          2)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::Internal, "rank-8 coverage"});
      float value = 0;
      for (unsigned i = 0; i < 2; ++i) {
        multi_result::check(phase.read_tensor(
            0, 0, std::vector<std::uint64_t>(8, i ? UINT64_C(1) << 39 : 0),
            &value, 4));
        sum += value;
      }
    } else if (mode != 10 && mode != 18) {
      for (const auto at : {1, 9}) {
        float value = 0;
        auto status = phase.read_tensor(0, 0, {static_cast<std::uint64_t>(at)},
                                        &value, 4);
        if (!status.ok())
          return Result<ResultProgramPoll>(status);
        sum += value;
      }
    }
    auto builder = multi_result::take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        std::vector<std::uint64_t>(phase.association->begin(),
                                   phase.association->end())));
    multi_result::check(builder.bind_descriptor_relation(
        multi_result::take(ResultRelation::cartesian(phase.resources, 1, {}))));
    const auto shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    auto relation =
        multi_result::take(ResultRelation::cartesian(phase.resources, 16, {}));
    if (mode != 18 && mode != 10 && mode != 25) {
      auto first = multi_result::take(ResultRelation::cartesian(
          phase.resources, 16, {0, 1, 1, 1, ResultSupportTarget::Tensor, 0}));
      auto second = multi_result::take(ResultRelation::cartesian(
          phase.resources, 16, {0, 1, 9, 1, ResultSupportTarget::Tensor, 0}));
      relation = multi_result::take(
          ResultRelation::unite(phase.resources, {first, second}));
    }
    if (mode == 25) {
      const auto input_shape =
          phase.query.inputs[0].result_schema->tensors[0].sample_shape();
      std::vector<ResultRelation> supports;
      for (unsigned i = 0; i < 2; ++i) {
        std::vector<ResultMappedAxis> axes(
            8, {-1, i ? UINT64_C(1) << 39 : 0, 1, 1});
        supports.push_back(multi_result::take(ResultRelation::mapped(
            phase.resources, shape, Region::whole(shape), input_shape, axes,
            {0, 1, 0, 0, ResultSupportTarget::Tensor, 0})));
      }
      relation =
          multi_result::take(ResultRelation::unite(phase.resources, supports));
    }
    const auto output = phase.query.tensor_outputs.value_or(
        multi_result::take(Footprint::all(shape)));
    for (const auto& box : output.boxes()) {
      std::vector<float> values(box.element_count().take_value(), sum);
      multi_result::check(builder.publish_tensor(
          0, box,
          {reinterpret_cast<const std::uint8_t*>(values.data()),
           values.size() * 4},
          relation, {true, true, true, true}));
    }
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
    op.traits.outputs[0].observation_kind = ObservationKind::RequestRecord;
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
    ResourceVector<ResultIoReply> io;
    const auto allocator = root.allocator();
    ResultProgramPhase phase{
        query,  inputs,
        io,     allocator,
        root,   [&](std::uint64_t units) { return root.consume({units}); },
        failure};
    Status observed;
    phase.failure_observer = [&](const Status& status) { observed = status; };
    phase.discover = [&](std::uint32_t, std::uint32_t, const auto& compute)
        -> Result<std::shared_ptr<const ResultDiscoveryReceipt>> {
      // This intentionally mocked host exercises only callback fencing.
      // Native table decoding and Need attachment run below on real Metal.
      if (throws) {
        auto table = multi_result::take(allocator.allocate(304));
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
int run(unsigned mode, ErrorCode expected, std::uint64_t work = 1048576,
        std::uint32_t capacity = 65536) {
  auto registry = std::make_shared<OperationRegistry>();
  const auto output_schema = multi_result::schema(ElementType::Float32, {16});
  auto input_schema = output_schema;
  if (mode == 24) {
    auto& tensor = input_schema.tensors[0];
    tensor.descriptor.shape = {1, 1, 4};
    tensor.layout.spatial = true;
    tensor.layout.channel_axis = 2;
    tensor.facets = {encode_semantic(rgba_semantics()).take_value()};
  }
  if (mode == 25)
    input_schema.tensors[0].descriptor.shape =
        std::vector<std::uint64_t>(8, UINT64_C(1) << 40);
  CancellationSource cancellation;
  OperationDefinition definition;
  definition.key = "test.gpu.discovery";
  definition.traits.input_count = 1;
  definition.traits.input_schema.resize(1);
  auto& input = definition.traits.input_schema[0];
  input.kind = OperationPortKind::Result;
  input.result_schema_id = input_schema.id;
  input.result_schema_version = input_schema.version;
  definition.traits.supports_cpu = false;
  definition.traits.supports_gpu = true;
  definition.traits.workspace_bytes = 16384;
  auto& output = definition.traits.outputs[0];
  output = multi_result::output("value", output_schema);
  output.continuation_bytes = sizeof(State);
  output.maximum_dependency_stages = 3;
  if (mode == 28)
    output.observation_kind = ObservationKind::RequestRecord;
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
    auto builder =
        multi_result::take(ResultBuilder::start(root, input_schema, "large"));
    multi_result::check(builder.bind_descriptor_relation(
        multi_result::take(ResultRelation::cartesian(root, 1, {}))));
    auto shape = input_schema.tensors[0].sample_shape();
    for (unsigned i = 0; i < 2; ++i) {
      std::vector<RegionDimension> dims(8, {i ? UINT64_C(1) << 39 : 0, 1});
      const float value = 21;
      multi_result::check(builder.publish_tensor(
          0, Region(dims), {reinterpret_cast<const std::uint8_t*>(&value), 4},
          multi_result::take(ResultRelation::cartesian(root, UINT64_MAX, {})),
          {true, true, true, true}));
    }
    input_result = multi_result::take(builder.seal());
  } else {
    input_result =
        multi_result::binding(root, "input", 21, input_schema).result;
  }
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", input_schema)};
  document.nodes = {{1, "test.gpu.discovery", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"out", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto compiled =
      multi_result::take(Compiler(registry).compile(graph, planning));
  auto frozen = multi_result::take(
      context.freeze(compiled.plan, {{{"input", input_result}}}));
  ExecutionOptions options;
  options.dependencies.maximum_work = work;
  options.dependencies.maximum_gpu_requests = capacity;
  if (mode == 22)
    options.dependencies.sets.maximum_boxes = 32;
  if (mode == 23)
    options.dependencies.sets.maximum_boxes = 4;
  const auto demand =
      mode == 28 ? point(0).unite(point(1)).take_value() : point(0);
  const auto before = root.statistics().live[ResourceKind::Payload];
  {
    auto progress = context.execute_fragments(frozen, {{"out", demand}},
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
      PS_CHECK(progress.value().diagnostics.native_dispatch_count == 1);
      const auto result = progress.value().results.at("out");
      float sum = 0;
      PS_CHECK(
          result.read_tensor(result.descriptor().take_value(), 0, {0}, &sum, 4)
              .ok());
      PS_CHECK(sum == (mode == 18 ? 0 : 42));
      PS_CHECK(result.descriptor().take_value().tensor_coverage(0) == demand);
      if (mode == 28)
        PS_CHECK(
            result
                .read_tensor(result.descriptor().take_value(), 0, {1}, &sum, 4)
                .ok() &&
            sum == 42);
      if (mode == 25) {
        const auto support =
            progress.value().dependencies.source_support().take_value();
        const auto input_shape = input_schema.tensors[0].sample_shape();
        auto first =
            Footprint::from_regions(
                input_shape, {Region(std::vector<RegionDimension>(8, {0, 1}))})
                .take_value();
        auto second = Footprint::from_regions(
                          input_shape, {Region(std::vector<RegionDimension>(
                                           8, {UINT64_C(1) << 39, 1}))})
                          .take_value();
        PS_CHECK(support.at("input") == first.unite(second).take_value());
        const auto dirty = progress.value()
                               .dependencies
                               .potential_dirty("input", second, 1, {},
                                                ResultSupportTarget::Tensor, 0)
                               .take_value();
        PS_CHECK(dirty.at("out") == demand);
      } else if (mode != 18) {
        const auto support =
            progress.value().dependencies.source_support().take_value();
        PS_CHECK(support.at("input") == point(1).unite(point(9)).take_value());
      }
    }
  }
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == before);
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(mock_callbacks() == 0);
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
  const auto native = run(0, ErrorCode::Ok);
  if (native == 77) {
    std::cout
        << "Footprint mock admission passed; native Result discovery skipped\n";
    return 77;
  }
  PS_CHECK(native == 0);
  PS_CHECK(run(1, ErrorCode::ResourceExhausted) == 0);
  for (unsigned mode : {2, 3, 4, 5, 6, 7, 8, 9, 10, 14, 15})
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
  std::cout
      << "Native Result discovery with GPU-emitted fixture records passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
