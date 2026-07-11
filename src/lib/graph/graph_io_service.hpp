#pragma once

#include <filesystem>

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {

/**
 * @brief Loads and saves GraphModel topology through YAML files.
 *
 * The service owns no graph state. Load parses into temporary node storage and
 * replaces GraphModel nodes only after the complete YAML has validated; save
 * serializes a point-in-time graph traversal to the caller-provided path.
 *
 * @note Callers serialize access to GraphModel. Public frontends reach this
 * service only through the embedded Host adapter and internal Kernel facade.
 */
class GraphIOService {
 public:
  /**
   * @brief Transactionally replaces graph nodes from a YAML sequence.
   *
   * @param graph Graph whose nodes are replaced after full parse/validation.
   * @param yaml_path Source YAML file path.
   * @return Nothing.
   * @throws std::bad_alloc if parsing or temporary node storage exhausts
   * memory.
   * @throws GraphError for other file, YAML-root, duplicate-id, node, or
   * topology validation failures.
   * @note The existing graph remains unchanged until replace_nodes() receives
   * the fully parsed temporary map. BUILD_TESTING may compile an immutable YAML
   * tag failpoint immediately before real node conversion; production builds
   * compile out the probe and expose no callable test seam.
   */
  void load(GraphModel& graph, const std::filesystem::path& yaml_path) const;

  /**
   * @brief Serializes the graph's current nodes to a YAML sequence.
   *
   * @param graph Graph to serialize.
   * @param yaml_path Destination YAML file path.
   * @return Nothing.
   * @throws std::bad_alloc if YAML or path serialization exhausts memory.
   * @throws GraphError if the destination cannot be opened.
   * @throws std::exception for other stream or YAML serialization failures.
   * @note The service does not create parent directories or mutate graph
   * state.
   */
  void save(const GraphModel& graph,
            const std::filesystem::path& yaml_path) const;
};

}  // namespace ps
