#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utility>

#include "photospider/plugin/plugin_api.hpp"

namespace {

constexpr const char* kTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";

/**
 * @brief Appends one replacement-plugin lifecycle event to the selected trace.
 *
 * @param event Stable event label owned by the caller.
 * @return Nothing.
 * @throws Nothing; missing environment and file I/O failures are ignored.
 * @note This duplicates the tiny probe locally so no cross-plugin test helper
 *       symbol becomes part of the operation plugin ABI.
 */
void append_lifecycle_trace(const char* event) noexcept {
  const char* path = std::getenv(kTraceEnvironment);
  if (!path || path[0] == '\0') {
    return;
  }
  std::FILE* output = std::fopen(path, "a");
  if (!output) {
    return;
  }
  (void)std::fputs(event, output);
  (void)std::fputc('\n', output);
  (void)std::fclose(output);
}

/**
 * @brief Shared state whose final destructor proves replacement callback order.
 *
 * @throws Nothing directly.
 * @note The registered callback captures one shared instance. Its destruction
 *       must precede replacement-library unmapping during explicit unload.
 */
struct OverrideCallbackLifetimeProbe {
  /**
   * @brief Records final replacement callback-state destruction.
   *
   * @throws Nothing; trace I/O failures are suppressed.
   * @note The event executes from plugin code and therefore requires a live
   *       replacement-library lease.
   */
  ~OverrideCallbackLifetimeProbe() {
    append_lifecycle_trace("override_callback_destroy");
  }
};

/**
 * @brief Static probe corresponding to real replacement-library unmapping.
 *
 * @throws Nothing directly.
 * @note Its event must follow `override_callback_destroy` in unload traces.
 */
struct OverrideLibraryLifetimeProbe {
  /**
   * @brief Records actual teardown of the replacement dynamic library.
   *
   * @throws Nothing; trace I/O failures are suppressed.
   * @note Static destruction is driven by the final host-side handle release.
   */
  ~OverrideLibraryLifetimeProbe() {
    append_lifecycle_trace("override_library_unload");
  }
};

/** @brief Per-load static whose destructor marks replacement library release.
 */
OverrideLibraryLifetimeProbe override_library_lifetime_probe;

/**
 * @brief Replacement operation used to verify plugin key overwrite handling.
 *
 * @param node Borrowed public node view; unused by this fixture.
 * @param inputs Borrowed public upstream views; unused by this fixture.
 * @return Empty output with a marker that distinguishes the replacement
 * implementation from the original lifecycle plugin.
 * @throws std::bad_alloc if output diagnostic string storage cannot allocate.
 * @note The callback lives in the replacement plugin library and must be
 * removed or replaced before that library is unloaded.
 */
ps::plugin::OperationOutput override_lifecycle_test_op(
    const ps::plugin::NodeView& node,
    ps::plugin::ArrayView<ps::plugin::OperationInputView> inputs) {
  (void)node;
  (void)inputs;

  ps::plugin::OperationOutput output;
  output.spatial.absolute_roi = ps::PixelRect{0, 0, 22, 9};
  output.debug.compute_device = "PLUGIN_OVERRIDE_TEST";
  return output;
}

/**
 * @brief Creates a replacement callback retaining plugin-owned trace state.
 *
 * @return Monolithic callback whose final captured state destruction is traced.
 * @throws std::bad_alloc if shared state or callback storage cannot allocate.
 * @note The callback body delegates to `override_lifecycle_test_op`; the probe
 *       exists only to audit callback-before-library retirement order.
 */
ps::plugin::MonolithicOperation make_override_lifecycle_test_op() {
  auto probe = std::make_shared<OverrideCallbackLifetimeProbe>();
  return [probe = std::move(probe)](
             const ps::plugin::NodeView& node,
             ps::plugin::ArrayView<ps::plugin::OperationInputView> inputs) {
    (void)probe;
    return override_lifecycle_test_op(node, inputs);
  };
}

/**
 * @brief Propagates replacement-fixture dirty demand unchanged.
 *
 * @param context Borrowed public dirty ROI snapshot.
 * @return The unchanged dirty region.
 * @throws Nothing.
 * @note Registering this slot makes the replacement plugin own the complete
 *       active lifecycle operation rather than leaving mixed plugin sources.
 */
ps::PixelRect override_dirty_propagator(const ps::plugin::RoiContext& context) {
  return context.requested_roi;
}

/**
 * @brief Propagates replacement-fixture affected regions unchanged.
 *
 * @param context Borrowed public forward ROI and active-edge snapshot.
 * @return The unchanged affected region.
 * @throws Nothing.
 * @note The host wraps this callback with the replacement library lease.
 */
ps::PixelRect override_forward_propagator(
    const ps::plugin::RoiContext& context) {
  return context.requested_roi;
}

/**
 * @brief Builds the replacement lifecycle plugin's dependency table.
 * @param context Borrowed public topology and output-extent snapshot.
 * @return One-cell table routed to input zero with x marker two.
 * @throws std::bad_alloc if cell vector growth cannot allocate.
 * @note The distinct marker proves cache identity follows callback ownership.
 */
ps::plugin::DependencyLutSnapshot override_dependency_builder(
    const ps::plugin::RoiContext& context) {
  ps::plugin::DependencyLutSnapshot result;
  result.upstream_input_index = 0;
  result.cell_size = context.output_extent;
  result.output_extent = context.output_extent;
  result.cell_to_upstream_roi.push_back(ps::PixelRect{2, 0, 1, 1});
  return result;
}

}  // namespace

/**
 * @brief Re-registers the lifecycle fixture key with a replacement callback.
 *
 * @param registrar Host-provided operation registration API.
 * @return Nothing.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws std::bad_alloc if callback state/wrappers, copied operation names,
 *         active capture bookkeeping, or registry storage cannot allocate.
 * @note This intentionally touches an existing key through the host registrar
 * so plugin unload can prove overwritten callbacks are tracked and the
 * previous implementation is restored.
 */
extern "C" PHOTOSPIDER_OPERATION_PLUGIN_EXPORT void register_photospider_ops_v2(
    ps::plugin::OperationPluginRegistrar* registrar) {
  if (!registrar) {
    throw std::invalid_argument(
        "register_photospider_ops_v2 requires registrar");
  }
  ps::plugin::OperationMetadata metadata;
  metadata.cost_score = 2;
  registrar->register_op_hp_monolithic(
      "plugin_lifecycle", "op", make_override_lifecycle_test_op(), metadata);
  registrar->register_dirty_propagator("plugin_lifecycle", "op",
                                       override_dirty_propagator);
  registrar->register_forward_propagator("plugin_lifecycle", "op",
                                         override_forward_propagator);
  registrar->register_dependency_builder("plugin_lifecycle", "op",
                                         override_dependency_builder);
  append_lifecycle_trace("override_registrar_return");
}
