#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"

namespace {

/**
 * @brief Parses one policy-class command token.
 * @param token User token.
 * @param[out] policy_class Parsed class on success.
 * @return True for `interactive` or `throughput`.
 * @throws Nothing.
 */
bool parse_policy_class(const std::string& token,
                        ps::PolicyClass* policy_class) noexcept {
  if (token == "interactive") {
    *policy_class = ps::PolicyClass::Interactive;
    return true;
  }
  if (token == "throughput") {
    *policy_class = ps::PolicyClass::Throughput;
    return true;
  }
  return false;
}

/**
 * @brief Returns the stable display label for one policy class.
 * @param policy_class Public class value.
 * @return Static lowercase class label.
 * @throws Nothing.
 */
const char* policy_class_label(ps::PolicyClass policy_class) noexcept {
  return policy_class == ps::PolicyClass::Interactive ? "interactive"
                                                      : "throughput";
}

/**
 * @brief Prints one policy binding snapshot or its exact Host failure.
 * @param host Borrowed Host.
 * @param policy_class Class to inspect.
 * @return Nothing.
 * @throws std::bad_alloc when Host or output storage exhausts memory.
 */
void print_policy_info(ps::Host& host, ps::PolicyClass policy_class) {
  const auto result = host.policy_info(policy_class);
  if (!result.status.ok) {
    std::cout << "Error: " << result.status.name << ": "
              << result.status.message << "\n";
    return;
  }
  std::cout << policy_class_label(policy_class) << ": "
            << result.value.policy_type << " (generation "
            << result.value.binding_generation << ")";
  if (result.value.fault) {
    std::cout << " [fault: " << result.value.fault->message << "]";
  }
  std::cout << "\n";
}

/**
 * @brief Prints one failed Host status with a stable CLI prefix.
 * @param status Failed or successful Host status.
 * @return Nothing.
 * @throws std::bad_alloc only through output formatting internals.
 */
void print_status_error(const ps::OperationStatus& status) {
  std::cout << "Error: " << status.name;
  if (!status.message.empty()) {
    std::cout << ": " << status.message;
  }
  std::cout << "\n";
}

}  // namespace

/** @copydoc handle_policy */
bool handle_policy(std::istringstream& iss, ps::Host& host,
                   std::string& current_graph, bool& modified,
                   CliConfig& config) {
  (void)current_graph;
  (void)modified;
  std::string subcommand;
  iss >> subcommand;
  if (subcommand.empty() || subcommand == "help") {
    print_help_policy(config);
    return true;
  }
  if (subcommand == "list") {
    const auto types = host.policy_available_types();
    if (!types.status.ok) {
      print_status_error(types.status);
      return true;
    }
    std::cout << "Available policy types:\n";
    for (const std::string& type : types.value) {
      const auto description = host.policy_description(type);
      std::cout << "  " << type;
      if (description.status.ok && !description.value.empty()) {
        std::cout << " - " << description.value;
      }
      std::cout << "\n";
    }
    return true;
  }
  if (subcommand == "get") {
    std::string target;
    iss >> target;
    if (target.empty() || target == "all") {
      print_policy_info(host, ps::PolicyClass::Interactive);
      print_policy_info(host, ps::PolicyClass::Throughput);
      return true;
    }
    ps::PolicyClass policy_class;
    if (!parse_policy_class(target, &policy_class)) {
      std::cout << "Error: unknown policy class '" << target << "'.\n";
      return true;
    }
    print_policy_info(host, policy_class);
    return true;
  }
  if (subcommand == "set") {
    std::string target;
    std::string first_type;
    std::string second_type;
    iss >> target >> first_type >> second_type;
    if (target == "all") {
      if (first_type.empty() || second_type.empty()) {
        std::cout << "Usage: policy set all <interactive-type> "
                     "<throughput-type>\n";
        return true;
      }
      ps::HostPolicyConfig defaults;
      defaults.interactive_type = first_type;
      defaults.throughput_type = second_type;
      const ps::VoidResult result = host.configure_policy_defaults(defaults);
      if (!result.status.ok) {
        print_status_error(result.status);
      } else {
        std::cout << "Updated interactive and throughput policy bindings.\n";
      }
      return true;
    }
    ps::PolicyClass policy_class;
    if (first_type.empty() || !second_type.empty() ||
        !parse_policy_class(target, &policy_class)) {
      std::cout << "Usage: policy set <interactive|throughput> <type>\n";
      return true;
    }
    const ps::VoidResult result = host.replace_policy(policy_class, first_type);
    if (!result.status.ok) {
      print_status_error(result.status);
    } else {
      std::cout << "Updated " << policy_class_label(policy_class)
                << " policy to '" << first_type << "'.\n";
    }
    return true;
  }
  if (subcommand == "scan") {
    std::string directory;
    iss >> directory;
    std::vector<std::string> directories =
        directory.empty() ? config.policy_dirs
                          : std::vector<std::string>{directory};
    const auto result = host.policy_scan(directories);
    if (!result.status.ok) {
      print_status_error(result.status);
    } else {
      std::cout << "Loaded " << result.value << " policy plugin(s).\n";
    }
    return true;
  }
  if (subcommand == "load") {
    std::string path;
    iss >> path;
    if (path.empty()) {
      std::cout << "Usage: policy load <path>\n";
      return true;
    }
    const ps::VoidResult result = host.policy_load(path);
    if (!result.status.ok) {
      print_status_error(result.status);
    } else {
      std::cout << "Loaded policy plugin '" << path << "'.\n";
    }
    return true;
  }
  if (subcommand == "plugins") {
    const auto result = host.policy_loaded_plugins();
    if (!result.status.ok) {
      print_status_error(result.status);
      return true;
    }
    if (result.value.empty()) {
      std::cout << "No policy plugins loaded.\n";
      return true;
    }
    std::cout << "Loaded policy plugins:\n";
    for (const std::string& plugin : result.value) {
      std::cout << "  " << plugin << "\n";
    }
    return true;
  }
  std::cout << "Unknown policy subcommand: " << subcommand << "\n";
  std::cout << "Use 'policy help' for usage information.\n";
  return true;
}

/** @copydoc print_help_policy */
void print_help_policy(const CliConfig& /*config*/) {
  print_help_from_file("help_policy.txt");
}
