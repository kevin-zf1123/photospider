#include "graph/roi_propagation_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/ops.hpp"

namespace ps {

namespace {

/**
 * @brief Checks whether a rectangle contains no positive-area pixels.
 * @param rect Rectangle to inspect without endpoint arithmetic.
 * @return True when width or height is non-positive.
 * @throws Nothing.
 * @note The helper deliberately does not add origins and dimensions, so even
 * hostile int endpoints cannot trigger signed overflow.
 */
bool is_rect_empty(const PixelRect& rect) noexcept {
  return rect.width <= 0 || rect.height <= 0;
}

/**
 * @brief Computes a representable bounding union with wide endpoint arithmetic.
 * @param a First rectangle or empty accumulator.
 * @param b Second rectangle or empty contribution.
 * @return Bounding rectangle, an unchanged non-empty operand when the other is
 * empty, or an empty rectangle when the union is not representable by int.
 * @throws Nothing.
 * @note Callers treat an empty unrepresentable result as a rejected unsafe
 * contribution and retain already accumulated state.
 */
PixelRect merge_rect(const PixelRect& a, const PixelRect& b) noexcept {
  if (is_rect_empty(a)) {
    return b;
  }
  if (is_rect_empty(b)) {
    return a;
  }
  const std::int64_t x0 = std::min(a.x, b.x);
  const std::int64_t y0 = std::min(a.y, b.y);
  const std::int64_t x1 = std::max(static_cast<std::int64_t>(a.x) + a.width,
                                   static_cast<std::int64_t>(b.x) + b.width);
  const std::int64_t y1 = std::max(static_cast<std::int64_t>(a.y) + a.height,
                                   static_cast<std::int64_t>(b.y) + b.height);
  if (x1 <= x0 || y1 <= y0 || x0 < std::numeric_limits<int>::min() ||
      y0 < std::numeric_limits<int>::min() ||
      x1 > std::numeric_limits<int>::max() ||
      y1 > std::numeric_limits<int>::max() ||
      x1 - x0 > std::numeric_limits<int>::max() ||
      y1 - y0 > std::numeric_limits<int>::max()) {
    return PixelRect{};
  }
  return PixelRect{static_cast<int>(x0), static_cast<int>(y0),
                   static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)};
}

/**
 * @brief Clips a rectangle to non-negative image bounds safely.
 * @param rect Candidate rectangle, including negative-origin halo regions.
 * @param bounds Positive image extent.
 * @return Intersection with `[0,width) x [0,height)`, or empty for invalid or
 * disjoint input.
 * @throws Nothing.
 * @note Origins and dimensions are added in signed 64-bit space before
 * clipping, avoiding signed-int overflow from plugin-controlled rectangles.
 */
PixelRect clamp_rect_to_bounds(const PixelRect& rect,
                               const PixelSize& bounds) noexcept {
  if (bounds.width <= 0 || bounds.height <= 0 || is_rect_empty(rect)) {
    return PixelRect{};
  }
  const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
  const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
  const std::int64_t x0 = std::clamp<std::int64_t>(rect.x, 0, bounds.width);
  const std::int64_t y0 = std::clamp<std::int64_t>(rect.y, 0, bounds.height);
  const std::int64_t x1 = std::clamp<std::int64_t>(right, 0, bounds.width);
  const std::int64_t y1 = std::clamp<std::int64_t>(bottom, 0, bounds.height);
  if (x1 <= x0 || y1 <= y0) {
    return PixelRect{};
  }
  return PixelRect{static_cast<int>(x0), static_cast<int>(y0),
                   static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)};
}

/**
 * @brief Applies one homogeneous transform only when all arithmetic is finite.
 * @param mat Row-major 3x3 transform matrix.
 * @param x Source x coordinate.
 * @param y Source y coordinate.
 * @return Transformed point, or nullopt for non-finite inputs/results or a
 * singular projective divisor.
 * @throws Nothing.
 * @note Invalid historical/private spatial state is rejected defensively even
 * though the v2 operation output adapter validates newly returned snapshots.
 */
