#include "cli/cli_autocompleter.hpp"

namespace ps {

void CliAutocompleter::CompleteCommand(
    const std::string& prefix, std::vector<std::string>& options) const {
  for (const auto& cmd : commands_) {
    if (cmd.rfind(prefix, 0) == 0) {
      options.push_back(cmd);
    }
  }
}

}  // namespace ps