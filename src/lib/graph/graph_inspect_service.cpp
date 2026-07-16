#include "graph/graph_inspect_service.hpp"

#include <algorithm>
#include <new>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ps {

namespace {

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Injects resource exhaustion during real graph inspection traversal.
 *
 * @param node GraphModel node selected by the inspect_graph collection loop.
 * @return Nothing.
 * @throws std::bad_alloc when node carries the private traversal probe name.
 * @note This BUILD_TESTING-only helper has internal linkage and immutable input
 * matching, so concurrent inspection is safe and no public/internal callable
 * seam is added to an installed header.
 */
void throw_if_graph_inspection_bad_alloc_probe(const Node& node) {
  if (node.name == "__photospider_test_bad_alloc_inspect_traversal__") {
    throw std::bad_alloc{};
  }
}
#endif

/**
 * @brief Selects the formal HP cache exposed through inspection metadata.
 *
 * @param node Node whose formal cache is inspected.
 * @param source_label Output label updated to describe the selected cache.
 * @return Borrowed cache pointer, or null when no formal cache exists.
 * @throws std::bad_alloc if source_label assignment exhausts memory.
 * @note The returned pointer remains owned by node and is used only while the
 * caller holds serialized graph inspection state.
 */
const NodeOutput* pick_cached_output(const Node& node,
                                     std::string& source_label) {
  if (node.cached_output_high_precision) {
    source_label = "HP formal cache";
    return &node.cached_output_high_precision.value();
  }
  source_label.clear();
  return nullptr;
}

/**
 * @brief Appends one upstream dependency branch to a flattened tree.
 *
 * @param graph Graph supplying node and upstream-edge snapshots.
 * @param inspect_service Service used to copy each visited node.
 * @param tree Destination tree owned by the caller.
 * @param node_id Current node id to append.
 * @param depth Display indentation depth for the current row.
 * @param incoming_edge Optional edge that reached the current row.
 * @param path Request-local recursion path used for cycle detection.
 * @param include_metadata Whether copied nodes include cache metadata.
 * @return Nothing.
 * @throws std::bad_alloc if path, edge, node, or tree storage exhausts memory.
 * @throws YAML::Exception if node parameter cloning fails.
 * @note path membership is removed during unwind so sibling branches do not
 * inherit one another's cycle state. All borrowed references remain call-local.
 */
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

/**
 * @brief Copies one Node into an inspection-safe value.
 *
 * @param node Node to inspect during serialized graph access.
 * @param include_metadata Whether formal HP cache metadata is copied.
 * @return Owned node inspection value.
 * @throws std::bad_alloc if string, YAML, or metadata storage exhausts memory.
 * @throws YAML::Exception if parameter cloning fails for another reason.
 * @note The result owns all values and retains no Node or NodeOutput reference.
 */
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

/**
 * @brief Traverses graph ids and copies every node inspection value.
 *
 * @param graph Graph to inspect during serialized graph access.
 * @param include_metadata Whether formal HP cache metadata is copied.
 * @return Owned graph inspection snapshot in deterministic node-id order.
 * @throws std::bad_alloc if id collection, node copying, or result storage
 * exhausts memory.
 * @throws GraphError if a node id disappears during caller-unsafe mutation.
 * @throws YAML::Exception if parameter cloning fails for another reason.
 * @note BUILD_TESTING may inject resource exhaustion inside the real
 * collection loop based on immutable test input; production builds compile
 * that branch out.
 */
GraphInspectionSnapshot GraphInspectService::inspect_graph(
    const GraphModel& graph, bool include_metadata) const {
  GraphInspectionSnapshot snapshot;
  std::vector<int> ids = graph.node_ids();
  snapshot.nodes.reserve(ids.size());
  for (int id : ids) {
    const Node& node = graph.node(id);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    throw_if_graph_inspection_bad_alloc_probe(node);
#endif
    snapshot.nodes.push_back(inspect_node(node, include_metadata));
  }
  return snapshot;
}

/**
 * @brief Builds a dependency tree rooted at every ending node.
 *
 * @param graph Graph whose upstream topology is traversed.
 * @param include_metadata Whether copied nodes include cache metadata.
 * @return Owned flattened dependency tree.
 * @throws std::bad_alloc if root, path, edge, or entry storage exhausts memory.
 * @throws YAML::Exception if parameter cloning fails for another reason.
 * @note The caller owns graph-state serialization; recursion state is local to
 * each root branch.
 */
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

/**
 * @brief Builds a dependency tree rooted at one requested node.
 *
 * @param graph Graph whose upstream topology is traversed.
 * @param start_node_id Requested traversal root.
 * @param include_metadata Whether copied nodes include cache metadata.
 * @return Owned flattened tree, or a value with start_node_found=false.
 * @throws std::bad_alloc if root, path, edge, or entry storage exhausts memory.
 * @throws YAML::Exception if parameter cloning fails for another reason.
 * @note The caller owns graph-state serialization; no graph reference escapes.
 */
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
