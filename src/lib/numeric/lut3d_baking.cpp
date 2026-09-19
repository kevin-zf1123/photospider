#include "photospider/numeric/lut3d_baking.hpp"

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "data/content_digest.hpp"

namespace ps::numeric {
namespace {
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
Status type_error(const char* message) {
  return {ErrorCode::TypeMismatch,
          message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
void edge(content_internal::Sha256* digest, const WorkflowInput& input) {
  digest->integer(input.index());
  if (const auto* ref = std::get_if<WorkflowNodeOutput>(&input)) {
    digest->integer(ref->source_node);
    digest->text(ref->source_port);
  } else {
    digest->integer(std::get<WorkflowInputReference>(input).input_id);
  }
}
std::string prefix(const WorkflowDocument& document, std::size_t count) {
  content_internal::Sha256 digest;
  digest.integer(document.schema_version);
  digest.integer(document.inputs.size());
  for (const auto& input : document.inputs) {
    digest.integer(input.id);
    digest.text(input.name);
    digest.integer(static_cast<unsigned>(input.descriptor.element_type));
    digest.integer(input.descriptor.shape.size());
    for (auto n : input.descriptor.shape)
      digest.integer(n);
    digest.integer(input.region.dimensions().size());
    for (auto dimension : input.region.dimensions()) {
      digest.integer(dimension.offset);
      digest.integer(dimension.extent);
    }
    digest.integer(input.layout.byte_offset);
    digest.integer(input.layout.origin.size());
    for (auto origin : input.layout.origin)
      digest.integer(origin);
    digest.integer(input.layout.byte_strides.size());
    for (auto stride : input.layout.byte_strides)
      digest.integer(stride);
    digest.integer(input.facets.size());
    for (const auto& facet : input.facets) {
      digest.text(facet.key);
      digest.integer(facet.version);
      digest.integer(facet.payload.size());
      digest.bytes(facet.payload.data(), facet.payload.size());
    }
  }
  digest.integer(document.outputs.size());
  for (const auto& output : document.outputs) {
    digest.text(output.name);
    digest.integer(output.node_id);
    digest.text(output.port);
  }
  digest.integer(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& node = document.nodes[i];
    digest.integer(node.id);
    digest.text(node.operation);
    digest.integer(node.inputs.size());
    for (const auto& input : node.inputs)
      edge(&digest, input);
    digest.integer(node.parameters.size());
    for (const auto& parameter : node.parameters) {
      digest.text(parameter.first);
      digest.integer(parameter.second.index());
      std::visit(
          [&](const auto& value) {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Type, std::string>) {
              digest.text(value);
            } else if constexpr (std::is_same_v<Type, double>) {
              std::uint64_t word;
              std::memcpy(&word, &value, 8);
              digest.integer(word);
            } else {
              digest.integer(static_cast<std::uint64_t>(value));
            }
          },
          parameter.second);
    }
  }
  return digest.finish();
}
Result<WorkflowNodeOutput> expand(WorkflowDocument* document,
                                  const Lut3dSourceBuilder& builder,
                                  Lut3dSourceInput input) {
  const auto count = document->nodes.size();
  auto before = prefix(*document, count);
  auto result = builder(*document, input);
  if (!result.ok())
    return result;
  if (document->nodes.size() < count || document->nodes.size() > 65536 ||
      prefix(*document, count) != before)
    return Result<WorkflowNodeOutput>(
        invalid("LUT3D source builder must only append nodes"));
  return result;
}
Result<ElementType> source_type(const SemanticGraphIR& graph,
                                const WorkflowNodeOutput& output,
                                const ValueDescriptor& descriptor,
                                const ColorArrayDescriptor& color) {
  for (const auto& node : graph.nodes()) {
    if (node.id != output.source_node)
      continue;
    for (const auto& value : node.outputs)
      if (value.key == output.source_port) {
        if (value.result_schema || value.descriptor.shape != descriptor.shape ||
            (value.descriptor.element_type != ElementType::Float32 &&
             value.descriptor.element_type != ElementType::Float64))
          break;
        auto expected = encode_color_array(color);
        if (!expected.ok())
          return Result<ElementType>(expected.status());
        for (const auto& facet : value.facets)
          if (facet.key == "photospider.color-array" &&
              (facet.version != expected.value().version ||
               facet.payload != expected.value().payload))
            return Result<ElementType>(
                type_error("source color description mismatch"));
        return Result<ElementType>(value.descriptor.element_type);
      }
  }
  return Result<ElementType>(
      type_error("source requires same-shape Float32/64 color output"));
}
}  // namespace
Result<BakedLut3d> bake_lut3d(WorkflowDocument& document,
                              std::shared_ptr<OperationRegistry> registry,
                              WorkflowInput axis,
                              const Lut3dSourceBuilder& source,
                              const Lut3dBakeOptions& options,
                              std::optional<Lut3dValidationPoints> extra,
                              ResourceBindings resources) {
  using Answer = Result<BakedLut3d>;
  if (!registry || !registry->frozen() || !source ||
      !options.source_pointwise ||
      (extra && (!extra->count || extra->count > 1048576)))
    return Answer(
        invalid("LUT3D baking requires frozen registry, pointwise source and "
                "valid extras"));
  const char* suffix = options.profile == CpuNumericProfile::Strict ? "_strict"
                       : options.profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : options.profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!suffix)
    return Answer(invalid("invalid bake CPU profile"));
  Lut3dBakeDescription spec;
  spec.shape = options.shape;
  spec.interpolation = options.interpolation;
  spec.atol = options.atol;
  spec.rtol = options.rtol;
  spec.table_dtype = options.table_dtype.value_or(ElementType::Float64);
  spec.extra_points = extra ? extra->count : 0;
  spec.input_description = options.input_description;
  spec.output_description = options.output_description;
  spec.recipe_identity.assign(64, '0');
  auto checked = lut3d_bake_schema(spec);
  if (!checked.ok())
    return Answer(checked.status());
  auto input_color = color_array_parameter(spec.input_description),
       output_color = color_array_parameter(spec.output_description);
  std::map<std::string, ParameterValue> geometry{
      {"n0", static_cast<std::int64_t>(spec.shape[0])},
      {"n1", static_cast<std::int64_t>(spec.shape[1])},
      {"n2", static_cast<std::int64_t>(spec.shape[2])},
      {"input_color_description", input_color.value()}};
  auto staged = document;
  std::vector<WorkflowInput> references{axis};
  if (extra)
    references.push_back(extra->values);
  auto ids = available_workflow_node_ids(staged, 3, references);
  if (!ids.ok())
    return Answer(ids.status());
  const WorkflowNodeOutput checked_axis{ids.value()[0], "values"},
      grid{ids.value()[1], "values"}, points{ids.value()[2], "values"};
  staged.nodes.push_back({checked_axis.source_node,
                          std::string("curve.bake_lut3d_axis") + suffix,
                          {axis},
                          geometry});
  staged.nodes.push_back({grid.source_node,
                          std::string("curve.bake_lut3d_grid") + suffix,
                          {checked_axis},
                          geometry});
  auto queries = geometry;
  queries.emplace("extra_count", static_cast<std::int64_t>(spec.extra_points));
  std::vector<WorkflowInput> query_inputs{checked_axis};
  if (extra)
    query_inputs.push_back(extra->values);
  staged.nodes.push_back({points.source_node,
                          std::string(extra ? "curve.bake_lut3d_points_extra"
                                            : "curve.bake_lut3d_points") +
                              suffix,
                          std::move(query_inputs), std::move(queries)});
  std::uint64_t count = 1;
  for (auto n : spec.shape)
    count *= n - 1;
  count += spec.extra_points;
  const ValueDescriptor grid_descriptor{
      ElementType::Float64,
      {spec.shape[0], spec.shape[1], spec.shape[2], 3}},
      point_descriptor{ElementType::Float64, {count, 3}};
  auto grid_source = expand(&staged, source, {grid, grid_descriptor});
  if (!grid_source.ok())
    return Answer(grid_source.status());
  auto point_source = expand(&staged, source, {points, point_descriptor});
  if (!point_source.ok())
    return Answer(point_source.status());
  auto exports = std::move(staged.outputs);
  staged.outputs = {{"grid_source", grid_source.value().source_node,
                     grid_source.value().source_port},
                    {"point_source", point_source.value().source_node,
                     point_source.value().source_port}};
  GraphContext graph(staged);
  auto analyzed =
      Compiler(registry).analyze(graph.snapshot(), std::move(resources));
  staged.outputs = std::move(exports);
  if (!analyzed.ok())
    return Answer(analyzed.status());
  auto grid_type = source_type(analyzed.value(), grid_source.value(),
                               grid_descriptor, spec.output_description),
       point_type = source_type(analyzed.value(), point_source.value(),
                                point_descriptor, spec.output_description);
  if (!grid_type.ok() || !point_type.ok())
    return Answer(!grid_type.ok() ? grid_type.status() : point_type.status());
  if (grid_type.value() != point_type.value())
    return Answer(type_error("source dtype depends on sample batch shape"));
  spec.source_dtype = grid_type.value();
  spec.table_dtype = options.table_dtype.value_or(spec.source_dtype);
  content_internal::Sha256 recipe;
  recipe.text("photospider.lut3d.source/1");
  recipe.text(analyzed.value().digest().value);
  recipe.text(prefix(staged, staged.nodes.size()));
  edge(&recipe, grid_source.value());
  edge(&recipe, point_source.value());
  spec.recipe_identity = recipe.finish();
  const auto type_name = [](ElementType type) {
    return std::string(type == ElementType::Float32 ? "float32" : "float64");
  };
  auto tail = available_workflow_node_ids(
      staged, 7,
      {grid_source.value(), point_source.value(), checked_axis, points});
  if (!tail.ok())
    return Answer(tail.status());
  const WorkflowNodeOutput table{tail.value()[0], "values"},
      reference{tail.value()[1], "values"}, applied{tail.value()[2], "values"},
      report{tail.value()[3], "report"}, gated{tail.value()[4], "values"},
      owned{tail.value()[5], "table"}, unpacked{tail.value()[6], "values"};
  for (bool is_table : {true, false})
    staged.nodes.push_back(
        {is_table ? table.source_node : reference.source_node,
         std::string("curve.bake_lut3d_color") + suffix,
         {is_table ? grid_source.value() : point_source.value()},
         {{"color_description", output_color.value()},
          {"dtype",
           type_name(is_table ? spec.table_dtype : spec.source_dtype)}}});
  Lut3dOptions apply;
  apply.dtype = spec.table_dtype;
  apply.profile = options.profile;
  auto applied_node =
      spec.interpolation == Lut3dInterpolation::Trilinear
          ? apply_lut3d_trilinear_node(applied.source_node, points, unpacked,
                                       checked_axis, ElementType::Float64,
                                       spec.input_description,
                                       spec.output_description, apply)
          : apply_lut3d_tetrahedral_node(applied.source_node, points, unpacked,
                                         checked_axis, ElementType::Float64,
                                         spec.input_description,
                                         spec.output_description, apply);
  if (!applied_node.ok())
    return Answer(applied_node.status());
  staged.nodes.push_back(applied_node.take_value());
  auto parameters = geometry;
  parameters.emplace("output_color_description", output_color.value());
  parameters.emplace(
      "interpolation",
      std::string(spec.interpolation == Lut3dInterpolation::Trilinear
                      ? "trilinear"
                      : "tetrahedral"));
  parameters.emplace("atol", spec.atol);
  parameters.emplace("rtol", spec.rtol);
  parameters.emplace("dtype", type_name(spec.table_dtype));
  parameters.emplace("source_dtype", type_name(spec.source_dtype));
  parameters.emplace("extra_count",
                     static_cast<std::int64_t>(spec.extra_points));
  parameters.emplace("source_recipe", spec.recipe_identity);
  staged.nodes.push_back(
      {owned.source_node, "curve.pack_lut3d", {table}, parameters});
  staged.nodes.push_back(
      {unpacked.source_node, "curve.unpack_lut3d", {owned}, {}});
  staged.nodes.push_back(
      {report.source_node,
       "curve.measure_lut3d",
       {checked_axis, grid, owned, points, reference, applied},
       std::move(parameters)});
  staged.nodes.push_back(
      {gated.source_node, "curve.gate_lut3d", {owned, report}, {}});
  Answer answer(BakedLut3d{gated, checked_axis, report});
  static_assert(std::is_nothrow_move_assignable_v<WorkflowDocument>);
  document = std::move(staged);
  return answer;
}
}  // namespace ps::numeric
