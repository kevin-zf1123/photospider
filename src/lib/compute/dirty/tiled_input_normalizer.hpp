#pragma once

#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Input collection prepared for tiled node execution.
 *
 * TiledInputContext keeps the original input order while optionally replacing
 * selected entries with normalized temporary named-Value outputs. It also
 * owns exact callback-local ImageBuffer projections for the current tiled ABI
 * edge. NodeExecutor derives authoritative format/extent facts from Values and
 * uses the projections only to build per-tile compatibility views.
 *
 * @note Pointers in inputs either reference upstream NodeOutput objects
 * supplied by the caller or elements owned by normalized_storage. Entries in
 * callback_images align with inputs and retain independent use-scoped
 * projections. CPU inputs are owned snapshots; exact imported device inputs
 * are opaque aliases retaining their canonical Values. The context must stay
 * alive until all TileTask callbacks using those pointers have finished.
 */
struct TiledInputContext {
  /** @brief Temporary normalized images used by image_mixing secondary inputs.
   */
  std::vector<NodeOutput> normalized_storage;

  /**
   * @brief Use-scoped ImageBuffer projections aligned with input slots.
   * @note These values are never cache, revision, Region, extent, or output
   * allocation authority and are destroyed after the tiled invocation.
   */
  std::vector<ImageBuffer> callback_images;

  /** @brief Ordered input pointers visible to tiled execution. */
  std::vector<const NodeOutput*> inputs;
};

/**
 * @brief Normalizes tiled node inputs without executing tile work.
 *
 * The normalizer preserves the previous image_mixing behavior: the first input
 * defines the base extent/channel count, secondary inputs are resized or
 * cropped according to merge_strategy, and supported channel conversions are
 * materialized into temporary NodeOutput storage. Crop/pad uses kernel-owned
 * stride-aware fill/copy primitives; resize/channel work uses the one
 * image-processing implementation selected by the build. Non-mixing nodes and
 * mixing nodes with fewer than two inputs pass through unchanged.
 *
 * @note This class owns no graph state. Returned temporary storage belongs to
 * the returned TiledInputContext and must outlive any tile dispatch that uses
 * the normalized inputs. Normalization replaces only image descriptors;
 * named-data, spatial/debug provenance, and plugin DSO leases remain copied
 * from each upstream NodeOutput. Every materialized normalized image is
 * imported through a Host binding and sealed before it enters the context.
 * Opaque non-CPU inputs pass through only while their shape already matches;
 * resize, crop, and channel conversion remain explicit CPU-only operations
 * and fail closed otherwise.
 */
class TiledInputNormalizer {
 public:
  /**
   * @brief Builds the tiled input context for one node invocation.
   *
   * @param node Node whose runtime parameters control image_mixing strategy.
   * @param inputs Resolved upstream image outputs in graph input order.
   * @return TiledInputContext containing pass-through and normalized inputs.
   * @throws GraphError when an image_mixing input is empty, missing, or
   * requests an unsupported merge_strategy/channel conversion.
   * @throws std::invalid_argument, std::out_of_range, std::overflow_error, or
   *         std::bad_alloc when kernel validation, allocation, fill, or copy
   *         fails.
   * @throws ReadyFenceAccessError or BufferAccessError when an input cannot
   *         enter the source-private ImageBuffer callback edge without an
   *         explicit access or transfer plan.
   * @throws std::exception when the selected resize/channel implementation
   *         fails.
   * @note The method performs whole-input normalization only when needed; tile
   * ROI clipping remains NodeExecutor's responsibility.
   */
  static TiledInputContext normalize(
      const Node& node, const std::vector<const NodeOutput*>& inputs);
};

}  // namespace ps::compute
