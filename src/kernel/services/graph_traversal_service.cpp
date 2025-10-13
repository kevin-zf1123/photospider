#include "kernel/services/graph_traversal_service.hpp"

#include "graph_model.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <yaml-cpp/emitter.h>

namespace ps {

namespace {

void topo_postorder_util(const GraphModel& graph,
                         int node_id,
                         std::vector<int>& order,
                         std::unordered_map<int, bool>& visited,
                         std::unordered_map<int, bool>& recursion_stack) {
    visited[node_id] = true;
    recursion_stack[node_id] = true;

    const auto& node = graph.nodes.at(node_id);
    auto process_dependency = [&](int dependency_id) {
        if (dependency_id == -1 || !graph.has_node(dependency_id)) {
            return;
        }
        if (!visited[dependency_id]) {
            topo_postorder_util(graph, dependency_id, order, visited, recursion_stack);
            return;
        }
        if (recursion_stack[dependency_id]) {
            throw GraphError(GraphErrc::Cycle,
                             "Cycle detected in graph during traversal involving " +
                                 std::to_string(dependency_id));
        }
    };

    for (const auto& input : node.image_inputs) {
        process_dependency(input.from_node_id);
    }
    for (const auto& input : node.parameter_inputs) {
        process_dependency(input.from_node_id);
    }

    order.push_back(node_id);
    recursion_stack[node_id] = false;
}

void print_dep_tree_recursive(const GraphModel& graph,
                              std::ostream& os,
                              int node_id,
                              int level,
                              std::unordered_set<int>& path,
                              bool show_parameters) {
    auto indent = [&](int l) {
        for (int i = 0; i < l; ++i) {
            os << "  ";
        }
    };

    os << "\n";

    if (path.count(node_id)) {
        indent(level);
        os << "- ... (Cycle detected on Node " << node_id << ") ...\n";
        return;
    }
    path.insert(node_id);

    indent(level);
    const auto& node = graph.nodes.at(node_id);
    os << "- Node " << node.id << " (" << node.name << " | " << node.type << ":" << node.subtype << ")\n";

    if (show_parameters && node.parameters && node.parameters.IsMap() && node.parameters.size() > 0) {
        indent(level + 1);
        os << "static_params:\n";

        std::function<void(const YAML::Node&, int)> dump_map = [&](const YAML::Node& m, int lvl) {
            for (auto it = m.begin(); it != m.end(); ++it) {
                indent(lvl);
                std::string key;
                YAML::Node key_node = it->first;
                try {
                    key = key_node.as<std::string>();
                } catch (...) {
                    YAML::Emitter ke;
                    ke << it->first;
                    key = ke.c_str();
                }
                YAML::Node val = it->second;
                if (val.IsMap()) {
                    os << key << ":\n";
                    dump_map(val, lvl + 1);
                } else {
                    os << key << ": ";
                    YAML::Emitter ve;
                    ve << YAML::Flow << val;
                    os << ve.c_str() << "\n";
                }
            }
        };

        for (auto it = node.parameters.begin(); it != node.parameters.end(); ++it) {
            YAML::Node key_node = it->first;
            YAML::Node val_node = it->second;
            std::string key;
            try {
                key = key_node.as<std::string>();
            } catch (...) {
                YAML::Emitter ke;
                ke << key_node;
                key = ke.c_str();
            }

            if (val_node.IsMap()) {
                indent(level + 2);
                os << key << ":\n";
                dump_map(val_node, level + 3);
            } else {
                indent(level + 2);
                os << key << ": ";
                YAML::Emitter ve;
                ve << YAML::Flow << val_node;
                os << ve.c_str() << "\n";
            }
        }
    }

    for (const auto& input : node.image_inputs) {
        if (input.from_node_id != -1 && graph.has_node(input.from_node_id)) {
            os << "\n";
            indent(level + 1);
            os << "(image from " << input.from_node_id << ":" << input.from_output_name << ")\n";
            print_dep_tree_recursive(graph, os, input.from_node_id, level + 2, path, show_parameters);
        }
    }
    for (const auto& input : node.parameter_inputs) {
        if (input.from_node_id != -1 && graph.has_node(input.from_node_id)) {
            os << "\n";
            indent(level + 1);
            os << "(param '" << input.to_parameter_name << "' from " << input.from_node_id << ":" << input.from_output_name << ")\n";
            print_dep_tree_recursive(graph, os, input.from_node_id, level + 2, path, show_parameters);
        }
    }
    path.erase(node_id);
}

} // namespace

std::vector<int> GraphTraversalService::topo_postorder_from(const GraphModel& graph, int end_node_id) const {
    if (!graph.has_node(end_node_id)) {
        throw GraphError(GraphErrc::NotFound,
                         "Node " + std::to_string(end_node_id) + " not in graph.");
    }

    std::vector<int> order;
    std::unordered_map<int, bool> visited;
    std::unordered_map<int, bool> recursion_stack;

    topo_postorder_util(graph, end_node_id, order, visited, recursion_stack);
    return order;
}

bool GraphTraversalService::is_ancestor(const GraphModel& graph,
                                        int potential_ancestor_id,
                                        int node_id,
                                        std::unordered_set<int>& visited) const {
    if (potential_ancestor_id == node_id) {
        return true;
    }

    if (visited.count(node_id)) {
        return false;
    }
    visited.insert(node_id);

    if (!graph.has_node(node_id)) {
        return false;
    }

    const auto& node = graph.nodes.at(node_id);
    for (const auto& input : node.image_inputs) {
        if (input.from_node_id != -1 &&
            is_ancestor(graph, potential_ancestor_id, input.from_node_id, visited)) {
            return true;
        }
    }
    for (const auto& input : node.parameter_inputs) {
        if (input.from_node_id != -1 &&
            is_ancestor(graph, potential_ancestor_id, input.from_node_id, visited)) {
            return true;
        }
    }
    return false;
}

std::vector<int> GraphTraversalService::ending_nodes(const GraphModel& graph) const {
    std::unordered_set<int> is_input_to_something;
    for (const auto& pair : graph.nodes) {
        for (const auto& input : pair.second.image_inputs) {
            is_input_to_something.insert(input.from_node_id);
        }
        for (const auto& input : pair.second.parameter_inputs) {
            is_input_to_something.insert(input.from_node_id);
        }
    }
    std::vector<int> ends;
    ends.reserve(graph.nodes.size());
    for (const auto& pair : graph.nodes) {
        if (is_input_to_something.find(pair.first) == is_input_to_something.end()) {
            ends.push_back(pair.first);
        }
    }
    return ends;
}

std::vector<int> GraphTraversalService::parents_of(const GraphModel& graph, int node_id) const {
    std::vector<int> parents;
    parents.reserve(graph.nodes.size());

    for (const auto& pair : graph.nodes) {
        const auto& candidate = pair.second;
        for (const auto& input : candidate.image_inputs) {
            if (input.from_node_id == node_id) {
                parents.push_back(candidate.id);
                break;
            }
        }
        for (const auto& input : candidate.parameter_inputs) {
            if (input.from_node_id == node_id) {
                parents.push_back(candidate.id);
                break;
            }
        }
    }
    return parents;
}

std::vector<int> GraphTraversalService::get_trees_containing_node(const GraphModel& graph,
                                                                  int node_id) const {
    std::vector<int> result_trees;
    auto all_end_nodes = ending_nodes(graph);
    for (int end_node : all_end_nodes) {
        try {
            auto order = topo_postorder_from(graph, end_node);
            if (std::find(order.begin(), order.end(), node_id) != order.end()) {
                result_trees.push_back(end_node);
            }
        } catch (const GraphError&) {
            continue;
        }
    }
    return result_trees;
}

void GraphTraversalService::print_dependency_tree(const GraphModel& graph,
                                                  std::ostream& os,
                                                  bool show_parameters) const {
    os << "Dependency Tree (reversed from ending nodes):\n";
    auto ends = ending_nodes(graph);
    if (ends.empty() && !graph.nodes.empty()) {
        os << "(Graph has cycles or is fully connected)\n";
    } else if (graph.nodes.empty()) {
        os << "(Graph is empty)\n";
    }

    for (int end_node_id : ends) {
        std::unordered_set<int> path;
        print_dep_tree_recursive(graph, os, end_node_id, 0, path, show_parameters);
    }
}

void GraphTraversalService::print_dependency_tree(const GraphModel& graph,
                                                  std::ostream& os,
                                                  int start_node_id,
                                                  bool show_parameters) const {
    os << "Dependency Tree (starting from Node " << start_node_id << "):\n";
    if (!graph.has_node(start_node_id)) {
        os << "(Node " << start_node_id << " not found in graph)\n";
        return;
    }
    std::unordered_set<int> path;
    print_dep_tree_recursive(graph, os, start_node_id, 0, path, show_parameters);
}

} // namespace ps
