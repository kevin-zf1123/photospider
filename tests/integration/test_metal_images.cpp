#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "s4_gpu_workflow/image_fixture.hpp"

namespace {
using s1_fixture::check;
using s1_fixture::take;
using s4_fixture::require;
using Poll = ps::Result<ps::ResultProgramPoll>;
struct NativeImage {
  bool channel_origin;
  explicit NativeImage(bool origin) : channel_origin(origin) {}
  Poll poll(const ps::ResultProgramPhase& call) {
    const auto* api = call.gpu;
    if (!api || api->backend != PS_GPU_BACKEND_METAL_V1)
      return Poll(
          ps::Status{ps::ErrorCode::BackendUnavailable, "requires Metal"});
    auto output = take(call.allocator.allocate(channel_origin ? 16 : 17));
    std::uint64_t token = 0;
    require(
        api->buffer(api->context, output.data(), output.size(), 1, &token) == 0,
        "native image buffer binding");
    const char shader[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void pad(device uchar* b [[buffer(0)]], "
        "constant uint& shift [[buffer(1)]], "
        "uint i [[thread_position_in_grid]]){"
        "uint bits=0x3f800000u; b[i+shift]=uchar(bits>>(8*(i%4)));}";
    const ps_gpu_buffer_binding_v1 binding{
        sizeof(ps_gpu_buffer_binding_v1), 0, token, 0, output.size(), 1};
    ps_gpu_dispatch_v1 command{};
    command.struct_size = sizeof(command);
    command.source = shader;
    command.source_size = sizeof(shader) - 1;
    command.code_format = PS_GPU_CODE_MSL_V1;
    command.entry = "pad";
    command.entry_size = 3;
    command.buffers = &binding;
    command.buffer_count = 1;
    const std::uint32_t shift = channel_origin ? 0 : 1;
    command.constants = &shift;
    command.constant_size = sizeof(shift);
    command.constant_index = 1;
    command.grid[0] = 16;
    command.grid[1] = command.grid[2] = 1;
    const auto submitted = api->execute(api->context, &command, 1);
    const auto released = api->release(api->context, token);
    require(submitted == 0 && released == 0, "native image dispatch/release");
    const auto layout =
        channel_origin
            ? ps::StridedLayout{4, {16, 16, 16, 16, 4}, {0, 0, 0, 0, 1}}
            : ps::StridedLayout{1, {16, 16, 16, 16, 4}};
    auto builder = take(ps::ResultBuilder::start(
        call.resources, *call.query.output.result_schema,
        call.query.semantic_key));
    check(builder.bind_descriptor_relation(
        take(ps::ResultRelation::cartesian(call.resources, 1, {}))));
    check(builder.publish_tensor(
        0, ps::Region::whole({1, 1, 1, 1, 4}), layout,
        std::move(output).freeze(),
        take(ps::ResultRelation::cartesian(call.resources, 4, {})),
        {true, true, true, true}));
    return Poll(ps::ResultPublication{take(builder.seal()), true});
  }
};
void check_native_alignment(bool channel_origin) {
  auto registry = ps::make_default_operation_registry(false);
  ps::OperationDefinition source;
  source.key = "test.native_image";
  source.traits.input_count = 0;
  source.traits.supports_cpu = false;
  source.traits.supports_gpu = true;
  source.traits.workspace_bytes = 64;
  auto& out = source.traits.outputs[0];
  out.output_schema.kind = ps::OperationPortKind::Result;
  out.output_schema.result_schema_id = "photospider.image";
  out.output_schema.result_schema_version = 1;
  out.result_schema = s1_fixture::schema({1, 1, 4});
  out.dependency_version = 2;
  out.region_rule = ps::OperationRegionRule::Whole;
  out.continuation_bytes = sizeof(NativeImage);
  out.maximum_dependency_stages = 1;
  source.start_result = [channel_origin](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<NativeImage>(allocator, channel_origin);
  };
  check(registry->register_operation(std::move(source)));
  check(registry->freeze());
  ps::ResultRef retained;
  {
    ps::ExecutionContextConfig config;
    config.gpu_enabled = true;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(registry, config);
    const auto root = take(execution.resource_budget());
    const auto factor = s1_fixture::scalar(root, .5F);
    ps::WorkflowDocument document;
    document.inputs = {s1_fixture::declaration(1, "factor", factor)};
    document.nodes = {
        {1, "test.native_image", {}, {}},
        {2,
         "image.opacity",
         {ps::WorkflowNodeOutput{1, "value"}, ps::WorkflowInputReference{1}},
         {}}};
    document.outputs = {{"result", 2, "value"}, {"source", 1, "value"}};
    ps::GraphContext graph(document);
    ps::PlanningOptions planning;
    planning.execution_mode = ps::ExecutionMode::NativeGpu;
    auto compiled = take(ps::Compiler(registry).compile(graph, planning));
    auto result =
        take(execution.execute(compiled.plan, {{{"factor", factor}}}));
    const auto& published = result.results.at("source");
    const auto window = take(published.acquire_tensor(
        take(published.descriptor()), 0, ps::Region::whole({1, 1, 1, 1, 4})));
    const auto row = take(window.row_run({0, 0, 0, 0, 0}));
    require(reinterpret_cast<std::uintptr_t>(row.data) % 4 ==
                (channel_origin ? 0 : 1),
            "native source must retain its sample alignment");
    const auto samples = s4_fixture::samples(published);
    require(samples == std::vector<float>({1, 1, 1, 1}),
            "native producer bytes or origin mapping changed");
    const auto& diagnostics = result.diagnostics;
    const auto expected_backend =
        channel_origin ? ps::Backend::Gpu : ps::Backend::Cpu;
    require(
        diagnostics.native_dispatch_count == (channel_origin ? 2 : 1) &&
            diagnostics.transfer_count == 0 &&
            diagnostics.fallback_reasons.size() == (channel_origin ? 0 : 1) &&
            diagnostics.selected_backends.at(
                compiled.plan.steps().back().result_ref()) == expected_backend,
        "native alignment admission/fallback changed");
    std::cout << "layout channel_origin=" << channel_origin
              << " native_dispatches=" << diagnostics.native_dispatch_count
              << " transfers=" << diagnostics.transfer_count
              << " fallbacks=" << diagnostics.fallback_reasons.size() << '\n';
    retained = result.results.at("result");
  }
  require(
      s4_fixture::samples(retained) == std::vector<float>({.5F, .5F, .5F, .5F}),
      "native image mapping or context retirement lost output samples");
}
void check_domain_failures(
    ps::ExecutionContext& execution,
    const std::shared_ptr<ps::OperationRegistry>& operations,
    ps::ExecutionMode mode) {
  ps::Compiler compiler(operations);
  const auto root = take(execution.resource_budget());
  unsigned rejected_samples = 0, rejected_schemas = 0;
  for (unsigned kind = 0; kind < 8; ++kind) {
    auto scene = s4_fixture::scene(root, kind);
    ps::GraphContext graph(scene.document);
    ps::PlanningOptions options;
    options.execution_mode = mode;
    auto compiled = take(compiler.compile(graph, options));
    for (unsigned failure = 0; failure < 5; ++failure) {
      auto bindings = scene.bindings;
      const auto& original = bindings.inputs[0].result;
      auto pixels = s4_fixture::samples(original);
      const float invalid =
          failure == 0   ? std::numeric_limits<float>::quiet_NaN()
          : failure == 1 ? std::numeric_limits<float>::infinity()
          : failure == 2 ? -1.F
                         : 1.25F;
      pixels[kind == 6 || failure < 2 ? 0 : 3] = invalid;
      if (failure == 4 && kind != 6) {
        pixels[0] = -1;
        pixels[3] = 0;
      }
      bindings.inputs[0].result = s1_fixture::tensor(
          root, pixels, original.schema().tensors[0].descriptor.shape,
          kind != 6);
      const auto before = root.statistics().live[ps::ResourceKind::Payload];
      auto rejected = execution.execute(compiled.plan, bindings);
      require(!rejected.ok() &&
                  rejected.status().code == ps::ErrorCode::InvalidArgument,
              "invalid image/mask samples reached operation");
      require(root.statistics().live[ps::ResourceKind::Payload] == before,
              "invalid image execution retained temporary payload");
      ++rejected_samples;
    }
    for (std::size_t input = 0; input < scene.document.inputs.size(); ++input) {
      const auto& original = *scene.document.inputs[input].result_schema;
      if (!original.tensors[0].layout.spatial)
        continue;
      auto document = scene.document;
      auto altered = std::make_shared<ps::SchemaTemplate>(original);
      document.inputs[input].result_schema = altered;
      altered->tensors[0].facets.clear();
      ps::GraphContext missing(document);
      require(compiler.compile(missing, options).status().code ==
                  ps::ErrorCode::TypeMismatch,
              "untyped spatial input accepted");
      ++rejected_schemas;
      if (original.tensors[0].descriptor.shape.size() != 3)
        continue;
      auto straight = ps::rgba_semantics();
      straight.association = "straight";
      altered->tensors[0].facets = {take(ps::encode_semantic(straight))};
      ps::GraphContext wrong_association(document);
      require(compiler.compile(wrong_association, options).status().code ==
                  ps::ErrorCode::TypeMismatch,
              "straight image accepted by premultiplied operation");
      altered->tensors[0].facets = {{"photospider.image", 1, {}}};
      ps::GraphContext old_image(document);
      require(compiler.compile(old_image, options).status().code ==
                  ps::ErrorCode::InvalidArgument,
              "malformed image metadata accepted");
      rejected_schemas += 2;
    }
  }
  std::cout << "domain rejected_samples=" << rejected_samples
            << " rejected_schemas=" << rejected_schemas << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const bool native = argc == 2 && std::string(argv[1]) == "--native-only";
#if !defined(__APPLE__)
    if (native) {
      std::cout << "native Metal fixture unavailable on this platform\n";
      return 77;
    }
#endif
    auto operations = ps::make_default_operation_registry();
    ps::ExecutionContextConfig config;
    config.gpu_enabled = native;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(operations, config);
    if (native && !execution.gpu_enabled())
      return 77;
    check_domain_failures(
        execution, operations,
        native ? ps::ExecutionMode::NativeGpu : ps::ExecutionMode::CpuExact);
    if (native) {
      check_native_alignment(false);
      check_native_alignment(true);
    }
    std::cout << "Result image domain/layout contracts passed native=" << native
              << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
