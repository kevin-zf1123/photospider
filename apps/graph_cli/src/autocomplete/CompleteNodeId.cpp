#include <string>
#include <vector>

#include "graph_cli/cli_autocompleter.hpp"
#include "photospider/host/host.hpp"

namespace ps {

/** @copydoc CliAutocompleter::CompleteNodeId */
void CliAutocompleter::CompleteNodeId(const std::string& prefix,
                                      std::vector<std::string>& options) const {
  if (std::string("all").rfind(prefix, 0) == 0) {
    options.push_back("all");
  }
  if (!current_graph_.empty()) {
    auto ids = svc_.list_node_ids(ps::GraphSessionId{current_graph_});
    if (ids.status.ok) {
      for (const auto& id : ids.value) {
        std::string id_str = std::to_string(id.value);
        if (id_str.rfind(prefix, 0) == 0)
          options.push_back(id_str);
      }
    }
  }
}

}  // namespace ps
