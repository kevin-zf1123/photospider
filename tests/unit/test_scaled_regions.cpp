#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  for (bool image : {false, true}) {
    OperationTraits traits;
    traits.input_count = 1;
    traits.output_element_type = ElementType::Float32;
    traits.shape_rule = OperationShapeRule::Shrink;
    traits.region_rule = OperationRegionRule::Shrink;
    traits.spatial_factor_parameter = "factor";
    traits.parameter_schema = {
        {"factor", OperationParameterType::Int64, true, true, 1, 16}};
    traits.output_schema.kind =
        image ? OperationPortKind::LinearPremultipliedRgbaFloat32
              : OperationPortKind::Float32Mask;
    traits.input_schema = {traits.output_schema};
    auto registry = std::make_shared<OperationRegistry>();
    PS_CHECK(registry
                 ->register_operation({"shrink", traits,
                                       [](const OperationInvocation&) {
                                         return Result<Value>(Value{});
                                       }})
                 .ok());
    auto invalid = traits;
    invalid.parameter_schema[0].maximum = 17;
    PS_CHECK(!registry
                  ->register_operation({"invalid", invalid,
                                        [](const OperationInvocation&) {
                                          return Result<Value>(Value{});
                                        }})
                  .ok());
    PS_CHECK(registry->freeze().ok());
    const std::string profile = "rgba;linear-srgb;premultiplied;hwc";
    WorkflowDocument document;
    const std::vector<std::uint64_t> shape =
        image ? std::vector<std::uint64_t>{7, 11, 4}
              : std::vector<std::uint64_t>{7, 11};
    document.inputs = {
        {1,
         "input",
         {ElementType::Float32, shape},
         Region::whole(shape),
         image ? StridedLayout{0, {176, 16, 4}} : StridedLayout{0, {44, 4}},
         image ? std::vector<ValueFacet>{{"photospider.image",
                                          1,
                                          {profile.begin(), profile.end()}}}
               : std::vector<ValueFacet>{}}};
    document.nodes = {
        {1, "shrink", {WorkflowInputReference{1}}, {{"factor", INT64_C(4)}}}};
    document.outputs = {{"result", 1, "value"}};
    GraphContext graph(document);
    Compiler compiler(registry);
    auto compiled = compiler.compile(graph);
    PS_CHECK(compiled.ok());
    const auto& step = compiled.value().plan.steps()[0];
    PS_CHECK(step.output_descriptor.shape[0] == 2 &&
             step.output_descriptor.shape[1] == 3);
    auto dims = Region::whole(step.output_descriptor.shape).dimensions();
    dims[0] = {1, 1};
    dims[1] = {2, 1};
    auto tile = compiled.value().plan.tile_plan("result", Region(dims));
    PS_CHECK(tile.ok());
    const auto& demand = tile.value().steps()[0].input_demands[0].dimensions();
    PS_CHECK(demand[0].offset == 4 && demand[0].extent == 3);
    PS_CHECK(demand[1].offset == 8 && demand[1].extent == 3);
    auto dirty_dims = Region::whole(shape).dimensions();
    dirty_dims[0] = {3, 2};
    dirty_dims[1] = {7, 2};
    auto dirty = operation_dirty_region(step.traits, Region(dirty_dims), shape,
                                        step.output_descriptor.shape,
                                        traits.output_schema.kind);
    PS_CHECK(dirty.ok() && dirty.value().dimensions()[0].extent == 2 &&
             dirty.value().dimensions()[1].extent == 2);
    for (std::int64_t bad : {0, 17}) {
      document.nodes[0].parameters["factor"] = bad;
      GraphContext invalid_graph(document);
      PS_CHECK(!compiler.compile(invalid_graph).ok());
    }
  }
  return 0;
}
