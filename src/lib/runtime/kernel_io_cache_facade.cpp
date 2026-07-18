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
 * @brief Reloads one existing graph document and records load errors.
 *
 * @param name Graph session name to reload.
 * @param document_path Source document path.
 * @return true when GraphIOService loads and replaces graph nodes; false when
 *         the graph is missing or reload fails.
 * @throws std::bad_alloc if reload execution or handled-failure LastError
 *         construction exhausts memory.
 * @note Missing sessions return false without creating LastError state. For an
 *       existing session, empty-path InvalidParameter, IO, syntax/schema,
 *       topology, and Unknown categories are recorded exactly. Embedded Host
 *       retains lifecycle admission through this call and its later LastError
 *       translation; direct callers racing close require equivalent runtime-
 *       lifetime admission. GraphIO parses and validates temporary ownership
 *       before GraphModel replacement, so every handled failure and propagated
 *       std::bad_alloc preserves the published nodes, topology adjacency/
 *       generation, runtime graph state, and session identity.
 */
bool Kernel::reload_graph_document(const std::string& name,
                                   const std::string& document_path) {
  const std::filesystem::path path = document_path;
  return with_graph_state_last_error(name, "Reload graph document failed: ",
                                     [this, path](GraphModel& graph) {
                                       io_service_.load(graph, path);
                                       return true;
                                     })
      .value_or(false);
}

/**
 * @brief Saves one required graph through its serialized graph-state lane.
 *
 * @param name Graph session name to save.
 * @param document_path Destination path copied into the worker request.
 * @return Nothing.
 * @throws GraphError with `GraphErrc::NotFound` when required-session
 *         resolution fails, or `GraphErrc::Io` when recoverable node
 *         document serialization or destination
 *         preparation/open/write/flush/close fails.
 * @throws std::bad_alloc if graph-state submission, document serialization,
 *         path handling, or diagnostic construction exhausts memory.
 * @throws std::exception for other graph-state submission or future failures.
 * @note with_required_graph_state() resolves and executes the save in the
 *       session's GraphStateExecutor. The graph, runtime state, and session
 *       owner remain unchanged on every outcome. The destination is written
 *       directly: pre-open failure preserves existing bytes, but post-open
 *       failure may leave created, truncated, or partial output.
 */
void Kernel::save_graph_document(const std::string& name,
                                 const std::string& document_path) {
  const std::filesystem::path path = document_path;
  with_required_graph_state(name, [this, path](GraphModel& graph) {
    try {
      io_service_.save(graph, path);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const GraphError& error) {
      throw GraphError(GraphErrc::Io, error.what());
    } catch (const std::exception& error) {
      throw GraphError(
          GraphErrc::Io,
          std::string("Graph document save failed: ") + error.what());
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