std::optional<std::array<double, 2>> apply_matrix(
    const std::array<double, 9>& mat, double x, double y) noexcept {
  if (!std::isfinite(x) || !std::isfinite(y) ||
      !std::all_of(mat.begin(), mat.end(),
                   [](double value) { return std::isfinite(value); })) {
    return std::nullopt;
  }
  const double w = mat[6] * x + mat[7] * y + mat[8];
  if (!std::isfinite(w) || std::abs(w) < 1e-9) {
    return std::nullopt;
  }
  const double inv_w = 1.0 / w;
  const double tx = (mat[0] * x + mat[1] * y + mat[2]) * inv_w;
  const double ty = (mat[3] * x + mat[4] * y + mat[5]) * inv_w;
  if (!std::isfinite(tx) || !std::isfinite(ty)) {
    return std::nullopt;
  }
  return std::array<double, 2>{tx, ty};
}

/**
 * @brief Transforms a rectangle's four corners into a safe integer bound.
 * @param rect Positive-area source rectangle.
 * @param mat Row-major homogeneous transform matrix.
 * @return Bounding transformed rectangle, or empty when source endpoints,
 * matrix arithmetic, rounded coordinates, or result size are unsafe.
 * @throws Nothing.
 * @note Every floating value is checked before floor/ceil and int conversion;
 * no NaN, infinity, or out-of-range double reaches a cast to int.
 */
PixelRect transform_rect_with_matrix(
    const PixelRect& rect, const std::array<double, 9>& mat) noexcept {
  if (is_rect_empty(rect)) {
    return PixelRect{};
  }
  const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
  const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
  std::array<std::optional<std::array<double, 2>>, 4> transformed{
      apply_matrix(mat, rect.x, rect.y),
      apply_matrix(mat, static_cast<double>(right), rect.y),
      apply_matrix(mat, rect.x, static_cast<double>(bottom)),
      apply_matrix(mat, static_cast<double>(right),
                   static_cast<double>(bottom))};
  if (std::any_of(transformed.begin(), transformed.end(),
                  [](const auto& point) { return !point.has_value(); })) {
    return PixelRect{};
  }
  std::array<std::array<double, 2>, 4> pts{*transformed[0], *transformed[1],
                                           *transformed[2], *transformed[3]};
  double min_x = pts[0][0];
  double max_x = pts[0][0];
  double min_y = pts[0][1];
  double max_y = pts[0][1];
  for (size_t i = 1; i < pts.size(); ++i) {
    min_x = std::min(min_x, pts[i][0]);
    max_x = std::max(max_x, pts[i][0]);
    min_y = std::min(min_y, pts[i][1]);
    max_y = std::max(max_y, pts[i][1]);
  }
  const double rounded_x0 = std::floor(min_x);
  const double rounded_y0 = std::floor(min_y);
  const double rounded_x1 = std::ceil(max_x);
  const double rounded_y1 = std::ceil(max_y);
  if (!std::isfinite(rounded_x0) || !std::isfinite(rounded_y0) ||
      !std::isfinite(rounded_x1) || !std::isfinite(rounded_y1) ||
      rounded_x0 < std::numeric_limits<int>::min() ||
      rounded_y0 < std::numeric_limits<int>::min() ||
      rounded_x1 > std::numeric_limits<int>::max() ||
      rounded_y1 > std::numeric_limits<int>::max() ||
      rounded_x1 <= rounded_x0 || rounded_y1 <= rounded_y0 ||
      rounded_x1 - rounded_x0 > std::numeric_limits<int>::max() ||
      rounded_y1 - rounded_y0 > std::numeric_limits<int>::max()) {
    return PixelRect{};
  }
  return PixelRect{static_cast<int>(rounded_x0), static_cast<int>(rounded_y0),
                   static_cast<int>(rounded_x1 - rounded_x0),
                   static_cast<int>(rounded_y1 - rounded_y0)};
}

/**
 * @brief Returns the authoritative cached HP output for spatial propagation.
 * @param node Node whose private cache is inspected.
 * @return Borrowed cache pointer, or nullptr when no HP output is committed.
 * @throws Nothing.
 * @note The pointer remains valid only while GraphModel keeps this node/cache
 * unchanged under the caller's propagation synchronization.
 */
const NodeOutput* pick_cached_output(const Node& node) noexcept {
  if (node.cached_output_high_precision) {
    return &node.cached_output_high_precision.value();
  }
  return nullptr;
}

/**
 * @brief Accumulates alternative upstream ROI contributions for one node.
 *
 * RoiAccumulator keeps the "no ROI yet" state separate from an empty PixelRect
 * so operator propagation, spatial metadata, and dependency LUT lookup can be
 * merged without losing the distinction between "no contribution" and "empty
 * contribution".
 *
 * @throws Nothing; all state has fixed size.
 * @note The accumulator performs bounding-box union only. Callers remain
 * responsible for clipping the final ROI to the appropriate node extent.
 */
