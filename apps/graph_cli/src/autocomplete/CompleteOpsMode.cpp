#include <string>
#include <vector>

#include "graph_cli/cli_autocompleter.hpp"

namespace ps {

/** @copydoc CliAutocompleter::CompleteOpsMode */
void CliAutocompleter::CompleteOpsMode(
    const std::string& prefix, std::vector<std::string>& options) const {
  const std::vector<std::string> args = {"all",     "a", "builtin", "b",
                                         "plugins", "p", "custom",  "c"};
  for (const auto& a : args) {
    if (a.rfind(prefix, 0) == 0)
      options.push_back(a);
  }
}

}  // namespace ps
