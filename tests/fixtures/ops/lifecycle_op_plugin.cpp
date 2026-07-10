#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "plugin_api.hpp"  // NOLINT(build/include_subdir)

namespace {

constexpr const char* kTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";
constexpr const char* kThrowEnvironment = "PS_LIFECYCLE_PLUGIN_REGISTRAR_THROW";
constexpr const char* kCallbackReleaseEnvironment =
    "PS_LIFECYCLE_PLUGIN_CALLBACK_RELEASE_FILE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kRegistrarReleaseEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTRAR_RELEASE_FILE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kResultProbeEnvironment =
    "PS_LIFECYCLE_PLUGIN_RESULT_PROBE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kDeviceRegistrarEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTER_DEVICES";  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Appends one real lifecycle event to the test-selected trace file.
 *
 * @param event Stable event label owned by the caller.
 * @return Nothing.
 * @throws Nothing; file/open/write failures are intentionally ignored in this
 * dynamic-library destructor probe.
 * @note No trace is emitted unless the test process sets
 * `PS_LIFECYCLE_PLUGIN_TRACE`; production-like fixture loads remain unchanged.
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
 * @brief Waits until the test process creates one release file.
 *
 * @param release_file Borrowed non-empty path selected through an environment
 *        variable.
 * @return Nothing.
 * @throws Nothing; file-open failures are treated as a not-yet-released state.
 * @note The fixture uses this bounded-by-test handshake for deterministic
 *       callback and registrar overlap. Production plugin behavior leaves the
 *       corresponding environment variables unset.
 */
void wait_for_release_file(const char* release_file) noexcept {
  while (true) {
    std::FILE* release = std::fopen(release_file, "r");
    if (release) {
      (void)std::fclose(release);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

/**
 * @brief Shared callback owner whose final destruction runs inside the plugin.
 *
 * @note The registered lambda captures one shared instance. Its trace proves
 * all host-side callback copies die before the library-unload destructor runs.
 */
struct CallbackLifetimeProbe {
  /**
   * @brief Records the final callable-state destruction without throwing.
   * @throws Nothing; trace I/O failures are suppressed by the helper.
   * @note This destructor must execute while plugin code is still mapped.
   */
  ~CallbackLifetimeProbe() { append_lifecycle_trace("callback_destroy"); }
};

/**
 * @brief Plugin-defined state stored inside a returned NodeOutput image
 * context.
 *
 * @throws Nothing directly.
 * @note The shared-pointer control block and this destructor are instantiated
 * in the dynamic plugin. The host must therefore keep the library mapped until
 * the final copied or moved result payload is destroyed.
 */
struct ResultLifetimeProbe {
  /**
   * @brief Records destruction of the final plugin-instantiated result payload.
   *
   * @throws Nothing; trace I/O failures are suppressed by the helper.
   * @note `result_payload_destroy` must precede `library_unload` after explicit
   *       global unload and any NodeOutput assignment path.
   */
  ~ResultLifetimeProbe() { append_lifecycle_trace("result_payload_destroy"); }
};

/**
 * @brief Dynamic-library static lifetime probe for real `dlclose`/FreeLibrary.
 *
 * @note The event must follow `callback_destroy` on every failed transaction or
 * explicit unload; otherwise host state may still reference unmapped code.
 */
struct LibraryLifetimeProbe {
  /**
   * @brief Records actual library teardown without surfacing I/O failures.
   * @throws Nothing; trace I/O failures are suppressed by the helper.
   * @note Static destruction corresponds to real library unmapping.
   */
  ~LibraryLifetimeProbe() { append_lifecycle_trace("library_unload"); }
};

/** @brief Process-per-load static whose destructor proves handle release. */
LibraryLifetimeProbe library_lifetime_probe;

/**
 * @brief Minimal operation used to verify dynamic plugin lifetime handling.
 *
 * The callback is registered through the multi-implementation operation API so
 * unload tests can prove `OpRegistry::unregister_key` removes `impl_table_`
 * state as well as legacy table entries.
 *
 * @param node Borrowed graph node; unused because the test only verifies
 * registration and unload behavior.
 * @param inputs Borrowed upstream outputs; unused by this deterministic test
 * fixture.
 * @return Data-bearing `NodeOutput` with a diagnostic device marker.
 * @throws std::bad_alloc if result-probe allocation, shared ownership, output
 *         map/YAML insertion, or diagnostic string storage cannot allocate.
 * @note The function lives inside the dynamic library, so host code must keep
 * the library mapped through invocation completion. Tests may set
 * `PS_LIFECYCLE_PLUGIN_CALLBACK_RELEASE_FILE` to hold a callback in flight
 * until that file appears, allowing deterministic explicit-unload coverage.
 */
ps::NodeOutput lifecycle_test_op(
    const ps::Node& node, const std::vector<const ps::NodeOutput*>& inputs) {
  (void)node;
  (void)inputs;

  const char* release_file = std::getenv(kCallbackReleaseEnvironment);
  if (release_file && release_file[0] != '\0') {
    append_lifecycle_trace("callback_enter");
    wait_for_release_file(release_file);
    append_lifecycle_trace("callback_return");
  }

  ps::NodeOutput output;
  const char* result_probe = std::getenv(kResultProbeEnvironment);
  if (result_probe && result_probe[0] != '\0') {
    output.image_buffer.context =
        std::shared_ptr<void>(new ResultLifetimeProbe());
  }
  output.data["lifecycle_marker"] = "PLUGIN_LIFECYCLE_TEST";
  output.space.absolute_roi = cv::Rect(0, 0, 11, 7);
  output.debug.compute_device = "PLUGIN_LIFECYCLE_TEST";
  return output;
}

/**
 * @brief Creates a lifecycle operation retaining one plugin-owned probe.
 *
 * @return Monolithic callback whose final state destruction is traced.
 * @throws std::bad_alloc if shared state or `std::function` allocation fails.
 * @note The callback body delegates to `lifecycle_test_op`; only ownership is
 * extended so tests can audit callback destruction before library unload.
 */
ps::MonolithicOpFunc make_lifecycle_test_op() {
  auto probe = std::make_shared<CallbackLifetimeProbe>();
  return [probe = std::move(probe)](
             const ps::Node& node,
             const std::vector<const ps::NodeOutput*>& inputs) {
    (void)probe;
    return lifecycle_test_op(node, inputs);
  };
}

/**
 * @brief Produces the lifecycle fixture's device-specific monolithic marker.
 *
 * @param node Borrowed graph node; unused by this deterministic fixture.
 * @param inputs Borrowed upstream outputs; unused by this fixture.
 * @return Output identifying the plugin-owned device implementation.
 * @throws std::bad_alloc if diagnostic string storage cannot allocate.
 * @note The callback intentionally owns no trace probe. Its host wrapper still
 *       retains the real plugin library, while the single HP callback probe
 *       keeps existing lifecycle traces unambiguous.
 */
ps::NodeOutput lifecycle_device_monolithic(
    const ps::Node& node, const std::vector<const ps::NodeOutput*>& inputs) {
  (void)node;
  (void)inputs;
  ps::NodeOutput output;
  output.debug.compute_device = "PLUGIN_DEVICE_MONOLITHIC";
  return output;
}

/**
 * @brief Executes the lifecycle fixture's device-specific tiled no-op.
 *
 * @param node Borrowed graph node; unused by this deterministic fixture.
 * @param output Borrowed writable tile; intentionally left unchanged.
 * @param inputs Borrowed input tiles; unused by this fixture.
 * @return Nothing.
 * @throws Nothing.
 * @note Registration through the real device registrar exercises a tiled
 *       plugin callback and its parallel ownership token without synthetic
 *       test-only registry writes.
 */
void lifecycle_device_tiled(const ps::Node& node, const ps::OutputTile& output,
                            const std::vector<ps::InputTile>& inputs) {
  (void)node;
  (void)output;
  (void)inputs;
}

/**
 * @brief Propagates lifecycle-fixture dirty demand unchanged.
 *
 * @param node Borrowed operation node; unused by the pointwise fixture.
 * @param roi Downstream dirty region to propagate upstream.
 * @param graph Borrowed graph model; unused by the pointwise fixture.
 * @return The unchanged dirty region.
 * @throws Nothing.
 * @note This explicit plugin-owned slot lets mixed-ownership unload prove that
 *       a later direct callback replacement does not leave plugin code active.
 */
cv::Rect lifecycle_dirty_propagator(const ps::Node& node, const cv::Rect& roi,
                                    const ps::GraphModel& graph) {
  (void)node;
  (void)graph;
  return roi;
}

/**
 * @brief Propagates lifecycle-fixture affected regions unchanged.
 *
 * @param node Borrowed operation node; unused by the pointwise fixture.
 * @param roi Upstream changed region to propagate downstream.
 * @param graph Borrowed graph model; unused by the pointwise fixture.
 * @param parent_size Parent extent; unused by identity propagation.
 * @param child_size Child extent; unused by identity propagation.
 * @return The unchanged affected region.
 * @throws Nothing.
 * @note The callback is registered through the host registrar and therefore
 *       carries the same plugin-library lease as the operation callback.
 */
cv::Rect lifecycle_forward_propagator(const ps::Node& node, const cv::Rect& roi,
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
 * @brief Registers the lifecycle fixture operation with Photospider.
 *
 * @param registrar Host-provided operation registration API.
 * @return Nothing.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws std::runtime_error when the fixture's ordinary-failure mode is set.
 * @throws std::bad_alloc if callback state/wrappers, throw-mode strings, copied
 *         operation names, active capture bookkeeping, or registry storage
 *         cannot allocate.
 * @note The host loader discovers this versioned C symbol and records the new
 * `plugin_lifecycle:op` key for later unload without the plugin touching
 * `OpRegistry::instance()`.
 */
extern "C" PLUGIN_API void register_photospider_ops_v1(
    ps::OperationPluginRegistrar* registrar) {
  if (!registrar) {
    throw std::invalid_argument(
        "register_photospider_ops_v1 requires registrar");
  }
  ps::OpMetadata metadata;
  metadata.cost_score = 1;
  registrar->register_op_hp_monolithic("plugin_lifecycle", "op",
                                       make_lifecycle_test_op(), metadata);
  registrar->register_dirty_propagator("plugin_lifecycle", "op",
                                       lifecycle_dirty_propagator);
  registrar->register_forward_propagator("plugin_lifecycle", "op",
                                         lifecycle_forward_propagator);
  const char* register_devices = std::getenv(kDeviceRegistrarEnvironment);
  if (register_devices && register_devices[0] != '\0') {
    ps::OpMetadata device_monolithic_metadata;
    device_monolithic_metadata.cost_score = 3;
    registrar->register_impl("plugin_lifecycle", "op", ps::Device::GPU_METAL,
                             lifecycle_device_monolithic,
                             device_monolithic_metadata);
    ps::OpMetadata device_tiled_metadata;
    device_tiled_metadata.cost_score = 4;
    device_tiled_metadata.tile_preference = ps::TileSizePreference::MICRO;
    registrar->register_impl("plugin_lifecycle", "op", ps::Device::GPU_CUDA,
                             lifecycle_device_tiled, device_tiled_metadata);
  }

  const char* throw_mode = std::getenv(kThrowEnvironment);
  if (throw_mode && std::string(throw_mode) == "bad_alloc") {
    append_lifecycle_trace("registrar_throw_bad_alloc");
    throw std::bad_alloc();
  }
  if (throw_mode && std::string(throw_mode) == "runtime_error") {
    append_lifecycle_trace("registrar_throw_runtime_error");
    throw std::runtime_error("lifecycle registrar runtime failure");
  }

  const char* registrar_release = std::getenv(kRegistrarReleaseEnvironment);
  if (registrar_release && registrar_release[0] != '\0') {
    append_lifecycle_trace("registrar_wait_enter");
    wait_for_release_file(registrar_release);
    append_lifecycle_trace("registrar_wait_exit");
  }
  append_lifecycle_trace("registrar_return");
}