class RoiAccumulator {
 public:
  /**
   * @brief Adds one non-empty ROI contribution.
   *
   * @param roi Candidate ROI contribution.
   * @throws Nothing.
   * @note Unsafe unrepresentable unions are ignored without replacing a prior
   * accepted ROI.
   * @note Empty ROIs are ignored to preserve existing fallback behavior.
   */
  void include(const PixelRect& roi) {
    if (is_rect_empty(roi)) {
      return;
    }
    const PixelRect merged = has_roi_ ? merge_rect(roi_, roi) : roi;
    if (is_rect_empty(merged)) {
      return;
    }
    roi_ = merged;
    has_roi_ = true;
  }

  /**
   * @brief Returns the accumulated ROI.
   *
   * @return Bounding-box union of included ROIs, or an empty rect when no ROI
   * was included.
   * @throws Nothing.
   * @note The returned value is a copy and cannot mutate accumulator state.
   */
  PixelRect value() const { return has_roi_ ? roi_ : PixelRect{}; }

 private:
  /** @brief Current representable bounding union. */
  PixelRect roi_;
  /** @brief Whether roi_ contains at least one accepted contribution. */
  bool has_roi_ = false;
};

/**
 * @brief Owns graph-level ROI frontier traversal state.
 *
 * RoiFrontier stores the best known ROI per node plus the pending queue used by
 * graph-level forward/backward projection. It hides the repeated "merge ROI,
 * clip to node bounds, enqueue when changed" sequence shared by both
 * projection directions.
 *
 * @throws std::bad_alloc when the ROI map or pending queue grows.
 * @note The frontier stores node ids and value-type PixelRect records only; it
 * never stores GraphModel pointers or execution-runtime state.
 */
class RoiFrontier {
 public:
  /**
   * @brief Seeds the traversal with a clipped ROI.
   *
   * @param node_id Starting node id.
   * @param roi Candidate starting ROI.
   * @param bounds Output extent used to clip the ROI.
   * @return True when a non-empty seed was accepted.
   * @throws std::bad_alloc if map or queue storage grows and allocation fails.
   * @note Existing state for node_id is replaced because seed starts a new
   * traversal.
   */
  bool seed(int node_id, const PixelRect& roi, const PixelSize& bounds) {
    const PixelRect clipped = clamp_rect_to_bounds(roi, bounds);
    if (is_rect_empty(clipped))
      return false;
    roi_map_[node_id] = clipped;
    pending_.push(node_id);
    return true;
  }

  /**
   * @brief Checks whether traversal has pending nodes.
   *
   * @return True when no frontier node remains queued.
   * @throws Nothing.
   * @note Stored ROI map entries may remain after the queue becomes empty.
   */
  bool empty() const { return pending_.empty(); }

  /**
   * @brief Removes and returns the next queued node id.
   *
   * @return Node id at the front of the traversal queue.
   * @throws Nothing when the required non-empty precondition holds.
   * @note Callers must check empty() before every pop; violating that
   * precondition invokes std::queue's undefined empty-front behavior.
   */
  int pop() {
    const int node_id = pending_.front();
    pending_.pop();
    return node_id;
  }

  /**
   * @brief Reads the current ROI for one node and clips it to bounds.
   *
   * @param node_id Node id to read.
   * @param bounds Output extent used to clip the ROI.
   * @return Clipped ROI when present and non-empty; otherwise nullopt.
   * @throws Nothing.
   * @note The returned rectangle is a value copy of frontier state.
   */
  std::optional<PixelRect> clipped_roi(int node_id,
                                       const PixelSize& bounds) const {
    const auto it = roi_map_.find(node_id);
    if (it == roi_map_.end())
      return std::nullopt;
    const PixelRect clipped = clamp_rect_to_bounds(it->second, bounds);
    if (is_rect_empty(clipped))
      return std::nullopt;
    return clipped;
  }

