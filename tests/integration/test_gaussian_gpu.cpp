#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "execution/execution_test_hooks.hpp"
#include "fixtures/native_vulkan_spirv.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
CancellationSource* active_cancellation = nullptr;
unsigned submitted_count = 0;
struct ClearHooks final {
  ~ClearHooks() { execution_testing::install_execution_test_hooks(nullptr); }
};
std::chrono::steady_clock::time_point cancelled_at;
void cancel_submitted() noexcept {
  ++submitted_count;
  cancelled_at = std::chrono::steady_clock::now();
  active_cancellation->cancel();
}
using Parameters = std::map<std::string, ParameterValue>;
Parameters parameters(unsigned rx, unsigned ry, const std::string& boundary) {
  return {{"radius_x", std::int64_t{rx}},
          {"radius_y", std::int64_t{ry}},
          {"sigma_x", 1.},
          {"sigma_y", 1.},
          {"x_axis", std::int64_t{1}},
          {"y_axis", std::int64_t{0}},
          {"cval", 0.},
          {"boundary", boundary}};
}
double number(std::uint64_t bits) {
  double value;
  std::memcpy(&value, &bits, 8);
  return value;
}
Value input_value(bool narrow, unsigned height, unsigned width,
                  const std::vector<std::uint64_t>& raw) {
  const unsigned size = narrow ? 4 : 8;
  std::vector<std::uint8_t> bytes(raw.size() * size);
  for (std::size_t i = 0; i < raw.size(); ++i)
    std::memcpy(bytes.data() + i * size, &raw[i], size);
  const ValueDescriptor descriptor{
      narrow ? ElementType::Float32 : ElementType::Float64,
      {height, width}};
  return Value::create(descriptor, Region::whole(descriptor.shape),
                       {0, {width * size, size}}, std::move(bytes))
      .take_value();
}
Result<ExecutionResult> run(const std::shared_ptr<OperationRegistry>& registry,
                            ExecutionContext& context, const Value& input,
                            Parameters parameters, bool gpu = true,
                            const CancellationToken& cancellation = {}) {
  WorkflowDocument document;
  document.inputs = {
      {1, "input", input.descriptor(), input.region(), input.layout(), {}}};
  document.nodes = {{1,
                     gpu ? "filter.gaussian_baked64_v1_strict_gpu"
                         : "filter.gaussian_baked64_v1_strict_cpu_whole",
                     {WorkflowInputReference{1}},
                     std::move(parameters)}};
  document.outputs = {{"output", 1, "output"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode =
      gpu ? ExecutionMode::NativeGpu : ExecutionMode::CpuExact;
  auto compiled = Compiler(registry).compile(graph, planning);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  return context.execute(compiled.value().plan, {{{"input", input}}},
                         cancellation);
}
int empty_request(const std::shared_ptr<OperationRegistry>& registry,
                  ExecutionContext& context) {
  const auto input = input_value(false, 1, 1, {UINT64_C(0x7ff0000000000001)});
  WorkflowDocument document;
  document.inputs = {
      {1, "input", input.descriptor(), input.region(), input.layout(), {}}};
  document.nodes = {{1,
                     "filter.gaussian_baked64_v1_strict_gpu",
                     {WorkflowInputReference{1}},
                     parameters(2, 2, "clamp")}};
  document.outputs = {{"output", 1, "output"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_CHECK(compiled.ok());
  auto frozen = context.freeze(compiled.value().plan, {{{"input", input}}});
  PS_CHECK(frozen.ok());
  auto result = context.execute_fragments(
      frozen.value(), {{"output", Footprint::none({1, 1}).take_value()}});
  if (!result.ok())
    std::cerr << result.status().message << '\n';
  PS_CHECK(result.ok() &&
           result.value().values.at("output").coverage().empty());
  PS_CHECK(result.value().diagnostics.native_dispatch_count == 0);
  PS_CHECK(result.value().diagnostics.fallback_reasons.empty());
  return 0;
}
int native_layout() {
  auto registry = make_default_operation_registry(false);
  OperationDefinition source;
  source.key = "test.gaussian_native_input";
  source.traits.supports_gpu = true;
  source.traits.estimated_bytes = 601;
  auto& output = source.traits.outputs[0];
  output.key = "input";
  output.shape_rule = OperationShapeRule::Fixed;
  output.fixed_output_shape = {2, 5, 7};
  output.output_element_type = ElementType::Float64;
  output.region_rule = OperationRegionRule::Whole;
  source.callback = [](const OperationInvocation& call) -> Result<Value> {
    auto made = call.allocator.allocate(601);
    if (!made.ok())
      return Result<Value>(made.status());
    auto buffer = made.take_value();
    std::uint64_t values[70]{};
    for (unsigned i = 0; i < 70; ++i) {
      const double value =
          static_cast<double>((i * 719U) % 65521U) / 16384. - 2.;
      std::memcpy(&values[i], &value, 8);
    }
    if (call.backend == Backend::Gpu) {
      const auto* api = call.gpu;
      if (!api)
        return Result<Value>(Status{ErrorCode::BackendUnavailable, "no GPU"});
      const char shader[] =
          "#include <metal_stdlib>\nusing namespace metal;\n"
          "kernel void produce(device uchar* b [[buffer(0)]], "
          "constant ulong* a [[buffer(1)]], uint i "
          "[[thread_position_in_grid]]) {"
          "uint at=49+(i/35)*320+((i%35)/7)*56-(i%7)*8;"
          "for(uint k=0;k<8;++k)b[at+k]=uchar(a[i]>>(8*k));}";
      std::uint64_t token = 0;
      if (api->buffer(api->context, buffer.data(), buffer.size(), 1, &token))
        return Result<Value>(
            Status{ErrorCode::OperationFailed, "source binding"});
      const ps_gpu_buffer_binding_v11 binding{
          sizeof(ps_gpu_buffer_binding_v11), 0, token, 0, buffer.size(), 1};
      ps_gpu_dispatch_v11 dispatch{};
      dispatch.struct_size = sizeof(dispatch);
      const bool vulkan = api->backend == PS_GPU_BACKEND_VULKAN_V11;
      std::uint64_t vulkan_values[140]{};
      if (vulkan) {
        for (unsigned i = 0; i < 70; ++i)
          vulkan_values[2 * i] = values[i];
      }
      dispatch.source =
          vulkan ? reinterpret_cast<const char*>(kGaussianInputSpirv) : shader;
      dispatch.source_size =
          vulkan ? sizeof(kGaussianInputSpirv) : sizeof(shader) - 1;
      dispatch.code_format =
          vulkan ? PS_GPU_CODE_SPIRV_V11 : PS_GPU_CODE_MSL_V11;
      dispatch.entry = "produce";
      dispatch.entry_size = 7;
      dispatch.buffers = &binding;
      dispatch.buffer_count = 1;
      dispatch.constants = vulkan ? vulkan_values : values;
      dispatch.constant_size = vulkan ? sizeof(vulkan_values) : sizeof(values);
      dispatch.constant_index = 1;
      dispatch.grid[0] = 70;
      dispatch.grid[1] = dispatch.grid[2] = 1;
      if (api->execute(api->context, &dispatch, 1))
        return Result<Value>(
            Status{ErrorCode::OperationFailed, "source dispatch"});
    } else {
      for (unsigned i = 0; i < 70; ++i)
        std::memcpy(buffer.data() + 49 + (i / 35) * 320 + ((i % 35) / 7) * 56 -
                        (i % 7) * 8,
                    &values[i], 8);
    }
    return Value::from_storage(
        {ElementType::Float64, {2, 5, 7}}, Region::whole({2, 5, 7}),
        {457, {320, 56, -8}, {1, 2, 3}}, std::move(buffer).freeze());
  };
  PS_CHECK(registry->register_operation(std::move(source)).ok());
  PS_CHECK(registry->freeze().ok());
  Value retained, reference;
  for (bool gpu : {false, true}) {
    auto p = parameters(2, 2, "reflect_whole");
    p["x_axis"] = std::int64_t{2};
    p["y_axis"] = std::int64_t{1};
    WorkflowDocument document;
    document.nodes = {{1, "test.gaussian_native_input", {}, {}},
                      {2,
                       gpu ? "filter.gaussian_baked64_v1_strict_gpu"
                           : "filter.gaussian_baked64_v1_strict_cpu_whole",
                       {WorkflowNodeOutput{1, "input"}},
                       p}};
    document.outputs = {{"output", 2, "output"}};
    GraphContext graph(document);
    PlanningOptions planning;
    planning.execution_mode =
        gpu ? ExecutionMode::NativeGpu : ExecutionMode::CpuExact;
    auto compiled = Compiler(registry).compile(graph, planning);
    PS_CHECK(compiled.ok());
    ExecutionContextConfig config;
    config.gpu_enabled = gpu;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    auto result = context.execute(compiled.value().plan);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok() &&
             result.value().diagnostics.fallback_reasons.empty());
    if (gpu) {
      PS_CHECK(result.value().diagnostics.native_dispatch_count > 1);
      retained = result.value().values.at("output");
    } else {
      reference = result.value().values.at("output");
    }
  }
  PS_CHECK(retained.copy_bytes() == reference.copy_bytes());
  return 0;
}
int dispatched(const ExecutionResult& result) {
  PS_CHECK(result.diagnostics.native_dispatch_count > 0);
  PS_CHECK(result.diagnostics.native_submission_count > 0);
  PS_CHECK(result.diagnostics.fallback_reasons.empty());
  PS_CHECK(result.diagnostics.selected_backends.at({1, 0}) == Backend::Gpu);
  return 0;
}
}  // namespace
int main(int argc, char** argv) {
  execution_testing::ExecutionTestHooks native_hooks;
  const ClearHooks clear_hooks;
  native_hooks.native_device = true;
  execution_testing::install_execution_test_hooks(&native_hooks);
  auto registry = make_default_operation_registry();
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.cpu_workers = 4;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  if (!context.gpu_enabled()) {
    std::cerr << "native GPU unavailable\n";
    return 77;
  }
  if (argc == 2 && std::string(argv[1]) == "--stdin") {
    unsigned narrow, height, width, rx, ry;
    std::uint64_t sx, sy, cval;
    std::string boundary;
    while (std::cin >> std::dec >> narrow >> height >> width >> rx >> ry >>
           std::hex >> sx >> sy >> cval >> boundary) {
      std::vector<std::uint64_t> data(height * width);
      for (auto& value : data)
        std::cin >> std::hex >> value;
      auto p = parameters(rx, ry, boundary);
      p["sigma_x"] = number(sx);
      p["sigma_y"] = number(sy);
      p["cval"] = number(cval);
      auto result =
          run(registry, context, input_value(narrow, height, width, data), p);
      if (!result.ok()) {
        std::cerr << result.status().message << '\n';
        return 1;
      }
      PS_CHECK(dispatched(result.value()) == 0);
      const auto& value = result.value().values.at("output");
      for (unsigned i = 0; i < data.size(); ++i) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, value.bytes().data() + i * (narrow ? 4 : 8),
                    narrow ? 4 : 8);
        std::cout << std::hex << bits << ' ';
      }
      std::cout << '\n';
    }
    return std::cin.eof() ? 0 : 2;
  }
  PS_CHECK(native_layout() == 0);
  std::vector<std::uint64_t> raw(5 * 17);
  for (unsigned i = 0; i < raw.size(); ++i) {
    const double value =
        static_cast<double>((i * 1709U + 719U) % 131071U) / 65536. - 1.;
    std::memcpy(&raw[i], &value, 8);
  }
  const auto input = input_value(false, 5, 17, raw);
  for (const auto* boundary :
       {"constant", "clamp", "wrap", "reflect_half", "reflect_whole"}) {
    auto p = parameters(3, 2, boundary);
    auto result = run(registry, context, input, p);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok() && dispatched(result.value()) == 0);
    PS_CHECK(result.value().diagnostics.native_dispatch_count > 1);
    auto reference = run(registry, context, input, p, false);
    PS_CHECK(reference.ok() &&
             result.value().values.at("output").copy_bytes() ==
                 reference.value().values.at("output").copy_bytes());
  }
  for (bool narrow : {false, true}) {
    // Cross multiple workgroups and scratch reuse in a partial final batch.
    std::vector<std::uint64_t> many(17 * 31);
    for (unsigned i = 0; i < many.size(); ++i) {
      const double value = static_cast<double>(i % 97) / 128. - .25;
      if (narrow) {
        const float small = static_cast<float>(value);
        std::uint32_t word;
        std::memcpy(&word, &small, 4);
        many[i] = word;
      } else {
        std::memcpy(&many[i], &value, 8);
      }
    }
    auto large_input = input_value(narrow, 17, 31, many);
    const auto p = parameters(2, 2, "reflect_half");
    auto actual = run(registry, context, large_input, p);
    auto expected = run(registry, context, large_input, p, false);
    PS_CHECK(actual.ok() && expected.ok() && dispatched(actual.value()) == 0);
    PS_CHECK(actual.value().values.at("output").copy_bytes() ==
             expected.value().values.at("output").copy_bytes());
    for (unsigned special = 0; special < 6; ++special) {
      std::vector<std::uint64_t> values(
          35, narrow ? UINT64_C(0x80000001) : UINT64_C(0x8000000000000001));
      if (special == 1) {
        values[0] =
            narrow ? UINT64_C(0x7f800003) : UINT64_C(0x7ff0000000000003);
        values[34] =
            narrow ? UINT64_C(0xffc00005) : UINT64_C(0xfff8000000000005);
      } else if (special == 2) {
        values[0] =
            narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
        values[34] =
            narrow ? UINT64_C(0xff800000) : UINT64_C(0xfff0000000000000);
      } else if (special == 4 || special == 5) {
        values[0] =
            narrow ? UINT64_C(0x7fa00000) : UINT64_C(0x7ff4000000000000);
        values[34] =
            narrow ? UINT64_C(0xffc00005) : UINT64_C(0xfff8000000000005);
        if (special == 5)
          std::swap(values[0], values[34]);
      } else if (special == 3) {
        std::fill(values.begin(), values.end(),
                  narrow ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000));
      }
      const auto special_input = input_value(narrow, 5, 7, values);
      auto p = parameters(3, 2, "constant");
      auto result = run(registry, context, special_input, p);
      auto reference = run(registry, context, special_input, p, false);
      PS_CHECK(result.ok() && reference.ok() &&
               dispatched(result.value()) == 0);
      PS_CHECK(result.value().diagnostics.native_dispatch_count == 3);
      PS_CHECK(result.value().values.at("output").copy_bytes() ==
               reference.value().values.at("output").copy_bytes());
    }
  }
  {
    CancellationSource midflight;
    active_cancellation = &midflight;
    execution_testing::ExecutionTestHooks hooks;
    hooks.native_device = true;
    hooks.native_submitted = cancel_submitted;
    execution_testing::install_execution_test_hooks(&hooks);
    std::vector<std::uint64_t> full_batch(17 * 31);
    for (unsigned i = 0; i < full_batch.size(); ++i)
      full_batch[i] = raw[i % raw.size()];
    const auto cancel_input = input_value(false, 17, 31, full_batch);
    auto cancelled = run(registry, context, cancel_input,
                         parameters(3, 2, "clamp"), true, midflight.token());
    execution_testing::install_execution_test_hooks(&native_hooks);
    active_cancellation = nullptr;
    const auto drain_us = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - cancelled_at)
                              .count();
    PS_CHECK(submitted_count == 1 && !cancelled.ok() &&
             cancelled.status().code == ErrorCode::Cancelled);
    PS_CHECK(context.resource_budget()
                 .value()
                 .statistics()
                 .live[ResourceKind::Payload] == 0);
    std::cout << "Gaussian native cancel-to-return us=" << drain_us << '\n';
  }
  PS_CHECK(empty_request(registry, context) == 0);
  CancellationSource cancellation;
  cancellation.cancel();
  auto cancelled = run(registry, context, input, parameters(2, 2, "clamp"),
                       true, cancellation.token());
  PS_CHECK(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled);
  config.managed_resources->maximum_work = 10000;
  ExecutionContext limited(registry, config);
  auto exhausted = run(registry, limited, input, parameters(2, 2, "clamp"));
  PS_CHECK(!exhausted.ok() &&
           exhausted.status().code == ErrorCode::ResourceExhausted);
  return 0;
}
