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
   * @throws std::bad_alloc if parsing, diagnostics, node conversion, or
   *         temporary storage exhausts memory.
   * @throws GraphError with `GraphErrc::InvalidParameter` for an empty path,
   *         `GraphErrc::Io` for an inaccessible source,
   *         `GraphErrc::InvalidYaml` for parser/root/duplicate-id/node-schema
   *         rejection, `GraphErrc::MissingDependency` or `GraphErrc::Cycle`
   *         for topology rejection, and `GraphErrc::Unknown` for unexpected
   *         ingestion failures.
   * @note The existing graph, topology index, topology generation, and runtime
   *       state remain unchanged until replace_nodes() receives the fully
   *       parsed temporary map and completes topology validation.
   *       BUILD_TESTING may compile immutable YAML-tag failpoints immediately
   *       before real node conversion; production builds compile out the
   *       probes and expose no callable test seam.
   */
  void load(GraphModel& graph, const std::filesystem::path& yaml_path) const;

  /**
   * @brief Serializes the graph's current nodes to a YAML sequence.
   *
   * @param graph Graph to serialize.
   * @param yaml_path Destination YAML file path.
   * @return Nothing.
   * @throws std::bad_alloc if node, YAML, path, or stream storage exhausts
   *         memory.
   * @throws GraphError with `GraphErrc::Io` if destination preparation/open or
   *         write, flush, or close reports a recoverable failure.
   * @throws std::exception for YAML node serialization failures outside those
   *         stream phases.
   * @note The service does not create parent directories or mutate graph,
   *       topology, runtime, or session-owner state on success or failure. It
   *       writes directly to the supplied path rather than using an atomic
   *       replacement. A failure observed before destination open preserves
   *       existing bytes, while a post-open failure may leave a created,
   *       truncated, or partially written destination.
   */
  void save(const GraphModel& graph,
            const std::filesystem::path& yaml_path) const;
};

}  // namespace ps