  /**
   * @brief Merges a propagated ROI into a node and enqueues it when changed.
   *
   * @param node_id Node receiving propagated ROI.
   * @param roi Candidate propagated ROI.
   * @param bounds Output extent used to clip the merged ROI.
   * @return True when node_id was newly inserted or its ROI changed.
   * @throws std::bad_alloc if map or queue storage grows and allocation fails.
   * @note The method preserves prior behavior where only changed bounding boxes
   * are requeued.
   */
  bool merge_or_enqueue(int node_id, const PixelRect& roi,
                        const PixelSize& bounds) {
    const PixelRect clipped = clamp_rect_to_bounds(roi, bounds);
    if (is_rect_empty(clipped))
      return false;

    const auto it = roi_map_.find(node_id);
    if (it == roi_map_.end()) {
      roi_map_[node_id] = clipped;
      pending_.push(node_id);
      return true;
    }

    const PixelRect merged =
        clamp_rect_to_bounds(merge_rect(it->second, clipped), bounds);
    if (merged == it->second || is_rect_empty(merged))
      return false;
    roi_map_[node_id] = merged;
    pending_.push(node_id);
    return true;
  }

 private:
  /** @brief Best known clipped ROI keyed by graph node id. */
  std::unordered_map<int, PixelRect> roi_map_;
  /** @brief Node ids whose current ROI still needs propagation. */
  std::queue<int> pending_;
};

/**
 * @brief Owns private dependency-cache inputs in destination-index order.
 * @throws std::bad_alloc when vectors grow.
 * @note Missing image inputs keep empty extents and zero revisions so index
 *       identity remains stable across sparse topology entries.
 */
struct DependencyInputState {
  /** @brief Known image-input extents by destination input index. */
  std::vector<PixelSize> extents;
  /** @brief Parent HP content revisions by destination input index. */
  std::vector<uint64_t> content_revisions;
};

/**
 * @brief Owns one validated dependency-LUT ROI and its selected input route.
 * @throws Nothing for ordinary value operations.
 * @note input_index uses destination image-input index semantics, matching
 *       InputEdgeView::input_index and graph topology edges.
 */
struct DependencyRoiContribution {
  /** @brief Destination image-input index selected by the cached LUT. */
  std::size_t input_index = 0;
  /** @brief Upstream ROI returned for the requested output cells. */
  PixelRect roi;
};

/**
 * @brief Builds all image-input extents and content revisions for one node.
 *
 * @tparam SizeResolver Callable taking a node id and returning PixelSize.
 * @param graph Graph containing image-input edges and parent revisions.
 * @param node_id Destination node id.
 * @param get_size Extent resolver with request-local caching.
 * @return Vectors indexed by each edge's destination input index.
 * @throws GraphError from get_size when extent resolution fails.
 * @throws std::bad_alloc when vector growth fails.
 */
template <typename SizeResolver>
DependencyInputState dependency_input_state(const GraphModel& graph,
                                            int node_id,
                                            SizeResolver get_size) {
  DependencyInputState state;
  for (const auto& edge : graph.upstream_edges(node_id)) {
    if (edge.kind != GraphTopologyEdgeKind::ImageInput) {
      continue;
    }
    if (edge.input_index >= state.extents.size()) {
      state.extents.resize(edge.input_index + 1);
      state.content_revisions.resize(edge.input_index + 1);
    }
    if (edge.from_node_id < 0 || !graph.has_node(edge.from_node_id)) {
      continue;
    }
    state.extents[edge.input_index] = get_size(edge.from_node_id);
    const int revision = graph.node(edge.from_node_id).hp_version;
    state.content_revisions[edge.input_index] =
        revision > 0 ? static_cast<uint64_t>(revision) : 0;
  }
  return state;
}

/**
 * @brief Checks whether at least one dependency input has a usable extent.
 * @param extents Input extents indexed by destination input index.
 * @return True when one width and height pair is positive.
 * @throws Nothing.
 */
bool has_valid_dependency_input(
    const std::vector<PixelSize>& extents) noexcept {
  return std::any_of(extents.begin(), extents.end(),
                     [](const PixelSize& extent) {
                       return extent.width > 0 && extent.height > 0;
                     });
}

/**
 * @brief Appends parameter-input content revisions to an identity vector.
 * @param node Node whose parameter-input edges are inspected in declaration
 *        order.
 * @param graph Graph providing parent HP content revisions.
 * @param revisions Destination vector populated in parameter declaration order.
 * @return Nothing.
 * @throws std::bad_alloc if vector growth fails.
 * @note Missing parents append zero so rewiring/missing state remains explicit;
 *       these revisions participate regardless of image data-dependency mode.
 */
