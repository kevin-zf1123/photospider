#include "cli/cli_autocompleter.hpp"
#include "kernel/interaction.hpp"

namespace ps {

void CliAutocompleter::CompleteGraphName(const std::string& prefix, std::vector<std::string>& options) const {
    auto names = svc_.cmd_list_graphs();
    for (const auto& n : names) {
        if (n.rfind(prefix, 0) == 0) options.push_back(n);
    }
}

} // namespace ps