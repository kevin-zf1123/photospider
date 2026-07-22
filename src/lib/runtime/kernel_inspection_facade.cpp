/**
 * @file kernel_inspection_facade.cpp
 * @brief Implements Kernel inspection, traversal, node-document, runtime-event,
 * and graph-state snapshot facades.
 */
#include <filesystem>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "graph/in_memory_graph_document_adapter.hpp"  // NOLINT(build/include_subdir)
#include "runtime/kernel.hpp"
#if defined(PHOTOSPIDER_INTERNAL_REQUIRED_TARGET_TESTING)
#include "runtime/kernel_required_target_test_access.hpp"
#endif

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

/** @copydoc Kernel::last_error */
std::optional<Kernel::LastError> Kernel::last_error(
    const std::string& name) const {
  return copy_last_error(name);
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
  const bool has_memory_cache = node.cached_output_high_precision.has_value();
  return Kernel::TraversalNodeInfo{node.id, node.name, has_memory_cache,
                                   has_node_disk_cache(graph, node)};
}

/**
 * @brief Builds traversal diagnostics for one requested end node.
 *
 * @param graph Graph whose topological postorder and cache state are inspected.
 * @param end_node_id End node that anchors the traversal.
 * @return Ordered diagnostics, or nullopt for recoverable traversal failures.
 * @throws std::bad_alloc if traversal or diagnostic storage exhausts memory.
 * @note Non-resource traversal failures preserve the existing optional-result
 * contract and are reduced to nullopt for the internal inspection facade.
 */
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
  } catch (const std::bad_alloc&) {
    throw;
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

/** @copydoc Kernel::get_node_document */
std::optional<std::string> Kernel::get_node_document(const std::string& name,
                                                     int node_id) {
  return with_graph_state(name, [this, node_id](GraphModel& graph) {
    if (!graph.has_node(node_id)) {
      throw std::runtime_error("node not found");
    }
    const InMemoryGraphDocumentAdapter adapter;
    const NodeDefinition definition = adapter.capture_node(graph.node(node_id));
    return io_service_.write_node_document(definition);
  });
}

/** @copydoc Kernel::set_node_document */
void Kernel::set_node_document(const std::string& name, int node_id,
                               const std::string& document_text) {
  with_required_graph_state(name, [this, node_id,
                                   document_text](GraphModel& graph) {
    if (!graph.has_node(node_id)) {
      throw GraphError(GraphErrc::NotFound,
                       "Node " + std::to_string(node_id) + " not in graph.");
    }
#if defined(PHOTOSPIDER_INTERNAL_REQUIRED_TARGET_TESTING)
    testing::notify_required_target_test_hook(
        testing::RequiredTargetTestEvent::SetNodeDocumentTargetResolved);
#endif
    try {
      NodeDefinition definition = io_service_.read_node_document(document_text);
      definition.id = node_id;
      const InMemoryGraphDocumentAdapter adapter;
      const Node updated = adapter.materialize_node(definition);
      graph.replace_node(updated);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const GraphError& error) {
      throw GraphError(GraphErrc::InvalidYaml, error.what());
    }
  });
}

/** @copydoc Kernel::drain_compute_events */
std::optional<ComputeEventBatch> Kernel::drain_compute_events(
    const std::string& name, std::size_t limit) {
  auto it = graphs_.find(name);
  if (it == graphs_.end()) {
    return std::nullopt;
  }
  return it->second->drain_compute_events_now(limit);
}

/** @copydoc Kernel::execution_trace */
std::optional<GraphRuntime::ExecutionEventPage> Kernel::execution_trace(
    const std::string& name, uint64_t after_sequence, std::size_t limit) {
  auto it = graphs_.find(name);
  if (it == graphs_.end()) {
    return std::nullopt;
  }
  return it->second->execution_trace_page(after_sequence, limit);
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

/**
 * @brief Copies the latest backend planning summary for one graph.
 *
 * @param name Loaded graph/session name.
 * @return Latest summary, or nullopt when the graph is missing or no summary
 *         has been recorded.
 * @throws std::bad_alloc if summary string/vector members allocate on copy.
 * @note The copy is made through graph-state serialization so callers never
 *       observe mutable graph-owned diagnostic storage directly.
 */
std::optional<compute::ComputePlanSummary> Kernel::compute_planning_snapshot(
    const std::string& name) {
  auto result = with_graph_state(
      name, [](GraphModel& graph) { return graph.last_compute_plan_summary; });
  if (!result) {
    return std::nullopt;
  }
  return *result;
}

/**
 * @brief Copies bounded backend planning summary history for one graph.
 *
 * @param name Loaded graph/session name.
 * @return Summary history, or nullopt when the graph is missing.
 * @throws std::bad_alloc if vector or summary copies allocate.
 * @note Empty history is a successful loaded-graph state before compute has
 *       produced any planning summary.
 */
std::optional<std::vector<compute::ComputePlanSummary>>
Kernel::recent_compute_planning_snapshots(const std::string& name) {
  return with_graph_state(name, [](GraphModel& graph) {
    return graph.recent_compute_plan_summaries;
  });
}

std::optional<double> Kernel::get_last_io_time(const std::string& name) {
  return with_graph_state(
      name, [](GraphModel& graph) { return graph.total_io_time_ms.load(); });
}

}  // namespace ps
