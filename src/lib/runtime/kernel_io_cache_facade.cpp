/**
 * @file kernel_io_cache_facade.cpp
 * @brief Implements Kernel graph IO, timing, cache, and graph-clear facades.
 */
#include <exception>
#include <filesystem>
#include <string>

#include "runtime/kernel.hpp"

namespace ps {

std::optional<TimingCollector> Kernel::get_timing(const std::string& name) {
  return with_graph_state(
      name, [](GraphModel& graph) { return graph.timing_results; });
}

/**
 * @brief Reloads one existing graph from a YAML file and records load errors.
 *
 * @param name Graph session name to reload.
 * @param yaml_path Source YAML file path.
 * @return true when GraphIOService loads and replaces graph nodes; false when
 *         the graph is missing or reload fails.
 * @throws std::bad_alloc if reload execution or handled-failure LastError
 *         construction exhausts memory.
 * @note Missing sessions return false without creating LastError state. For an
 *       existing session, empty-path InvalidParameter, IO, syntax/schema,
 *       topology, and Unknown categories are recorded exactly. GraphIO parses
 *       and validates temporary ownership before GraphModel replacement, so
 *       every handled failure and propagated std::bad_alloc preserves the
 *       published nodes, topology adjacency/generation, runtime graph state,
 *       and session identity.
 */
bool Kernel::reload_graph_yaml(const std::string& name,
                               const std::string& yaml_path) {
  const std::filesystem::path path = yaml_path;
  return with_graph_state_last_error(name, "Reload graph YAML failed: ",
                                     [this, path](GraphModel& graph) {
                                       io_service_.load(graph, path);
                                       return true;
                                     })
      .value_or(false);
}

/** @copydoc Kernel::save_graph_yaml */
void Kernel::save_graph_yaml(const std::string& name,
                             const std::string& yaml_path) {
  const std::filesystem::path path = yaml_path;
  with_required_graph_state(name, [this, path](GraphModel& graph) {
    try {
      io_service_.save(graph, path);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const GraphError& error) {
      throw GraphError(GraphErrc::Io, error.what());
    } catch (const std::exception& error) {
      throw GraphError(GraphErrc::Io,
                       std::string("Graph YAML save failed: ") + error.what());
    }
  });
}

bool Kernel::clear_drive_cache(const std::string& name) {
  return with_graph_state(name,
                          [this](GraphModel& graph) {
                            cache_service_.clear_drive_cache(graph);
                            return true;
                          })
      .value_or(false);
}

bool Kernel::clear_memory_cache(const std::string& name) {
  return with_graph_state(name,
                          [this](GraphModel& graph) {
                            cache_service_.clear_memory_cache(graph);
                            return true;
                          })
      .value_or(false);
}

bool Kernel::clear_cache(const std::string& name) {
  return with_graph_state(name,
                          [this](GraphModel& graph) {
                            cache_service_.clear_cache(graph);
                            return true;
                          })
      .value_or(false);
}

bool Kernel::cache_all_nodes(const std::string& name,
                             const std::string& cache_precision) {
  return with_graph_state(name,
                          [this, cache_precision](GraphModel& graph) {
                            cache_service_.cache_all_nodes(graph,
                                                           cache_precision);
                            return true;
                          })
      .value_or(false);
}

bool Kernel::free_transient_memory(const std::string& name) {
  return with_graph_state(name,
                          [this](GraphModel& graph) {
                            cache_service_.free_transient_memory(graph);
                            return true;
                          })
      .value_or(false);
}

std::optional<GraphModel::DriveClearResult> Kernel::clear_drive_cache_stats(
    const std::string& name) {
  return with_graph_state(name, [this](GraphModel& graph) {
    return cache_service_.clear_drive_cache(graph);
  });
}

std::optional<GraphModel::MemoryClearResult> Kernel::clear_memory_cache_stats(
    const std::string& name) {
  return with_graph_state(name, [this](GraphModel& graph) {
    return cache_service_.clear_memory_cache(graph);
  });
}

std::optional<GraphModel::CacheSaveResult> Kernel::cache_all_nodes_stats(
    const std::string& name, const std::string& cache_precision) {
  return with_graph_state(name, [this, cache_precision](GraphModel& graph) {
    return cache_service_.cache_all_nodes(graph, cache_precision);
  });
}

std::optional<GraphModel::MemoryClearResult>
Kernel::free_transient_memory_stats(const std::string& name) {
  return with_graph_state(name, [this](GraphModel& graph) {
    return cache_service_.free_transient_memory(graph);
  });
}

std::optional<GraphModel::DiskSyncResult> Kernel::synchronize_disk_cache_stats(
    const std::string& name, const std::string& cache_precision) {
  return with_graph_state(name, [this, cache_precision](GraphModel& graph) {
    return cache_service_.synchronize_disk_cache(graph, cache_precision);
  });
}

bool Kernel::synchronize_disk_cache(const std::string& name,
                                    const std::string& cache_precision) {
  return with_graph_state(name,
                          [this, cache_precision](GraphModel& graph) {
                            cache_service_.synchronize_disk_cache(
                                graph, cache_precision);
                            return true;
                          })
      .value_or(false);
}

bool Kernel::clear_graph(const std::string& name) {
  return with_graph_state(name,
                          [](GraphModel& graph) {
                            graph.clear();
                            return true;
                          })
      .value_or(false);
}

}  // namespace ps
