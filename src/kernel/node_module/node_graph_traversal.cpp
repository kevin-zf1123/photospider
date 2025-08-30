// NodeGraph traversal utilities
#include "node_graph.hpp"
#include <unordered_map>
#include <algorithm>

namespace ps {

static void topo_postorder_util(const NodeGraph& g, int node_id, std::vector<int>& order, std::unordered_map<int, bool>& visited, std::unordered_map<int, bool>& recursion_stack) {
    visited[node_id] = true; recursion_stack[node_id] = true;
    const auto& node = g.nodes.at(node_id);
    auto process = [&](int dep){ if (dep==-1 || !g.has_node(dep)) return; if (!visited[dep]) topo_postorder_util(g, dep, order, visited, recursion_stack); else if (recursion_stack[dep]) throw GraphError(GraphErrc::Cycle, "Cycle detected in graph during traversal involving " + std::to_string(dep)); };
    for (const auto& input : node.image_inputs) process(input.from_node_id);
    for (const auto& input : node.parameter_inputs) process(input.from_node_id);
    order.push_back(node_id); recursion_stack[node_id] = false;
}

std::vector<int> NodeGraph::topo_postorder_from(int end_node_id) const {
    if (!has_node(end_node_id)) throw GraphError(GraphErrc::NotFound, "Node " + std::to_string(end_node_id) + " not in graph.");
    std::vector<int> order; std::unordered_map<int,bool> visited, rec;
    topo_postorder_util(*this, end_node_id, order, visited, rec); return order;
}

bool NodeGraph::is_ancestor(int potential_ancestor_id, int node_id, std::unordered_set<int>& visited) const {
    if (potential_ancestor_id == node_id) return true; if (visited.count(node_id)) return false; visited.insert(node_id); if (!has_node(node_id)) return false;
    const auto& node = nodes.at(node_id);
    for (const auto& input : node.image_inputs) { if (input.from_node_id != -1 && is_ancestor(potential_ancestor_id, input.from_node_id, visited)) return true; }
    for (const auto& input : node.parameter_inputs) { if (input.from_node_id != -1 && is_ancestor(potential_ancestor_id, input.from_node_id, visited)) return true; }
    return false;
}

std::vector<int> NodeGraph::ending_nodes() const {
    std::unordered_set<int> is_input_to_something;
    for (const auto& pair : nodes) {
        for (const auto& input : pair.second.image_inputs) is_input_to_something.insert(input.from_node_id);
        for (const auto& input : pair.second.parameter_inputs) is_input_to_something.insert(input.from_node_id);
    }
    std::vector<int> ends; for (const auto& pair : nodes) if (is_input_to_something.find(pair.first) == is_input_to_something.end()) ends.push_back(pair.first);
    return ends;
}

std::vector<int> NodeGraph::get_trees_containing_node(int node_id) const {
    std::vector<int> result_trees; auto all_ends = ending_nodes();
    for (int end_node : all_ends) {
        try { auto order = topo_postorder_from(end_node); if (std::find(order.begin(), order.end(), node_id) != order.end()) result_trees.push_back(end_node); }
        catch (const GraphError&) { continue; }
    }
    return result_trees;
}

} // namespace ps
