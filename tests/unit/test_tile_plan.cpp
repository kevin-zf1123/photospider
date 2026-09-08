#include <cstdint>
#include <limits>
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
  traits.output_element_type = ps::ElementType::Float32;
  traits.shape_rule = ps::OperationShapeRule::PreserveFirstInput;
  traits.region_rule = ps::OperationRegionRule::Elementwise;
  traits.input_schema.resize(
      inputs, {ps::OperationPortKind::LinearPremultipliedRgbaFloat32, 0, 0});
  traits.output_schema = traits.input_schema[0];
  return traits;
}
bool region(const ps::Region& r, std::uint64_t y, std::uint64_t h,
            std::uint64_t x, std::uint64_t w) {
  return r.rank() >= 2 && r.dimensions()[0].offset == y &&
         r.dimensions()[0].extent == h && r.dimensions()[1].offset == x &&
         r.dimensions()[1].extent == w;
}
}  // namespace
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  auto dummy = [](const OperationInvocation& call) {
    return Result<Value>(call.inputs[0]);
  };
  auto blur = image_traits(1);
  blur.region_rule = OperationRegionRule::Halo;
  blur.halo_radius_parameter = "radius";
  blur.parameter_schema = {
      {"radius", OperationParameterType::Int64, true, true, 1, 64},
      {"sigma", OperationParameterType::Float64, true, true, .1, 64}};
  PS_CHECK(registry->register_operation({"blur", blur, dummy}).ok());
  auto gain = image_traits(2);
  gain.input_schema[1] = {OperationPortKind::Float32Scalar, 0, 16};
  PS_CHECK(registry->register_operation({"gain", gain, dummy}).ok());
  auto mask = image_traits(2);
  mask.input_schema[1] = {OperationPortKind::Float32Mask, 0, 0};
  PS_CHECK(registry->register_operation({"mask", mask, dummy}).ok());
  PS_CHECK(registry->register_operation({"over", image_traits(2), dummy}).ok());
  auto whole = image_traits(1);
  whole.region_rule = OperationRegionRule::Whole;
  PS_CHECK(registry->register_operation({"whole", whole, dummy}).ok());
  auto effect = image_traits(1);
  effect.side_effect_free = false;
  effect.cacheable = false;
  PS_CHECK(registry->register_operation({"effect", effect, dummy}).ok());
  auto generic = image_traits(1);
  generic.input_schema[0] = {};
  generic.output_schema = {};
  PS_CHECK(registry->register_operation({"generic", generic, dummy}).ok());
  PS_CHECK(registry->freeze().ok());
  const std::string profile = "rgba;linear-srgb;premultiplied;hwc";
  const ValueFacet facet{"photospider.image",
                         1,
                         {profile.begin(), profile.end()}};
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
                      {}},
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
  PS_CHECK(plan.steps()[0].traits.halo_radius == 2);
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
  auto generic_parent = compiler.compile(generic_graph);
  PS_CHECK(generic_parent.ok());
  PS_CHECK(generic_parent.value()
               .plan.tile_plan("result", Region({{1, 1}, {1, 1}, {1, 3}}))
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
