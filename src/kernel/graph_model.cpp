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

void validate_node_dfs(const GraphModel& graph, int node_id,
                       std::unordered_set<int>& visiting,
                       std::unordered_set<int>& visited) {
  if (visited.count(node_id)) {
    return;
  }
  if (visiting.count(node_id)) {
    throw GraphError(GraphErrc::Cycle,
                     "Cycle detected while validating graph topology.");
  }

  auto node_it = graph.nodes.find(node_id);
  if (node_it == graph.nodes.end()) {
    throw GraphError(GraphErrc::MissingDependency,
                     "Missing node " + std::to_string(node_id) +
                         " while validating graph topology.");
  }

  visiting.insert(node_id);
  const Node& node = node_it->second;
  auto visit_dependency = [&](int dependency_id) {
    if (dependency_id == -1) {
      return;
    }
    if (!graph.has_node(dependency_id)) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Node " + std::to_string(node.id) +
                           " references missing node " +
                           std::to_string(dependency_id) + ".");
    }
    validate_node_dfs(graph, dependency_id, visiting, visited);
  };

  for (const auto& input : node.image_inputs) {
    visit_dependency(input.from_node_id);
  }
  for (const auto& input : node.parameter_inputs) {
    visit_dependency(input.from_node_id);
  }
  visiting.erase(node_id);
  visited.insert(node_id);
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
  timing_results.node_timings.clear();
  timing_results.total_ms = 0.0;
  last_dirty_region_snapshot_debug.reset();
  last_dirty_region_snapshot.reset();
  recent_dirty_region_snapshots.clear();
  last_compute_plan.reset();
  recent_compute_plans.clear();
  total_io_time_ms.store(0.0, std::memory_order_relaxed);
  skip_save_cache_.store(false, std::memory_order_relaxed);
  quiet_ = true;
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

void GraphModel::validate_topology() const {
  std::unordered_set<int> visiting;
  std::unordered_set<int> visited;
  for (const auto& [id, _] : nodes) {
    validate_node_dfs(*this, id, visiting, visited);
  }
}

void GraphModel::set_skip_save_cache(bool v) {
  skip_save_cache_.store(v, std::memory_order_relaxed);
}

bool GraphModel::skip_save_cache() const {
  return skip_save_cache_.load(std::memory_order_relaxed);
}

}  // namespace ps
