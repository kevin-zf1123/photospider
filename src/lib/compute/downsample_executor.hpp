#pragma once

#include <vector>

#include "compute/realtime_proxy_graph.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphEventService;
class GraphRuntime;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Refreshes real-time proxy buffers from committed HP dirty outputs.
 *
 * DownsampleExecutor owns the HP-to-RT refresh that follows high-precision
 * dirty execution when a GraphRuntime is present. Each request records the HP
 * ROI and HP version committed for a node. Execution skips stale requests,
 * allocates or reuses proxy buffers, copies non-image payloads, and records the
 * same downsample/downsample_passthrough events used before the dirty executor
 * split.
 *
 * @note Instances borrow GraphModel, RealtimeProxyGraph, GraphRuntime, and
 * GraphEventService for a single call chain. RT output is committed only to the
 * proxy graph; GraphModel keeps HP cache authority. CPU images are resized.
 * Opaque non-CPU images are preserved as full-resolution shared descriptors in
 * the proxy graph because only their device adapter can clone or downsample
 * them; generic code never interprets the context type.
 */
class DownsampleExecutor {
 public:
  /**
   * @brief One pending HP-to-RT refresh request.
   *
   * @note hp_version is compared against the node's current HP and RT versions
   * to avoid overwriting newer proxy state with stale downsample work.
   */
  struct Request {
    /** @brief Node whose HP output should refresh the RT proxy. */
    int node_id = -1;

    /** @brief HP-space region that changed during dirty execution. */
    PixelRect roi_hp;

    /** @brief HP version captured after the dirty node update. */
    int hp_version = 0;
  };

  /**
   * @brief Constructs a downsample executor for one graph-owned update.
   *
   * @param graph Graph containing committed HP node cache state.
   * @param proxy_graph RT proxy graph receiving downsampled output.
   * @param runtime Optional runtime used only for scheduler trace events.
   * @param events Event service that receives downsample status events.
   * @throws Nothing directly.
   * @note The executor stores borrowed references and performs no ownership
   * transfer.
   */
  DownsampleExecutor(GraphModel& graph, RealtimeProxyGraph& proxy_graph,
                     GraphRuntime* runtime, GraphEventService& events);

  /**
   * @brief Executes all pending downsample requests in caller order.
   *
   * @param requests Pending node refreshes created by HP dirty execution.
   * @return Nothing.
   * @throws std::bad_alloc if proxy cloning or resize allocation exhausts
   * memory.
   * @throws GraphError or OpenCV exceptions if image conversion or resize
   * otherwise fails unexpectedly.
   * @note Empty request vectors are valid and leave graph state unchanged.
   */
  void execute(const std::vector<Request>& requests);

 private:
  /**
   * @brief Applies one HP-to-RT refresh request.
   *
   * @param request Request describing the node, ROI, and HP version to copy.
   * @return Nothing.
   * @throws std::bad_alloc from proxy cloning or resize allocation.
   * @throws GraphError or OpenCV exceptions from other buffer conversion or
   * resize failures.
   * @note Missing nodes, missing HP outputs, and stale generations are skipped
   * to preserve the previous dirty update behavior.
   */
  void execute_one(const Request& request);

  /**
   * @brief Resolves the node and validates generation freshness.
   *
   * @param request Downsample request being inspected.
   * @return Mutable node when HP output exists and the request is current.
   * @throws Nothing directly.
   * @note Stale checks emit the same SKIP_STALE_GENERATION trace event as the
   * previous combined dirty executor.
   */
  Node* find_current_node(const Request& request);

  /**
   * @brief Clips the HP request ROI to the HP output extent.
   *
   * @param request_roi HP-space ROI from the dirty update.
   * @param hp_size HP output extent.
   * @return Clipped ROI, or full output when the requested ROI is empty.
   * @throws Nothing directly.
   * @note Empty HP outputs remain empty so passthrough payloads keep their
   * previous metadata behavior.
   */
  PixelRect normalize_hp_roi(const PixelRect& request_roi,
                             const PixelSize& hp_size) const;

  /**
   * @brief Copies HP output directly to RT cache for non-image payloads.
   *
   * @param node Node whose HP output is copied.
   * @param proxy_state Proxy node state receiving RT output and metadata.
   * @param roi_hp HP-space region represented by the update.
   * @param hp_size HP output extent used to merge RT ROI metadata.
   * @param hp_version HP version that becomes the RT version.
   * @throws std::bad_alloc if optional cache assignment allocates.
   * @note This path handles empty image buffers, zero-sized RT proxy extents,
   * and opaque non-CPU images. Backend passthrough preserves the complete HP
   * descriptor and lifetime rather than claiming a fabricated downscaled
   * extent.
   */
  void apply_passthrough(Node& node, RealtimeProxyGraph::NodeState& proxy_state,
                         const PixelRect& roi_hp, const PixelSize& hp_size,
                         int hp_version);

  /**
   * @brief Ensures the RT image buffer matches the downscaled HP image shape.
   *
   * @param proxy_state Proxy node state whose RT buffer is allocated.
   * @param hp_buffer Source HP image buffer.
   * @param rt_size Downscaled RT image extent.
   * @return Mutable RT image buffer with matching dimensions and format.
   * @throws GraphError or std::bad_alloc if allocation fails.
   * @note Existing proxy payload data map is preserved by reusing NodeOutput.
   */
  ImageBuffer& ensure_rt_buffer(RealtimeProxyGraph::NodeState& proxy_state,
                                const ImageBuffer& hp_buffer,
                                const PixelSize& rt_size);

  /**
   * @brief Downsamples one HP ROI into the matching RT ROI.
   *
   * @param hp_buffer Source HP image buffer.
   * @param rt_buffer Destination RT image buffer.
   * @param roi_hp HP-space ROI to resize.
   * @param rt_size Full RT image extent.
   * @return RT-space ROI that received the resized pixels.
   * @throws OpenCV exceptions from Mat conversion, ROI slicing, resize, or
   * copy.
   * @note Empty RT ROI is widened to the full RT extent, matching previous
   * dirty downsample behavior.
   */
  PixelRect downsample_roi(const ImageBuffer& hp_buffer, ImageBuffer& rt_buffer,
                           const PixelRect& roi_hp,
                           const PixelSize& rt_size) const;

  /**
   * @brief Updates RT ROI/version metadata after a successful image refresh.
   *
   * @param proxy_state Proxy node state whose RT metadata is updated.
   * @param roi_hp HP-space ROI represented by the RT proxy update.
   * @param hp_size HP output extent used to clamp merged ROI state.
   * @param hp_version HP version copied into RT metadata.
   * @throws Nothing directly.
   * @note RT ROI remains stored in HP coordinates for inspection consistency.
   */
  void commit_rt_metadata(RealtimeProxyGraph::NodeState& proxy_state,
                          const PixelRect& roi_hp, const PixelSize& hp_size,
                          int hp_version);

  /**
   * @brief Emits a stale-generation scheduler trace event when possible.
   *
   * @param node_id Node whose request was skipped.
   * @throws Any exception propagated by GraphRuntime::log_event.
   * @note A null runtime intentionally suppresses scheduler trace output.
   */
  void log_stale_generation(int node_id) const;

  /** @brief Borrowed graph whose HP cache state is read. */
  GraphModel& graph_;

  /** @brief Borrowed proxy graph whose RT state is refreshed. */
  RealtimeProxyGraph& proxy_graph_;

  /** @brief Optional runtime used for stale-generation and tile trace events.
   */
  GraphRuntime* runtime_;

  /** @brief Borrowed event sink for downsample status events. */
  GraphEventService& events_;
};

}  // namespace ps::compute
