#include "cli/cli_autocompleter.hpp"

namespace ps {

std::string CliAutocompleter::FindLongestCommonPrefix(const std::vector<std::string>& options) const {
    if (options.empty()) return "";
    if (options.size() == 1) return options[0];

    std::string prefix = options[0];
    for (size_t i = 1; i < options.size(); ++i) {
        while (options[i].rfind(prefix, 0) != 0) {
            prefix.pop_back();
            if (prefix.empty()) return "";
        }
    }
    return prefix;
}

} // namespace ps