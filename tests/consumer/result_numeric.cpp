#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/icc_fixture.hpp"
#include "photospider/ops/numeric/bezier.hpp"
#include "photospider/ops/numeric/color_ramps.hpp"
#include "photospider/ops/numeric/curves.hpp"
#include "photospider/ops/numeric/inverse_curves.hpp"
#include "photospider/ops/numeric/lowpass.hpp"
#include "photospider/ops/numeric/lut1d.hpp"
#include "photospider/ops/numeric/lut3d.hpp"
#include "photospider/ops/numeric/lut3d_baking.hpp"
#include "photospider/ops/numeric/shapers.hpp"
#include "photospider/photospider.hpp"

namespace {
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
ps::ResultRef source(const ps::ResourceBudget& root,
                     const std::vector<double>& values,
                     std::vector<uint64_t> shape = {},
                     ps::ElementType type = ps::ElementType::Float64) {
  if (shape.empty())
    shape = {values.size()};
  ps::SchemaTemplate schema;
  schema.id = "consumer.numeric";
  schema.version = 2;
  ps::ResultTensorSpec tensor;
  tensor.key = "coordinates";
  tensor.descriptor = {type, shape};
  schema.tensors.push_back(std::move(tensor));
  auto builder =
      take(ps::ResultBuilder::start(root, schema, "consumer.source"));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
              .ok(),
          "consumer source descriptor");
  auto bytes = take(root.allocator().allocate(values.size() * sizeof(double)));
  for (size_t i = 0; i < values.size(); ++i) {
    if (type == ps::ElementType::Int64) {
      const auto value = static_cast<int64_t>(values[i]);
      std::memcpy(bytes.data() + 8 * i, &value, 8);
    } else {
      std::memcpy(bytes.data() + 8 * i, &values[i], 8);
    }
  }
  std::vector<int64_t> strides(shape.size());
  int64_t stride = 8;
  for (size_t i = shape.size(); i > 0; --i) {
    strides[i - 1] = stride;
    stride *= shape[i - 1];
  }
  require(builder
              .publish_tensor(0, ps::Region::whole(shape), {0, strides},
                              std::move(bytes).freeze(),
                              take(ps::ResultRelation::cartesian(
                                  root, values.size(), {0, 1, 0, 0})),
                              {true, true, true, true})
              .ok(),
          "consumer source publication");
  return take(builder.seal());
}
uint64_t bits(const ps::ResultRef& result, std::vector<uint64_t> at = {0}) {
  uint64_t word = 0;
  require(result.read_tensor(take(result.descriptor()), 0, at, &word, 8).ok(),
          "consumer output read");
  return word;
}
}  // namespace

