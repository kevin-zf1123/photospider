#include "cli/cli_autocompleter.hpp"
#include "kernel/interaction.hpp"

namespace ps {

void CliAutocompleter::CompleteNodeId(const std::string& prefix, std::vector<std::string>& options) const {
    if (std::string("all").rfind(prefix, 0) == 0) {
        options.push_back("all");
    }
    if (!current_graph_.empty()) {
        auto ids = svc_.cmd_list_node_ids(current_graph_);
        if (ids) {
            for (int id : *ids) {
                std::string id_str = std::to_string(id);
                if (id_str.rfind(prefix, 0) == 0) options.push_back(id_str);
            }
        }
    }
}

} // namespace ps