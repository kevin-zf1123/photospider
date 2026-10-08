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
using Poll = Result<ResultProgramPoll>;
struct MockFailure {
  unsigned mode;
  explicit MockFailure(unsigned mode) : mode(mode) {}
  Poll poll(const ResultProgramPhase& phase) {
    if (mode == 0) {
      static_cast<void>(phase.read_tensor(0, 0, {0}, nullptr, 4));
    } else if (mode == 1) {
      static_cast<void>(phase.acquire_native_atlas(0, 0));
    } else {
      try {
        static_cast<void>(phase.allocator.allocate(4));
      } catch (...) {
      }
    }
    return Poll(ResultProgramNeed{});
  }
};
int mock_failures() {
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  const auto schema = multi_result::schema(ElementType::Float32);
  for (unsigned mode : {0, 1, 2, 3}) {
    OperationRegistry registry;
    OperationDefinition op;
    op.key = "test.mock.failure";
    op.traits.outputs[0] = multi_result::output("value", schema);
    op.traits.outputs[0].observation_kind = ObservationKind::RequestRecord;
    op.traits.outputs[0].continuation_bytes = sizeof(MockFailure);
    op.start_result = [mode](const auto&, const auto& allocator) {
      return ResultContinuation::make<MockFailure>(allocator, mode);
    };
    multi_result::check(registry.register_operation(std::move(op)));
    ResultProgramMetadata metadata;
    metadata.output.result_schema =
        std::make_shared<const SchemaTemplate>(schema);
    const std::map<std::string, ParameterValue> parameters;
    ResultProgramQuery query(metadata, parameters);
    query.semantic_key = "test.mock";
    auto continuation = multi_result::take(
        registry.start_result("test.mock.failure", query, root.allocator()));
    auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
    BufferAllocator throwing(
        [mode](std::uint64_t) -> Result<std::shared_ptr<void>> {
          if (mode == 3)
            throw std::bad_alloc();
          throw std::runtime_error("mock host materialization exception");
        });
    auto allocator =
        throwing.limited(4096, [&](ErrorCode code) { failure->store(code); });
    ResultObjectInputs inputs;
    ResourceVector<ResultIoReply> io;
    ResultProgramPhase phase{
        query,  inputs,
        io,     allocator,
        root,   [&](std::uint64_t units) { return root.consume({units}); },
        failure};
    const auto expected = mode < 2    ? ErrorCode::InvalidArgument
                          : mode == 2 ? ErrorCode::OperationFailed
                                      : ErrorCode::ResourceExhausted;
    auto answer = continuation.poll(phase);
    PS_CHECK(!answer.ok() && answer.status().code == expected);
    PS_CHECK(failure->load() == expected);
    // Direct phases are mock hosts: the second poll observes the sticky error.
    PS_CHECK(continuation.poll(phase).status().code == expected);
  }
  return 0;
}
struct State {
  unsigned mode;
  std::weak_ptr<const CpuStorage>* payload;
  bool supplied = false;
  State(unsigned mode, std::weak_ptr<const CpuStorage>* payload)
      : mode(mode), payload(payload) {}
  Poll poll(const ResultProgramPhase& phase) {
    if (phase.gpu && phase.gpu->backend != PS_GPU_BACKEND_METAL_V1)
      return Poll(
          Status{ErrorCode::BackendUnavailable, "fixture requires Metal"});
    if (!supplied) {
      supplied = true;
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0, *phase.query.tensor_outputs, mode == 7 ? 8u : 1u});
      return Poll(std::move(need));
    }
    if (mode == 3) {
      std::uint64_t token = 0;
      static_cast<void>(
          phase.gpu->buffer(phase.gpu->context, nullptr, 0, 0, &token));
    } else if (mode == 4) {
      static_cast<void>(phase.gpu->execute(phase.gpu->context, nullptr, 0));
    } else {
      auto atlas = phase.acquire_native_atlas(mode == 1 ? 1 : 0, 0);
      if (atlas.ok()) {
        auto again = phase.acquire_native_atlas(0, 0);
        if (!again.ok() || again.value() != atlas.value())
          return Poll(Status{ErrorCode::Internal, "atlas repeat differs"});
        *payload = atlas.value()->payload.storage();
        // Native computation reads the packed atlas payload and publishes 7.
        const auto* api = phase.gpu;
        std::uint64_t token = 0;
        if (api->buffer(api->context, atlas.value()->payload.bytes().data(),
                        atlas.value()->payload.bytes().size(), 0, &token))
          return Poll(phase.gpu_status());
        auto output = multi_result::take(phase.allocator.allocate(4));
        std::uint64_t target = 0;
        if (api->buffer(api->context, output.data(), 4, 1, &target))
          return Poll(phase.gpu_status());
        const char shader[] =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void atlas_copy(device const float* a [[buffer(0)]],"
            "device float* b [[buffer(1)]]){b[0]=a[0];}";
        const ps_gpu_buffer_binding_v1 bindings[] = {
            {sizeof(ps_gpu_buffer_binding_v1), 0, token, 0, 4, 0},
            {sizeof(ps_gpu_buffer_binding_v1), 1, target, 0, 4, 1}};
        ps_gpu_dispatch_v1 dispatch{};
        dispatch.struct_size = sizeof(dispatch);
        dispatch.source = shader;
        dispatch.source_size = sizeof(shader) - 1;
        dispatch.entry = "atlas_copy";
        dispatch.entry_size = 10;
        dispatch.buffers = bindings;
        dispatch.buffer_count = 2;
        dispatch.grid[0] = dispatch.grid[1] = dispatch.grid[2] = 1;
        if (api->execute(api->context, &dispatch, 1))
          return Poll(phase.gpu_status());
        auto builder = multi_result::take(ResultBuilder::start(
            phase.resources, *phase.query.output.result_schema,
            phase.query.semantic_key, {},
            std::vector<std::uint64_t>(phase.association->begin(),
                                       phase.association->end())));
        multi_result::check(builder.bind_descriptor_relation(multi_result::take(
            ResultRelation::cartesian(phase.resources, 1, {}))));
        multi_result::check(builder.publish_tensor(
            0, Region::whole({1}), {0, {4}}, std::move(output).freeze(),
            multi_result::take(ResultRelation::cartesian(
                phase.resources, 1,
                {0, 1, 0, 1, ResultSupportTarget::Tensor, 0})),
            {true, true, true, true}));
        return Poll(
            ResultPublication{multi_result::take(builder.seal()), true});
      }
      // Ignored service errors must still reject a callback's Need response.
    }
    return Poll(ResultProgramNeed{});
  }
};
int run(unsigned mode, Backend backend, ErrorCode expected,
        std::uint64_t work = 1048576) {
  std::weak_ptr<const CpuStorage> payload;
  auto registry = std::make_shared<OperationRegistry>();
  const auto schema = multi_result::schema(ElementType::Float32);
  OperationDefinition definition;
  definition.key = "test.native.services";
  definition.traits.input_count = 1;
  definition.traits.input_schema.resize(1);
  auto& input = definition.traits.input_schema[0];
  input.kind = OperationPortKind::Result;
  input.result_schema_id = schema.id;
  input.result_schema_version = schema.version;
  definition.traits.outputs[0] = multi_result::output("value", schema);
  definition.traits.outputs[0].continuation_bytes = sizeof(State);
  definition.traits.workspace_bytes = 16384;
  definition.traits.supports_gpu = true;
  definition.start_result = [&](const auto&, const auto& allocator) {
    return ResultContinuation::make<State>(allocator, mode, &payload);
  };
  multi_result::check(registry->register_operation(std::move(definition)));
  multi_result::check(registry->freeze());
  ExecutionContextConfig config;
  config.gpu_enabled = backend == Backend::Gpu;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  if (backend == Backend::Gpu && !context.gpu_enabled())
    return 77;
  auto root = context.resource_budget().take_value();
  auto input_result = multi_result::binding(root, "input", 7, schema);
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", schema)};
  document.nodes = {
      {1, "test.native.services", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"out", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode = backend == Backend::Gpu ? ExecutionMode::NativeGpu
                                                    : ExecutionMode::CpuExact;
  auto compiled =
      multi_result::take(Compiler(registry).compile(graph, planning));
  ExecutionOptions options;
  options.dependencies.maximum_work = work;
  options.maximum_dependency_work = work;
  const auto before = root.statistics().live[ResourceKind::Payload];
  {
    auto result = context.execute(compiled.plan, {{input_result}}, {}, options);
    if (result.status().code == ErrorCode::BackendUnavailable &&
        result.status().message == "fixture requires Metal")
      return 77;
    if (result.status().code != expected)
      std::cerr << "mode=" << mode << " " << result.status().message << '\n';
    PS_CHECK(result.status().code == expected);
    if (result.ok()) {
      float actual = 0;
      auto output = result.value().results.at("out");
      PS_CHECK(
          output
              .read_tensor(output.descriptor().take_value(), 0, {0}, &actual, 4)
              .ok() &&
          actual == 7);
      PS_CHECK(result.value().diagnostics.native_dispatch_count == 1);
    }
  }
  PS_CHECK(payload.expired());
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == before);
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(mock_failures() == 0);
  PS_CHECK(run(0, Backend::Cpu, ErrorCode::InvalidArgument) == 0);
  const auto native = run(0, Backend::Gpu, ErrorCode::Ok);
  if (native == 77) {
    std::cout << "Result mock failures and CPU fencing passed; native atlas "
                 "skipped\n";
    return 77;
  }
  PS_CHECK(native == 0);
  PS_CHECK(run(1, Backend::Gpu, ErrorCode::InvalidArgument) == 0);
  PS_CHECK(run(3, Backend::Gpu, ErrorCode::InvalidArgument) == 0);
  PS_CHECK(run(4, Backend::Gpu, ErrorCode::InvalidArgument) == 0);
  PS_CHECK(run(7, Backend::Gpu, ErrorCode::InvalidArgument) == 0);
  PS_CHECK(run(0, Backend::Gpu, ErrorCode::ResourceExhausted, 8) == 0);
  std::cout << "Result native atlas execution, reuse, authorization and "
               "retirement passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
