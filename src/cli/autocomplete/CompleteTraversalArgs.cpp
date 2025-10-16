#include <string>
#include <vector>

#include "cli/cli_autocompleter.hpp"

namespace ps {

void CliAutocompleter::CompleteTraversalArgs(
    const std::string& prefix, std::vector<std::string>& options) const {
  // Support print-style tree mode args for traversal as well
  const std::vector<std::string> tree_args = {"full", "simplified", "no_tree",
                                              "f",    "s",          "n"};
  for (const auto& arg : tree_args) {
    if (arg.rfind(prefix, 0) == 0)
      options.push_back(arg);
  }
  // And common traversal cache/check flags (optional but helpful)
  const std::vector<std::string> trav_flags = {"m", "md", "d", "c", "cr"};
  for (const auto& arg : trav_flags) {
    if (arg.rfind(prefix, 0) == 0)
      options.push_back(arg);
  }
}

}  // namespace ps