#pragma once

#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Input collection prepared for tiled node execution.
 *
 * TiledInputContext keeps the original input order while optionally replacing
 * selected entries with normalized temporary NodeOutput objects. It is used by
 * NodeExecutor to infer output format and to build per-tile input views.
 *
 * @note Pointers in inputs either reference upstream NodeOutput objects
 * supplied by the caller or elements owned by normalized_storage. The context
 * must stay alive until all TileTask callbacks using those pointers have
 * finished.
 */
struct TiledInputContext {
  /** @brief Temporary normalized images used by image_mixing secondary inputs.
   */
  std::vector<NodeOutput> normalized_storage;

  /** @brief Ordered input pointers visible to tiled execution. */
  std::vector<const NodeOutput*> inputs;
};

/**
 * @brief Normalizes tiled node inputs without executing tile work.
 *
 * The normalizer preserves the previous image_mixing behavior: the first input
 * defines the base extent/channel count, secondary inputs are resized or
 * cropped according to merge_strategy, and supported channel conversions are
 * materialized into temporary NodeOutput storage. Non-mixing nodes and mixing
 * nodes with fewer than two inputs pass through unchanged.
 *
 * @note This class owns no graph state. Returned temporary storage belongs to
 * the returned TiledInputContext and must outlive any tile dispatch that uses
 * the normalized inputs. Normalization replaces only image descriptors;
 * named-data, spatial/debug provenance, and plugin DSO leases remain copied
 * from each upstream NodeOutput.
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
   * @throws cv::Exception or std::runtime_error when OpenCV conversion fails.
   * @note The method performs whole-input normalization only when needed; tile
   * ROI clipping remains NodeExecutor's responsibility.
   */
  static TiledInputContext normalize(
      const Node& node, const std::vector<const NodeOutput*>& inputs);
};

}  // namespace ps::compute
