#include "graph_cli/do_traversal.hpp"

#include <iostream>
#include <string>
#include <vector>

void do_traversal(ps::Host& host, const std::string& graph_name, bool show_mem,
                  bool show_disk) {
  auto details = host.traversal_details(ps::GraphSessionId{graph_name});
  if (!details.status.ok || details.value.empty()) {
    std::cout << "(no ending nodes or graph is cyclic)\n";
    return;
  }

  for (const auto& [end, order] : details.value) {
    std::cout << "\nPost-order (eval order) for end node " << end << ":\n";
    for (size_t i = 0; i < order.size(); ++i) {
      const auto& node = order[i];
      std::cout << (i + 1) << ". " << node.node.value << " (" << node.name
                << ")";

      std::vector<std::string> statuses;
      if (show_mem && node.has_memory_cache) {
        statuses.push_back("HP in memory");
      }
      if (show_disk && node.has_disk_cache) {
        statuses.push_back("on disk");
      }
      if (!statuses.empty()) {
        std::cout << " (";
        for (size_t j = 0; j < statuses.size(); ++j) {
          std::cout << statuses[j] << (j + 1 < statuses.size() ? ", " : "");
        }
        std::cout << ")";
      }
      std::cout << "\n";
    }
  }
}
