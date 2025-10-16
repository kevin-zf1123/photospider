#include "cli/cli_autocompleter.hpp"
#include "cli/path_complete.hpp"

namespace ps {

void CliAutocompleter::CompleteYamlPath(
    const std::string& prefix, std::vector<std::string>& options) const {
  // Use generic path options, then filter to YAML files, but keep directories
  // to allow traversal.
  auto opts = PathCompleteOptions(prefix);
  for (const auto& o : opts) {
    if (!o.empty() && o.back() == '/') {
      options.push_back(o);  // allow continuing into subdirectories
    } else {
      // Match .yaml or .yml (case-sensitive as typical on *nix)
      if (o.size() >= 5 && o.rfind(".yaml") == o.size() - 5)
        options.push_back(o);
      else if (o.size() >= 4 && o.rfind(".yml") == o.size() - 4)
        options.push_back(o);
    }
  }
}

}  // namespace ps