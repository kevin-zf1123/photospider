#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/result.hpp"
#include "photospider/ops/numeric/workflow_authoring.hpp"

namespace ps::generation {
/** @brief Fixed positive backing capacities; effective counts remain payload.
 * Coordinates are pixel x/y, right/down. These factories validate structure
 * only, allocate no sample backing, and never inspect geometric payloads.
 * Logical zero capacity is represented by one physical row.
 * Factories return owning metadata and may throw std::bad_alloc. They read no
 * sample payload and can be called concurrently with independent arguments.
 */
struct CorePathCapacity final {
  std::uint64_t verbs = 1, controls = 1, subpaths = 1;
};
struct PrimitivePathCapacity final {
  std::uint64_t references = 1, subpaths = 1, beziers = 1, arcs = 1,
                hermites = 1, splines = 1, controls = 1, knots = 1;
};
namespace detail {
inline ResultTensorSpec path_member(const char* key, ElementType type,
                                    std::vector<std::uint64_t> shape,
                                    unsigned atomic = 0, int component = -1) {
  ResultTensorSpec field;
  field.key = key;
  field.descriptor = {type, std::move(shape)};
  field.atomic_trailing_axes = atomic;
  field.layout.channel_axis = component < 0
                                  ? std::optional<std::uint32_t>{}
                                  : std::optional<std::uint32_t>(component);
  return field;
}
inline Result<SchemaTemplate> path_schema(
    const char* id, std::vector<ResultTensorSpec> fields) {
  SchemaTemplate schema;
  schema.id = id;
  ResultPublicationScope scope;
  scope.policy = ResultPublicationPolicy::CompleteBundle;
  for (unsigned i = 0; i < fields.size(); ++i) {
    std::uint64_t count = 1;
    for (auto n : fields[i].sample_shape()) {
      if (!n || n > (UINT64_C(1) << 40) / count)
        return Result<SchemaTemplate>(
            Status{ErrorCode::InvalidArgument, "path schema capacity limit"});
      count *= n;
    }
    schema.tensors.push_back(std::move(fields[i]));
    scope.members.push_back(i);
  }
  schema.publication_scopes.push_back(std::move(scope));
  auto status = schema.validate();
  if (!status.ok())
    return Result<SchemaTemplate>(status);
  return Result<SchemaTemplate>(std::move(schema));
}
}  // namespace detail
/** @brief CoreVerbs v1: verbs, control_offsets, controls, subpath_offsets,
 * closed, counts. All six members publish as one CompleteBundle. */
inline Result<SchemaTemplate> core_path_schema(CorePathCapacity capacity) {
  using detail::path_member;
  const auto v = std::max(UINT64_C(1), capacity.verbs);
  const auto c = std::max(UINT64_C(1), capacity.controls);
  const auto s = std::max(UINT64_C(1), capacity.subpaths);
  if (v >= (UINT64_C(1) << 40) || s >= (UINT64_C(1) << 40))
    return Result<SchemaTemplate>(
        Status{ErrorCode::InvalidArgument, "path offset capacity overflow"});
  return detail::path_schema(
      "photospider.ops.path_core",
      {path_member("verbs", ElementType::UInt8, {v}),
       path_member("control_offsets", ElementType::Int64, {v + 1}),
       path_member("controls", ElementType::Float64, {c, 2}, 1, 1),
       path_member("subpath_offsets", ElementType::Int64, {s + 1}),
       path_member("closed", ElementType::UInt8, {s}),
       path_member("counts", ElementType::Int64, {3}, 1)});
}
/** @brief Primitives v1, in the normative 13-member directory order.
 * The eight counts are R,S,B,A,H,T,U,K. Tags, effective prefix counts,
 * connections, finite geometry and canonical padding are caller premises.
 */
inline Result<SchemaTemplate> primitive_path_schema(
    PrimitivePathCapacity capacity) {
  using detail::path_member;
  auto r = std::max(UINT64_C(1), capacity.references),
       s = std::max(UINT64_C(1), capacity.subpaths),
       b = std::max(UINT64_C(1), capacity.beziers),
       a = std::max(UINT64_C(1), capacity.arcs),
       h = std::max(UINT64_C(1), capacity.hermites),
       t = std::max(UINT64_C(1), capacity.splines),
       u = std::max(UINT64_C(1), capacity.controls),
       k = std::max(UINT64_C(1), capacity.knots);
  if (s >= (UINT64_C(1) << 40))
    return Result<SchemaTemplate>(
        Status{ErrorCode::InvalidArgument, "path offset capacity overflow"});
  return detail::path_schema(
      "photospider.ops.path_primitives",
      {path_member("references", ElementType::Int64, {r, 2}, 1),
       path_member("subpath_offsets", ElementType::Int64, {s + 1}),
       path_member("closed", ElementType::UInt8, {s}),
       path_member("isolated_positions", ElementType::Float64, {s, 2}, 1, 1),
       path_member("bezier_degrees", ElementType::Int64, {b}),
       path_member("bezier_controls", ElementType::Float64, {b, 4, 2}, 2, 2),
       path_member("arcs", ElementType::Float64, {a, 8}, 1),
       path_member("hermite_controls", ElementType::Float64, {h, 4, 2}, 2, 2),
       path_member("splines", ElementType::Int64, {t, 5}, 1),
       path_member("spline_controls", ElementType::Float64, {u, 2}, 1, 1),
       path_member("spline_knots", ElementType::Float64, {k}),
       path_member("spline_weights", ElementType::Float64, {u}),
       path_member("counts", ElementType::Int64, {8}, 1)});
}
/** @brief Partition of paired flat output subpaths, with the source identity
 * encoded as eight little-endian bytes. No target identity is synthesized. */
inline Result<SchemaTemplate> path_partition_schema(std::uint64_t groups) {
  using detail::path_member;
  if (!groups || groups >= (UINT64_C(1) << 40))
    return Result<SchemaTemplate>(
        Status{ErrorCode::InvalidArgument, "path group capacity"});
  return detail::path_schema(
      "photospider.ops.path_partition",
      {path_member("group_offsets", ElementType::Int64, {groups + 1}),
       path_member("subpath_count", ElementType::Int64, {1}),
       path_member("source_object_id", ElementType::UInt8, {8}, 1)});
}

/** @brief Explicit numerical profiles; availability is checked at compilation.
 */
enum class Profile { Strict, AppleSilicon, X86_64 };
using Parameters = std::map<std::string, ParameterValue>;
namespace detail {
inline std::string profile_suffix(Profile profile) {
  return profile == Profile::AppleSilicon ? "accelerated_apple_silicon"
         : profile == Profile::X86_64     ? "accelerated_x86_64"
                                          : "strict";
}
inline Result<WorkflowNodeOutput> append(WorkflowDocument* document,
                                         std::string key,
                                         std::vector<WorkflowInput> inputs,
                                         Parameters parameters,
                                         const char* port) {
  if (!document)
    return Result<WorkflowNodeOutput>(
        Status{ErrorCode::InvalidArgument, "null generation workflow"});
  auto ids = numeric::available_workflow_node_ids(*document, 1, inputs);
  if (!ids.ok())
    return Result<WorkflowNodeOutput>(ids.status());
  auto staged = *document;
  const auto id = ids.value()[0];
  staged.nodes.push_back(
      {id, std::move(key), std::move(inputs), std::move(parameters)});
  *document = std::move(staged);
  return Result<WorkflowNodeOutput>(WorkflowNodeOutput{id, port});
}
}  // namespace detail
/** @brief GEN-03C expands to exact PTH-09A polygon filling.
 * Parameters are the explicit height, width, origin, dtype, and fill_rule.
 * Open contours close only for this fill consumption. The compiler checks
 * the CoreVerbs directory; valid M/L/Z payloads remain the caller's premise.
 * Requires exclusive access to document; inputs and parameters are copied into
 * the appended node. Returns an owning node reference or a static authoring
 * error. No sample backing, execution state or cache is retained.
 */
inline Result<WorkflowNodeOutput> polygon_coverage(
    WorkflowDocument* document, WorkflowInput path, Parameters parameters,
    Profile profile = Profile::Strict) {
  return detail::append(
      document, "path.fill_polygon_area_v1_" + detail::profile_suffix(profile),
      {std::move(path)}, std::move(parameters), "coverage");
}
/** @brief NOI-03B retains the published Gaussian rounding before correlation.
 * Gaussian and correlation parameters are separate explicit node arguments.
 * In particular, select the white source dtype independently from the final
 * dtype, and specify boundary/normalization on the correlation node.
 * The document is unchanged if node-ID allocation fails. The caller grants
 * exclusive document access; returned IDs and port names own no sample data.
 * Container allocation can throw std::bad_alloc.
 */
inline Result<WorkflowNodeOutput> correlated_gaussian(
    WorkflowDocument* document, WorkflowInput mean, WorkflowInput sigma,
    WorkflowInput kernel, Parameters gaussian_parameters,
    Parameters correlation_parameters, Profile profile = Profile::Strict) {
  if (!document)
    return Result<WorkflowNodeOutput>(
        Status{ErrorCode::InvalidArgument, "null generation workflow"});
  auto ids =
      numeric::available_workflow_node_ids(*document, 2, {mean, sigma, kernel});
  if (!ids.ok())
    return Result<WorkflowNodeOutput>(ids.status());
  auto staged = *document;
  const auto suffix = detail::profile_suffix(profile);
  staged.nodes.push_back({ids.value()[0],
                          "noise.gaussian_box_muller_v1_" + suffix,
                          {std::move(mean), std::move(sigma)},
                          std::move(gaussian_parameters)});
  staged.nodes.push_back(
      {ids.value()[1],
       "noise.correlate_kernel_v1_" + suffix,
       {WorkflowNodeOutput{ids.value()[0], "values"}, std::move(kernel)},
       std::move(correlation_parameters)});
  *document = std::move(staged);
  return Result<WorkflowNodeOutput>(
      WorkflowNodeOutput{ids.value()[1], "values"});
}
/** @brief GEN-06C appends explicit spread followed by an existing CRV-06 node.
 * Build color_ramp with the public numeric::color_ramp_*_node constructors.
 * Its first input is the unspread query; its temporary id is replaced by an
 * unused document id. All 15 CRV model/unit variants, including rational-pi
 * numerator/denominator ports, retain their parameters, resources and profile.
 * Spread preserves the query dtype and is a separate public rounding stage.
 * A tuple description uses channel_axis=0; downstream image consumers supply
 * their own description for the resulting S+[C] tensor.
 *
 * The caller grants exclusive document access. Inputs and parameters are
 * copied/moved into two nodes; no samples, execution state or cache are
 * retained. Invalid helper arguments or ID allocation leave document unchanged.
 * Allocation may throw std::bad_alloc. Compiler checks the selected CRV
 * schema/description/resources. Numerical validity remains a caller premise.
 */
inline Result<WorkflowNodeOutput> gradient_color(
    WorkflowDocument* document, WorkflowNode color_ramp,
    std::string method = "pad", Profile spread_profile = Profile::Strict) {
  using Answer = Result<WorkflowNodeOutput>;
  if (!document ||
      (method != "pad" && method != "repeat" && method != "reflect") ||
      (spread_profile != Profile::Strict &&
       spread_profile != Profile::AppleSilicon &&
       spread_profile != Profile::X86_64))
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid gradient color arguments"});
  bool known = false;
  unsigned inputs = 0;
  for (const auto* model :
       {"rgb", "xyz", "cmyk", "cielab", "oklab", "ycbcr", "cielch", "cielch_pi",
        "cielch_rational_pi", "oklch", "oklch_pi", "oklch_rational_pi", "hsl",
        "hsl_pi", "hsl_rational_pi"}) {
    for (auto profile :
         {Profile::Strict, Profile::AppleSilicon, Profile::X86_64}) {
      if (color_ramp.operation == std::string("curve.color_ramp_") + model +
                                      "_" + detail::profile_suffix(profile)) {
        known = true;
        inputs =
            std::string(model).find("rational_pi") != std::string::npos ? 5 : 3;
      }
    }
  }
  if (!known || color_ramp.inputs.size() != inputs)
    return Answer(Status{ErrorCode::InvalidArgument,
                         "gradient color requires a CRV-06 node"});
  auto ids =
      numeric::available_workflow_node_ids(*document, 2, color_ramp.inputs);
  if (!ids.ok())
    return Answer(ids.status());
  auto staged = *document;
  staged.nodes.push_back({ids.value()[0],
                          "generation.spread_coordinate_v1_" +
                              detail::profile_suffix(spread_profile),
                          {color_ramp.inputs[0]},
                          {{"method", std::move(method)}}});
  color_ramp.id = ids.value()[1];
  color_ramp.inputs[0] = WorkflowNodeOutput{ids.value()[0], "values"};
  staged.nodes.push_back(std::move(color_ramp));
  *document = std::move(staged);
  return Answer(WorkflowNodeOutput{ids.value()[1], "values"});
}

struct PathEvaluation final {
  WorkflowNodeOutput position, derivative, tangent, tangent_valid;
};
/** @brief PTH-02E statically dispatches the normative PathSet authority.
 * Core segment IDs are evaluable verb rows; primitive IDs are reference rows.
 * No global-parameter segment selection or intermediate endpoint rounding is
 * introduced. The compiler also checks the actual producer's complete schema.
 * Requires exclusive document access. The schema is borrowed only for this
 * synchronous call; returned references own their IDs and port names. Static
 * schema/authoring errors leave the document unchanged.
 */
inline Result<PathEvaluation> evaluate_pathset(
    WorkflowDocument* document, const SchemaTemplate& schema,
    WorkflowInput path, WorkflowInput indices, WorkflowInput t,
    Parameters parameters = {}, Profile profile = Profile::Strict) {
  using Answer = Result<PathEvaluation>;
  auto status = schema.validate();
  if (!status.ok())
    return Answer(status);
  const bool core = schema.id == "photospider.ops.path_core";
  const bool primitive = schema.id == "photospider.ops.path_primitives";
  if ((!core && !primitive) || schema.version != 1 ||
      schema.tensors.size() != (core ? 6U : 13U))
    return Answer(
        Status{ErrorCode::TypeMismatch, "unsupported PathSet authority"});
  for (const auto& field : schema.tensors)
    if (field.sample_shape().empty())
      return Answer(Status{ErrorCode::TypeMismatch,
                           "PathSet member rank must be positive"});
  Result<SchemaTemplate> expected =
      core ? core_path_schema({schema.tensors[0].sample_shape()[0],
                               schema.tensors[2].sample_shape()[0],
                               schema.tensors[4].sample_shape()[0]})
           : primitive_path_schema({schema.tensors[0].sample_shape()[0],
                                    schema.tensors[2].sample_shape()[0],
                                    schema.tensors[4].sample_shape()[0],
                                    schema.tensors[6].sample_shape()[0],
                                    schema.tensors[7].sample_shape()[0],
                                    schema.tensors[8].sample_shape()[0],
                                    schema.tensors[9].sample_shape()[0],
                                    schema.tensors[10].sample_shape()[0]});
  if (!expected.ok())
    return Answer(expected.status());
  if (!schema.same_schema(expected.value()))
    return Answer(
        Status{ErrorCode::TypeMismatch, "noncanonical PathSet directory"});
  auto node = detail::append(
      document,
      std::string("path.evaluate_") + (core ? "core" : "primitive") +
          "_pathset_v1_" + detail::profile_suffix(profile),
      {std::move(path), std::move(indices), std::move(t)},
      std::move(parameters), "position");
  if (!node.ok())
    return Answer(node.status());
  const auto id = node.value().source_node;
  return Answer(PathEvaluation{{id, "position"},
                               {id, "derivative"},
                               {id, "tangent"},
                               {id, "tangent_valid"}});
}

}  // namespace ps::generation
