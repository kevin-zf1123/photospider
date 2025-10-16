#include "cli/cli_autocompleter.hpp"

namespace ps {

void CliAutocompleter::CompletePrintArgs(const std::string& prefix,
                                         std::vector<std::string>& options,
                                         bool only_mode_args) const {
  // When completing the first argument, only suggest id/all.
  // From the second argument onward, only suggest print mode args.
  if (!only_mode_args) {
    CompleteNodeId(prefix, options);
    return;
  }
  const std::vector<std::string> mode_args = {"full", "simplified", "f", "s"};
  for (const auto& arg : mode_args) {
    if (arg.rfind(prefix, 0) == 0)
      options.push_back(arg);
  }
}

}  // namespace ps