#pragma once

#include <filesystem>

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {

/**
 * @brief Orchestrates YAML files around the detached graph-document seam.
 *
 * The service owns no graph state. Load parses a detached GraphDefinition and
 * applies it through InMemoryGraphDocumentAdapter only after complete
 * conversion; save captures a detached definition before serializing it to the
 * caller-provided path.
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
   * @throws std::bad_alloc if parsing, diagnostics, definition conversion, or
   *         temporary storage exhausts memory.
   * @throws GraphError with `GraphErrc::InvalidParameter` for an empty path,
   *         `GraphErrc::Io` for an inaccessible source,
   *         `GraphErrc::InvalidYaml` for parser/root/duplicate-id/node-schema
   *         rejection, `GraphErrc::MissingDependency` or `GraphErrc::Cycle`
   *         for topology rejection, and `GraphErrc::Unknown` for unexpected
   *         ingestion failures.
   * @note The existing graph, topology index, topology generation, and runtime
   *       state remain unchanged until replace_nodes() receives the fully
   *       staged definition map and completes topology validation.
   *       BUILD_TESTING may compile immutable YAML-tag failpoints immediately
   *       before real definition conversion; production builds compile out the
   *       probes and expose no callable test seam.
   */
  void load(GraphModel& graph, const std::filesystem::path& yaml_path) const;

  /**
   * @brief Captures and serializes current persistent graph state to YAML.
   *
   * @param graph Graph to serialize.
   * @param yaml_path Destination YAML file path.
   * @return Nothing.
   * @throws std::bad_alloc if node, YAML, path, or stream storage exhausts
   *         memory.
   * @throws GraphError with `GraphErrc::Io` if destination preparation/open or
   *         write, flush, or close reports a recoverable failure.
   * @throws std::exception for definition/YAML serialization failures outside
   *         those stream phases.
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