void append_parameter_input_revisions(const Node& node, const GraphModel& graph,
                                      std::vector<uint64_t>& revisions) {
  revisions.reserve(revisions.size() + node.parameter_inputs.size());
  for (const auto& input : node.parameter_inputs) {
    if (input.from_node_id < 0 || !graph.has_node(input.from_node_id)) {
      revisions.push_back(0);
      continue;
    }
    const int revision = graph.node(input.from_node_id).hp_version;
    revisions.push_back(revision > 0 ? static_cast<uint64_t>(revision) : 0);
  }
}

/**
 * @brief Copies image-input source identities in destination-index order.
 * @param node Node whose declared image inputs define callback topology.
 * @return Complete source node/output identity vector, including disconnected
 *         entries.
 * @throws std::bad_alloc when vector or output-name storage allocation fails.
 * @note Extent and content revision equality cannot distinguish a rewire to a
 *       different source with coincidentally equal values, so source identity
 *       is an independent cache-key component.
 */
std::vector<DependencyImageInputIdentity> image_input_source_identities(
    const Node& node) {
  std::vector<DependencyImageInputIdentity> identities;
  identities.reserve(node.image_inputs.size());
  for (const ImageInput& input : node.image_inputs) {
    identities.push_back(DependencyImageInputIdentity{input.from_node_id,
                                                      input.from_output_name});
  }
  return identities;
}

/**
 * @brief Validates one private dependency table against current host inputs.
 * @param lut Candidate table returned by a builder.
 * @param input_extents Current image-input extents by destination index.
 * @param child_size Current output extent.
 * @return Nothing.
 * @throws GraphError when structure, output extent, or upstream index is
 *         invalid.
 * @note Validation happens before either LUT or identity cache publication.
 */
void validate_dependency_lut(const SpatialDependencyMap& lut,
                             const std::vector<PixelSize>& input_extents,
                             const PixelSize& child_size) {
  if (!lut.is_valid_for(child_size)) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dependency LUT is invalid for the output extent.");
  }
  if (lut.upstream_input_index >= input_extents.size()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dependency LUT names an unknown image input.");
  }
  const PixelSize& upstream = input_extents[lut.upstream_input_index];
  if (upstream.width <= 0 || upstream.height <= 0) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Dependency LUT names an input with unknown extent.");
  }
}

/**
 * @brief Reuses or strongly replaces one data-dependent LUT cache.
 *
 * @param node Node owning private cached state.
 * @param graph Graph passed to the private builder adapter.
 * @param builder_snapshot Coherent callback, flag, and registry revisions.
 * @param current_roi Downstream ROI to look up.
 * @param input_state Current extents and content revisions.
 * @param child_size Current output extent.
 * @param effective_parameters Exact parameters supplied to the builder.
 * @return Routed contribution, or nullopt when geometry has no usable demand.
 * @throws GraphError when the private table or current input route is invalid.
 * @throws std::invalid_argument when the public operation adapter rejects LUT
 * dimensions, output/input identity, cell geometry, or normalized extents.
 * @throws Any other exception emitted by the registered builder unchanged.
 * @throws std::bad_alloc unchanged from identity, builder, or table storage.
 * @throws std::overflow_error if the private diagnostic version is exhausted.
 * @note LUT and identity replacements use no-throw optional swaps after all
 *       conversion and validation succeed, preserving the previous pair on
 *       every failure.
 */
