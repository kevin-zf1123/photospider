#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "plugin_api.hpp"  // NOLINT(build/include_subdir)

namespace {

constexpr const char* kTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";
constexpr const char* kThrowEnvironment = "PS_LIFECYCLE_PLUGIN_REGISTRAR_THROW";

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
 * @return Empty `NodeOutput` with a diagnostic device marker.
 * @throws Nothing.
 * @note The function lives inside the dynamic library, so host code must remove
 * the registry entry before releasing the library handle.
 */
ps::NodeOutput lifecycle_test_op(
    const ps::Node& node, const std::vector<const ps::NodeOutput*>& inputs) {
  (void)node;
  (void)inputs;

  ps::NodeOutput output;
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

}  // namespace

/**
 * @brief Registers the lifecycle fixture operation with Photospider.
 *
 * @param registrar Host-provided operation registration API.
 * @return Nothing.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws std::runtime_error when the fixture's ordinary-failure mode is set.
 * @throws std::bad_alloc if host registry storage allocation fails.
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

  const char* throw_mode = std::getenv(kThrowEnvironment);
  if (throw_mode && std::string(throw_mode) == "bad_alloc") {
    append_lifecycle_trace("registrar_throw_bad_alloc");
    throw std::bad_alloc();
  }
  if (throw_mode && std::string(throw_mode) == "runtime_error") {
    append_lifecycle_trace("registrar_throw_runtime_error");
    throw std::runtime_error("lifecycle registrar runtime failure");
  }
  append_lifecycle_trace("registrar_return");
}
