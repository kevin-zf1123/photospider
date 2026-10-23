#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/result.hpp"
#include "photospider/ops/numeric/arrays.hpp"
#include "photospider/ops/numeric/binary.hpp"
#include "photospider/ops/numeric/curves.hpp"
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
/** @brief DynamicPoints v1: Float64 positions[P,2], Int64 ids[P], count[1].
 * P is at least one even for a zero logical target. All members publish as one
 * CompleteBundle. Valid prefixes, ordinal IDs and zero padding are payload
 * premises. The factory owns metadata only and does not inspect samples.
 *
 * @param capacity Requested row capacity in [0, 2^39]; zero uses P=1.
 * @return Owned schema metadata on success. Returns InvalidArgument when
 * capacity exceeds 2^39, since positions[P,2] is limited to 2^40 elements;
 * otherwise propagates schema validation failures.
 */
inline Result<SchemaTemplate> dynamic_points_schema(std::uint64_t capacity) {
  using detail::path_member;
  const auto p = std::max(UINT64_C(1), capacity);
  return detail::path_schema(
      "photospider.ops.dynamic_points",
      {path_member("positions", ElementType::Float64, {p, 2}, 1, 1),
       path_member("ids", ElementType::Int64, {p}),
       path_member("count", ElementType::Int64, {1})});
}
/** @brief Termination reason and Run-local identity of the complete points.
 * stop_reason is 0=count_reached, 1=candidates_exhausted, 2=active_exhausted.
 * points_object_id stores eight little-endian bytes. Pairing is unchecked.
 */
inline Result<SchemaTemplate> point_generation_report_schema() {
  using detail::path_member;
  return detail::path_schema(
      "photospider.ops.point_generation_report",
      {path_member("stop_reason", ElementType::Int64, {1}),
       path_member("points_object_id", ElementType::UInt8, {8})});
}
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

/** @brief Fixed backing for certified length tables. Effective prefixes and
 * source identity are runtime payload. Factories own metadata only. */
inline Result<SchemaTemplate> path_arclength_schema(std::uint64_t segments,
                                                    std::uint64_t knots,
                                                    std::uint64_t subpaths) {
  using detail::path_member;
  segments = std::max(UINT64_C(1), segments);
  knots = std::max(UINT64_C(1), knots);
  subpaths = std::max(UINT64_C(1), subpaths);
  if (segments >= (UINT64_C(1) << 40) || subpaths >= (UINT64_C(1) << 40))
    return Result<SchemaTemplate>(Status{
        ErrorCode::InvalidArgument, "length table offset capacity overflow"});
  return detail::path_schema(
      "photospider.ops.path_arclength_table",
      {path_member("segment_ids", ElementType::Int64, {segments}),
       path_member("segment_offsets", ElementType::Int64, {segments + 1}),
       path_member("knots", ElementType::Float64, {knots, 3}, 1),
       path_member("subpath_offsets", ElementType::Int64, {subpaths + 1}),
       path_member("totals", ElementType::Float64, {subpaths + 1, 3}, 1),
       path_member("counts", ElementType::Int64, {3}, 1),
       path_member("source_object_id", ElementType::UInt8, {8}, 1)});
}
/** @brief Parameter or true-arc samples. Source geometry remains an explicit
 * workflow input; the identity bytes do not reconstruct an owning reference. */