std::optional<DependencyRoiContribution> dependency_lookup(
    const Node& node, const GraphModel& graph,
    const DependencyBuilderSnapshot& builder_snapshot,
    const PixelRect& current_roi, DependencyInputState input_state,
    const PixelSize& child_size,
    const plugin::ParameterMap& effective_parameters) {
  if (is_rect_empty(current_roi) ||
      !has_valid_dependency_input(input_state.extents) ||
      child_size.width <= 0 || child_size.height <= 0) {
    return std::nullopt;
  }
  DependencyLutCacheIdentity identity;
  identity.effective_parameters = effective_parameters;
  identity.static_parameter_revision = node.parameters_version;
  append_parameter_input_revisions(node, graph,
                                   identity.parameter_input_content_revisions);
  identity.image_input_sources = image_input_source_identities(node);
  identity.input_extents = input_state.extents;
  identity.data_dependent = builder_snapshot.data_dependent;
  identity.dependency_builder_revision =
      builder_snapshot.dependency_builder_revision;
  identity.data_dependent_revision = builder_snapshot.data_dependent_revision;
  if (builder_snapshot.data_dependent) {
    identity.upstream_content_revisions =
        std::move(input_state.content_revisions);
  }

  const bool reusable = node.dependency_lut_cache &&
                        node.dependency_lut_cache->identity == identity &&
                        node.dependency_lut_cache->lut.is_valid_for(child_size);
  if (!reusable) {
    if (node.dependency_lut_version == std::numeric_limits<uint64_t>::max()) {
      throw std::overflow_error("dependency LUT version exhausted");
    }
    SpatialDependencyMap lut = builder_snapshot.callback(
        node, graph, input_state.extents, child_size, effective_parameters);
    validate_dependency_lut(lut, input_state.extents, child_size);
    std::optional<DependencyLutCache> staged_cache(
        std::in_place, DependencyLutCache{std::move(lut), std::move(identity)});
    static_assert(noexcept(node.dependency_lut_cache.swap(staged_cache)),
                  "dependency LUT cache publication must be no-throw");
    node.dependency_lut_cache.swap(staged_cache);
    node.dependency_lut_version += 1;
  }
  return DependencyRoiContribution{
      node.dependency_lut_cache->lut.upstream_input_index,
      node.dependency_lut_cache->lut.lookup(current_roi)};
}

/**
 * @brief Adds operator-provided dirty propagation to an accumulator.
 *
 * @param node Node whose operator propagator is resolved.
 * @param clamped_roi Downstream ROI already clipped to node output extent.
 * @param output_extent Current node output extent.
 * @param graph Graph passed to the operator propagator.
 * @param input_extents Known image-input extents by destination index.
 * @param effective_parameters Exact parameters resolved for this request.
 * @param accumulator Accumulator receiving the propagated upstream ROI.
 * @throws Exceptions propagated by registered operator propagators.
 */
void append_operator_upstream_roi(
    const Node& node, const PixelRect& clamped_roi,
    const PixelSize& output_extent, const GraphModel& graph,
    const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& effective_parameters,
    RoiAccumulator& accumulator) {
  auto propagate_fn =
      OpRegistry::instance().get_dirty_propagator(node.type, node.subtype);
  accumulator.include(propagate_fn(node, clamped_roi, graph, output_extent,
                                   input_extents, effective_parameters,
                                   nullptr));
}

/**
 * @brief Adds cached spatial metadata projection to an accumulator.
 *
 * @param node Node whose single-input spatial metadata may be used.
 * @param clamped_roi Downstream ROI already clipped to node output extent.
 * @param accumulator Accumulator receiving the matrix-projected ROI.
 * @throws Nothing directly.
 * @note Spatial metadata contributes only for single-image-input nodes with a
 * high-precision cached output, preserving the existing HP-authoritative rule.
 */
void append_spatial_metadata_roi(const Node& node, const PixelRect& clamped_roi,
                                 RoiAccumulator& accumulator) {
  if (node.image_inputs.size() != 1)
    return;
  if (const NodeOutput* cached = pick_cached_output(node)) {
    accumulator.include(transform_rect_with_matrix(
        clamped_roi, cached->space.local_inverse_matrix));
  }
}

/**
 * @brief Resolves a dependency LUT contribution without losing its input route.
 *
 * @param node Node whose dependency builder may be registered.
 * @param graph Graph used by the dependency LUT builder.
 * @param clamped_roi Downstream ROI already clipped to node output extent.
 * @param downstream_size Current node output extent.
 * @param input_state Current input extents and revisions.
 * @param effective_parameters Exact parameters resolved for this request.
 * @return Routed LUT contribution, or nullopt when no builder/demand exists.
 * @throws Exceptions propagated by the registered LUT builder or extent
 * resolver.
 * @note The caller keeps this contribution separate until it selects the
 * matching destination image-input edge.
 */
std::optional<DependencyRoiContribution> dependency_lut_roi(
    const Node& node, const GraphModel& graph, const PixelRect& clamped_roi,
    const PixelSize& downstream_size, DependencyInputState input_state,
    const plugin::ParameterMap& effective_parameters) {
  const auto builder_snapshot =
      OpRegistry::instance().get_dependency_builder_snapshot(node.type,
                                                             node.subtype);
  if (!builder_snapshot) {
    return std::nullopt;
  }

  if (!has_valid_dependency_input(input_state.extents) ||
      downstream_size.width <= 0 || downstream_size.height <= 0) {
    return std::nullopt;
  }
  return dependency_lookup(node, graph, *builder_snapshot, clamped_roi,
                           std::move(input_state), downstream_size,
                           effective_parameters);
}

