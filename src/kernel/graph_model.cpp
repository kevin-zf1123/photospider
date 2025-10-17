#include "graph_model.hpp"

#include <filesystem>
#include <unordered_set>

namespace ps {

namespace {

bool is_ancestor(const GraphModel& graph, int potential_ancestor_id,
                 int node_id, std::unordered_set<int>& visited) {
  if (potential_ancestor_id == node_id) {
    return true;
  }
  if (visited.count(node_id)) {
    return false;
  }
  visited.insert(node_id);

  auto it = graph.nodes.find(node_id);
  if (it == graph.nodes.end()) {
    return false;
  }
  const Node& node = it->second;
  for (const auto& input : node.image_inputs) {
    if (input.from_node_id != -1 && is_ancestor(graph, potential_ancestor_id,
                                                input.from_node_id, visited)) {
      return true;
    }
  }
  for (const auto& input : node.parameter_inputs) {
    if (input.from_node_id != -1 && is_ancestor(graph, potential_ancestor_id,
                                                input.from_node_id, visited)) {
      return true;
    }
  }
  return false;
}

}  // namespace

GraphModel::GraphModel(fs::path cache_root_dir)
    : cache_root(std::move(cache_root_dir)) {
  if (!cache_root.empty()) {
    fs::create_directories(cache_root);
  }
}

void GraphModel::set_quiet(bool q) {
  quiet_ = q;
}

bool GraphModel::is_quiet() const {
  return quiet_;
}

void GraphModel::clear() {
  nodes.clear();
}

void GraphModel::add_node(const Node& node) {
  if (has_node(node.id)) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "Node with id " + std::to_string(node.id) + " already exists.");
  }

  std::unordered_set<int> potential_inputs;
  for (const auto& input : node.image_inputs) {
    potential_inputs.insert(input.from_node_id);
  }
  for (const auto& input : node.parameter_inputs) {
    potential_inputs.insert(input.from_node_id);
  }
  for (int input_id : potential_inputs) {
    if (input_id != -1) {
      std::unordered_set<int> visited;
      if (is_ancestor(*this, node.id, input_id, visited)) {
        throw GraphError(
            GraphErrc::Cycle,
            "Adding node " + std::to_string(node.id) + " creates a cycle.");
      }
    }
  }
  nodes[node.id] = node;
}

bool GraphModel::has_node(int id) const {
  return nodes.count(id) > 0;
}

void GraphModel::set_skip_save_cache(bool v) {
  skip_save_cache_.store(v, std::memory_order_relaxed);
}

bool GraphModel::skip_save_cache() const {
  return skip_save_cache_.load(std::memory_order_relaxed);
}

}  // namespace ps