int main() {
  try {
    ps::ResultRef curve, identity, inverse, bezier, parametric, lut, channels,
        trilinear, tetrahedral, baked_table, bake_report, log_shaper,
        log_inverse, linear_inverse, xyz_ramp, cmyk_ramp, uniform, nonuniform,
        finite_sum, finite_clamp;
    ps::ColorProfileIdentity cmyk_identity;
    ps::ResultRelation prefix;
    {
      auto registry = ps::make_default_operation_registry();
      ps::ExecutionContextConfig config;
      config.managed_resources = ps::ResourceLimits{};
      config.managed_resources->capacity[ps::ResourceKind::Metadata] =
          64 * 1048576;
      ps::ExecutionContext execution(registry, config);
      auto root = take(execution.resource_budget());
      prefix = take(ps::ResultRelation::prefix(
          root, UINT64_MAX, 0, 1, ps::ResultSupportTarget::Tensor, 0));
      auto x = source(root, {0, 1, 3, 4});
      auto y = source(root, {0, 2, 3, 0});
      auto query = source(root, {.5});
      auto inverse_x = source(root, {0, 1, 2});
      auto inverse_y = source(root, {0, 1, 4});
      auto anchors = source(root, {0, 0, 1, 0}, {2, 2});
      auto handles = source(root, {0, 1}, {1, 1, 2});
      auto bezier_query = source(root, {.25});
      auto segments = source(root, {0}, {1}, ps::ElementType::Int64);
      auto channel_tables = source(root, {0, 10, 2, 8}, {2, 2});
      auto channel_axis = source(root, {0, 1, 1});
      auto color_query = source(root, {.75, .25, .5}, {1, 3});
      std::vector<double> cross_components;
      for (unsigned r = 0; r < 2; ++r)
        for (unsigned g = 0; g < 2; ++g)
          for (unsigned b = 0; b < 2; ++b)
            cross_components.insert(
                cross_components.end(),
                {static_cast<double>(r * g), static_cast<double>(g * b),
                 static_cast<double>(b * r)});
      auto color_table = source(root, cross_components, {2, 2, 2, 3});
      auto color_axes = source(root, {0, 1, 1, 0, 1, 1, 0, 1, 1}, {3, 3});
      auto lower = source(root, {1});
      auto upper = source(root, {16});
      auto ramp_colors = source(root, {0, 0, 0, 1, 2, 3}, {2, 3});
      auto ramp_stops = source(root, {0, 1});
      auto inks = source(root, {0, 0, 0, 0, 1, .5, 0, .25}, {2, 4});
      auto irregular_positions = source(root, {0, .75, 2});
      auto affine_values = source(root, {1, 2.5, 5});
      auto profile_bytes = numeric_fixture::fixture();
      auto profile = take(ps::IccProfile::import(
          {profile_bytes.data(), profile_bytes.size()}, root));
      cmyk_identity = profile.identity();
      auto profiles = take(ps::ResourceBindings::create({profile}, root));
      ps::WorkflowDocument document;
      ps::ExecutionBindings bindings;
      uint64_t id = 1;
      for (const auto& input : {x,
                                y,
                                query,
                                inverse_x,
                                inverse_y,
                                anchors,
                                handles,
                                bezier_query,
                                segments,
                                channel_tables,
                                channel_axis,
                                color_query,
                                color_table,
                                color_axes,
                                lower,
                                upper,
                                ramp_colors,
                                ramp_stops,
                                inks,
                                irregular_positions,
                                affine_values}) {
        ps::WorkflowInputDeclaration declaration;
        declaration.id = id++;
        declaration.name = "input" + std::to_string(declaration.id);
        declaration.result_schema =
            std::make_shared<ps::SchemaTemplate>(input.schema());
        bindings.inputs.push_back({declaration.name, input});
        document.inputs.push_back(std::move(declaration));
      }
      document.nodes.push_back(take(ps::numeric::interpolate_pchip_node(
          1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
          ps::WorkflowInputReference{3})));
      document.nodes.push_back(
          {2, "core.identity", {ps::WorkflowInputReference{3}}, {}});
      document.nodes.push_back(take(ps::numeric::invert_pchip_node(
          3, ps::WorkflowInputReference{4}, ps::WorkflowInputReference{5},
          ps::WorkflowInputReference{3})));
      document.nodes.push_back(take(ps::numeric::sample_bezier_function_node(
          4, ps::WorkflowInputReference{6}, ps::WorkflowInputReference{7},
          ps::WorkflowInputReference{8}, ps::WorkflowInputReference{8}, 2, 1)));
      document.nodes.push_back(take(ps::numeric::evaluate_bezier_node(
          5, ps::WorkflowInputReference{6}, ps::WorkflowInputReference{7},
          ps::WorkflowInputReference{9}, ps::WorkflowNodeOutput{4, "values"},
          2)));
      document.nodes.push_back(take(ps::numeric::apply_lut1d_node(
          6, ps::WorkflowNodeOutput{5, "values"},
          ps::WorkflowNodeOutput{4, "values"},
          ps::WorkflowNodeOutput{4, "axis"}, ps::ElementType::Float64, {},
          ps::numeric::CurveDomain::Clamp)));
      document.nodes.push_back(take(ps::numeric::apply_lut1d_channels_node(
          7, ps::WorkflowNodeOutput{5, "values"},
          ps::WorkflowInputReference{10}, ps::WorkflowInputReference{11},
          ps::ElementType::Float64)));
      ps::ColorArrayDescriptor color;
      color.model = ps::ColorModel::Rgb;
      color.primaries =
          take(ps::color_primary_coordinates(ps::ColorPrimaryPreset::Srgb))
              .primaries;
      color.transfer = ps::ColorTransfer{ps::ColorTransferKind::Linear, {}};
      document.nodes.push_back(take(ps::numeric::apply_lut3d_trilinear_node(
          8, ps::WorkflowInputReference{12}, ps::WorkflowInputReference{13},
          ps::WorkflowInputReference{14}, ps::ElementType::Float64, color,
          color)));
      document.nodes.push_back(take(ps::numeric::apply_lut3d_tetrahedral_node(
          9, ps::WorkflowInputReference{12}, ps::WorkflowInputReference{13},
          ps::WorkflowInputReference{14}, ps::ElementType::Float64, color,
          color)));
      document.nodes.push_back(take(ps::numeric::log2_shaper_node(
          10, ps::WorkflowInputReference{3}, ps::WorkflowInputReference{15},
          ps::WorkflowInputReference{16})));
      document.nodes.push_back(take(ps::numeric::log2_shaper_inverse_node(
          11, ps::WorkflowNodeOutput{10, "values"},
          ps::WorkflowInputReference{15}, ps::WorkflowInputReference{16})));
      auto linear = take(ps::numeric::linear_shaper_inverse(
          document, ps::WorkflowInputReference{3},
          ps::WorkflowInputReference{15}, ps::WorkflowInputReference{16},
          {ps::ElementType::Float64, {1}}));
      document.nodes.push_back(take(ps::numeric::color_ramp_xyz_node(
          100, ps::WorkflowInputReference{3}, ps::WorkflowInputReference{18},
          ps::WorkflowInputReference{17}, ps::ElementType::Float64)));
      document.nodes.push_back(take(ps::numeric::color_ramp_cmyk_node(
          101, ps::WorkflowInputReference{3}, ps::WorkflowInputReference{18},
          ps::WorkflowInputReference{19}, ps::ElementType::Float64,
          ps::numeric::color_ramp_cmyk_description(cmyk_identity))));
      document.nodes.push_back(take(ps::numeric::lowpass_uniform_hann_sinc_node(
          102, ps::WorkflowInputReference{4}, 0, 1, .25)));
      document.nodes.push_back(
          take(ps::numeric::lowpass_nonuniform_gaussian_node(
              103, ps::WorkflowInputReference{20},
              ps::WorkflowInputReference{21}, 0, .5, 1.)));
      document.nodes.push_back(
          {104,
           "numeric.add",
           {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
           {}});
      document.nodes.push_back({105,
                                "numeric.clamp",
                                {ps::WorkflowNodeOutput{104, "value"}},
                                {{"min", 0.}, {"max", 4.}}});
      ps::numeric::Lut3dBakeOptions bake;
      bake.shape = {2, 2, 2};
      bake.interpolation = ps::Lut3dInterpolation::Trilinear;
      bake.atol = bake.rtol = 0;
      bake.input_description = bake.output_description = color;
      bake.source_pointwise = true;
      auto generated = take(ps::numeric::bake_lut3d(
          document, registry, ps::WorkflowInputReference{14},
          [](ps::WorkflowDocument&,
             const ps::numeric::Lut3dSourceInput& input) {
            return ps::Result<ps::WorkflowNodeOutput>(input.colors);
          },
          bake, {}, profiles));
      document.outputs = {
          {"curve", 1, "values"},
          {"position", 2, "value"},
          {"inverse", 3, "values"},
          {"bezier", 4, "values"},
          {"parametric", 5, "values"},
          {"lut", 6, "values"},
          {"channels", 7, "values"},
          {"trilinear", 8, "values"},
          {"tetrahedral", 9, "values"},
          {"baked_table", generated.table.source_node,
           generated.table.source_port},
          {"bake_report", generated.report.source_node,
           generated.report.source_port},
          {"log_shaper", 10, "values"},
          {"log_inverse", 11, "values"},
          {"linear_inverse", linear.source_node, linear.source_port},
          {"xyz_ramp", 100, "values"},
          {"cmyk_ramp", 101, "values"},
          {"uniform", 102, "values"},
          {"nonuniform", 103, "samples"},
          {"finite_sum", 104, "value"},
          {"finite_clamp", 105, "value"}};
      ps::GraphContext graph(document);
      auto compiled = take(ps::Compiler(registry).compile(graph, {}, profiles));
      ps::ExecutionOptions options;
      options.maximum_dependency_work = UINT64_C(1) << 30;
      options.dependencies.maximum_work = UINT64_C(1) << 30;
      options.maximum_result_window_bytes = 72;
      auto result =
          take(execution.execute(compiled.plan, bindings, {}, options));
      baked_table = result.results.at("baked_table");
      bake_report = result.results.at("bake_report");
      log_shaper = result.results.at("log_shaper");
      log_inverse = result.results.at("log_inverse");
      linear_inverse = result.results.at("linear_inverse");
      xyz_ramp = result.results.at("xyz_ramp");
      cmyk_ramp = result.results.at("cmyk_ramp");
      uniform = result.results.at("uniform");
      nonuniform = result.results.at("nonuniform");
      finite_sum = result.results.at("finite_sum");
      finite_clamp = result.results.at("finite_clamp");
      require(take(ps::read_lut3d_bake_report(bake_report, 72)).passed &&
                  baked_table.schema().tensors[0].atomic_trailing_axes == 1,
              "installed authoring executes measured identity bake and "
              "quality-gated table");
      curve = result.results.at("curve");
      identity = result.results.at("position");
      inverse = result.results.at("inverse");
      bezier = result.results.at("bezier");
      parametric = result.results.at("parametric");
      lut = result.results.at("lut");
      channels = result.results.at("channels");
      trilinear = result.results.at("trilinear");
      tetrahedral = result.results.at("tetrahedral");
      require(
          trilinear.schema().tensors[0].facets[0].payload ==
                  take(ps::encode_color_array(color)).payload &&
              tetrahedral.schema().tensors[0].atomic_trailing_axes == 1,
          "installed LUT3D preserves ColorArray v1 and complete-color closure");
      require(identity.schema().id == "consumer.numeric" &&
                  identity.schema().version == 2 &&
                  identity.schema().tensors[0].key == "coordinates",
              "installed identity resolves the original custom schema");
      auto original = take(query.acquire_tensor(take(query.descriptor()), 0,
                                                ps::Region::whole({1})));
      auto forwarded = take(identity.acquire_tensor(take(identity.descriptor()),
                                                    0, ps::Region::whole({1})));
      require(original.storage_owner_token() == forwarded.storage_owner_token(),
              "installed identity retains the authorized source view");
    }
    require(bits(curve) == UINT64_C(0x3ff3492492492492) &&
                bits(identity) == UINT64_C(0x3fe0000000000000) &&
                bits(inverse) == UINT64_C(0x3fe4e2f2c0fa463b) &&
                bits(bezier) == UINT64_C(0x3fe0000000000000) &&
                bits(parametric, {0, 0}) == UINT64_C(0x3fd0000000000000) &&
                bits(parametric, {0, 1}) == UINT64_C(0x3fe0000000000000) &&
                bits(lut, {0, 0}) == UINT64_C(0x3fe0000000000000) &&
                bits(lut, {0, 1}) == UINT64_C(0x3fe0000000000000) &&
                bits(channels, {0, 0}) == UINT64_C(0x3fe0000000000000) &&
                bits(channels, {0, 1}) == UINT64_C(0x4022000000000000) &&
                bits(trilinear, {0, 0}) == UINT64_C(0x3fc8000000000000) &&
                bits(trilinear, {0, 1}) == UINT64_C(0x3fc0000000000000) &&
                bits(trilinear, {0, 2}) == UINT64_C(0x3fd8000000000000) &&
                bits(tetrahedral, {0, 0}) == UINT64_C(0x3fd0000000000000) &&
                bits(tetrahedral, {0, 1}) == UINT64_C(0x3fd0000000000000) &&
                bits(tetrahedral, {0, 2}) == UINT64_C(0x3fe0000000000000),
            "installed numeric Results survive context retirement");
    require(
        bits(baked_table, {1, 0, 1, 0}) == UINT64_C(0x3ff0000000000000) &&
            bits(baked_table, {1, 0, 1, 1}) == 0 &&
            take(ps::read_lut3d_bake_report(bake_report, 72)).passed &&
            take(ps::read_lut3d_bake_report(bake_report, 72))
                    .validation_count == 1,
        "installed baked Result view and report survive context retirement");
    require(bits(log_shaper) == UINT64_C(0xbfd0000000000000) &&
                bits(log_inverse) == UINT64_C(0x3fe0000000000000) &&
                bits(linear_inverse) == UINT64_C(0x4021000000000000),
            "installed log and inverse shapers compose through owning Results");
    require(bits(finite_sum, {2}) == UINT64_C(0x4018000000000000) &&
                bits(finite_clamp, {2}) == UINT64_C(0x4010000000000000) &&
                finite_clamp.schema().tensors[0].facets.empty() &&
                finite_clamp.resources().size() == 0,
            "installed finite arithmetic and static clamp compose through "
            "owning Results");
    require(bits(uniform, {1}) == UINT64_C(0x3ff0000000000000) &&
                bits(nonuniform, {1}) == UINT64_C(0x4004000000000000) &&
                uniform.schema().tensors[0].facets.empty() &&
                nonuniform.schema().tensors[0].sample_shape() ==
                    std::vector<uint64_t>{3},
            "installed lowpass Results preserve exact identities and lifetime");
    require(bits(xyz_ramp, {0, 1}) == UINT64_C(0x3ff0000000000000) &&
                bits(cmyk_ramp, {0, 0}) == UINT64_C(0x3fe0000000000000) &&
                bits(cmyk_ramp, {0, 3}) == UINT64_C(0x3fc0000000000000) &&
                cmyk_ramp.resources().icc_profile(cmyk_identity).ok() &&
                cmyk_ramp.schema().tensors[0].atomic_trailing_axes == 1,
            "installed XYZ/CMYK Result ramps retain color schema and ICC "
            "ownership");
    const auto domain = take(ps::Footprint::all({UINT64_MAX}));
    const auto changed = take(ps::Footprint::from_regions(
        {UINT64_MAX}, {ps::Region({{UINT64_MAX - 2, 1}})}));
    require(take(prefix.preimage(
                domain, {0, 1, 0, 0, ps::ResultSupportTarget::Tensor, 0},
                changed)) ==
                take(ps::Footprint::from_regions(
                    {UINT64_MAX}, {ps::Region({{UINT64_MAX - 2, 2}})})),
            "installed compact prefix inverse survives execution context "
            "retirement");
    std::cout << "Installed Result numeric workflow passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
