#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "compute/dirty/tiled_input_normalizer.hpp"
#include "core/host_output_authorization.hpp"
#include "core/ps_types.hpp"      // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/region.hpp"

namespace ps::compute {

/**
 * @brief Runtime controls for tiled node execution.
 *
 * TiledExecutionConfig scopes tile size, halo policy, output extent, optional
 * dirty Region/ROI clipping, and metadata overrides for one node invocation.
 *
 * @note The config is borrowed by NodeExecutor only for the duration of the
 * call. on_tile is invoked synchronously before each tile callback.
 */
struct TiledExecutionConfig {
  /** @brief Nominal square tile size in output pixels. */
  int tile_size = 256;

  /** @brief Default halo expansion used by operators that need neighbor pixels.
   */
  int halo_size = 16;

  /**
   * @brief Optional zero-based storage ROI limiting recomputed output pixels.
   * @note NodeExecutor adds the frozen plan data-window origin only when
   * issuing the logical Host grant; callbacks keep this storage projection.
   */
  std::optional<PixelRect> output_roi;

  /**
   * @brief Exact normalized logical work selection for Region-aware core work.
   * @note Current tiled callbacks continue to consume output_roi as a derived
   *       zero-based storage projection. Generic monolithic callbacks retain
   * their existing complete-output behavior when no matching core bridge
   * exists.
   */
  std::optional<RegionSet> output_region;

  /** @brief Optional full output size when output_buffer is not the size
   * source. */
  std::optional<PixelSize> output_size;

  /** @brief Optional halo override; zero disables implicit halo expansion. */
  std::optional<int> forced_halo;

  /** @brief Optional operator metadata override for ROI access pattern
   * handling. */
  std::optional<OpMetadata> metadata;

  /**
   * @brief Optional dirty-ROI callback from the exact selected implementation.
   * @note When present this callback is authoritative for RandomAccess input
   * mapping. It must be copied from the same registry snapshot as metadata so
   * planning and execution cannot mix callbacks from different candidates.
   * A nonzero `implementation_identity` plus absence selects that exact
   * implementation's identity fallback, never an operation-level callback.
   */
  std::optional<DirtyRoiPropFunc> dirty_propagator;

  /**
   * @brief Registry identity paired with dirty_propagator and metadata.
   * @note Zero is permitted only for legacy callers that provide no exact
   * callback. Core planning paths always retain a nonzero identity.
   */
  std::uint64_t implementation_identity = 0U;

  /** @brief Synchronous hook called with each output tile ROI before execution.
   */
  std::function<void(const PixelRect&)> on_tile;
};

/**
 * @brief Executes monolithic and tiled operator implementations for one node.
 *
 * NodeExecutor owns node-local execution mechanics: Region-aware core
 * monolithic dispatch, input normalization for
 * tiled image_mixing, Host output-plan binding, checked tile grants, tile ROI
 * clipping, input ROI mapping, and exception wrapping. Higher-level compute
 * services still own
 * dependency resolution, cache policy, scheduling, and commit decisions.
 *
 * @note Tiled input compatibility views are read-only InputTile objects, while
 * output views borrow revocable Host grants. The executor never casts away
 * constness from upstream Values or stores compatibility projections.
 */
class NodeExecutor {
 public:
  /**
   * @brief Executes a selected operator variant and returns the node output.
   *
   * @param graph Graph used for random-access ROI propagation.
   * @param node Node whose runtime parameters and identity drive execution.
   * @param op Selected monolithic or tiled operator implementation.
   * @param inputs Resolved upstream image outputs in graph input order.
   * @param config Optional tiled execution controls.
   * @return NodeOutput produced by the operator.
   * @throws std::bad_alloc if input normalization, output allocation, or the
   *         selected operation exhausts memory.
   * @throws GraphError for other dependency, parameter, or compute failures.
   * @note Region-aware core monolithic operators receive output_region when
   * present; other monolithic operators retain complete-output behavior. Tiled
   * operators receive normalized input views when image_mixing requires
   * resize/crop/channel conversion.
   */
  static NodeOutput execute(GraphModel& graph, Node& node,
                            const OpRegistry::OpVariant& op,
                            const std::vector<const NodeOutput*>& inputs,
                            const TiledExecutionConfig& config = {});

