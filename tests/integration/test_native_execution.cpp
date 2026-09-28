#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>

#include "fixtures/native_scale_spirv.h"
#include "photospider/execution/resource_allocator.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
std::uint64_t native_capacity = 0;
/** @brief A real independently registered GPU operation with exact CPU oracle.
 */
ps::Result<ps::Value> scale(const ps::OperationInvocation& call) {
  auto allocated = ps::MutableValue::allocate(
      call.inputs[0].descriptor(), call.output_region, call.allocator);
  if (!allocated.ok())
    return ps::Result<ps::Value>(allocated.status());
  auto output = allocated.take_value();
  if (call.backend == ps::Backend::Cpu) {
    for (std::size_t i = 0; i < output.size() / 4; ++i) {
      float number;
      std::memcpy(&number, call.inputs[0].bytes().data() + i * 4, 4);
      number *= .5F;
      std::memcpy(output.data() + i * 4, &number, 4);
    }
  } else {
    const char source[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void scale(device const float* a [[buffer(0)]], "
        "device float* b [[buffer(1)]], uint i [[thread_position_in_grid]])"
        "{b[i]=a[i]*.5f;}";
    const auto* api = call.gpu;
    if (!api)
      return ps::Result<ps::Value>(ps::Status::failure(
          ps::ErrorCode::BackendUnavailable, "no native service"));
    native_capacity = call.inputs[0].storage()->capacity();
    std::uint64_t input = 0, result = 0;
    if (api->buffer(api->context, call.inputs[0].bytes().data(),
                    call.inputs[0].bytes().size(), 0, &input) ||
        api->buffer(api->context, output.data(), output.size(), 1, &result))
      return ps::Result<ps::Value>(ps::Status::failure(
          ps::ErrorCode::OperationFailed, "native binding failed"));
    ps_gpu_buffer_binding_v11 buffers[] = {
        {sizeof(ps_gpu_buffer_binding_v11), 0, input, 0,
         call.inputs[0].bytes().size(), 0},
        {sizeof(ps_gpu_buffer_binding_v11), 1, result, 0, output.size(), 1}};
    ps_gpu_dispatch_v11 command{};
    command.struct_size = sizeof(command);
    command.source = source;
    command.source_size = sizeof(source) - 1;
    if (api->backend == PS_GPU_BACKEND_VULKAN_V11) {
      command.source = reinterpret_cast<const char*>(kNativeScaleSpirv);
      command.source_size = sizeof(kNativeScaleSpirv);
      command.code_format = PS_GPU_CODE_SPIRV_V11;
    }
    command.entry = "scale";
    command.entry_size = 5;
    command.buffers = buffers;
    command.buffer_count = 2;
    command.grid[0] = output.size() / 4;
    command.grid[1] = command.grid[2] = 1;
    if (api->execute(api->context, &command, 1))
      return ps::Result<ps::Value>(ps::Status::failure(
          ps::ErrorCode::OperationFailed, "native execution failed"));
  }
  return std::move(output).publish(call.inputs[0].facets());
}
int sticky_resource_before_fallback(const ps::Value& input) {
  for (bool exhaust : {false, true}) {
    auto registry = std::make_shared<ps::OperationRegistry>();
    unsigned cpu_calls = 0;
    ps::OperationDefinition operation;
    operation.key = "native.failure";
    operation.traits.input_count = 1;
    operation.traits.input_schema.resize(1);
    operation.traits.supports_gpu = true;
    operation.traits.allows_cpu_fallback = true;
    operation.traits.outputs[0].output_element_type = ps::ElementType::Float32;
    operation.traits.outputs[0].shape_rule =
        ps::OperationShapeRule::PreserveFirstInput;
    operation.callback = [&](const ps::OperationInvocation& call) {
      if (call.backend == ps::Backend::Cpu) {
        ++cpu_calls;
        return ps::Result<ps::Value>(call.inputs[0]);
      }
      const auto* root = ps::resource_internal::metadata_budget();
      if (!root || !call.gpu)
        return ps::Result<ps::Value>(
            ps::Status{ps::ErrorCode::Internal, "missing GPU callback root"});
      if (exhaust)
        static_cast<void>(root->consume({1048577}));
      const char source[] =
          "#include <metal_stdlib>\nusing namespace metal;\n"
          "kernel void present(uint i [[thread_position_in_grid]]) {}";
      ps_gpu_dispatch_v11 command{};
      command.struct_size = sizeof(command);
      command.source = source;
      command.source_size = sizeof(source) - 1;
      command.entry = "missing";
      command.entry_size = 7;
      command.grid[0] = command.grid[1] = command.grid[2] = 1;
      static_cast<void>(call.gpu->execute(call.gpu->context, &command, 1));
      return ps::Result<ps::Value>(
          ps::Status{ps::ErrorCode::OperationFailed, "missing entry"});
    };
    PS_CHECK(registry->register_operation(std::move(operation)).ok());
    PS_CHECK(registry->freeze().ok());
    ps::WorkflowDocument document;
    document.inputs = {
        {1, "input", input.descriptor(), input.region(), input.layout(), {}}};
    document.nodes = {
        {1, "native.failure", {ps::WorkflowInputReference{1}}, {}}};
    document.outputs = {{"output", 1, "value"}};
    ps::GraphContext graph(document);
    ps::PlanningOptions planning;
    planning.execution_mode = ps::ExecutionMode::NativeGpu;
    auto compiled = ps::Compiler(registry).compile(graph, planning);
    PS_CHECK(compiled.ok());
    ps::ExecutionContextConfig config;
    config.gpu_enabled = true;
    config.managed_resources = ps::ResourceLimits{};
    config.managed_resources->maximum_work = 1048576;
    ps::ExecutionContext context(registry, config);
    PS_CHECK(context.gpu_enabled());
    auto result = context.execute(compiled.value().plan, {{{"input", input}}});
    if (exhaust) {
      PS_CHECK(!result.ok() &&
               result.status().code == ps::ErrorCode::ResourceExhausted);
      PS_CHECK(cpu_calls == 0);
    } else {
      PS_CHECK(result.ok() && cpu_calls == 1);
      PS_CHECK(result.value().diagnostics.fallback_reasons.size() == 1);
    }
  }
  return 0;
}
}  // namespace

int main() {
  auto registry = std::make_shared<ps::OperationRegistry>();
  ps::OperationTraits traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.outputs[0].output_element_type = ps::ElementType::Float32;
  traits.outputs[0].shape_rule = ps::OperationShapeRule::PreserveFirstInput;
  traits.outputs[0].region_rule = ps::OperationRegionRule::Elementwise;
  traits.supports_gpu = traits.allows_cpu_fallback = true;
  std::function<void()> after_dispatch;
  PS_CHECK(registry
               ->register_operation({"native.scale", traits,
                                     [&](const ps::OperationInvocation& call) {
                                       auto result = scale(call);
                                       if (call.backend == ps::Backend::Gpu &&
                                           after_dispatch)
                                         after_dispatch();
                                       return result;
                                     }})
               .ok());
  traits.supports_gpu = traits.allows_cpu_fallback = false;
  PS_CHECK(registry
               ->register_operation({"host.read", traits,
                                     [](const ps::OperationInvocation& call) {
                                       return ps::Result<ps::Value>(
                                           call.inputs[0]);
                                     }})
               .ok());
  PS_CHECK(registry->freeze().ok());
  ps::WorkflowDocument document;
  document.inputs = {{1,
                      "input",
                      {ps::ElementType::Float32, {4}},
                      ps::Region::whole({4}),
                      {0, {4}},
                      {}}};
  document.nodes = {
      {1, "native.scale", {ps::WorkflowInputReference{1}}, {}},
      {2, "native.scale", {ps::WorkflowNodeOutput{1, "value"}}, {}},
      {3, "host.read", {ps::WorkflowNodeOutput{2, "value"}}, {}}};
  document.outputs = {{"result", 3, "value"}};
  ps::GraphContext graph(document);
  ps::PlanningOptions options;
  options.execution_mode = ps::ExecutionMode::NativeGpu;
  auto compiled = ps::Compiler(registry).compile(graph, options);
  PS_CHECK(compiled.ok());
  auto buffer = ps::BufferAllocator().allocate(16).take_value();
  const float input[] = {0, .25F, .5F, 1};
  std::memcpy(buffer.data(), input, 16);
  auto value = ps::Value::from_storage({ps::ElementType::Float32, {4}},
                                       ps::Region::whole({4}), {0, {4}},
                                       std::move(buffer).freeze())
                   .take_value();
  ps::ExecutionBindings bindings{{{"input", value}}};
  ps::ExecutionContext cpu(registry);
  auto fallback = cpu.execute(compiled.value().plan, bindings);
  PS_CHECK(fallback.ok());
  PS_CHECK(fallback.value().diagnostics.fallback_reasons.size() == 2);
  PS_CHECK(fallback.value().diagnostics.native_dispatch_count == 0);
  ps::ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.cpu_workers = 2;
  config.collect_scheduler_timing = true;
  ps::Value retained;
  {
    ps::ExecutionContext execution(registry, config);
    if (!execution.gpu_enabled()) {
      std::cout << "CPU fallback passed; native execution skipped\n";
      return 77;
    }
    auto result = execution.execute(compiled.value().plan, bindings);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok());
    const auto queue = execution.scheduler_statistics();
    PS_CHECK(queue.enabled && !queue.cpu.saturated && !queue.gpu.saturated);
    PS_CHECK(queue.gpu.accepted_callbacks == 2 &&
             queue.gpu.started_callbacks == 2);
    PS_CHECK(queue.cpu.accepted_callbacks == 1 &&
             queue.cpu.started_callbacks == 1);
    PS_CHECK(queue.gpu.submission_ns > 0 && queue.gpu.queue_wait_ns > 0);
    const auto& d = result.value().diagnostics;
    PS_CHECK(d.native_dispatch_count == 2 && d.native_submission_count == 2);
    PS_CHECK(d.transfer_count == 1 && d.transfer_bytes == 16);
    PS_CHECK(d.host_access_count == 1);
    PS_CHECK(d.selected_backends.at({1, 0}) == ps::Backend::Gpu);
    PS_CHECK(d.selected_backends.at({3, 0}) == ps::Backend::Cpu);
    retained = result.value().values.at("result");
    ps::CancellationSource cancellation;
    after_dispatch = [&] { cancellation.cancel(); };
    auto cancelled = execution.execute(compiled.value().plan, bindings,
                                       cancellation.token());
    PS_CHECK(!cancelled.ok() &&
             cancelled.status().code == ps::ErrorCode::Cancelled);
    after_dispatch = [&] { graph.replace(document); };
    auto stale = execution.execute(compiled.value().plan, bindings);
    PS_CHECK(!stale.ok() && stale.status().code == ps::ErrorCode::Stale);
    after_dispatch = {};
    compiled = ps::Compiler(registry).compile(graph, options);
    PS_CHECK(execution.execute(compiled.value().plan, bindings).ok());
  }
  PS_CHECK(retained.bytes() == fallback.value().values.at("result").bytes());
  // Generic Value execution must evict retained uploads before reserving again.
  document.nodes.resize(1);
  document.outputs = {{"result", 1, "value"}};
  ps::GraphContext bounded_graph(document);
  auto bounded_plan = ps::Compiler(registry).compile(bounded_graph, options);
  PS_CHECK(bounded_plan.ok());
  config.maximum_live_bytes = native_capacity * 3;
  config.result_cache_bytes = native_capacity;
  ps::ExecutionContext bounded(registry, config);
  for (int repeat = 0; repeat < 3; ++repeat) {
    auto result = bounded.execute(bounded_plan.value().plan, bindings);
    PS_CHECK(result.ok());
    PS_CHECK(result.value().diagnostics.native_dispatch_count == 1);
    PS_CHECK(bounded.cache_statistics().retained_bytes == native_capacity);
  }
  // Distinct uploads must evict optional native cache before actual admission,
  // including when only a Device or Shared sublimit is tight.
  for (auto kind : {ps::ResourceKind::Payload, ps::ResourceKind::Device,
                    ps::ResourceKind::Shared}) {
    auto tight = config;
    tight.maximum_live_bytes =
        kind == ps::ResourceKind::Payload ? native_capacity * 2 : 4096;
    tight.result_cache_bytes = native_capacity * 2;
    tight.managed_resources = ps::ResourceLimits{};
    tight.managed_resources->capacity[kind] = native_capacity * 2;
    ps::ExecutionContext context(registry, tight);
    for (int repeat = 0; repeat < 4; ++repeat) {
      auto bytes = ps::BufferAllocator().allocate(16).take_value();
      const float first = static_cast<float>(repeat + 1) / 8;
      std::memcpy(bytes.data(), &first, sizeof(first));
      auto changed =
          ps::Value::from_storage(value.descriptor(), value.region(),
                                  value.layout(), std::move(bytes).freeze())
              .take_value();
      auto result =
          context.execute(bounded_plan.value().plan, {{{"input", changed}}});
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      PS_CHECK(result.value().diagnostics.native_dispatch_count == 1);
      float actual = 0;
      std::memcpy(&actual, result.value().values.at("result").bytes().data(),
                  sizeof(actual));
      PS_CHECK(actual == first * .5F);
      const auto stats = context.resource_budget().value().statistics();
      PS_CHECK(stats.peak[kind] <= native_capacity * 2);
    }
  }
  // One uploaded allocation can back two Values with distinct semantic facets.
  auto first = ps::Value::from_storage(value.descriptor(), value.region(),
                                       value.layout(), value.storage(),
                                       {{"variant", 1, {1}}})
                   .take_value();
  auto second = ps::Value::from_storage(value.descriptor(), value.region(),
                                        value.layout(), value.storage(),
                                        {{"variant", 1, {2}}})
                    .take_value();
  document.inputs = {{1, "first", first.descriptor(), first.region(),
                      first.layout(), first.facets()},
                     {2, "second", second.descriptor(), second.region(),
                      second.layout(), second.facets()}};
  document.nodes = {{1, "native.scale", {ps::WorkflowInputReference{1}}, {}},
                    {2, "native.scale", {ps::WorkflowInputReference{2}}, {}}};
  document.outputs = {{"first", 1, "value"}, {"second", 2, "value"}};
  ps::GraphContext facets_graph(document);
  auto facets_plan = ps::Compiler(registry).compile(facets_graph, options);
  PS_CHECK(facets_plan.ok());
  config.maximum_live_bytes = 4096;
  config.result_cache_bytes = 0;
  ps::ExecutionContext facets_context(registry, config);
  auto facets_result = facets_context.execute(
      facets_plan.value().plan, {{{"first", first}, {"second", second}}});
  PS_CHECK(facets_result.ok());
  PS_CHECK(facets_result.value().diagnostics.transfer_count == 1);
  PS_CHECK(facets_result.value().values.at("first").facets()[0].payload[0] ==
           1);
  PS_CHECK(facets_result.value().values.at("second").facets()[0].payload[0] ==
           2);
  // Length prefixes prevent shape/origin/stride field-boundary collisions.
  auto rank_five =
      ps::Value::from_storage({ps::ElementType::Float32, {1, 1, 1, 1, 1}},
                              ps::Region::whole({1, 1, 1, 1, 1}),
                              {0, {0, 0, 0, 0, 0}}, value.storage())
          .take_value();
  auto rank_four =
      ps::Value::from_storage({ps::ElementType::Float32, {1, 1, 1, 1}},
                              ps::Region::whole({1, 1, 1, 1}),
                              {0, {0, 0, 0, 1}, {1, 0, 0, 0}}, value.storage())
          .take_value();
  auto ranks_registry = std::make_shared<ps::OperationRegistry>();
  auto source_traits = ps::OperationTraits{};
  source_traits.estimated_bytes = value.storage()->capacity();
  source_traits.outputs[0].output_element_type = ps::ElementType::Float32;
  source_traits.outputs[0].shape_rule = ps::OperationShapeRule::Fixed;
  source_traits.outputs[0].fixed_output_shape = rank_five.descriptor().shape;
  PS_CHECK(
      ranks_registry
          ->register_operation({"source.five", source_traits,
                                [rank_five](const ps::OperationInvocation&) {
                                  return ps::Result<ps::Value>(rank_five);
                                }})
          .ok());
  source_traits.outputs[0].fixed_output_shape = rank_four.descriptor().shape;
  PS_CHECK(
      ranks_registry
          ->register_operation({"source.four", source_traits,
                                [rank_four](const ps::OperationInvocation&) {
                                  return ps::Result<ps::Value>(rank_four);
                                }})
          .ok());
  PS_CHECK(ranks_registry
               ->register_operation(
                   {"native.scale",
                    registry->find_traits("native.scale").take_value(), scale})
               .ok());
  PS_CHECK(ranks_registry->freeze().ok());
  document.inputs.clear();
  document.nodes = {
      {1, "source.five", {}, {}},
      {2, "source.four", {}, {}},
      {3, "native.scale", {ps::WorkflowNodeOutput{1, "value"}}, {}},
      {4, "native.scale", {ps::WorkflowNodeOutput{2, "value"}}, {}}};
  document.outputs = {{"first", 3, "value"}, {"second", 4, "value"}};
  ps::GraphContext ranks_graph(document);
  auto ranks_plan = ps::Compiler(ranks_registry).compile(ranks_graph, options);
  PS_CHECK(ranks_plan.ok());
  ps::ExecutionContext ranks_context(ranks_registry, config);
  auto ranks_result = ranks_context.execute(ranks_plan.value().plan, {});
  if (!ranks_result.ok())
    std::cerr << ranks_result.status().message << '\n';
  PS_CHECK(ranks_result.ok());
  PS_CHECK(ranks_result.value().values.at("first").descriptor().shape.size() ==
           5);
  PS_CHECK(ranks_result.value().values.at("second").descriptor().shape.size() ==
           4);
  PS_CHECK(sticky_resource_before_fallback(value) == 0);
  std::cout << "native chain: dispatches=2 uploads=1 bytes=16 host_access=1 "
               "oracle=passed\n";
  return 0;
}
