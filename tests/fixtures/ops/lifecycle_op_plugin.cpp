#include <vector>

#include "plugin_api.hpp"  // NOLINT(build/include_subdir)

namespace {

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

}  // namespace

/**
 * @brief Registers the lifecycle fixture operation with Photospider.
 *
 * @param registrar Host-provided operation registration API.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
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
                                       ps::MonolithicOpFunc(lifecycle_test_op),
                                       metadata);
}
