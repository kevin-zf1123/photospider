#include "graph/graph_traversal_service.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ps {

namespace {

void topo_postorder_util(const GraphModel& graph, int node_id,
                         std::vector<int>& order,
                         std::unordered_map<int, bool>& visited,
                         std::unordered_map<int, bool>& recursion_stack) {
  visited[node_id] = true;
  recursion_stack[node_id] = true;

  for (const auto& edge : graph.upstream_edges(node_id)) {
    const int dependency_id = edge.from_node_id;
    if (dependency_id < 0 || !graph.has_node(dependency_id)) {
      continue;
    }
    if (!visited[dependency_id]) {
      topo_postorder_util(graph, dependency_id, order, visited,
                          recursion_stack);
      continue;
    }
    if (recursion_stack[dependency_id]) {
      throw GraphError(GraphErrc::Cycle,
                       "Cycle detected in graph during traversal involving " +
                           std::to_string(dependency_id));
    }
  }

  order.push_back(node_id);
  recursion_stack[node_id] = false;
}

std::vector<int> unique_sorted_node_ids(std::vector<int> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

}  // namespace

std::vector<int> GraphTraversalService::topo_postorder_from(
    const GraphModel& graph, int end_node_id) const {
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

std::vector<int> GraphTraversalService::ending_nodes(
    const GraphModel& graph) const {
  std::vector<int> ends;
  for (int node_id : graph.node_ids()) {
    if (graph.downstream_edges(node_id).empty()) {
      ends.push_back(node_id);
    }
  }
  return ends;
}

bool GraphTraversalService::is_ancestor(
    const GraphModel& graph, int potential_ancestor_id, int node_id,
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

  for (const auto& edge : graph.upstream_edges(node_id)) {
    if (edge.from_node_id >= 0 &&
        is_ancestor(graph, potential_ancestor_id, edge.from_node_id, visited)) {
      return true;
    }
  }
  return false;
}

std::vector<int> GraphTraversalService::dependencies_of(const GraphModel& graph,
                                                        int node_id) const {
  if (!graph.has_node(node_id)) {
    throw GraphError(GraphErrc::NotFound,
                     "Node " + std::to_string(node_id) + " not in graph.");
  }
  std::vector<int> dependencies;
  dependencies.reserve(graph.upstream_edges(node_id).size());
  for (const auto& edge : graph.upstream_edges(node_id)) {
    if (edge.from_node_id >= 0 && graph.has_node(edge.from_node_id)) {
      dependencies.push_back(edge.from_node_id);
    }
  }
  return unique_sorted_node_ids(std::move(dependencies));
}

std::vector<int> GraphTraversalService::dependents_of(const GraphModel& graph,
                                                      int node_id) const {
  if (!graph.has_node(node_id)) {
    throw GraphError(GraphErrc::NotFound,
                     "Node " + std::to_string(node_id) + " not in graph.");
  }
  std::vector<int> dependents;
  dependents.reserve(graph.downstream_edges(node_id).size());
  for (const auto& edge : graph.downstream_edges(node_id)) {
    if (edge.to_node_id >= 0 && graph.has_node(edge.to_node_id)) {
      dependents.push_back(edge.to_node_id);
    }
  }
  return unique_sorted_node_ids(std::move(dependents));
}

std::vector<int> GraphTraversalService::get_trees_containing_node(
    const GraphModel& graph, int node_id) const {
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

}  // namespace ps
