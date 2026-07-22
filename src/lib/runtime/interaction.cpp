#include "runtime/interaction.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/host/host.hpp"

namespace ps {

namespace {

/** @brief Local shorthand for lists returned by discovery commands. */
using StringList = std::vector<std::string>;

}  // namespace

/** @copydoc InteractionService::cmd_policy_available_types */
StringList InteractionService::cmd_policy_available_types() const {
  return kernel_.policy_available_types();
}

/** @copydoc InteractionService::cmd_policy_description */
std::string InteractionService::cmd_policy_description(
    const std::string& type_name) const {
  return kernel_.policy_description(type_name);
}

/** @copydoc InteractionService::cmd_policy_scan */
std::size_t InteractionService::cmd_policy_scan(
    const std::vector<std::string>& dirs) {
  return kernel_.policy_scan(dirs);
}

/** @copydoc InteractionService::cmd_policy_load */
void InteractionService::cmd_policy_load(const std::string& path) {
  kernel_.policy_load(path);
}

/** @copydoc InteractionService::cmd_policy_loaded_plugins */
std::vector<std::string> InteractionService::cmd_policy_loaded_plugins() const {
  return kernel_.policy_loaded_plugins();
}

/** @copydoc InteractionService::cmd_configure_policy_defaults */
void InteractionService::cmd_configure_policy_defaults(
    const HostPolicyConfig& config) {
  kernel_.configure_policy_defaults(config);
}

/** @copydoc InteractionService::cmd_policy_info */
PolicyInfoSnapshot InteractionService::cmd_policy_info(
    PolicyClass policy_class) const {
  return kernel_.policy_info(policy_class);
}

/** @copydoc InteractionService::cmd_replace_policy */
void InteractionService::cmd_replace_policy(PolicyClass policy_class,
                                            const std::string& type) {
  kernel_.replace_policy(policy_class, type);
}

/** @copydoc InteractionService::cmd_execution_available_types */
StringList InteractionService::cmd_execution_available_types() const {
  return kernel_.execution_available_types();
}

/** @copydoc InteractionService::cmd_execution_description */
std::string InteractionService::cmd_execution_description(
    const std::string& type_name) const {
  return kernel_.execution_description(type_name);
}

/** @copydoc InteractionService::cmd_configure_execution_defaults */
void InteractionService::cmd_configure_execution_defaults(
    const Kernel::ExecutionConfig& config) {
  kernel_.set_execution_config(config);
}

/** @copydoc InteractionService::cmd_execution_info */
std::optional<std::pair<std::string, std::string>>
InteractionService::cmd_execution_info(const std::string& graph,
                                       ComputeIntent intent) {
  return kernel_.get_execution_info(graph, intent);
}

/** @copydoc InteractionService::cmd_replace_execution */
bool InteractionService::cmd_replace_execution(const std::string& graph,
                                               ComputeIntent intent,
                                               const std::string& type) {
  return kernel_.replace_execution(graph, intent, type);
}

}  // namespace ps
