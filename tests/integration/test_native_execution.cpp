#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
/** @brief A real independently registered GPU operation with exact CPU oracle.
 */
ps::Result<ps::Value> scale(const ps::OperationInvocation& call) {
  auto allocated = ps::MutableValue::allocate(
      call.inputs[0].descriptor(), call.output_region, call.allocator);
  if (!allocated.ok())
    return ps::Result<ps::Value>(allocated.status());
  auto output = allocated.take_value();
  if (call.backend == ps::Backend::Cpu) {
    for (std::size_t i = 0; i < 4; ++i) {
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
    std::uint64_t input = 0, result = 0;
    if (api->buffer(api->context, call.inputs[0].bytes().data(), 16, 0,
                    &input) ||
        api->buffer(api->context, output.data(), 16, 1, &result))
      return ps::Result<ps::Value>(ps::Status::failure(
          ps::ErrorCode::OperationFailed, "native binding failed"));
    ps_gpu_buffer_binding_v6 buffers[] = {
        {sizeof(ps_gpu_buffer_binding_v6), 0, input, 0, 16, 0},
        {sizeof(ps_gpu_buffer_binding_v6), 1, result, 0, 16, 1}};
    ps_gpu_dispatch_v6 command{};
    command.struct_size = sizeof(command);
    command.source = source;
    command.source_size = sizeof(source) - 1;
    command.entry = "scale";
    command.entry_size = 5;
    command.buffers = buffers;
    command.buffer_count = 2;
    command.grid[0] = 4;
    command.grid[1] = command.grid[2] = 1;
    if (api->execute(api->context, &command, 1))
      return ps::Result<ps::Value>(ps::Status::failure(
          ps::ErrorCode::OperationFailed, "native execution failed"));
  }
  return std::move(output).publish();
}
}  // namespace

int main() {
  auto registry = std::make_shared<ps::OperationRegistry>();
  ps::OperationTraits traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.output_element_type = ps::ElementType::Float32;
  traits.shape_rule = ps::OperationShapeRule::PreserveFirstInput;
  traits.region_rule = ps::OperationRegionRule::Elementwise;
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
  options.execution_mode = ps::ExecutionMode::MetalFp32;
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
    const auto& d = result.value().diagnostics;
    PS_CHECK(d.native_dispatch_count == 2 && d.native_submission_count == 2);
    PS_CHECK(d.transfer_count == 1 && d.transfer_bytes == 16);
    PS_CHECK(d.host_access_count == 1);
    PS_CHECK(d.selected_backends.at(1) == ps::Backend::Gpu);
    PS_CHECK(d.selected_backends.at(3) == ps::Backend::Cpu);
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
  std::cout << "native chain: dispatches=2 uploads=1 bytes=16 host_access=1 "
               "oracle=passed\n";
  return 0;
}
