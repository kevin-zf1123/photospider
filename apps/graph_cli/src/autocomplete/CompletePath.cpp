#include <string>
#include <vector>

#include "graph_cli/cli_autocompleter.hpp"
#include "graph_cli/path_complete.hpp"

namespace ps {

/** @copydoc CliAutocompleter::CompletePath */
void CliAutocompleter::CompletePath(const std::string& prefix,
                                    std::vector<std::string>& options) const {
  auto opts = PathCompleteOptions(prefix);
  options.insert(options.end(), opts.begin(), opts.end());
}

}  // namespace ps
