#include <vector>

#include "plugin_api.hpp"  // NOLINT(build/include_subdir)

namespace {

/**
 * @brief Replacement operation used to verify plugin key overwrite handling.
 *
 * @param node Borrowed graph node; unused by this deterministic lifecycle
 * fixture.
 * @param inputs Borrowed upstream outputs; unused by this deterministic
 * lifecycle fixture.
 * @return Empty output with a marker that distinguishes the replacement
 * implementation from the original lifecycle plugin.
 * @throws Nothing.
 * @note The callback lives in the replacement plugin library and must be
 * removed or replaced before that library is unloaded.
 */
ps::NodeOutput override_lifecycle_test_op(
    const ps::Node& node, const std::vector<const ps::NodeOutput*>& inputs) {
  (void)node;
  (void)inputs;

  ps::NodeOutput output;
  output.debug.compute_device = "PLUGIN_OVERRIDE_TEST";
  return output;
}

}  // namespace

/**
 * @brief Re-registers the lifecycle fixture key with a replacement callback.
 *
 * @param registrar Host-provided operation registration API.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws std::bad_alloc if host registry storage allocation fails.
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
      "plugin_lifecycle", "op",
      ps::MonolithicOpFunc(override_lifecycle_test_op), metadata);
}
