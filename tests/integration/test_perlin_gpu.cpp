#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/perlin_workflow/result_fixture.hpp"
#include "../../examples/unified_result_workflow/minimal_ops.hpp"
#include "fixtures/native_vulkan_spirv.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Value input_value(const std::vector<std::uint64_t>& raw, bool narrow) {
  const unsigned width = narrow ? 4 : 8;
  std::vector<std::uint8_t> bytes(raw.size() * width);
  for (std::size_t i = 0; i < raw.size(); ++i)
    std::memcpy(bytes.data() + i * width, &raw[i], width);
  const ValueDescriptor descriptor{
      narrow ? ElementType::Float32 : ElementType::Float64,
      {raw.size() / 3, 3}};
  return Value::create(descriptor, Region::whole(descriptor.shape),
                       {0, {3 * width, width}}, std::move(bytes))
      .take_value();
}
Result<ExecutionResult> run(const std::shared_ptr<OperationRegistry>& registry,
                            ExecutionContext& context, const Value& input,
                            bool narrow, bool gpu = true,
                            const CancellationToken& cancellation = {},
                            bool fragmented = false) {
  WorkflowDocument document;
  perlin_fixture::declare(&document, input);
  document.nodes = {{1,
                     gpu ? "noise.perlin2002_3d_v1_strict_gpu"
                         : "noise.perlin2002_3d_v1_strict_cpu_whole",
                     {WorkflowInputReference{1}},
                     {{"dtype", std::string(narrow ? "float32" : "float64")}}}};
  document.outputs = {{"values", 1, "values"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode =
      gpu ? ExecutionMode::NativeGpu : ExecutionMode::CpuExact;
  auto compiled = Compiler(registry).compile(graph, planning);
  if (!compiled.ok())
    return Result<ExecutionResult>(compiled.status());
  const auto root = context.resource_budget().take_value();
  auto bindings = perlin_fixture::bind(root, input);
  if (fragmented) {
    const auto& schema = *document.inputs[0].result_schema;
    auto builder = numeric_result_fixture::take(
        ResultBuilder::start(root, schema, "pieces"));
    auto relation = numeric_result_fixture::take(ResultRelation::cartesian(
        root, input.region().element_count().take_value(), {}));
    numeric_result_fixture::require(
        builder
            .bind_descriptor_relation(numeric_result_fixture::take(
                ResultRelation::cartesian(root, 1, {})))
            .ok(),
        "descriptor");
    const auto rows = input.descriptor().shape[0];
    const auto width = Value::element_size(input.descriptor().element_type);
    for (std::uint64_t row = 0; row < rows; ++row)
      numeric_result_fixture::require(
          builder
              .publish_tensor(
                  0, Region({{row, 1}, {0, 3}}),
                  ByteView(input.bytes().data() + row * 3 * width, 3 * width),
                  relation, {true, true, true, true})
              .ok(),
          "coordinate piece");
    bindings.inputs[0].result = numeric_result_fixture::take(builder.seal());
  }
  return context.execute(compiled.value().plan, std::move(bindings),
                         cancellation);
}
int empty_request(const std::shared_ptr<OperationRegistry>& registry,
                  ExecutionContext& context) {
  const auto input = input_value({UINT64_C(0x7ff0000000000001), 0, 0}, false);
  WorkflowDocument document;
  perlin_fixture::declare(&document, input);
  document.nodes = {{1,
                     "noise.perlin2002_3d_v1_strict_gpu",
                     {WorkflowInputReference{1}},
                     {}}};
  document.outputs = {{"values", 1, "values"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_CHECK(compiled.ok());
  auto frozen = context.freeze(
      compiled.value().plan,
      perlin_fixture::bind(context.resource_budget().take_value(), input));
  PS_CHECK(frozen.ok());
  auto result = context.execute_fragments(
      frozen.value(), {{"values", Footprint::none({1}).take_value()}});
  if (!result.ok())
    std::cerr << result.status().message << '\n';
  PS_CHECK(result.ok() && result.value()
                              .results.at("values")
                              .descriptor()
                              .value()
                              .tensor_coverage(0)
                              .empty());
  PS_CHECK(result.value().diagnostics.native_dispatch_count == 0);
  PS_CHECK(result.value().diagnostics.fallback_reasons.empty());
  return 0;
}
struct NativeCoordinates {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& call) {
    // The final binary64 read ends at byte 200, exactly the view's last byte.
    auto made = call.allocator.allocate(201);
    if (!made.ok())
      return Result<ResultProgramPoll>(made.status());
    auto buffer = made.take_value();
    std::uint64_t values[15]{};
    for (unsigned i = 0; i < 5; ++i) {
      const double value = .125 * (i + 1);
      std::memcpy(&values[3 * i], &value, 8);
    }
    const char shader[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void produce(device uchar* b [[buffer(0)]], "
        "constant ulong* a [[buffer(1)]], uint i [[thread_position_in_grid]]) {"
        "uint at=65+(i/3)*32-(i%3)*8;"
        "for(uint k=0;k<8;++k)b[at+k]=uchar(a[i]>>(8*k));}";
    const auto* api = call.gpu;
    if (!api)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::BackendUnavailable, "no GPU"});
    const bool vulkan = api->backend == PS_GPU_BACKEND_VULKAN_V1;
    std::uint64_t vulkan_values[30]{};
    for (unsigned i = 0; i < 15; ++i)
      vulkan_values[2 * i] = values[i];
    std::uint64_t token = 0;
    if (api->buffer(api->context, buffer.data(), buffer.size(), 1, &token))
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "source binding"});
    const ps_gpu_buffer_binding_v1 binding{
        sizeof(ps_gpu_buffer_binding_v1), 0, token, 0, buffer.size(), 1};
    ps_gpu_dispatch_v1 dispatch{};
    dispatch.struct_size = sizeof(dispatch);
    dispatch.source =
        vulkan ? reinterpret_cast<const char*>(kPerlinCoordinatesSpirv)
               : shader;
    dispatch.source_size =
        vulkan ? sizeof(kPerlinCoordinatesSpirv) : sizeof(shader) - 1;
    dispatch.code_format = vulkan ? PS_GPU_CODE_SPIRV_V1 : PS_GPU_CODE_MSL_V1;
    dispatch.entry = "produce";
    dispatch.entry_size = 7;
    dispatch.buffers = &binding;
    dispatch.buffer_count = 1;
    dispatch.constants = vulkan ? vulkan_values : values;
    dispatch.constant_size = vulkan ? sizeof(vulkan_values) : sizeof(values);
    dispatch.constant_index = 1;
    dispatch.grid[0] = 15;
    dispatch.grid[1] = dispatch.grid[2] = 1;
    if (api->execute(api->context, &dispatch, 1))
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "source dispatch"});
    auto builder = numeric_result_fixture::take(
        ResultBuilder::start(call.resources, *call.query.output.result_schema,
                             call.query.semantic_key));
    auto relation = numeric_result_fixture::take(
        ResultRelation::cartesian(call.resources, 15, {}));
    auto status = builder.bind_descriptor_relation(numeric_result_fixture::take(
        ResultRelation::cartesian(call.resources, 1, {})));
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    status = builder.publish_tensor(
        0, Region::whole({5, 3}), {81, {32, -8}, {1, 2}},
        std::move(buffer).freeze(), relation, {true, true, true, true});
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    return Result<ResultProgramPoll>(
        ResultPublication{numeric_result_fixture::take(builder.seal()), true});
  }
};
int native_layout() {
  auto registry = make_default_operation_registry(false);
  OperationDefinition source;
  source.key = "test.native_coordinates";
  source.traits = unified_example::traits(0, sizeof(NativeCoordinates));
  source.traits.supports_cpu = false;
  source.traits.supports_gpu = true;
  auto& output = source.traits.outputs[0];
  output.key = "coordinates";
  output.region_rule = OperationRegionRule::Whole;
  auto schema = numeric_result_fixture::source_schema(
      input_value(std::vector<std::uint64_t>(15), false));
  unified_example::result_output(&output, schema);
  source.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<NativeCoordinates>(allocator);
  };
  PS_CHECK(registry->register_operation(std::move(source)).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.nodes = {{1, "test.native_coordinates", {}, {}},
                    {2,
                     "noise.perlin2002_3d_v1_strict_gpu",
                     {WorkflowNodeOutput{1, "coordinates"}},
                     {}}};
  document.outputs = {{"values", 2, "values"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_CHECK(compiled.ok());
  ResultRef retained;
  {
    ExecutionContextConfig config;
    config.gpu_enabled = true;
    config.managed_resources = ResourceLimits{};
    ExecutionContext context(registry, config);
    auto result = context.execute(compiled.value().plan);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok() &&
             result.value().diagnostics.native_dispatch_count == 2);
    PS_CHECK(result.value().diagnostics.fallback_reasons.empty());
    retained = result.value().results.at("values");
    PS_CHECK(result.value().diagnostics.transfer_count == 0);
  }
  const auto retained_bytes = numeric_result_fixture::bytes(retained);
  const double expected[] = {1785. / 16384., 75. / 512., 1635. / 16384., 0.,
                             -1635. / 16384.};
  for (unsigned i = 0; i < 5; ++i) {
    double result = 0;
    std::memcpy(&result, retained_bytes.data() + i * 8, 8);
    PS_CHECK(result == expected[i]);
  }
  return 0;
}
struct CancelAfterSubmission {
  ResultContinuation inner;
  CancellationSource* cancellation;
  unsigned* submissions;
  unsigned* dispatches;
  CancelAfterSubmission(ResultContinuation continuation,
                        CancellationSource* token, unsigned* calls,
                        unsigned* commands)
      : inner(std::move(continuation)),
        cancellation(token),
        submissions(calls),
        dispatches(commands) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& call) {
    struct State {
      const ps_gpu_service_v1* native;
      CancellationSource* cancellation;
      unsigned* submissions;
      unsigned* dispatches;
    } state{call.gpu, cancellation, submissions, dispatches};
    ps_gpu_service_v1 proxy = *call.gpu;
    proxy.context = &state;
    proxy.buffer = [](void* context, const std::uint8_t* bytes,
                      std::uint64_t size, std::uint32_t writable,
                      std::uint64_t* token) {
      const auto& s = *static_cast<State*>(context);
      return s.native->buffer(s.native->context, bytes, size, writable, token);
    };
    proxy.release = [](void* context, std::uint64_t token) {
      const auto& s = *static_cast<State*>(context);
      return s.native->release(s.native->context, token);
    };
    proxy.execute = [](void* context, const ps_gpu_dispatch_v1* commands,
                       std::uint32_t count) {
      auto& s = *static_cast<State*>(context);
      const auto result = s.native->execute(s.native->context, commands, count);
      if (!result) {
        ++*s.submissions;
        *s.dispatches += count;
        s.cancellation->cancel();
      }
      return result;
    };
    auto nested = call;
    nested.gpu = &proxy;
    return inner.poll(nested);
  }
};
int cancellation_between_submissions() {
  auto registry = make_default_operation_registry(false);
  auto* borrowed_registry = registry.get();
  CancellationSource cancellation;
  unsigned submissions = 0, dispatches = 0;
  OperationDefinition wrapper;
  wrapper.key = "test.cancel_native_perlin";
  wrapper.traits =
      registry->find_traits("noise.perlin2002_3d_v1_strict_gpu").take_value();
  wrapper.traits.requires_metadata_specialization = false;
  wrapper.traits.outputs[0].result_schema->tensors[0].descriptor.shape = {2057};
  wrapper.traits.outputs[0].continuation_bytes += sizeof(CancelAfterSubmission);
  wrapper.start_result =
      [&](const auto& query,
          const auto& allocator) -> Result<ResultContinuation> {
    auto nested = query;
    nested.prepared.reset();
    auto inner = borrowed_registry->start_result(
        "noise.perlin2002_3d_v1_strict_gpu", nested, allocator);
    if (!inner.ok())
      return inner;
    return ResultContinuation::make<CancelAfterSubmission>(
        allocator, inner.take_value(), &cancellation, &submissions,
        &dispatches);
  };
  PS_CHECK(registry->register_operation(std::move(wrapper)).ok());
  PS_CHECK(registry->freeze().ok());
  std::vector<std::uint64_t> raw(2057 * 3, UINT64_C(0x3fd0000000000000));
  const auto input = input_value(raw, false);
  WorkflowDocument document;
  perlin_fixture::declare(&document, input);
  document.nodes = {
      {1, "test.cancel_native_perlin", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"values", 1, "values"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_CHECK(compiled.ok());
  ExecutionContextConfig config;
  config.gpu_enabled = true;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto result = context.execute(
      compiled.value().plan,
      perlin_fixture::bind(context.resource_budget().take_value(), input),
      cancellation.token());
  if (result.ok() || result.status().code != ErrorCode::Cancelled)
    std::cerr << "unexpected mid-GPU cancellation result "
              << (result.ok() ? "success" : result.status().message)
              << " calls=" << submissions << '\n';
  PS_CHECK(!result.ok() && result.status().code == ErrorCode::Cancelled);
  PS_CHECK(submissions == 1 && dispatches == 8);
  PS_CHECK(context.resource_budget()
               .value()
               .statistics()
               .live[ResourceKind::Payload] == 0);
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
    unsigned input_narrow, output_narrow;
    std::vector<std::uint64_t> raw(3);
    while (std::cin >> std::dec >> input_narrow >> output_narrow >> std::hex >>
           raw[0] >> raw[1] >> raw[2]) {
      auto result =
          run(registry, context, input_value(raw, input_narrow), output_narrow);
      if (!result.ok()) {
        std::cerr << result.status().message << '\n';
        return 1;
      }
      PS_CHECK(dispatched(result.value()) == 0);
      std::uint64_t bits = 0;
      const auto value =
          numeric_result_fixture::bytes(result.value().results.at("values"));
      std::memcpy(&bits, value.data(), output_narrow ? 4 : 8);
      std::cout << std::hex << bits << '\n';
    }
    return std::cin.eof() ? 0 : 2;
  }
  PS_CHECK(native_layout() == 0);
  PS_CHECK(cancellation_between_submissions() == 0);
  std::vector<std::uint64_t> raw(2057 * 3, 0);
  for (unsigned i = 0; i < raw.size(); ++i) {
    const double value =
        static_cast<double>((i * 1709U + 719U) % 131071U) / 65536. - 1.;
    std::memcpy(&raw[i], &value, 8);
  }
  for (bool narrow : {false, true}) {
    const auto input = input_value(raw, false);
    auto result = run(registry, context, input, narrow);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok() && dispatched(result.value()) == 0);
    PS_CHECK(result.value().diagnostics.native_dispatch_count > 1);
    PS_CHECK(result.value().diagnostics.native_submission_count > 1);
    auto reference = run(registry, context, input, narrow, false);
    PS_CHECK(reference.ok());
    PS_CHECK(
        numeric_result_fixture::bytes(result.value().results.at("values")) ==
        numeric_result_fixture::bytes(reference.value().results.at("values")));
  }
  const auto pieces = input_value(
      {UINT64_C(0x3fd0000000000000), 0, 0, UINT64_C(0x3fe0000000000000), 0, 0},
      false);
  for (bool gpu : {false, true}) {
    auto fragmented = run(registry, context, pieces, false, gpu, {}, true);
    auto packed = run(registry, context, pieces, false, gpu);
    PS_CHECK(fragmented.ok() && packed.ok());
    PS_CHECK(
        numeric_result_fixture::bytes(
            fragmented.value().results.at("values")) ==
        numeric_result_fixture::bytes(packed.value().results.at("values")));
    if (gpu) {
      PS_CHECK(dispatched(fragmented.value()) == 0);
      PS_CHECK(fragmented.value().diagnostics.transfer_count == 1 &&
               fragmented.value().diagnostics.transfer_bytes == 48);
    }
  }
  const auto tiny = input_value(
      {1, UINT64_C(0x8000000000000001), UINT64_C(0x3fd0000000000000)}, false);
  for (bool narrow : {false, true}) {
    auto result = run(registry, context, tiny, narrow);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok() && dispatched(result.value()) == 0);
    auto reference = run(registry, context, tiny, narrow, false);
    PS_CHECK(reference.ok() && numeric_result_fixture::bytes(
                                   result.value().results.at("values")) ==
                                   numeric_result_fixture::bytes(
                                       reference.value().results.at("values")));
  }
  auto invalid =
      run(registry, context,
          input_value({UINT64_C(0x7ff0000000000001), 0, 0}, false), false);
  PS_CHECK(!invalid.ok() &&
           invalid.status().code == ErrorCode::InvalidArgument);
  PS_CHECK(empty_request(registry, context) == 0);
  CancellationSource cancellation;
  cancellation.cancel();
  auto cancelled =
      run(registry, context, tiny, false, true, cancellation.token());
  PS_CHECK(!cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled);
  config.managed_resources->maximum_work = 10000;
  ExecutionContext limited(registry, config);
  auto exhausted = run(registry, limited, tiny, false);
  PS_CHECK(!exhausted.ok() &&
           exhausted.status().code == ErrorCode::ResourceExhausted);
  return 0;
}
