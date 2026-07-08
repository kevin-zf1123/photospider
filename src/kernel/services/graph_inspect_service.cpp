#include "kernel/services/graph_inspect_service.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ps {

namespace {
const NodeOutput* pick_cached_output(const Node& node,
                                     std::string& source_label) {
  if (node.cached_output_high_precision) {
    source_label = "HP formal cache";
    return &node.cached_output_high_precision.value();
  }
  source_label.clear();
  return nullptr;
}

void append_dependency_tree_entries(
    const GraphModel& graph, const GraphInspectService& inspect_service,
    DependencyTree& tree, int node_id, int depth,
    std::optional<GraphTopologyEdge> incoming_edge,
    std::unordered_set<int>& path, bool include_metadata) {
  DependencyTreeEntry entry;
  entry.depth = depth;
  entry.incoming_edge = std::move(incoming_edge);

  const Node* node = graph.find_node(node_id);
  if (node) {
    entry.node = inspect_service.inspect_node(*node, include_metadata);
  } else {
    entry.node.id = node_id;
  }

  if (path.count(node_id)) {
    entry.cycle = true;
    tree.entries.push_back(std::move(entry));
    return;
  }

  if (!node) {
    tree.entries.push_back(std::move(entry));
    return;
  }

  path.insert(node_id);
  tree.entries.push_back(std::move(entry));

  for (const auto& edge : graph.upstream_edges(node_id)) {
    if (edge.from_node_id < 0 || !graph.has_node(edge.from_node_id)) {
      continue;
    }
    append_dependency_tree_entries(graph, inspect_service, tree,
                                   edge.from_node_id, depth + 2, edge, path,
                                   include_metadata);
  }

  path.erase(node_id);
}

/**
 * @brief Copies cache metadata into an inspection-safe summary.
 *
 * @param node Node whose high-precision cache may be inspected.
 * @return Metadata summary with local output dimensions when a cache exists.
 * @throws std::bad_alloc if source label copying allocates and fails.
 * @note The summary intentionally copies only scalar/value metadata so public
 *       Host inspection can report extents without exposing NodeOutput storage.
 */
NodeMetadataSummary metadata_summary_for(const Node& node) {
  NodeMetadataSummary summary;
  std::string source_label;
  const NodeOutput* cached = pick_cached_output(node, source_label);
  if (!cached) {
    return summary;
  }

  summary.has_cached_output = true;
  summary.source_label = source_label;
  summary.output_width = cached->image_buffer.width;
  summary.output_height = cached->image_buffer.height;
  summary.debug = cached->debug;
  summary.space = cached->space;
  return summary;
}
}  // namespace

GraphNodeInspectInfo GraphInspectService::inspect_node(
    const Node& node, bool include_metadata) const {
  GraphNodeInspectInfo info;
  info.id = node.id;
  info.name = node.name;
  info.type = node.type;
  info.subtype = node.subtype;
  info.parameters = YAML::Clone(node.parameters);
  if (include_metadata) {
    info.metadata = metadata_summary_for(node);
  }
  return info;
}

GraphInspectionSnapshot GraphInspectService::inspect_graph(
    const GraphModel& graph, bool include_metadata) const {
  GraphInspectionSnapshot snapshot;
  std::vector<int> ids = graph.node_ids();
  snapshot.nodes.reserve(ids.size());
  for (int id : ids) {
    snapshot.nodes.push_back(inspect_node(graph.node(id), include_metadata));
  }
  return snapshot;
}

DependencyTree GraphInspectService::dependency_tree(
    const GraphModel& graph, bool include_metadata) const {
  DependencyTree tree;
  tree.scope = DependencyTree::Scope::EndingNodes;
  tree.graph_empty = graph.empty();
  if (tree.graph_empty) {
    return tree;
  }

  for (int node_id : graph.node_ids()) {
    if (graph.downstream_edges(node_id).empty()) {
      tree.root_node_ids.push_back(node_id);
    }
  }
  std::sort(tree.root_node_ids.begin(), tree.root_node_ids.end());
  tree.no_ending_nodes = tree.root_node_ids.empty();

  for (int root_node_id : tree.root_node_ids) {
    std::unordered_set<int> path;
    append_dependency_tree_entries(graph, *this, tree, root_node_id, 0,
                                   std::nullopt, path, include_metadata);
  }
  return tree;
}

DependencyTree GraphInspectService::dependency_tree(
    const GraphModel& graph, int start_node_id, bool include_metadata) const {
  DependencyTree tree;
  tree.scope = DependencyTree::Scope::StartNode;
  tree.start_node_id = start_node_id;
  tree.graph_empty = graph.empty();
  tree.start_node_found = graph.has_node(start_node_id);
  if (!tree.start_node_found) {
    return tree;
  }

  tree.root_node_ids.push_back(start_node_id);
  std::unordered_set<int> path;
  append_dependency_tree_entries(graph, *this, tree, start_node_id, 0,
                                 std::nullopt, path, include_metadata);
  return tree;
}

}  // namespace ps
