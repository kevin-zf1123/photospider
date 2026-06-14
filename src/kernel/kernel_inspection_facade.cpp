/**
 * @file kernel_inspection_facade.cpp
 * @brief Implements Kernel inspection, traversal, node YAML, runtime-event, and
 * graph-state snapshot facades.
 */
#include <filesystem>
#include <sstream>
#include <stdexcept>

#include "kernel/kernel.hpp"

namespace ps {

std::optional<DependencyTree> Kernel::dependency_tree(
    const std::string& name, std::optional<int> node_id,
    bool include_metadata) {
  return with_graph_state(
      name, [this, node_id, include_metadata](GraphModel& graph) {
        if (node_id) {
          return inspect_service_.dependency_tree(graph, *node_id,
                                                  include_metadata);
        }
        return inspect_service_.dependency_tree(graph, include_metadata);
      });
}

std::optional<GraphNodeInspectInfo> Kernel::inspect_node(
    const std::string& name, int node_id) {
  return with_graph_state(
      name, [this, node_id](GraphModel& graph) -> GraphNodeInspectInfo {
        const Node* node_ptr = graph.find_node(node_id);
        if (!node_ptr) {
          throw std::runtime_error("node not found");
        }
        return inspect_service_.inspect_node(*node_ptr);
      });
}

std::optional<GraphInspectionSnapshot> Kernel::inspect_graph(
    const std::string& name) {
  return with_graph_state(name, [this](GraphModel& graph) {
    return inspect_service_.inspect_graph(graph);
  });
}

std::optional<Kernel::LastError> Kernel::last_error(
    const std::string& name) const {
  auto it = last_error_.find(name);
  if (it == last_error_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::vector<int>> Kernel::ending_nodes(const std::string& name) {
  return with_graph_state(name, [this](GraphModel& graph) {
    return traversal_service_.ending_nodes(graph);
  });
}

std::optional<std::vector<int>> Kernel::topo_postorder_from(
    const std::string& name, int end_node_id) {
  return with_graph_state(name, [this, end_node_id](GraphModel& graph) {
    return traversal_service_.topo_postorder_from(graph, end_node_id);
  });
}

std::optional<std::map<int, std::vector<int>>> Kernel::traversal_orders(
    const std::string& name) {
  return with_graph_state(name, [this](GraphModel& graph) {
    std::map<int, std::vector<int>> out;
    for (int end : traversal_service_.ending_nodes(graph)) {
      out[end] = traversal_service_.topo_postorder_from(graph, end);
    }
    return out;
  });
}

std::optional<std::map<int, std::vector<Kernel::TraversalNodeInfo>>>
Kernel::traversal_details(const std::string& name) {
  return with_graph_state(name, [this](GraphModel& graph) {
    std::map<int, std::vector<Kernel::TraversalNodeInfo>> result;
    for (int end : traversal_service_.ending_nodes(graph)) {
      auto nodes = traversal_details_for_end(graph, end);
      if (nodes) {
        result[end] = std::move(*nodes);
      }
    }
    return result;
  });
}

bool Kernel::has_node_disk_cache(const GraphModel& graph,
                                 const Node& node) const {
  for (const auto& cache : node.caches) {
    std::filesystem::path cache_file =
        cache_service_.node_cache_dir(graph, node.id) / cache.location;
    std::filesystem::path meta_file = cache_file;
    meta_file.replace_extension(".yml");
    if (std::filesystem::exists(cache_file) ||
        std::filesystem::exists(meta_file)) {
      return true;
    }
  }
  return false;
}

Kernel::TraversalNodeInfo Kernel::build_traversal_node_info(
    const GraphModel& graph, int node_id) const {
  const auto& node = graph.node(node_id);
  const bool has_memory_cache = node.cached_output_high_precision.has_value() ||
                                node.cached_output_real_time.has_value();
  return Kernel::TraversalNodeInfo{node.id, node.name, has_memory_cache,
                                   has_node_disk_cache(graph, node)};
}

std::optional<std::vector<Kernel::TraversalNodeInfo>>
Kernel::traversal_details_for_end(GraphModel& graph, int end_node_id) const {
  try {
    auto order = traversal_service_.topo_postorder_from(graph, end_node_id);
    std::vector<Kernel::TraversalNodeInfo> nodes;
    nodes.reserve(order.size());
    for (int node_id : order) {
      nodes.push_back(build_traversal_node_info(graph, node_id));
    }
    return nodes;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::vector<int>> Kernel::trees_containing_node(
    const std::string& name, int node_id) {
  return with_graph_state(name, [this, node_id](GraphModel& graph) {
    return traversal_service_.get_trees_containing_node(graph, node_id);
  });
}

std::optional<std::vector<int>> Kernel::list_node_ids(const std::string& name) {
  return with_graph_state(name,
                          [](GraphModel& graph) { return graph.node_ids(); });
}

std::optional<std::string> Kernel::get_node_yaml(const std::string& name,
                                                 int node_id) {
  return with_graph_state(name, [node_id](GraphModel& graph) {
    if (!graph.has_node(node_id)) {
      throw std::runtime_error("node not found");
    }
    auto yaml = graph.node(node_id).to_yaml();
    std::stringstream ss;
    ss << yaml;
    return ss.str();
  });
}

bool Kernel::set_node_yaml(const std::string& name, int node_id,
                           const std::string& yaml_text) {
  return with_graph_state(name,
                          [node_id, yaml_text](GraphModel& graph) {
                            if (!graph.has_node(node_id)) {
                              return false;
                            }
                            YAML::Node root = YAML::Load(yaml_text);
                            ps::Node updated = ps::Node::from_yaml(root);
                            updated.id = node_id;
                            graph.replace_node(updated);
                            return true;
                          })
      .value_or(false);
}

std::optional<std::vector<GraphEventService::ComputeEvent>>
Kernel::drain_compute_events(const std::string& name) {
  return with_runtime(name, [](GraphRuntime& runtime) {
    return runtime.drain_compute_events_now();
  });
}

std::optional<std::vector<GraphRuntime::SchedulerEvent>>
Kernel::scheduler_trace(const std::string& name) {
  return with_runtime(
      name, [](GraphRuntime& runtime) { return runtime.get_scheduler_log(); });
}

std::optional<std::string> Kernel::dirty_region_snapshot_debug(
    const std::string& name) {
  auto result = with_graph_state(name, [](GraphModel& graph) {
    return graph.last_dirty_region_snapshot_debug;
  });
  if (!result) {
    return std::nullopt;
  }
  return *result;
}

std::optional<compute::DirtyRegionSnapshot> Kernel::dirty_region_snapshot(
    const std::string& name) {
  auto result = with_graph_state(
      name, [](GraphModel& graph) { return graph.last_dirty_region_snapshot; });
  if (!result) {
    return std::nullopt;
  }
  return *result;
}

std::optional<double> Kernel::get_last_io_time(const std::string& name) {
  return with_graph_state(
      name, [](GraphModel& graph) { return graph.total_io_time_ms.load(); });
}

}  // namespace ps