/**
 * @brief Propagates one forward image edge into a child ROI.
 *
 * @param graph Graph containing the child node.
 * @param edge Image-input edge from the current parent to a child.
 * @param parent_roi Current ROI in parent output coordinates.
 * @param parent_size Parent output extent.
 * @param child_size Child output extent.
 * @param input_extents Known child image-input extents by destination index.
 * @param effective_parameters Exact child parameters for this request.
 * @return Child ROI clipped to child output extent, or nullopt when no work is
 * affected.
 * @throws Exceptions propagated by registered forward propagators.
 */
std::optional<PixelRect> propagate_forward_edge_roi(
    const GraphModel& graph, const GraphTopologyEdge& edge,
    const PixelRect& parent_roi, const PixelSize& parent_size,
    const PixelSize& child_size, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& effective_parameters) {
  const Node& child = graph.node(edge.to_node_id);
  auto forward_fn =
      OpRegistry::instance().get_forward_propagator(child.type, child.subtype);
  const PixelRect propagated = clamp_rect_to_bounds(
      forward_fn(child, parent_roi, graph, parent_size, child_size,
                 edge.input_index, input_extents, effective_parameters),
      child_size);
  if (is_rect_empty(propagated))
    return std::nullopt;
  return propagated;
}

/**
 * @brief Enqueues parent ROIs produced by a backward propagation step.
 *
 * @tparam SizeResolver Callable taking a node id and returning PixelSize.
 * @param graph Graph containing the current node's image-input edges.
 * @param current_id Node whose parents receive upstream demand.
 * @param projection Shared and input-selected upstream contributions.
 * @param get_size Extent resolver with request-local caching.
 * @param frontier Frontier receiving parent ROI merges.
 * @throws GraphError from get_size when extent resolution fails.
 * @note Operator/spatial demand reaches every image parent. Dependency-LUT
 * demand reaches only the edge whose destination input index it names.
 */
template <typename SizeResolver>
void enqueue_backward_parent_rois(const GraphModel& graph, int current_id,
                                  const UpstreamRoiProjection& projection,
                                  SizeResolver get_size,
                                  RoiFrontier& frontier) {
  for (const auto& edge : graph.upstream_edges(current_id)) {
    if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
        edge.from_node_id < 0 || !graph.has_node(edge.from_node_id)) {
      continue;
    }
    const int parent_id = edge.from_node_id;
    const PixelSize parent_extent = get_size(parent_id);
    frontier.merge_or_enqueue(
        parent_id, projection.roi_for_input(edge.input_index, parent_extent),
        parent_extent);
  }
}

}  // namespace

PixelRect UpstreamRoiProjection::roi_for_input(
    std::size_t input_index) const noexcept {
  if (dependency_input_index && *dependency_input_index == input_index) {
    return merge_rect(shared_roi, dependency_roi);
  }
  return shared_roi;
}

PixelRect UpstreamRoiProjection::roi_for_input(
    std::size_t input_index, const PixelSize& input_extent) const noexcept {
  const PixelRect bounded_shared =
      clamp_rect_to_bounds(shared_roi, input_extent);
  if (!dependency_input_index || *dependency_input_index != input_index) {
    return bounded_shared;
  }
  const PixelRect bounded_dependency =
      clamp_rect_to_bounds(dependency_roi, input_extent);
  return merge_rect(bounded_shared, bounded_dependency);
}

PixelRect UpstreamRoiProjection::combined_roi() const noexcept {
  return merge_rect(shared_roi, dependency_roi);
}

