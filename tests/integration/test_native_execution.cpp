#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fixtures/native_scale_spirv.h"
#include "photospider/core/resource_allocator.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
std::uint64_t native_capacity = 0;
SchemaTemplate schema(const ValueDescriptor& descriptor,
                      std::vector<ValueFacet> facets = {}) {
  auto result = multi_result::schema(descriptor.element_type, descriptor.shape);
  result.id = "test.native.tensor";
  result.tensors[0].facets = std::move(facets);
  return result;
}
ResultRef source(const ResourceBudget& root, const Value& backing) {
  auto builder = multi_result::take(ResultBuilder::start(
      root, schema(backing.descriptor(), backing.facets()), "native.source"));
  multi_result::check(builder.bind_descriptor_relation(
      multi_result::take(ResultRelation::cartesian(root, 1, {}))));
  multi_result::check(builder.publish_tensor(
      0, backing.region(), backing.layout(), backing.storage(),
      multi_result::take(ResultRelation::cartesian(
          root, backing.region().element_count().take_value(), {})),
      {true, true, true, true}));
  return multi_result::take(builder.seal());
}
std::vector<float> numbers(const ResultRef& result) {
  auto facts = multi_result::take(result.descriptor());
  std::vector<float> values;
  multi_result::check(facts.tensor_coverage(0).visit(
      [&](const auto& at) {
        float number = 0;
        multi_result::check(result.read_tensor(facts, 0, at, &number, 4));
        values.push_back(number);
        return Status::success();
      },
      1024));
  return values;
}
ResultRelation identity(const ResultProgramPhase& phase, const Region& box) {
  const auto shape =
      phase.query.output.result_schema->tensors[0].sample_shape();
  std::vector<ResultMappedAxis> axes;
  for (unsigned axis = 0; axis < shape.size(); ++axis)
    axes.push_back({static_cast<std::int32_t>(axis), 0, 1, 1});
  return multi_result::take(
      ResultRelation::mapped(phase.resources, shape, box, shape, axes,
                             {0, 1, 0, 1, ResultSupportTarget::Tensor, 0}));
}
struct Scale {
  std::function<void()>* after;
  bool host, requested = false;
  Scale(bool host, std::function<void()>* after) : after(after), host(host) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto& spec = phase.query.output.result_schema->tensors[0];
    const auto demand = phase.query.tensor_outputs.value_or(
        multi_result::take(Footprint::all(spec.sample_shape())));
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back({0, 0, demand, 1});
      return Result<ResultProgramPoll>(std::move(need));
    }
    auto builder = multi_result::take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        std::vector<std::uint64_t>(phase.association->begin(),
                                   phase.association->end())));
    multi_result::check(builder.bind_descriptor_relation(
        multi_result::take(ResultRelation::cartesian(phase.resources, 1, {}))));
    for (const auto& box : demand.boxes()) {
      if (host) {
        std::vector<std::uint64_t> probe;
        for (const auto& dimension : box.dimensions())
          probe.push_back(dimension.offset);
        probe.back() += box.dimensions().back().extent - 1;
        float actual = 0;
        multi_result::check(phase.read_tensor(0, 0, probe, &actual, 4));
        if (actual != .25F)
          return Result<ResultProgramPoll>(Status{
              ErrorCode::OperationFailed, "host probe differs from oracle"});
        auto window =
            multi_result::take(phase.tensors->at({0, 0}).acquire(box));
        ResultTensorViewTransform transform;
        for (unsigned axis = 0; axis < spec.sample_shape().size(); ++axis)
          transform.source_axes.push_back(
              {static_cast<std::int32_t>(axis), 0, 1, 1});
        multi_result::check(builder.publish_tensor_view(
            0, box, window, transform, identity(phase, box),
            {true, true, true, true}));
        continue;
      }
      const auto count = multi_result::take(box.element_count());
      auto output = multi_result::take(phase.allocator.allocate(count * 4));
      if (phase.query.backend == Backend::Cpu) {
        auto samples = multi_result::take(
            Footprint::from_regions(spec.sample_shape(), {box}));
        std::uint64_t offset = 0;
        multi_result::check(samples.visit(
            [&](const auto& at) {
              float value = 0;
              multi_result::check(phase.read_tensor(0, 0, at, &value, 4));
              value *= .5F;
              std::memcpy(output.data() + offset, &value, 4);
              offset += 4;
              return Status::success();
            },
            count));
      } else {
        auto window =
            multi_result::take(phase.acquire_native_tensor(0, 0, box));
        std::vector<std::uint64_t> at;
        for (const auto& dim : box.dimensions())
          at.push_back(dim.offset);
        const auto run = multi_result::take(window.row_run(at));
        if (run.samples != count || (count > 1 && run.sample_stride_bytes != 4))
          return Result<ResultProgramPoll>(Status{
              ErrorCode::BackendUnavailable, "fixture requires dense row"});
        const auto* api = phase.gpu;
        std::uint64_t input = 0, result = 0;
        if (api->buffer(api->context, run.data, count * 4, 0, &input) ||
            api->buffer(api->context, output.data(), output.size(), 1, &result))
          return Result<ResultProgramPoll>(phase.gpu_status());
        const ps_gpu_buffer_binding_v1 buffers[] = {
            {sizeof(ps_gpu_buffer_binding_v1), 0, input, 0, count * 4, 0},
            {sizeof(ps_gpu_buffer_binding_v1), 1, result, 0, count * 4, 1}};
        const char metal[] =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void scale(device const float* a [[buffer(0)]], "
            "device float* b [[buffer(1)]], uint i [[thread_position_in_grid]])"
            "{b[i]=a[i]*.5f;}";
        ps_gpu_dispatch_v1 command{};
        command.struct_size = sizeof(command);
        command.source = metal;
        command.source_size = sizeof(metal) - 1;
        if (api->backend == PS_GPU_BACKEND_VULKAN_V1) {
          command.source = reinterpret_cast<const char*>(kNativeScaleSpirv);
          command.source_size = sizeof(kNativeScaleSpirv);
          command.code_format = PS_GPU_CODE_SPIRV_V1;
        }
        command.entry = "scale";
        command.entry_size = 5;
        command.buffers = buffers;
        command.buffer_count = 2;
        command.grid[0] = count;
        command.grid[1] = command.grid[2] = 1;
        if (api->execute(api->context, &command, 1))
          return Result<ResultProgramPoll>(phase.gpu_status());
        multi_result::check(phase.gpu_status());
        if (after && *after)
          (*after)();
      }
      std::vector<std::int64_t> strides(spec.sample_shape().size());
      std::uint64_t stride = 4;
      for (std::size_t axis = strides.size(); axis-- > 0;) {
        strides[axis] = stride;
        stride *= box.dimensions()[axis].extent;
      }
      auto storage = std::move(output).freeze();
      native_capacity = storage->capacity();
      multi_result::check(builder.publish_tensor(
          0, box, {0, strides}, std::move(storage), identity(phase, box),
          {true, true, true, true}));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{multi_result::take(builder.seal()), true});
  }
};
OperationDefinition operation(std::string key, bool host,
                              std::function<void()>* after = nullptr) {
  OperationDefinition op;
  op.key = std::move(key);
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  auto& input = op.traits.input_schema[0];
  input.kind = OperationPortKind::Result;
  input.element_type = static_cast<unsigned>(ElementType::Float32);
  auto& output = op.traits.outputs[0];
  output = multi_result::output("value", schema({ElementType::Float32, {4}}));
  output.continuation_bytes = sizeof(Scale);
  if (host) {
    output.region_rule = OperationRegionRule::Whole;
    output.preserve_output_views = true;
  }
  output.output_schema.result_schema_id.clear();
  output.output_schema.result_schema_version = 0;
  output.output_schema.element_type = input.element_type;
  op.traits.workspace_bytes = 16384;
  op.traits.supports_gpu = op.traits.allows_cpu_fallback = !host;
  op.traits.requires_metadata_specialization = true;
  op.specialize_metadata = [](const auto& inputs, const auto&) {
    OperationOutputSpecialization out;
    out.metadata.result_schema = inputs[0].result_schema;
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{out});
  };
  op.start_result = [host, after](const auto&, const auto& allocator) {
    return ResultContinuation::make<Scale>(allocator, host, after);
  };
  return op;
}
Status typed_native_failure(unsigned mode) {
  if (mode == 7 || mode == 9)
    return {ErrorCode::BackendUnavailable,
            "typed protocol failure",
            FailureReason::MalformedEnvelope,
            {FailureOrigin::Protocol, FailureScope::Group}};
  return {ErrorCode::BackendUnavailable,
          "typed Run failure",
          FailureReason::None,
          {FailureOrigin::Backend, FailureScope::Run}};
}
struct NativeFailure {
  unsigned mode;
  bool requested = false;
  explicit NativeFailure(unsigned mode) : mode(mode) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (mode == 5 && !requested) {
      requested = true;
      ResultProgramNeed need;
      need.tensors.push_back({0, 0, *phase.query.tensor_outputs, 1});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (phase.query.backend == Backend::Gpu) {
      if (mode == 7 || mode == 8)
        return Result<ResultProgramPoll>(typed_native_failure(mode));
      if (mode == 1)
        static_cast<void>(phase.consume_work(1048577));
      const char metal[] =
          "#include <metal_stdlib>\nusing namespace metal;\n"
          "kernel void present(uint i [[thread_position_in_grid]]){}";
      ps_gpu_dispatch_v1 dispatch{};
      dispatch.struct_size = sizeof(dispatch);
      dispatch.source = metal;
      dispatch.source_size = sizeof(metal) - 1;
      // Vulkan rejects MSL as BackendUnavailable; Metal rejects the missing
      // entry with the same recoverable category. InvalidArgument is not a
      // CPU-retry trigger.
      dispatch.entry = "missing";
      dispatch.entry_size = 7;
      dispatch.grid[0] = dispatch.grid[1] = dispatch.grid[2] = 1;
      static_cast<void>(phase.gpu->execute(phase.gpu->context, &dispatch, 1));
      if (mode == 2)
        static_cast<void>(phase.consume_work(UINT64_MAX));
      if (mode == 3) {
        float ignored = 0;
        static_cast<void>(phase.read_tensor(99, 0, {0}, &ignored, 4));
      }
      if (mode == 4)
        static_cast<void>(phase.allocator.limited_requested(1).allocate(2));
      if (mode == 5) {
        float ignored = 0;
        static_cast<void>(phase.tensors->at({0, 0}).read({99}, &ignored, 4));
      }
      if (mode == 6)
        throw std::runtime_error("late operation exception");
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "missing native entry"});
    }
    auto builder = multi_result::take(
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key));
    multi_result::check(builder.bind_descriptor_relation(
        multi_result::take(ResultRelation::cartesian(phase.resources, 1, {}))));
    const std::array<float, 4> values{7, 7, 7, 7};
    multi_result::check(builder.publish_tensor(
        0, Region::whole({4}),
        {reinterpret_cast<const std::uint8_t*>(values.data()), 16},
        multi_result::take(ResultRelation::cartesian(phase.resources, 4, {})),
        {true, true, true, true}));
    return Result<ResultProgramPoll>(
        ResultPublication{multi_result::take(builder.seal()), true});
  }
};
int sticky_resource_before_fallback() {
  for (unsigned mode : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) {
    auto registry = std::make_shared<OperationRegistry>();
    unsigned calls = 0;
    auto op = operation("native.failure", false);
    op.start_result = [&](const ResultProgramQuery& query,
                          const auto& allocator) {
      if (query.backend == Backend::Cpu)
        ++calls;
      if (query.backend == Backend::Gpu && (mode == 9 || mode == 10))
        return Result<ResultContinuation>(typed_native_failure(mode));
      return ResultContinuation::make<NativeFailure>(allocator, mode);
    };
    op.traits.outputs[0].continuation_bytes = sizeof(NativeFailure);
    PS_CHECK(registry->register_operation(std::move(op)).ok());
    PS_CHECK(registry->freeze().ok());
    ExecutionContextConfig config;
    config.gpu_enabled = true;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->maximum_work = 1048576;
    ExecutionContext context(registry, config);
    auto input =
        multi_result::binding(context.resource_budget().take_value(), "input",
                              1, schema({ElementType::Float32, {4}}));
    WorkflowDocument doc;
    doc.inputs = {multi_result::declaration(1, "input", input.result.schema())};
    doc.nodes = {{1, "native.failure", {WorkflowInputReference{1}}, {}}};
    doc.outputs = {{"out", 1, "value"}};
    GraphContext graph(doc);
    PlanningOptions options;
    options.execution_mode = ExecutionMode::NativeGpu;
    auto plan = multi_result::take(Compiler(registry).compile(graph, options));
    auto result = context.execute(plan.plan, {{input}});
    if (mode >= 7) {
      const auto expected = typed_native_failure(mode);
      PS_CHECK(!result.ok() && calls == 0 &&
               result.status().code == expected.code &&
               result.status().reason == expected.reason &&
               result.status().detail.origin == expected.detail.origin &&
               result.status().detail.scope == expected.detail.scope);
    } else if (mode == 1) {
      PS_CHECK(result.status().code == ErrorCode::ResourceExhausted &&
               calls == 0);
    } else if (mode >= 2) {
      PS_CHECK(!result.ok() &&
               result.status().code == ErrorCode::BackendUnavailable &&
               calls == 0);
    } else {
      PS_CHECK(result.ok() && calls == 1);
      PS_CHECK(result.value().diagnostics.fallback_reasons.size() == 1);
    }
  }
  return 0;
}
}  // namespace
int main() try {
  auto registry = std::make_shared<OperationRegistry>();
  std::function<void()> after;
  multi_result::check(
      registry->register_operation(operation("native.scale", false, &after)));
  multi_result::check(
      registry->register_operation(operation("host.read", true)));
  // Execute every Run so this fixture measures native upload retention.
  auto upload_reuse = operation("native.upload_reuse", false);
  upload_reuse.traits.cacheable = false;
  multi_result::check(registry->register_operation(std::move(upload_reuse)));
  multi_result::check(registry->freeze());
  ResourceBudget root;
  auto bytes = multi_result::take(root.allocator().allocate(16));
  const float inputs[] = {0, .25F, .5F, 1};
  std::memcpy(bytes.data(), inputs, 16);
  auto backing = multi_result::take(
      Value::from_storage({ElementType::Float32, {4}}, Region::whole({4}),
                          {0, {4}}, std::move(bytes).freeze()));
  auto input = source(root, backing);
  WorkflowDocument doc;
  doc.inputs = {multi_result::declaration(1, "input", input.schema())};
  doc.nodes = {{1, "native.scale", {WorkflowInputReference{1}}, {}},
               {2, "native.scale", {WorkflowNodeOutput{1, "value"}}, {}},
               {3, "host.read", {WorkflowNodeOutput{2, "value"}}, {}}};
  doc.outputs = {{"result", 3, "value"}};
  GraphContext graph(doc);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto compiled =
      multi_result::take(Compiler(registry).compile(graph, planning));
  ExecutionContext cpu(registry);
  ExecutionBindings bindings{
      {{"input", source(cpu.resource_budget().take_value(), backing)}}};
  auto fallback = multi_result::take(cpu.execute(compiled.plan, bindings));
  PS_CHECK(fallback.diagnostics.fallback_reasons.size() == 2 &&
           fallback.diagnostics.native_dispatch_count == 0);
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.cpu_workers = 2;
  config.collect_scheduler_timing = true;
  ResultRef retained;
  {
    ExecutionContext execution(registry, config);
    if (!execution.gpu_enabled()) {
      std::cout << "Result CPU fallback passed; native execution skipped\n";
      return 77;
    }
    bindings = {
        {{"input", source(execution.resource_budget().take_value(), backing)}}};
    auto result =
        multi_result::take(execution.execute(compiled.plan, bindings));
    const auto queue = execution.scheduler_statistics();
    PS_CHECK(queue.enabled && !queue.cpu.saturated && !queue.gpu.saturated);
    PS_CHECK(queue.gpu.accepted_callbacks == 4 &&
             queue.gpu.started_callbacks == 4);
    PS_CHECK(queue.cpu.accepted_callbacks == 5 &&
             queue.cpu.started_callbacks == 5);
    PS_CHECK(queue.gpu.submission_ns > 0 && queue.gpu.queue_wait_ns > 0);
    const auto& d = result.diagnostics;
    PS_CHECK(d.native_dispatch_count == 2 && d.native_submission_count == 2);
    PS_CHECK(d.transfer_count == 1 && d.transfer_bytes == 16);
    PS_CHECK(d.host_access_count == 1);
    PS_CHECK(d.selected_backends.at({1, 0}) == Backend::Gpu);
    PS_CHECK(d.selected_backends.at({3, 0}) == Backend::Cpu);
    retained = result.results.at("result");
    CancellationSource cancellation;
    after = [&] { cancellation.cancel(); };
    PS_CHECK(execution.execute(compiled.plan, bindings, cancellation.token())
                 .status()
                 .code == ErrorCode::Cancelled);
    after = [&] { graph.replace(doc); };
    PS_CHECK(execution.execute(compiled.plan, bindings).status().code ==
             ErrorCode::Stale);
    after = {};
    compiled = multi_result::take(Compiler(registry).compile(graph, planning));
    PS_CHECK(execution.execute(compiled.plan, bindings).ok());
  }
  PS_CHECK(numbers(retained) == numbers(fallback.results.at("result")));
  PS_CHECK(sticky_resource_before_fallback() == 0);
  doc.nodes.resize(1);
  doc.nodes[0].operation = "native.upload_reuse";
  doc.outputs = {{"result", 1, "value"}};
  GraphContext bounded_graph(doc);
  auto bounded_plan =
      multi_result::take(Compiler(registry).compile(bounded_graph, planning));
  config.maximum_live_bytes = native_capacity * 3;
  config.result_cache_bytes = native_capacity;
  ExecutionContext bounded(registry, config);
  bindings = {
      {{"input", source(bounded.resource_budget().take_value(), backing)}}};
  for (int repeat = 0; repeat < 3; ++repeat) {
    auto run = multi_result::take(bounded.execute(bounded_plan.plan, bindings));
    PS_CHECK(run.diagnostics.native_dispatch_count == 1);
    PS_CHECK(run.diagnostics.fallback_reasons.empty());
    PS_CHECK(run.diagnostics.transfer_count == (repeat == 0 ? 1u : 0u));
    const auto cache = bounded.cache_statistics();
    PS_CHECK(cache.retained_bytes == native_capacity);
    PS_CHECK(cache.native_retained_bytes == native_capacity);
    PS_CHECK(numbers(run.results.at("result"))[3] == .5F);
  }
  for (auto kind :
       {ResourceKind::Payload, ResourceKind::Device, ResourceKind::Shared}) {
    auto tight = config;
    const auto limit = native_capacity * 2 + sizeof(Scale);
    tight.maximum_live_bytes = kind == ResourceKind::Payload ? limit : 16384;
    tight.result_cache_bytes = native_capacity * 2;
    tight.managed_resources = ResourceLimits{};
    tight.managed_resources->capacity[kind] = limit;
    ExecutionContext context(registry, tight);
    for (int repeat = 0; repeat < 4; ++repeat) {
      auto changed_bytes = multi_result::take(root.allocator().allocate(16));
      const float first = static_cast<float>(repeat + 1) / 8;
      std::memcpy(changed_bytes.data(), &first, 4);
      auto changed_backing = multi_result::take(Value::from_storage(
          backing.descriptor(), backing.region(), backing.layout(),
          std::move(changed_bytes).freeze()));
      auto changed =
          source(context.resource_budget().take_value(), changed_backing);
      auto run = multi_result::take(
          context.execute(bounded_plan.plan, {{{"input", changed}}}));
      PS_CHECK(run.diagnostics.native_dispatch_count == 1);
      PS_CHECK(numbers(run.results.at("result"))[0] == first * .5F);
      PS_CHECK(context.resource_budget().take_value().statistics().peak[kind] <=
               limit);
    }
  }
  auto first_backing = multi_result::take(Value::from_storage(
      backing.descriptor(), backing.region(), backing.layout(),
      backing.storage(), {{"variant", 1, {1}}}));
  auto second_backing = multi_result::take(Value::from_storage(
      backing.descriptor(), backing.region(), backing.layout(),
      backing.storage(), {{"variant", 1, {2}}}));
  auto first = source(root, first_backing),
       second = source(root, second_backing);
  doc.inputs = {multi_result::declaration(1, "first", first.schema()),
                multi_result::declaration(2, "second", second.schema())};
  doc.nodes = {{1, "native.scale", {WorkflowInputReference{1}}, {}},
               {2, "native.scale", {WorkflowInputReference{2}}, {}}};
  doc.outputs = {{"first", 1, "value"}, {"second", 2, "value"}};
  GraphContext facets_graph(doc);
  auto facets_plan =
      multi_result::take(Compiler(registry).compile(facets_graph, planning));
  config.maximum_live_bytes = 4096;
  config.result_cache_bytes = 0;
  ExecutionContext facets_context(registry, config);
  first = source(facets_context.resource_budget().take_value(), first_backing);
  second =
      source(facets_context.resource_budget().take_value(), second_backing);
  auto facets_result = multi_result::take(facets_context.execute(
      facets_plan.plan, {{{"first", first}, {"second", second}}}));
  PS_CHECK(facets_result.results.at("first")
               .schema()
               .tensors[0]
               .facets[0]
               .payload[0] == 1);
  PS_CHECK(facets_result.results.at("second")
               .schema()
               .tensors[0]
               .facets[0]
               .payload[0] == 2);
  // Distinct rank/origin/stride layouts sharing backing must not collide.
  auto rank_five = multi_result::take(Value::from_storage(
      {ElementType::Float32, {1, 1, 1, 1, 1}}, Region::whole({1, 1, 1, 1, 1}),
      {0, {0, 0, 0, 0, 0}}, backing.storage()));
  auto rank_four = multi_result::take(Value::from_storage(
      {ElementType::Float32, {1, 1, 1, 1}}, Region::whole({1, 1, 1, 1}),
      {0, {0, 0, 0, 1}, {1, 0, 0, 0}}, backing.storage()));
  first = source(facets_context.resource_budget().take_value(), rank_five);
  second = source(facets_context.resource_budget().take_value(), rank_four);
  doc.inputs = {multi_result::declaration(1, "first", first.schema()),
                multi_result::declaration(2, "second", second.schema())};
  GraphContext ranks_graph(doc);
  auto ranks_plan =
      multi_result::take(Compiler(registry).compile(ranks_graph, planning));
  auto ranks = multi_result::take(facets_context.execute(
      ranks_plan.plan, {{{"first", first}, {"second", second}}}));
  PS_CHECK(
      ranks.results.at("first").schema().tensors[0].descriptor.shape.size() ==
      5);
  PS_CHECK(
      ranks.results.at("second").schema().tensors[0].descriptor.shape.size() ==
      4);
  PS_CHECK(ranks.diagnostics.native_dispatch_count == 2 &&
           ranks.diagnostics.fallback_reasons.empty());
  PS_CHECK(facets_result.diagnostics.transfer_count == 1);
  std::cout
      << "native Result chain: dispatches=2 uploads=1 bytes=16 oracle=passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "native fixture failure: " << error.what() << '\n';
  return 1;
}
