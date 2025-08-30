// FILE: src/cli/do_traversal.cpp
#include <iostream>
#include <vector>
#include "cli/do_traversal.hpp"

void do_traversal(const ps::NodeGraph& graph, bool show_mem, bool show_disk) {
    auto ends = graph.ending_nodes();
    if (ends.empty()) {
        std::cout << "(no ending nodes or graph is cyclic)\n";
        return;
    }

    for (int end : ends) {
        try {
            auto order = graph.topo_postorder_from(end);
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
                    for (const auto& cache : node.caches) {
                        ps::fs::path cache_file = graph.node_cache_dir(node.id) / cache.location;
                        ps::fs::path meta_file = cache_file;
                        meta_file.replace_extension(".yml");
                        if (ps::fs::exists(cache_file) || ps::fs::exists(meta_file)) {
                            on_disk = true;
                            break;
                        }
                    }
                    if (on_disk) statuses.push_back("on disk");
                }
                if (!statuses.empty()) {
                    std::cout << " (";
                    for (size_t j = 0; j < statuses.size(); ++j) {
                        std::cout << statuses[j] << (j < statuses.size() - 1 ? ", " : "");
                    }
                    std::cout << ")";
                }
                std::cout << "\n";
            }
        } catch (const std::exception& e) {
            std::cout << "Traversal error on end node " << end << ": " << e.what() << "\n";
        }
    }
}

