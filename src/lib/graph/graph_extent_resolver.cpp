#include "graph/graph_extent_resolver.hpp"

#include <unordered_map>

#include "core/param_utils.hpp"

namespace ps {

/** @copydoc GraphExtentResolver::resolve_output_extent */
PixelSize GraphExtentResolver::resolve_output_extent(
    const GraphModel& graph, int node_id,
    std::unordered_map<int, PixelSize>& cache) const {
  auto cached = cache.find(node_id);
  if (cached != cache.end()) {
    return cached->second;
  }

  PixelSize size{0, 0};
  const Node& node = graph.node(node_id);

  if (node.cached_output_high_precision) {
    const auto& buf = node.cached_output_high_precision->image_buffer;
    if (buf.width > 0 && buf.height > 0) {
      size = PixelSize{buf.width, buf.height};
      return cache[node_id] = size;
    }
  }

  const int width =
      as_int_flexible(node.runtime_parameters, "width",
                      as_int_flexible(node.parameters, "width", 0));
  const int height =
      as_int_flexible(node.runtime_parameters, "height",
                      as_int_flexible(node.parameters, "height", 0));
  if (width > 0 && height > 0) {
    size = PixelSize{width, height};
    return cache[node_id] = size;
  }

  for (const auto& edge : graph.upstream_edges(node_id)) {
    if (edge.kind != GraphTopologyEdgeKind::ImageInput ||
        edge.from_node_id < 0) {
      continue;
    }
    PixelSize parent_size =
        resolve_output_extent(graph, edge.from_node_id, cache);
    if (parent_size.width > 0 && parent_size.height > 0) {
      size = parent_size;
      break;
    }
  }

  return cache[node_id] = size;
}

}  // namespace ps