UpstreamRoiProjection RoiPropagationService::compute_upstream_projection(
    const Node& node, const PixelRect& downstream_roi, const GraphModel& graph,
    std::unordered_map<int, PixelSize>& size_cache) const {
  if (is_rect_empty(downstream_roi))
    return {};

  auto get_size = [&](int nid) {
    return extent_resolver_.resolve_output_extent(graph, nid, size_cache);
  };
  PixelRect clamped_roi =
      clamp_rect_to_bounds(downstream_roi, get_size(node.id));
  if (is_rect_empty(clamped_roi))
    return {};

  const PixelSize output_extent = get_size(node.id);
  DependencyInputState input_state =
      dependency_input_state(graph, node.id, get_size);
  const plugin::ParameterMap effective_parameters =
      resolve_effective_parameter_snapshot(node, graph);
  RoiAccumulator upstream;
  append_operator_upstream_roi(node, clamped_roi, output_extent, graph,
                               input_state.extents, effective_parameters,
                               upstream);
  append_spatial_metadata_roi(node, clamped_roi, upstream);
  UpstreamRoiProjection projection;
  projection.shared_roi = upstream.value();
  const auto dependency =
      dependency_lut_roi(node, graph, clamped_roi, output_extent,
                         std::move(input_state), effective_parameters);
  if (dependency && !is_rect_empty(dependency->roi)) {
    projection.dependency_input_index = dependency->input_index;
    projection.dependency_roi = dependency->roi;
  }
  return projection;
}

PixelRect RoiPropagationService::compute_upstream_roi(
    const Node& node, const PixelRect& downstream_roi, const GraphModel& graph,
    std::unordered_map<int, PixelSize>& size_cache) const {
  return compute_upstream_projection(node, downstream_roi, graph, size_cache)
      .combined_roi();
}

std::optional<PixelRect> RoiPropagationService::project_roi_forward(
    const GraphModel& graph, int start_node_id, const PixelRect& start_roi,
    int target_node_id) const {
  if (!graph.has_node(start_node_id) || !graph.has_node(target_node_id))
    return std::nullopt;
  if (is_rect_empty(start_roi))
    return std::nullopt;

  std::unordered_map<int, PixelSize> size_cache;
  auto get_size = [&](int nid) {
    return extent_resolver_.resolve_output_extent(graph, nid, size_cache);
  };

  RoiFrontier frontier;

  if (!frontier.seed(start_node_id, start_roi, get_size(start_node_id)))
    return std::nullopt;

  while (!frontier.empty()) {
    int current = frontier.pop();
    auto current_roi = frontier.clipped_roi(current, get_size(current));
    if (!current_roi)
      continue;
    if (current == target_node_id)
      break;

    PixelSize parent_size = get_size(current);
    for (const auto& edge : graph.downstream_edges(current)) {
      if (edge.kind != GraphTopologyEdgeKind::ImageInput) {
        continue;
      }
      int child_id = edge.to_node_id;
      PixelSize child_size = get_size(child_id);
      if (child_size.width <= 0 || child_size.height <= 0)
        continue;

      const Node& child = graph.node(child_id);
      const DependencyInputState input_state =
          dependency_input_state(graph, child_id, get_size);
      const plugin::ParameterMap effective_parameters =
          resolve_effective_parameter_snapshot(child, graph);
      auto propagated = propagate_forward_edge_roi(
          graph, edge, *current_roi, parent_size, child_size,
          input_state.extents, effective_parameters);
      if (propagated) {
        frontier.merge_or_enqueue(child_id, *propagated, child_size);
      }
    }
  }

  return frontier.clipped_roi(target_node_id, get_size(target_node_id));
}

std::optional<PixelRect> RoiPropagationService::project_roi_backward(
    const GraphModel& graph, int target_node_id, const PixelRect& target_roi,
    int source_node_id) const {
  if (!graph.has_node(target_node_id) || !graph.has_node(source_node_id))
    return std::nullopt;
  if (is_rect_empty(target_roi))
    return std::nullopt;

  std::unordered_map<int, PixelSize> size_cache;
  auto get_size = [&](int nid) {
    return extent_resolver_.resolve_output_extent(graph, nid, size_cache);
  };
  RoiFrontier frontier;

  if (!frontier.seed(target_node_id, target_roi, get_size(target_node_id)))
    return std::nullopt;

  while (!frontier.empty()) {
    int current = frontier.pop();
    auto current_roi = frontier.clipped_roi(current, get_size(current));
    if (!current_roi)
      continue;
    if (current == source_node_id)
      return current_roi;

    const Node& node = graph.node(current);

    const UpstreamRoiProjection projection =
        compute_upstream_projection(node, *current_roi, graph, size_cache);
    if (is_rect_empty(projection.shared_roi) &&
        is_rect_empty(projection.dependency_roi)) {
      continue;
    }

    enqueue_backward_parent_rois(graph, current, projection, get_size,
                                 frontier);
  }

  return std::nullopt;
}

}  // namespace ps