inline Result<SchemaTemplate> path_samples_schema(std::uint64_t points,
                                                  std::uint64_t subpaths,
                                                  bool arc = false) {
  using detail::path_member;
  points = std::max(UINT64_C(1), points);
  subpaths = std::max(UINT64_C(1), subpaths);
  if (subpaths >= (UINT64_C(1) << 40))
    return Result<SchemaTemplate>(
        Status{ErrorCode::InvalidArgument, "sample offset capacity overflow"});
  std::vector<ResultTensorSpec> fields{
      path_member("positions", ElementType::Float64, {points, 2}, 1, 1),
      path_member("source_segment", ElementType::Int64, {points}),
      path_member("local_t", ElementType::Float64, {points}),
      path_member("subpath_offsets", ElementType::Int64, {subpaths + 1}),
      path_member("counts", ElementType::Int64, {2}, 1),
      path_member("source_object_id", ElementType::UInt8, {8}, 1)};
  if (arc)
    fields.push_back(path_member("arc_s", ElementType::Float64, {points}));
  return detail::path_schema(arc ? "photospider.ops.path_samples_arc"
                                 : "photospider.ops.path_samples_param",
                             std::move(fields));
}
/** @brief Canonical rational polygons; limbs are base-2^32, little endian.
 * No numerical or topological payload is inspected by this schema factory. */
inline Result<SchemaTemplate> exact_polygon_schema(std::uint64_t vertices,
                                                   std::uint64_t contours,
                                                   std::uint64_t limbs) {
  using detail::path_member;
  vertices = std::max(UINT64_C(1), vertices);
  contours = std::max(UINT64_C(1), contours);
  if (contours >= (UINT64_C(1) << 40))
    return Result<SchemaTemplate>(
        Status{ErrorCode::InvalidArgument, "polygon offset capacity overflow"});
  return detail::path_schema(
      "photospider.ops.exact_polygon",
      {path_member("coordinate_signs", ElementType::UInt8, {vertices, 2}, 1),
       path_member("coordinate_sizes", ElementType::Int64, {vertices, 2, 2}, 2),
       path_member("coordinate_limbs", ElementType::UInt8,
                   {vertices, 2, 2, limbs, 4}, 4),
       path_member("contour_offsets", ElementType::Int64, {contours + 1}),
       path_member("counts", ElementType::Int64, {2}, 1)});
}
/** @brief Integer grid coordinates. Grid origin and spacing remain explicit
 * parameters of every producer and consumer. */
inline Result<SchemaTemplate> grid_polygon_schema(std::uint64_t vertices,
                                                  std::uint64_t contours) {
  using detail::path_member;
  vertices = std::max(UINT64_C(1), vertices);
  contours = std::max(UINT64_C(1), contours);
  if (contours >= (UINT64_C(1) << 40))
    return Result<SchemaTemplate>(
        Status{ErrorCode::InvalidArgument, "polygon offset capacity overflow"});
  return detail::path_schema(
      "photospider.ops.grid_polygon",
      {path_member("coordinates", ElementType::Int64, {vertices, 2}, 1, 1),
       path_member("contour_offsets", ElementType::Int64, {contours + 1}),
       path_member("counts", ElementType::Int64, {2}, 1)});
}
/** @brief Construction-time geometry error and provenance, with no implicit
 * validity gate or hidden ownership binding. */
inline Result<SchemaTemplate> geometry_report_schema(std::uint64_t sources) {
  using detail::path_member;
  sources = std::max(UINT64_C(1), sources);
  return detail::path_schema(
      "photospider.ops.geometry_report",
      {path_member("error_bounds_px", ElementType::Float64, {3}, 1),
       path_member("topology_status", ElementType::Int64, {1}),
       path_member("dropped_width", ElementType::UInt8, {1}),
       path_member("source_object_ids", ElementType::UInt8, {sources, 8}, 1)});
}
inline Result<SchemaTemplate> path_fit_report_schema() {
  using detail::path_member;
  return detail::path_schema(
      "photospider.ops.path_fit_report",
      {path_member("error_bound_px", ElementType::Float64, {1}),
       path_member("topology_status", ElementType::Int64, {1}),
       path_member("source_object_id", ElementType::UInt8, {8}, 1)});
}
/** @brief Linear width bindings. Counts are bindings, profiles, stops, sources.
 * The caller pairs the Result with geometry and retains explicit source inputs.
 */
