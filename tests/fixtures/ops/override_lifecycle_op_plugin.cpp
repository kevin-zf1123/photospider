#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "plugin_api.hpp"  // NOLINT(build/include_subdir)

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
 * @param node Borrowed graph node; unused by this deterministic lifecycle
 * fixture.
 * @param inputs Borrowed upstream outputs; unused by this deterministic
 * lifecycle fixture.
 * @return Empty output with a marker that distinguishes the replacement
 * implementation from the original lifecycle plugin.
 * @throws std::bad_alloc if output diagnostic string storage cannot allocate.
 * @note The callback lives in the replacement plugin library and must be
 * removed or replaced before that library is unloaded.
 */
ps::NodeOutput override_lifecycle_test_op(
    const ps::Node& node, const std::vector<const ps::NodeOutput*>& inputs) {
  (void)node;
  (void)inputs;

  ps::NodeOutput output;
  output.space.absolute_roi = cv::Rect(0, 0, 22, 9);
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
ps::MonolithicOpFunc make_override_lifecycle_test_op() {
  auto probe = std::make_shared<OverrideCallbackLifetimeProbe>();
  return [probe = std::move(probe)](
             const ps::Node& node,
             const std::vector<const ps::NodeOutput*>& inputs) {
    (void)probe;
    return override_lifecycle_test_op(node, inputs);
  };
}

/**
 * @brief Propagates replacement-fixture dirty demand unchanged.
 *
 * @param node Borrowed operation node; unused by the pointwise fixture.
 * @param roi Downstream dirty region.
 * @param graph Borrowed graph model; unused by identity propagation.
 * @return The unchanged dirty region.
 * @throws Nothing.
 * @note Registering this slot makes the replacement plugin own the complete
 *       active lifecycle operation rather than leaving mixed plugin sources.
 */
cv::Rect override_dirty_propagator(const ps::Node& node, const cv::Rect& roi,
                                   const ps::GraphModel& graph) {
  (void)node;
  (void)graph;
  return roi;
}

/**
 * @brief Propagates replacement-fixture affected regions unchanged.
 *
 * @param node Borrowed operation node; unused by the pointwise fixture.
 * @param roi Upstream affected region.
 * @param graph Borrowed graph model; unused by identity propagation.
 * @param parent_size Parent extent; unused by identity propagation.
 * @param child_size Child extent; unused by identity propagation.
 * @return The unchanged affected region.
 * @throws Nothing.
 * @note The host wraps this callback with the replacement library lease.
 */
cv::Rect override_forward_propagator(const ps::Node& node, const cv::Rect& roi,
                                     const ps::GraphModel& graph,
                                     const cv::Size& parent_size,
                                     const cv::Size& child_size) {
  (void)node;
  (void)graph;
  (void)parent_size;
  (void)child_size;
  return roi;
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
extern "C" PLUGIN_API void register_photospider_ops_v1(
    ps::OperationPluginRegistrar* registrar) {
  if (!registrar) {
    throw std::invalid_argument(
        "register_photospider_ops_v1 requires registrar");
  }
  ps::OpMetadata metadata;
  metadata.cost_score = 2;
  registrar->register_op_hp_monolithic(
      "plugin_lifecycle", "op", make_override_lifecycle_test_op(), metadata);
  registrar->register_dirty_propagator("plugin_lifecycle", "op",
                                       override_dirty_propagator);
  registrar->register_forward_propagator("plugin_lifecycle", "op",
                                         override_forward_propagator);
  append_lifecycle_trace("override_registrar_return");
}
