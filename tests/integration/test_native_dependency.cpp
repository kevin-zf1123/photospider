#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "execution/execution_test_hooks.hpp"
#include "fixtures/native_scale_spirv.h"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
std::atomic<unsigned> queries{0};
ErrorCode injected = ErrorCode::Ok;
ErrorCode fail_first_query() noexcept {
  return ++queries == 1 ? injected : ErrorCode::Ok;
}
struct Counts {
  unsigned gpu_starts = 0, cpu_starts = 0, destroyed = 0;
  unsigned gpu_polls = 0, cpu_polls = 0;
  bool ordered_retirement = true;
};
struct State {
  State(Counts* counts, Backend backend) : counts(counts), backend(backend) {
    if (backend == Backend::Cpu && counts->destroyed != counts->gpu_starts)
      counts->ordered_retirement = false;
    ++(backend == Backend::Gpu ? counts->gpu_starts : counts->cpu_starts);
  }
  ~State() noexcept { ++counts->destroyed; }
  Counts* counts;
  Backend backend;
  bool supplied = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++(backend == Backend::Gpu ? counts->gpu_polls : counts->cpu_polls);
    if (!supplied) {
      supplied = true;
      ResultProgramNeed need;
      need.tensors.push_back({0, 0, *phase.query.tensor_outputs, 1});
      return Result<ResultProgramPoll>(std::move(need));
    }
    float sample = 0;
    auto read = phase.read_tensor(0, 0, {0}, &sample, sizeof(sample));
    if (!read.ok())
      return Result<ResultProgramPoll>(read);
    auto output = multi_result::take(phase.allocator.allocate(4));
    if (backend == Backend::Gpu) {
      auto first = phase.acquire_native_tensor(0, 0, Region::whole({1}));
      if (!first.ok())
        return Result<ResultProgramPoll>(first.status());
      auto second = phase.acquire_native_tensor(0, 0, Region::whole({1}));
      if (!second.ok())
        return Result<ResultProgramPoll>(second.status());
      const auto row = multi_result::take(second.value().row_run({0}));
      const auto* api = phase.gpu;
      std::uint64_t source = 0, target = 0;
      if (api->buffer(api->context, row.data, 4, 0, &source) ||
          api->buffer(api->context, output.data(), 4, 1, &target))
        return Result<ResultProgramPoll>(phase.gpu_status());
      const ps_gpu_buffer_binding_v1 bindings[] = {
          {sizeof(ps_gpu_buffer_binding_v1), 0, source, 0, 4, 0},
          {sizeof(ps_gpu_buffer_binding_v1), 1, target, 0, 4, 1}};
      const char metal[] =
          "#include <metal_stdlib>\nusing namespace metal;\n"
          "kernel void scale(device const float* a [[buffer(0)]],"
          "device float* b [[buffer(1)]]){b[0]=a[0]*.5f;}";
      ps_gpu_dispatch_v1 dispatch{};
      dispatch.struct_size = sizeof(dispatch);
      dispatch.source = metal;
      dispatch.source_size = sizeof(metal) - 1;
      if (api->backend == PS_GPU_BACKEND_VULKAN_V1) {
        dispatch.source = reinterpret_cast<const char*>(kNativeScaleSpirv);
        dispatch.source_size = sizeof(kNativeScaleSpirv);
        dispatch.code_format = PS_GPU_CODE_SPIRV_V1;
      }
      dispatch.entry = "scale";
      dispatch.entry_size = 5;
      dispatch.buffers = bindings;
      dispatch.buffer_count = 2;
      dispatch.grid[0] = dispatch.grid[1] = dispatch.grid[2] = 1;
      if (api->execute(api->context, &dispatch, 1))
        return Result<ResultProgramPoll>(phase.gpu_status());
    } else {
      sample *= .5F;
      std::memcpy(output.data(), &sample, 4);
    }
    auto builder = multi_result::take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        std::vector<std::uint64_t>(phase.association->begin(),
                                   phase.association->end())));
    multi_result::check(builder.bind_descriptor_relation(
        multi_result::take(ResultRelation::cartesian(phase.resources, 1, {}))));
    multi_result::check(builder.publish_tensor(
        0, Region::whole({1}), {0, {4}}, std::move(output).freeze(),
        multi_result::take(ResultRelation::cartesian(
            phase.resources, 1, {0, 1, 0, 1, ResultSupportTarget::Tensor, 0})),
        {true, true, true, true}));
    return Result<ResultProgramPoll>(
        ResultPublication{multi_result::take(builder.seal()), true});
  }
};
float read(const ResultRef& result) {
  float number = 0;
  multi_result::check(result.read_tensor(
      multi_result::take(result.descriptor()), 0, {0}, &number, 4));
  return number;
}
int capacity_failure(ErrorCode error) {
  queries = 0;
  injected = error;
  Counts counts;
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "native.capacity";
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.supports_gpu = true;
  traits.allows_cpu_fallback = true;
  const auto schema = multi_result::schema(ElementType::Float32);
  traits.input_schema[0].kind = OperationPortKind::Result;
  traits.input_schema[0].result_schema_id = schema.id;
  traits.input_schema[0].result_schema_version = schema.version;
  traits.outputs[0] = multi_result::output("value", schema);
  traits.outputs[0].continuation_bytes = sizeof(State);
  traits.outputs[0].maximum_dependency_stages = 4;
  traits.workspace_bytes = 4096;
  operation.start_result = [&](const ResultProgramQuery& query,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<State>(allocator, &counts, query.backend);
  };
  PS_CHECK(registry->register_operation(std::move(operation)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", schema)};
  document.nodes = {{1, "native.capacity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"output", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_CHECK(compiled.ok());
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  if (!context.gpu_enabled())
    return 77;
  auto input = multi_result::binding(context.resource_budget().take_value(),
                                     "input", 14, schema);
  auto result = context.execute(compiled.value().plan, {{input}});
  PS_CHECK(queries == 1 && counts.gpu_starts == 1 && counts.gpu_polls == 2);
  if (error == ErrorCode::BackendUnavailable) {
    PS_CHECK(result.ok());
    PS_CHECK(counts.cpu_starts == 1 && counts.cpu_polls == 2 &&
             counts.destroyed == 2 && counts.ordered_retirement);
    PS_CHECK(result.value().diagnostics.fallback_reasons.size() == 1);
    PS_CHECK(result.value().diagnostics.selected_backends.at({1, 0}) ==
             Backend::Cpu);
    PS_CHECK(read(result.value().results.at("output")) == 7);
  } else {
    PS_CHECK(!result.ok() && result.status().code == error);
    PS_CHECK(result.status().message == "injected native capacity query");
    PS_CHECK(counts.cpu_starts == 0 && counts.destroyed == 1);
  }
  const auto retired = context.resource_budget().take_value().statistics();
  PS_CHECK(retired.live[ResourceKind::Device] == 0 &&
           retired.live[ResourceKind::Shared] == 0);
  // A failed speculative session releases its owners before a later Run.
  injected = ErrorCode::Ok;
  queries = 0;
  auto retry = context.execute(compiled.value().plan, {{input}});
  PS_CHECK(retry.ok() && retry.value().diagnostics.fallback_reasons.empty());
  PS_CHECK(read(retry.value().results.at("output")) == 7);
  PS_CHECK(counts.destroyed == counts.cpu_starts + counts.gpu_starts);
  return 0;
}
}  // namespace
int main() {
  ps::execution_testing::ExecutionTestHooks hooks;
  hooks.native_device = true;
  hooks.native_capacity_error = fail_first_query;
  ps::execution_testing::install_execution_test_hooks(&hooks);
  for (auto error :
       {ps::ErrorCode::BackendUnavailable, ps::ErrorCode::ResourceExhausted,
        ps::ErrorCode::OperationFailed}) {
    const int result = capacity_failure(error);
    if (result != 0)
      return result;
  }
  ps::execution_testing::install_execution_test_hooks(nullptr);
  std::cout
      << "native dependency capacity: typed errors, restart and retry passed\n";
  return 0;
}
