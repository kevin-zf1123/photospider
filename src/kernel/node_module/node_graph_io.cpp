// NodeGraph IO and basic graph mutation
#include "node_graph.hpp"
#include <fstream>
#include <algorithm>

namespace ps {

void NodeGraph::clear() { nodes.clear(); }

void NodeGraph::add_node(const Node& node) {
    if (has_node(node.id)) {
        throw GraphError(GraphErrc::InvalidParameter, "Node with id " + std::to_string(node.id) + " already exists.");
    }
    // Basic cycle pre-check using declared inputs
    std::unordered_set<int> potential_inputs;
    for (const auto& input : node.image_inputs) potential_inputs.insert(input.from_node_id);
    for (const auto& input : node.parameter_inputs) potential_inputs.insert(input.from_node_id);
    for (int input_id : potential_inputs) {
        if (input_id != -1) {
            std::unordered_set<int> visited;
            if (is_ancestor(node.id, input_id, visited)) {
                throw GraphError(GraphErrc::Cycle, "Adding node " + std::to_string(node.id) + " creates a cycle.");
            }
        }
    }
    nodes[node.id] = node;
}

bool NodeGraph::has_node(int id) const { return nodes.count(id) > 0; }

void NodeGraph::load_yaml(const fs::path& yaml_path) {
    YAML::Node config;
    try {
        config = YAML::LoadFile(yaml_path.string());
    } catch (const std::exception& e) {
        throw GraphError(GraphErrc::Io, "Failed to load YAML file " + yaml_path.string() + ": " + e.what());
    }
    if (!config.IsSequence()) throw GraphError(GraphErrc::InvalidYaml, "YAML root is not a sequence of nodes.");
    clear();
    for (const auto& n : config) {
        Node node = Node::from_yaml(n);
        add_node(node);
    }
}

void NodeGraph::save_yaml(const fs::path& yaml_path) const {
    YAML::Node n(YAML::NodeType::Sequence);
    std::vector<int> sorted_ids; sorted_ids.reserve(nodes.size());
    for(const auto& pair : nodes) sorted_ids.push_back(pair.first);
    std::sort(sorted_ids.begin(), sorted_ids.end());
    for (int id : sorted_ids) n.push_back(nodes.at(id).to_yaml());

    std::ofstream fout(yaml_path);
    if (!fout) throw GraphError(GraphErrc::Io, "Failed to open file for writing: " + yaml_path.string());
    fout << n;
}

} // namespace ps