inline Result<SchemaTemplate> path_width_schema(std::uint64_t profiles,
                                                std::uint64_t bindings,
                                                std::uint64_t stops,
                                                std::uint64_t sources) {
  using detail::path_member;
  profiles = std::max(UINT64_C(1), profiles);
  bindings = std::max(UINT64_C(1), bindings);
  stops = std::max(UINT64_C(1), stops);
  sources = std::max(UINT64_C(1), sources);
  return detail::path_schema(
      "photospider.ops.path_width",
      {path_member("profiles", ElementType::Int64, {profiles, 4}, 1),
       path_member("bindings", ElementType::Int64, {bindings, 3}, 1),
       path_member("stops", ElementType::Float64, {stops}),
       path_member("widths", ElementType::Float64, {stops}),
       path_member("source_object_ids", ElementType::UInt8, {sources, 8}, 1),
       path_member("counts", ElementType::Int64, {4}, 1)});
}
/** @brief Exact affine parameter maps a+b*t for complete target segments.
 * Source IDs are copied provenance; they are not lookup handles. */
inline Result<SchemaTemplate> path_source_map_schema(std::uint64_t mappings,
                                                     std::uint64_t sources,
                                                     std::uint64_t limbs) {
  using detail::path_member;
  mappings = std::max(UINT64_C(1), mappings);
  sources = std::max(UINT64_C(1), sources);
  return detail::path_schema(
      "photospider.ops.path_source_map",
      {path_member("records", ElementType::Int64, {mappings, 4}, 1),
       path_member("source_object_ids", ElementType::UInt8, {sources, 8}, 1),
       path_member("affine_signs", ElementType::UInt8, {mappings, 2}, 1),
       path_member("affine_sizes", ElementType::Int64, {mappings, 2, 2}, 2),
       path_member("affine_limbs", ElementType::UInt8,
                   {mappings, 2, 2, limbs, 4}, 4),
       path_member("counts", ElementType::Int64, {2}, 1)});
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
struct GeneratedPoints final {
  WorkflowNodeOutput points, report;
};
namespace detail {
inline Result<GeneratedPoints> point_workflow(WorkflowDocument* document,
                                              WorkflowInput bounds,
                                              WorkflowInput distance,
                                              Parameters parameters,
                                              const char* producer) {
  using Answer = Result<GeneratedPoints>;
  if (!document)
    return Answer(
        Status{ErrorCode::InvalidArgument, "null generation workflow"});
  auto ids =
      numeric::available_workflow_node_ids(*document, 2, {bounds, distance});
  if (!ids.ok())
    return Answer(ids.status());
  auto staged = *document;
  const WorkflowNodeOutput points{ids.value()[0], "points"};
  staged.nodes.push_back({ids.value()[0],
                          producer,
                          {std::move(bounds), std::move(distance)},
                          std::move(parameters)});
  staged.nodes.push_back(
      {ids.value()[1],
       "generation.bind_point_generation_report_v1",
       {points, WorkflowNodeOutput{ids.value()[0], "stop_reason"}},
       {}});
  *document = std::move(staged);
  return Answer(GeneratedPoints{points, {ids.value()[1], "report"}});
}
}  // namespace detail
/** @brief GEN-08C finite-candidate rejection and report binding.
 * bounds is Float32/64 [4], min_distance is Float32/64 [1]. Parameters include
 * required candidate_count, seed, stream and optional max_count (1048576).
 * Returns the same complete Produced points object consumed by the binder.
 * Export that complete value alongside report; copying or partially projecting
 * points after binding creates a different object identity.
 *
 * Requires exclusive document access. Metadata and node references are owned;
 * no payload, runtime state or cache is retained. Authoring failure leaves the
 * document unchanged. Allocation may throw std::bad_alloc. Payload domains,
 * counts and associations remain unchecked; static compiler checks still apply.
 */
inline Result<GeneratedPoints> poisson_rejection_points(
    WorkflowDocument* document, WorkflowInput bounds,
    WorkflowInput min_distance, Parameters parameters) {
  return detail::point_workflow(
      document, std::move(bounds), std::move(min_distance),
      std::move(parameters),
      "generation.poisson_rejection_candidates_v1_strict");
}
/** @brief GEN-08D FIFO Bridson with the same ownership contract as GEN-08C.
 * Required seed/stream; optional k=30, max_count=1048576,
 * max_iterations=2097153, max_refinements=4096 and max_work=INT64_MAX.
 * The iteration budget is independent of normal target/active-list completion.
 */
inline Result<GeneratedPoints> bridson_points(WorkflowDocument* document,
                                              WorkflowInput bounds,
                                              WorkflowInput min_distance,
                                              Parameters parameters) {
  return detail::point_workflow(document, std::move(bounds),
                                std::move(min_distance), std::move(parameters),
                                "generation.bridson_candidates_v1_strict");
}
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

/** @brief PTH-05B: source query operands, NUM division, then CRV PCHIP.
 * s and source_length are Float64 [N] and Float64 [1] or [N]. They already
 * refer to the explicit source domain, after any caller-owned source mapping.
 * Zero length constructs operands 0/1. The NUM node publishes RN64 u before
 * CRV-01B consumes it. Controls may use either floating dtype.
 * Owns all authored metadata. Requires exclusive document access; failure
 * leaves it unchanged. Numerical domains and source associations are unchecked.
 */
inline Result<WorkflowNodeOutput> width_profile_pchip(
    WorkflowDocument* document, WorkflowInput s, WorkflowInput source_length,
    WorkflowInput stops, WorkflowInput widths,
    std::string domain = "subpath_normalized_arclength",
    ElementType output_type = ElementType::Float64) {
  using Answer = Result<WorkflowNodeOutput>;
  if (!document || (domain != "pathset_normalized_arclength" &&
                    domain != "subpath_normalized_arclength" &&
                    domain != "subpath_arclength_px"))
    return Answer(Status{ErrorCode::InvalidArgument, "invalid width domain"});
  auto ids = numeric::available_workflow_node_ids(
      *document, 3, {s, source_length, stops, widths});
  if (!ids.ok())
    return Answer(ids.status());
  const auto a = ids.value()[0], b = ids.value()[1], c = ids.value()[2];
  auto divided = numeric::divide_node(b, WorkflowNodeOutput{a, "numerator"},
                                      WorkflowNodeOutput{a, "denominator"});
  if (!divided.ok())
    return Answer(divided.status());
  auto interpolated = numeric::interpolate_pchip_node(
      c, std::move(stops), std::move(widths), WorkflowNodeOutput{b, "values"},
      output_type);
  if (!interpolated.ok())
    return Answer(interpolated.status());
  auto staged = *document;
  staged.nodes.push_back({a,
                          "path.width_query_operands_v1_strict",
                          {std::move(s), std::move(source_length)},
                          {{"domain", std::move(domain)}}});
  staged.nodes.push_back(divided.take_value());
  staged.nodes.push_back(interpolated.take_value());
  *document = std::move(staged);
  return Answer(WorkflowNodeOutput{c, "values"});
}

struct PathWithWidth final {
  WorkflowInput path;
  WorkflowNodeOutput width;
};
/** @brief PTH-05C preserves the original geometry reference and ObjectId.
 * profiles is Int64 [P,3]: domain, stop_begin, stop_end; source_slot is zero.
 * bindings is Int64 [B,3]: target_subpath, profile_row, source_subpath.
 * stops/widths are Float64 [K]. Optional static prefix counts are authored in
 * parameters; source IDs are recorded by the width producer without checking
 * payload associations. The returned pair owns references, not geometry data.
 * An existing binding is supplied explicitly; replacement is never implicit.
 */
inline Result<PathWithWidth> attach_linear_width(
    WorkflowDocument* document, WorkflowInput path, WorkflowInput profiles,
    WorkflowInput bindings, WorkflowInput stops, WorkflowInput widths,
    Parameters parameters = {}, bool replace_existing = false,
    const WorkflowNodeOutput* existing_width = nullptr) {
  using Answer = Result<PathWithWidth>;
  if (existing_width && !replace_existing)
    return Answer(
        Status{ErrorCode::InvalidArgument, "width binding already exists"});
  auto result = detail::append(document, "path.linear_width_binding_v1_strict",
                               {path, std::move(profiles), std::move(bindings),
                                std::move(stops), std::move(widths)},
                               std::move(parameters), "width");
  if (!result.ok())
    return Answer(result.status());
  return Answer(PathWithWidth{std::move(path), result.take_value()});
}

/** @brief Publish RN64 vertex widths from an explicitly retained source
 * binding. Sources are ordered by the binding's source slots. With source_map,
 * the binding remains paired with the source geometry; the map connects target
 * segments to those source segments. Without a map, segment/subpath identity
 * is a caller premise. Source ObjectIds are never used to discover inputs.
 * The resulting Float64 [target control capacity] value is the independent
 * width input of the variable-round stroke, outline or stroke-SDF operation.
 * Interior vertices use the following positive segment, final endpoints the
 * preceding segment's end. A closed seam reuses its single published M width.
 */
inline Result<WorkflowNodeOutput> evaluate_linear_width_binding(
    WorkflowDocument* document, WorkflowInput target, WorkflowInput binding,
    std::vector<WorkflowInput> sources, Parameters parameters = {},
    const WorkflowInput* source_map = nullptr) {
  if (sources.empty())
    return Result<WorkflowNodeOutput>(Status{
        ErrorCode::InvalidArgument, "width source geometry is required"});
  std::vector<WorkflowInput> inputs{std::move(target), std::move(binding)};
  if (source_map)
    inputs.push_back(*source_map);
  inputs.insert(inputs.end(), sources.begin(), sources.end());
  return detail::append(document,
                        source_map
                            ? "path.bound_linear_width_mapped_values_v1_strict"
                            : "path.bound_linear_width_values_v1_strict",
                        std::move(inputs), std::move(parameters), "values");
}

/** @brief GEN-05F explicitly publishes distance, broadcasts scale, divides.
 * distance_source selects centerline_unsigned, fill_signed or stroke_signed.
 * Additional distance_inputs follow the geometry port (for example stroke
 * width); parameters contain the complete PTH-11 canvas and stroke/fill policy.
 * scale must match the requested distance dtype and has shape [1]. The
 * explicit broadcast maps its singleton axis onto output axis 1.
 * Authoring is transactional and requires exclusive document access. No
 * execution state, sample backing or runtime owner is retained by this helper.
 */
inline Result<WorkflowNodeOutput> path_distance_coordinate(
    WorkflowDocument* document, WorkflowInput path, WorkflowInput scale,
    std::uint64_t height, std::uint64_t width,
    std::string distance_source = "centerline_unsigned",
    Parameters parameters = {}, std::vector<WorkflowInput> distance_inputs = {},
    Profile profile = Profile::Strict) {
  using Answer = Result<WorkflowNodeOutput>;
  const char* key =
      distance_source == "centerline_unsigned" ? "path.centerline_distance_v1_"
      : distance_source == "fill_signed"       ? "path.fill_region_sdf_v1_"
      : distance_source == "stroke_signed"     ? "path.stroke_region_sdf_v1_"
                                               : nullptr;
  if (!document || !key || height > INT64_MAX || width > INT64_MAX ||
      (profile != Profile::Strict && profile != Profile::AppleSilicon &&
       profile != Profile::X86_64))
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid distance workflow"});
  distance_inputs.insert(distance_inputs.begin(), path);
  auto dependencies = distance_inputs;
  dependencies.push_back(scale);
  auto ids = numeric::available_workflow_node_ids(*document, 3, dependencies);
  if (!ids.ok())
    return Answer(ids.status());
  auto broadcast = numeric::broadcast_node(ids.value()[1], std::move(scale),
                                           {height, width}, {1});
  if (!broadcast.ok())
    return Answer(broadcast.status());
  auto divided = numeric::divide_node(
      ids.value()[2], WorkflowNodeOutput{ids.value()[0], "distance"},
      WorkflowNodeOutput{ids.value()[1], "values"});
  if (!divided.ok())
    return Answer(divided.status());
  parameters["height"] = static_cast<std::int64_t>(height);
  parameters["width"] = static_cast<std::int64_t>(width);
  auto staged = *document;
  staged.nodes.push_back({ids.value()[0],
                          std::string(key) + detail::profile_suffix(profile),
                          std::move(distance_inputs), std::move(parameters)});
  staged.nodes.push_back(broadcast.take_value());
  staged.nodes.push_back(divided.take_value());
  *document = std::move(staged);
  return Answer(WorkflowNodeOutput{ids.value()[2], "values"});
}

struct PathRasterization final {
  WorkflowNodeOutput coverage, report;
};
/** @brief PTH-09B: deterministic flattening, followed by exact polygon area.
 * The intermediate polyline and its RN64 publication are observable stages.
 * The report refers to the source curve; it is a geometry-distance certificate,
 * not an area-error certificate. Parameters are separate for both nodes.
 */
inline Result<PathRasterization> fill_bezier_path(
    WorkflowDocument* document, WorkflowInput path,
    Parameters flatten_parameters, Parameters fill_parameters,
    Profile fill_profile = Profile::Strict) {
  using Answer = Result<PathRasterization>;
  if (!document || (fill_profile != Profile::Strict &&
                    fill_profile != Profile::AppleSilicon &&
                    fill_profile != Profile::X86_64))
    return Answer(Status{ErrorCode::InvalidArgument, "invalid fill workflow"});
  auto ids = numeric::available_workflow_node_ids(*document, 2, {path});
  if (!ids.ok())
    return Answer(ids.status());
  auto staged = *document;
  staged.nodes.push_back({ids.value()[0],
                          "path.flatten_bezier_path_v1_strict",
                          {std::move(path)},
                          std::move(flatten_parameters)});
  staged.nodes.push_back(
      {ids.value()[1],
       "path.fill_polygon_area_v1_" + detail::profile_suffix(fill_profile),
       {WorkflowNodeOutput{ids.value()[0], "path"}},
       std::move(fill_parameters)});
  *document = std::move(staged);
  return Answer(PathRasterization{{ids.value()[1], "coverage"},
                                  {ids.value()[0], "report"}});
}

struct PathWithReport final {
  WorkflowNodeOutput path, report;
};
/** @brief PTH-06B authors affine geometry and a separate propagated report.
 * The prior report is an explicit dependency. Approximation, quantization and
 * prior publication bounds scale by the exact spectral norm; the new affine
 * publication contributes another directed bound. The affine is nonsingular
 * by caller premise. Fill the returned outline explicitly with PTH-09.
 * This helper owns metadata only and transactionally updates the document.
 */
inline Result<PathWithReport> transform_stroke_outline(
    WorkflowDocument* document, WorkflowInput outline, WorkflowInput affine,
    WorkflowInput prior_report) {
  using Answer = Result<PathWithReport>;
  if (!document)
    return Answer(Status{ErrorCode::InvalidArgument, "null outline workflow"});
  auto ids = numeric::available_workflow_node_ids(
      *document, 2, {outline, affine, prior_report});
  if (!ids.ok())
    return Answer(ids.status());
  auto staged = *document;
  staged.nodes.push_back({ids.value()[0],
                          "path.transform_centerline_v1_strict",
                          {outline, affine},
                          {}});
  staged.nodes.push_back(
      {ids.value()[1],
       "path.transform_outline_report_v1_strict",
       {std::move(outline), std::move(affine), std::move(prior_report)},
       {}});
  *document = std::move(staged);
  return Answer(
      PathWithReport{{ids.value()[0], "path"}, {ids.value()[1], "report"}});
}

}  // namespace ps::generation
