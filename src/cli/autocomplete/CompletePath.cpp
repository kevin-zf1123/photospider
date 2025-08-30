#include "cli/cli_autocompleter.hpp"
#include "cli/path_complete.hpp"

namespace ps {

void CliAutocompleter::CompletePath(const std::string& prefix, std::vector<std::string>& options) const {
    auto opts = PathCompleteOptions(prefix);
    options.insert(options.end(), opts.begin(), opts.end());
}

} // namespace ps