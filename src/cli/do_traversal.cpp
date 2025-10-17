#include "cli/do_traversal.hpp"

#include <iostream>
#include <vector>

#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"

void do_traversal(const ps::GraphModel& graph, bool show_mem, bool show_disk) {
  ps::GraphTraversalService traversal;
  ps::GraphCacheService cache;

  auto ends = traversal.ending_nodes(graph);
  if (ends.empty()) {
    std::cout << "(no ending nodes or graph is cyclic)\n";
    return;
  }

  for (int end : ends) {
    try {
      auto order = traversal.topo_postorder_from(graph, end);
      std::cout << "\nPost-order (eval order) for end node " << end << ":\n";
      for (size_t i = 0; i < order.size(); ++i) {
        const auto& node = graph.nodes.at(order[i]);
        std::cout << (i + 1) << ". " << node.id << " (" << node.name << ")";

        std::vector<std::string> statuses;
        if (show_mem && node.cached_output.has_value()) {
          statuses.push_back("in memory");
        }
        if (show_disk && !node.caches.empty()) {
          bool on_disk = false;
          for (const auto& cache_entry : node.caches) {
            ps::fs::path cache_file =
                cache.node_cache_dir(graph, node.id) / cache_entry.location;
            ps::fs::path meta_file = cache_file;
            meta_file.replace_extension(".yml");
            if (ps::fs::exists(cache_file) || ps::fs::exists(meta_file)) {
              on_disk = true;
              break;
            }
          }
          if (on_disk)
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
    } catch (const std::exception& e) {
      std::cout << "Traversal error on end node " << end << ": " << e.what()
                << "\n";
    }
  }
}
