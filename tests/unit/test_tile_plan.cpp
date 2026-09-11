#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
ps::OperationTraits image_traits(std::uint32_t inputs) {
  ps::OperationTraits traits;
  traits.input_count = inputs;
  traits.outputs[0].output_element_type = ps::ElementType::Float32;
  traits.outputs[0].output_semantic_rule =
      ps::OperationSemanticRule::PreserveInput;
  traits.outputs[0].shape_rule = ps::OperationShapeRule::PreserveFirstInput;
  traits.outputs[0].region_rule = ps::OperationRegionRule::Elementwise;
  traits.input_schema.resize(inputs,
                             {ps::OperationPortKind::RgbaFloat32, 0, 0});
  traits.outputs[0].output_schema = traits.input_schema[0];
  return traits;
}
bool region(const ps::Region& r, std::uint64_t y, std::uint64_t h,
            std::uint64_t x, std::uint64_t w) {
  return r.rank() >= 2 && r.dimensions()[0].offset == y &&
         r.dimensions()[0].extent == h && r.dimensions()[1].offset == x &&
         r.dimensions()[1].extent == w;
}
int inferred_image_regions() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = make_default_operation_registry();
  Compiler compiler(registry);
  ExecutionContext execution(registry);
  for (std::uint64_t channels : {3U, 4U}) {
    auto semantic = rgba_semantics();
    if (channels == 3) {
      semantic.channels.pop_back();
      semantic.association = "none";
    }
    const auto facet = encode_semantic(semantic).take_value();
    const ValueDescriptor descriptor{ElementType::Float32, {2, 3, channels}};
    const auto full = Region::whole(descriptor.shape);
    const Region roi({{1, 1}, {1, 2}, {0, channels}});
    const Region partial({{1, 1}, {1, 2}, {1, channels - 1}});
    const auto bytes = std::vector<std::uint8_t>(2 * 3 * channels * 4);
    OperationRegistry regional;
    unsigned calls = 0;
    OperationTraits traits;
    traits.input_count = 1;
    traits.input_schema.resize(1);
    traits.outputs[0].output_element_type = ElementType::Float32;
    traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
    traits.outputs[0].region_rule = OperationRegionRule::Elementwise;
    traits.outputs[0].output_semantic_rule =
        OperationSemanticRule::PreserveInput;
    PS_CHECK(regional
                 .register_operation({"regional", traits,
                                      [&](const OperationInvocation& call) {
                                        ++calls;
                                        return call.inputs[0].view(
                                            call.output_region);
                                      }})
                 .ok());
    PS_CHECK(regional.freeze().ok());
    const auto typed =
        Value::create(descriptor, full,
                      {0,
                       {static_cast<std::int64_t>(3 * channels * 4),
                        static_cast<std::int64_t>(channels * 4), 4}},
                      bytes, {facet})
            .take_value();
    const std::vector<Value> regional_inputs{typed};
    const std::vector<Region> regional_demands{full};
    const std::map<std::string, ParameterValue> no_parameters;
    OperationInvocation regional_call{regional_inputs, regional_demands,
                                      no_parameters};
    regional_call.output_region = partial;
    PS_CHECK(regional.invoke("regional", regional_call).status().code ==
             ErrorCode::InvalidArgument);
    PS_CHECK(calls == 0);
    regional_call.output_region = roi;
    auto regional_result = regional.invoke("regional", regional_call);
    PS_CHECK(regional_result.ok() && calls == 1);
    PS_CHECK(region(regional_result.value().region(), 1, 1, 1, 2));
    PS_CHECK(validate_semantic_value(semantic, regional_result.value()).ok());
    for (bool assign : {false, true}) {
      auto value = Value::create(descriptor, full,
                                 {0,
                                  {static_cast<std::int64_t>(3 * channels * 4),
                                   static_cast<std::int64_t>(channels * 4), 4}},
                                 bytes,
                                 assign ? std::vector<ValueFacet>{}
                                        : std::vector<ValueFacet>{facet})
                       .take_value();
      WorkflowDocument doc;
      doc.inputs = {
          {1, "input", descriptor, full, value.layout(), value.facets()}};
      doc.nodes = {{1,
                    assign ? "color.assign" : "core.identity",
                    {WorkflowInputReference{1}},
                    {}}};
      if (assign)
        doc.nodes[0].parameters = {
            {"semantic", semantic_parameter(semantic).take_value()}};
      doc.outputs = {{"output", 1, "value"}};
      if (assign) {
        auto generic = doc;
        generic.nodes[0].operation = "core.identity";
        generic.nodes[0].parameters.clear();
        GraphContext generic_graph(generic);
        PlanningOptions generic_options;
        generic_options.output_regions = {{"output", partial}};
        auto generic_plan = compiler.compile(generic_graph, generic_options);
        PS_CHECK(generic_plan.ok());
        auto generic_result =
            execution.execute(generic_plan.value().plan, {{{"input", value}}});
        PS_CHECK(generic_result.ok() &&
                 generic_result.value().values.at("output").facets().empty());
      }
      GraphContext graph(doc);
      PlanningOptions options;
      options.output_regions = {{"output", partial}};
      PS_CHECK(compiler.compile(graph, options).status().code ==
               ErrorCode::InvalidArgument);
      options.output_regions = {
          {"output", Region({{1, 1}, {1, 2}, {0, channels - 1}})}};
      PS_CHECK(compiler.compile(graph, options).status().code ==
               ErrorCode::InvalidArgument);
      const std::vector<Value> inputs{value};
      const std::vector<Region> demands{full};
      OperationInvocation call{inputs, demands, doc.nodes[0].parameters};
      call.output_region = partial;
      PS_CHECK(registry->invoke(doc.nodes[0].operation, call).status().code ==
               ErrorCode::InvalidArgument);
      call.output_region = full;
      auto direct = registry->invoke(doc.nodes[0].operation, call);
      PS_CHECK(direct.ok() &&
               validate_semantic_value(semantic, direct.value()).ok());
      options.output_regions = {{"output", roi}};
      options.tile_height = options.tile_width = 1;
      auto compiled = compiler.compile(graph, options);
      PS_CHECK(compiled.ok());
      const auto& plan = compiled.value().plan;
      PS_CHECK(plan.tile_plan("output", partial).status().code ==
               ErrorCode::InvalidArgument);
      PS_CHECK(plan.tile_plan("output", roi).ok());
      ExecutionBindings bindings{{{"input", value}}};
      auto result = execution.execute(plan, bindings);
      PS_CHECK(result.ok());
      const auto& output = result.value().values.at("output");
      PS_CHECK(output.region().dimensions()[0].offset == 1);
      PS_CHECK(output.region().dimensions()[2].extent == channels);
      PS_CHECK(validate_semantic_value(semantic, output).ok());
      auto frozen = execution.freeze(plan, bindings);
      PS_CHECK(frozen.ok());
      PS_CHECK(frozen.value().for_region("output", partial).status().code ==
               ErrorCode::InvalidArgument);
      unsigned tiles = 0;
      auto stream = execution.execute_stream(
          frozen.value(), [&](const std::string&, ValueView view) {
            ++tiles;
            const auto& region = view.region();
            if (region.dimensions()[2].offset != 0 ||
                region.dimensions()[2].extent != channels ||
                view.facets()[0].payload != facet.payload)
              return Status::failure(ErrorCode::TypeMismatch, "invalid tile");
            return Status::success();
          });
      PS_CHECK(stream.ok() && tiles == 2);
    }
  }
  return 0;
}
}  // namespace
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(inferred_image_regions() == 0);
  auto registry = std::make_shared<OperationRegistry>();
  auto dummy = [](const OperationInvocation& call) {
    return Result<Value>(call.inputs[0]);
  };
  auto blur = image_traits(1);
  blur.outputs[0].region_rule = OperationRegionRule::Halo;
  blur.outputs[0].halo_radius_parameter = "radius";
  blur.parameter_schema = {
      {"radius", OperationParameterType::Int64, true, true, 1, 64},
      {"sigma", OperationParameterType::Float64, true, true, .1, 64}};
  PS_CHECK(registry->register_operation({"blur", blur, dummy}).ok());
  auto gain = image_traits(2);
  gain.supports_gpu = gain.allows_cpu_fallback = true;
  gain.input_schema[1] = {OperationPortKind::Float32Scalar, 0, 16};
  PS_CHECK(registry->register_operation({"gain", gain, dummy}).ok());
  auto mask = image_traits(2);
  mask.supports_gpu = mask.allows_cpu_fallback = true;
  mask.input_schema[1] = {OperationPortKind::Float32Mask, 0, 0};
  PS_CHECK(registry->register_operation({"mask", mask, dummy}).ok());
  PS_CHECK(registry->register_operation({"over", image_traits(2), dummy}).ok());
  auto whole = image_traits(1);
  whole.outputs[0].region_rule = OperationRegionRule::Whole;
  PS_CHECK(registry->register_operation({"whole", whole, dummy}).ok());
  auto effect = image_traits(1);
  effect.side_effect_free = false;
  effect.cacheable = false;
  PS_CHECK(registry->register_operation({"effect", effect, dummy}).ok());
  auto generic = image_traits(1);
  generic.input_schema[0] = {};
  generic.outputs[0].output_schema = {};
  PS_CHECK(
      registry->register_operation({"generic_preserve", generic, dummy}).ok());
  generic.outputs[0].output_semantic_rule = OperationSemanticRule::Drop;
  PS_CHECK(registry->register_operation({"generic", generic, dummy}).ok());
  PS_CHECK(registry->freeze().ok());

  const ValueFacet facet =
      ps::encode_semantic(ps::rgba_semantics()).take_value();
  WorkflowDocument document;
  document.inputs = {{1,
                      "foreground",
                      {ElementType::Float32, {7, 11, 4}},
                      Region::whole({7, 11, 4}),
                      {0, {176, 16, 4}},
                      {facet}},
                     {2,
                      "gain",
                      {ElementType::Float32, {1}},
                      Region::whole({1}),
                      {0, {4}},
                      {}},
                     {3,
                      "mask",
                      {ElementType::Float32, {7, 11}},
                      Region::whole({7, 11}),
                      {0, {44, 4}},
                      {encode_semantic(coverage_semantics()).take_value()}},
                     {4,
                      "background",
                      {ElementType::Float32, {7, 11, 4}},
                      Region::whole({7, 11, 4}),
                      {0, {176, 16, 4}},
                      {facet}}};
  document.nodes = {
      {10,
       "blur",
       {WorkflowInputReference{1}},
       {{"radius", INT64_C(2)}, {"sigma", 1.25}}},
      {20,
       "gain",
       {WorkflowNodeOutput{10, "value"}, WorkflowInputReference{2}},
       {}},
      {30,
       "mask",
       {WorkflowNodeOutput{20, "value"}, WorkflowInputReference{3}},
       {}},
      {40,
       "over",
       {WorkflowNodeOutput{30, "value"}, WorkflowInputReference{4}},
       {}}};
  document.outputs = {{"result", 40, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  PlanningOptions options;
  options.output_regions = {{"result", Region({{2, 3}, {3, 5}, {0, 4}})}};
  options.tile_height = 1;
  options.tile_width = 2;
  auto compiled = compiler.compile(graph, options);
  PS_CHECK(compiled.ok());
  const auto& plan = compiled.value().plan;
  PS_CHECK(plan.tile_height() == 1 && plan.tile_width() == 2);
  PS_CHECK(plan.steps()[0].traits.outputs[0].halo_radius == 2);
  auto tile = plan.tile_plan("result", Region({{2, 1}, {3, 2}, {0, 4}}));
  PS_CHECK(tile.ok() && tile.value().steps().size() == 4);
  PS_CHECK(region(tile.value().steps()[0].input_demands[0], 0, 5, 1, 6));
  PS_CHECK(region(tile.value().steps()[2].input_demands[1], 2, 1, 3, 2));
  PS_CHECK(tile.value().steps()[2].input_demands[1].rank() == 2);
  PS_CHECK(tile.value().steps()[1].input_demands[1].rank() == 1);
  PS_CHECK(tile.value().steps()[0].planned_bytes == 32);
  PS_CHECK(!plan.tile_plan("missing", Region({{2, 1}, {3, 2}, {0, 4}})).ok());
  PS_CHECK(!plan.tile_plan("result", Region({{1, 1}, {3, 2}, {0, 4}})).ok());
  PS_CHECK(!plan.tile_plan("result", Region({{2, 1}, {3, 2}, {1, 3}})).ok());
  options.tile_width = 3;
  auto changed = compiler.plan(compiled.value().optimized, options);
  PS_CHECK(changed.ok() &&
           changed.value().digest().value != plan.digest().value);
  PS_CHECK(changed.value().optimized_digest().value ==
           plan.optimized_digest().value);
  options.execution_mode = ExecutionMode::MetalFp32;
  auto native = compiler.plan(compiled.value().optimized, options);
  PS_CHECK(native.ok());
  auto native_tile =
      native.value().tile_plan("result", Region({{2, 1}, {3, 2}, {0, 4}}));
  PS_CHECK(native_tile.ok());
  std::uint64_t uploads = 0, bytes = 0, host_access = 0;
  for (const auto& action : native_tile.value().physical_steps()) {
    if (action.kind == PhysicalStepKind::Upload) {
      ++uploads;
      bytes += action.packed_bytes;
      PS_CHECK(action.allocation_bytes == action.packed_bytes);
      PS_CHECK(action.destination_backend == Backend::Gpu);
      PS_CHECK(action.packed_layout.origin.size() == action.region.rank());
    }
    if (action.kind == PhysicalStepKind::HostAccess)
      ++host_access;
  }
  PS_CHECK(uploads == 3 && bytes == 44 && host_access == 1);
  PS_CHECK(native_tile.value().execution_mode() == ExecutionMode::MetalFp32);
  PS_CHECK(native.value().optimized_digest().value ==
           plan.optimized_digest().value);
  options.execution_mode = static_cast<ExecutionMode>(99);
  PS_CHECK(!compiler.plan(compiled.value().optimized, options).ok());
  options.execution_mode = ExecutionMode::CpuExact;
  options.tile_height = 0;
  PS_CHECK(compiler.plan(compiled.value().optimized, options).status().code ==
           ErrorCode::InvalidArgument);
  for (const ParameterValue& radius :
       {ParameterValue{INT64_C(0)}, ParameterValue{INT64_C(65)},
        ParameterValue{2.0}}) {
    auto bad = document;
    bad.nodes[0].parameters["radius"] = radius;
    GraphContext bad_graph(bad);
    PS_CHECK(compiler.compile(bad_graph).status().code ==
             ErrorCode::InvalidArgument);
  }
  for (double sigma : {0.0, 65.0, std::numeric_limits<double>::quiet_NaN()}) {
    auto bad = document;
    bad.nodes[0].parameters["sigma"] = sigma;
    GraphContext bad_graph(bad);
    PS_CHECK(compiler.compile(bad_graph).status().code ==
             ErrorCode::InvalidArgument);
  }
  for (const auto& key : {"whole", "effect"}) {
    auto forced = document;
    forced.nodes[0].operation = key;
    forced.nodes[0].parameters.clear();
    GraphContext forced_graph(forced);
    auto parent = compiler.compile(forced_graph);
    PS_CHECK(parent.ok());
    auto sub = parent.value().plan.tile_plan("result",
                                             Region({{6, 1}, {10, 1}, {0, 4}}));
    PS_CHECK(sub.ok() && sub.value().steps()[0].whole_boundary);
    PS_CHECK(region(sub.value().steps()[0].output_demand, 0, 7, 0, 11));
    PS_CHECK(region(sub.value().steps()[0].input_demands[0], 0, 7, 0, 11));
  }
  auto fan = document;
  fan.nodes.push_back({50,
                       "blur",
                       {WorkflowNodeOutput{10, "value"}},
                       {{"radius", INT64_C(1)}, {"sigma", 1.0}}});
  fan.nodes[3].inputs[1] = WorkflowNodeOutput{50, "value"};
  GraphContext fan_graph(fan);
  auto fan_parent = compiler.compile(fan_graph);
  PS_CHECK(fan_parent.ok());
  auto fan_tile = fan_parent.value().plan.tile_plan(
      "result", Region({{3, 1}, {5, 1}, {0, 4}}));
  PS_CHECK(fan_tile.ok());
  PS_CHECK(region(fan_tile.value().steps()[0].output_demand, 2, 3, 4, 3));
  PS_CHECK(region(fan_tile.value().steps()[0].input_demands[0], 0, 7, 2, 7));
  auto generic_document = document;
  generic_document.nodes.push_back(
      {60, "generic", {WorkflowNodeOutput{40, "value"}}, {}});
  generic_document.outputs = {{"result", 60, "value"}};
  GraphContext generic_graph(generic_document);
  PlanningOptions partial_options;
  partial_options.output_regions = {
      {"result", Region({{1, 1}, {1, 1}, {1, 3}})}};
  PS_CHECK(compiler.compile(generic_graph, partial_options).status().code ==
           ErrorCode::InvalidArgument);
  auto generic_parent = compiler.compile(generic_graph);
  PS_CHECK(generic_parent.ok());
  PS_CHECK(generic_parent.value()
               .plan.tile_plan("result", Region({{1, 1}, {1, 1}, {1, 3}}))
               .status()
               .code == ErrorCode::InvalidArgument);
  WorkflowDocument inferred_document;
  inferred_document.inputs = {document.inputs[0]};
  inferred_document.nodes = {
      {1, "generic_preserve", {WorkflowInputReference{1}}, {}},
      {2, "generic", {WorkflowNodeOutput{1, "value"}}, {}}};
  inferred_document.outputs = {{"result", 2, "value"}};
  GraphContext inferred_graph(inferred_document);
  PS_CHECK(compiler.compile(inferred_graph, partial_options).status().code ==
           ErrorCode::InvalidArgument);
  auto inferred_parent = compiler.compile(inferred_graph);
  PS_CHECK(inferred_parent.ok());
  PS_CHECK(
      inferred_parent.value()
          .plan.tile_plan("result", partial_options.output_regions.at("result"))
          .status()
          .code == ErrorCode::InvalidArgument);
  auto mismatched = document;
  mismatched.inputs[2].descriptor.shape = {11, 7};
  mismatched.inputs[2].region = Region::whole({11, 7});
  mismatched.inputs[2].layout = {0, {28, 4}};
  GraphContext mismatch_graph(mismatched);
  PS_CHECK(compiler.compile(mismatch_graph).status().code ==
           ErrorCode::TypeMismatch);
  return 0;
}
