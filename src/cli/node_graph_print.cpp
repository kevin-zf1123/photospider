// CLI-side implementation of NodeGraph tree printing (formatting only)
#include "node_graph.hpp"

#include <functional>
#include <ostream>
#include <unordered_set>
#include <yaml-cpp/emitter.h>

using namespace ps;

namespace {
static void print_dep_tree_recursive(const NodeGraph& g, std::ostream& os, int node_id, int level, std::unordered_set<int>& path, bool show_parameters) {
    auto indent = [&](int l) { for (int i = 0; i < l; ++i) os << "  "; };

    os << "\n"; // spacing between nodes/levels

    if (path.count(node_id)) {
        indent(level);
        os << "- ... (Cycle detected on Node " << node_id << ") ...\n";
        return;
    }
    path.insert(node_id);

    indent(level);
    const auto& node = g.nodes.at(node_id);
    os << "- Node " << node.id << " (" << node.name << " | " << node.type << ":" << node.subtype << ")\n";

    if (show_parameters && node.parameters && node.parameters.IsMap() && node.parameters.size() > 0) {
        indent(level + 1);
        os << "static_params:\n";

        std::function<void(const YAML::Node&, int)> dump_map = [&](const YAML::Node& m, int lvl) {
            for (auto it = m.begin(); it != m.end(); ++it) {
                indent(lvl);
                std::string key;
                try { key = it->first.as<std::string>(); }
                catch(...) { YAML::Emitter ke; ke << it->first; key = ke.c_str(); }
                const YAML::Node& val = it->second;
                if (val.IsMap()) {
                    os << key << ":\n";
                    dump_map(val, lvl + 1);
                } else {
                    os << key << ": ";
                    YAML::Emitter ve; ve << YAML::Flow << val;
                    os << ve.c_str() << "\n";
                }
            }
        };

        for (auto it = node.parameters.begin(); it != node.parameters.end(); ++it) {
            const auto& key_node = it->first;
            const auto& val_node = it->second;
            std::string key;
            try { key = key_node.as<std::string>(); }
            catch(...) { YAML::Emitter ke; ke << key_node; key = ke.c_str(); }

            if (val_node.IsMap()) {
                indent(level + 2); os << key << ":\n"; dump_map(val_node, level + 3);
            } else {
                indent(level + 2); os << key << ": "; YAML::Emitter ve; ve << YAML::Flow << val_node; os << ve.c_str() << "\n";
            }
        }
    }

    for (const auto& input : node.image_inputs) {
        if (input.from_node_id != -1 && g.has_node(input.from_node_id)) {
            os << "\n"; indent(level + 1);
            os << "(image from " << input.from_node_id << ":" << input.from_output_name << ")\n";
            print_dep_tree_recursive(g, os, input.from_node_id, level + 2, path, show_parameters);
        }
    }
    for (const auto& input : node.parameter_inputs) {
        if (input.from_node_id != -1 && g.has_node(input.from_node_id)) {
            os << "\n"; indent(level + 1);
            os << "(param '" << input.to_parameter_name << "' from " << input.from_node_id << ":" << input.from_output_name << ")\n";
            print_dep_tree_recursive(g, os, input.from_node_id, level + 2, path, show_parameters);
        }
    }
    path.erase(node_id);
}
} // namespace

void NodeGraph::print_dependency_tree(std::ostream& os, bool show_parameters) const {
    os << "Dependency Tree (reversed from ending nodes):\n";
    auto ends = ending_nodes();
    if (ends.empty() && !nodes.empty()) os << "(Graph has cycles or is fully connected)\n";
    else if(nodes.empty()) os << "(Graph is empty)\n";

    for (int end_node_id : ends) {
        std::unordered_set<int> path;
        print_dep_tree_recursive(*this, os, end_node_id, 0, path, show_parameters);
    }
}

void NodeGraph::print_dependency_tree(std::ostream& os, int start_node_id, bool show_parameters) const {
    os << "Dependency Tree (starting from Node " << start_node_id << "):\n";
    if (!has_node(start_node_id)) { os << "(Node " << start_node_id << " not found in graph)\n"; return; }
    std::unordered_set<int> path;
    print_dep_tree_recursive(*this, os, start_node_id, 0, path, show_parameters);
}