  /**
   * @brief Freezes the sole internal Host plan for one tiled image output.
   *
   * @param inputs Canonical named-Value inputs used only for channel/type
   * inference; disconnected slots may be null.
   * @param output_size Positive output extent fixed by the task plan.
   * @return Complete immutable zero-origin, interleaved, 64-byte-aligned plan.
   * @throws std::invalid_argument for missing/unsupported image facts or
   * invalid extents.
   * @throws std::overflow_error when layout or envelope arithmetic is
   * unrepresentable.
   * @throws std::bad_alloc when plan metadata storage cannot allocate.
   * @note This is the unique DI-2 internal output-plan derivation used by
   * full, dirty HP, and RT tiled execution. It allocates no payload, mints no
   * identity or revision, and enters no producer callback.
   */
  static DenseImageOutputPlan freeze_tiled_output_plan(
      const std::vector<const NodeOutput*>& inputs,
      const PixelSize& output_size);

  /**
   * @brief Allocates the frozen Host binding for one tiled output.
   *
   * @param inputs Canonical named-Value inputs used only for channel/type
   * inference; disconnected slots may be null.
   * @param output_size Positive output extent fixed by the task plan.
   * @return Open aligned binding with one immutable interleaved image plan.
   * @throws std::invalid_argument for missing/unsupported image facts or
   * invalid extents.
   * @throws std::overflow_error when plan arithmetic is unrepresentable.
   * @throws std::bad_alloc when plan or aligned allocation storage fails.
   * @note No callback is entered and no mutable address is exposed. The caller
   * must cancel or seal the returned binding exactly once.
   */
  static HostOutputBinding allocate_tiled_output_binding(
      const std::vector<const NodeOutput*>& inputs,
      const PixelSize& output_size);

  /**
   * @brief Executes a tiled operator into an existing Host binding.
   *
   * @param graph Graph used for random-access ROI propagation.
   * @param node Node whose tiled operator is being executed.
   * @param tiled_op Selected tiled operator implementation.
   * @param inputs Resolved upstream image outputs in graph input order.
   * @param output_binding Open binding whose immutable plan fixes destination
   * allocation, layout, extent, and identity.
   * @param config Optional tiled execution controls.
   * @return Nothing.
   * @throws std::bad_alloc if normalization, allocation, or tile execution
   *         exhausts memory.
   * @throws GraphError when required inputs are missing or tile execution
   *         otherwise fails.
   * @note Used by task, dirty HP, and RT paths that already own the destination
   * binding. The method retires each issued tile grant but never seals;
   * callers seal only at their atomic publication boundary.
   */
  static void execute_tiled_into_binding(
      GraphModel& graph, Node& node, const TileOpFunc& tiled_op,
      const std::vector<const NodeOutput*>& inputs,
      HostOutputBinding& output_binding,
      const TiledExecutionConfig& config = {});

  /**
   * @brief Maps one output tile ROI to the required input ROI.
   *
   * @param graph Graph used by random-access dirty propagators.
   * @param node Node whose operator metadata defines access behavior.
   * @param output_roi Output tile ROI being computed.
   * @param input_buffer Input buffer whose bounds clip the mapped ROI.
   * @param config Tiled execution metadata, exact implementation callback,
   * and halo overrides.
   * @param known_input_extents Request-local image-input extents by destination
   *        index; empty means resolve committed cache hints.
   * @param known_effective_parameters Optional caller-resolved parameter
   *        snapshot. Null uses execution-local runtime/static parameters.
   * @param available_inputs Optional destination-indexed inputs from the
   *        current execution batch. Null selects planning snapshots.
   * @return Input ROI clipped to input_buffer bounds.
   * @throws GraphError or propagator exceptions from random-access ROI mapping.
   * @note Empty propagated ROIs fall back to clipped output_roi, matching
   * legacy tiled execution behavior.
   */
  static PixelRect input_roi_for_tile(
      GraphModel& graph, const Node& node, const PixelRect& output_roi,
      const ImageBuffer& input_buffer, const TiledExecutionConfig& config,
      const std::vector<PixelSize>& known_input_extents = {},
      const plugin::ParameterMap* known_effective_parameters = nullptr,
      const std::vector<const NodeOutput*>* available_inputs = nullptr);

  /**
   * @brief Invokes the tiled operator for a prepared tile task.
   *
   * @param task Prepared node/output/input tile views.
   * @param tiled_op Tiled operator implementation to call.
   * @throws Any exception thrown by the tiled operator.
   * @note The task does not own buffers; all referenced storage must outlive
   * the callback.
   */
  static void execute_tile_task(const TileTask& task,
                                const TileOpFunc& tiled_op);
};

}  // namespace ps::compute
