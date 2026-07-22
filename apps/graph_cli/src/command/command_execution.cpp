#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"

namespace {

/**
 * @brief Parses one execution intent token.
 * @param token User token.
 * @param[out] intent Parsed compute intent on success.
 * @return True for `hp` or `rt`.
 * @throws Nothing.
 */
bool parse_execution_intent(const std::string& token,
                            ps::ComputeIntent* intent) noexcept {
  if (token == "hp") {
    *intent = ps::ComputeIntent::GlobalHighPrecision;
    return true;
  }
  if (token == "rt") {
    *intent = ps::ComputeIntent::RealTimeUpdate;
    return true;
  }
  return false;
}

/**
 * @brief Returns the short display label for one compute intent.
 * @param intent Public compute intent.
 * @return Static `hp` or `rt` label.
 * @throws Nothing.
 */
const char* execution_intent_label(ps::ComputeIntent intent) noexcept {
  return intent == ps::ComputeIntent::GlobalHighPrecision ? "hp" : "rt";
}

/**
 * @brief Prints one execution-route snapshot or Host failure.
 * @param host Borrowed Host.
 * @param graph Active session label.
 * @param intent Intent to inspect.
 * @return Nothing.
 * @throws std::bad_alloc when Host or output storage exhausts memory.
 */
void print_execution_info(ps::Host& host, const std::string& graph,
                          ps::ComputeIntent intent) {
  const auto result = host.execution_info(ps::GraphSessionId{graph}, intent);
  if (!result.status.ok) {
    std::cout << "Error: " << result.status.name << ": "
              << result.status.message << "\n";
    return;
  }
  std::cout << execution_intent_label(intent) << ": "
            << result.value.execution_type << " - " << result.value.stats
            << "\n";
}

/**
 * @brief Prints one failed Host status with a stable CLI prefix.
 * @param status Failed or successful Host status.
 * @return Nothing.
 * @throws std::bad_alloc only through output formatting internals.
 */
void print_execution_error(const ps::OperationStatus& status) {
  std::cout << "Error: " << status.name;
  if (!status.message.empty()) {
    std::cout << ": " << status.message;
  }
  std::cout << "\n";
}

}  // namespace

/** @copydoc handle_execution */
bool handle_execution(std::istringstream& iss, ps::Host& host,
                      std::string& current_graph, bool& modified,
                      CliConfig& config) {
  (void)modified;
  std::string subcommand;
  iss >> subcommand;
  if (subcommand.empty() || subcommand == "help") {
    print_help_execution(config);
    return true;
  }
  if (subcommand == "list") {
    const auto types = host.execution_available_types();
    if (!types.status.ok) {
      print_execution_error(types.status);
      return true;
    }
    std::cout << "Available execution types:\n";
    for (const std::string& type : types.value) {
      const auto description = host.execution_description(type);
      std::cout << "  " << type;
      if (description.status.ok && !description.value.empty()) {
        std::cout << " - " << description.value;
      }
      std::cout << "\n";
    }
    return true;
  }
  if (current_graph.empty()) {
    std::cout << "Error: No graph loaded. Use 'load' first.\n";
    return true;
  }
  if (subcommand == "get") {
    std::string target;
    iss >> target;
    if (target.empty() || target == "all") {
      print_execution_info(host, current_graph,
                           ps::ComputeIntent::GlobalHighPrecision);
      print_execution_info(host, current_graph,
                           ps::ComputeIntent::RealTimeUpdate);
      return true;
    }
    ps::ComputeIntent intent;
    if (!parse_execution_intent(target, &intent)) {
      std::cout << "Error: unknown execution intent '" << target << "'.\n";
      return true;
    }
    print_execution_info(host, current_graph, intent);
    return true;
  }
  if (subcommand == "set") {
    std::string target;
    std::string first_type;
    std::string second_type;
    iss >> target >> first_type >> second_type;
    std::vector<std::string> available;
    const auto types = host.execution_available_types();
    if (!types.status.ok) {
      print_execution_error(types.status);
      return true;
    }
    available = types.value;
    const auto supported = [&available](const std::string& type) {
      return std::find(available.begin(), available.end(), type) !=
             available.end();
    };
    if (target == "all") {
      if (first_type.empty() || second_type.empty() || !supported(first_type) ||
          !supported(second_type)) {
        std::cout << "Usage: execution set all <hp-type> <rt-type>\n";
        return true;
      }
      const ps::VoidResult hp = host.replace_execution(
          ps::GraphSessionId{current_graph},
          ps::ComputeIntent::GlobalHighPrecision, first_type);
      if (!hp.status.ok) {
        print_execution_error(hp.status);
        return true;
      }
      const ps::VoidResult rt = host.replace_execution(
          ps::GraphSessionId{current_graph}, ps::ComputeIntent::RealTimeUpdate,
          second_type);
      if (!rt.status.ok) {
        print_execution_error(rt.status);
      } else {
        std::cout << "Updated hp and rt execution routes.\n";
      }
      return true;
    }
    ps::ComputeIntent intent;
    if (first_type.empty() || !second_type.empty() ||
        !parse_execution_intent(target, &intent) || !supported(first_type)) {
      std::cout << "Usage: execution set <hp|rt> <type>\n";
      return true;
    }
    const ps::VoidResult result = host.replace_execution(
        ps::GraphSessionId{current_graph}, intent, first_type);
    if (!result.status.ok) {
      print_execution_error(result.status);
    } else {
      std::cout << "Updated " << execution_intent_label(intent)
                << " execution route to '" << first_type << "'.\n";
    }
    return true;
  }
  std::cout << "Unknown execution subcommand: " << subcommand << "\n";
  std::cout << "Use 'execution help' for usage information.\n";
  return true;
}

/** @copydoc print_help_execution */
void print_help_execution(const CliConfig& /*config*/) {
  print_help_from_file("help_execution.txt");
}
