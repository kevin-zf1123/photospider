#include "cli/cli_autocompleter.hpp"

namespace ps {

void CliAutocompleter::CompleteComputeArgs(
    const std::string& prefix, std::vector<std::string>& options) const {
  // Only compute flags here. Node id/all is handled as the first arg in
  // Complete().
  const std::vector<std::string> mode_args = {
      "force", "force-deep", "parallel", "t",    "-t",     "timer", "tl",
      "-tl",   "m",          "-m",       "mute", "nosave", "ns"};
  for (const auto& arg : mode_args) {
    if (arg.rfind(prefix, 0) == 0)
      options.push_back(arg);
  }
}

}  // namespace ps
