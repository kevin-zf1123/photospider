#pragma once

/**
 * @file graph_io_service.hpp
 * @brief Declares format-neutral graph-document/model orchestration.
 */

#include <filesystem>
#include <memory>
#include <string>

#include "graph/graph_document_reader.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_document_writer.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"            // NOLINT(build/include_subdir)

namespace ps {

/**
 * @class GraphIOService
 * @brief Orchestrates detached graph documents around `GraphModel`.
 *
 * The service owns no graph state or representation implementation. Load asks
 * the retained reader for a detached `GraphDefinition` and publishes it
 * through `InMemoryGraphDocumentAdapter`; save captures a detached definition
 * before calling the retained writer. Single-node text operations cross the
 * same injected boundary.
 *
 * @note Callers serialize access to each `GraphModel`. Immutable reader and
 *       writer contracts may be shared by independent graph-state lanes, and
 *       remain retained for this service's complete lifetime.
 */
class GraphIOService {
 public:
  /**
   * @brief Constructs graph IO with required representation dependencies.
   *
   * @param reader Shared owner used for complete and single-node reads.
   * @param writer Shared owner used for complete and single-node writes.
   * @throws std::invalid_argument if either owner is empty.
   * @note Construction performs no graph, parser, filesystem, execution, or
   *       session operation.
   */
  GraphIOService(std::shared_ptr<const GraphDocumentReader> reader,
                 std::shared_ptr<const GraphDocumentWriter> writer);

  /**
   * @brief Transactionally replaces graph nodes from one external document.
   *
   * @param graph Graph whose nodes are replaced after full read/validation.
   * @param document_path Source path selected by graph lifecycle policy.
   * @return Nothing.
   * @throws std::bad_alloc if reading, staging, topology preparation, or
   *         diagnostics exhaust memory.
   * @throws GraphError with `GraphErrc::InvalidParameter` for an empty path,
   *         the reader's exact category for read/representation failure,
   *         `GraphErrc::InvalidYaml` for detached-definition schema rejection,
   *         `GraphErrc::MissingDependency` or `GraphErrc::Cycle` for topology
   *         rejection, and `GraphErrc::Unknown` for unexpected apply failure.
   * @throws std::exception for reader failures not normalized by its contract.
   * @note The existing graph, topology index, topology generation, and runtime
   *       state remain unchanged until the fully staged replacement passes
   *       topology validation and publishes once.
   */
  void load(GraphModel& graph,
            const std::filesystem::path& document_path) const;

  /**
   * @brief Captures persistent graph state and writes one external document.
   *
   * @param graph Graph whose persistent definition is captured.
   * @param document_path Destination selected by the caller.
   * @return Nothing.
   * @throws std::bad_alloc if capture, path, representation, or destination
   *         storage exhausts memory.
   * @throws GraphError according to the injected writer contract.
   * @throws std::exception for writer failures not represented by GraphError.
   * @note Capture completes before the writer is called. This operation does
   *       not mutate graph, topology, runtime, or session-owner state; concrete
   *       destination side effects belong to the writer implementation.
   */
  void save(const GraphModel& graph,
            const std::filesystem::path& document_path) const;

  /**
   * @brief Parses one detached node through the injected reader.
   *
   * @param document_text Representation text borrowed for this call.
   * @return Deep-owned persistent node definition.
   * @throws GraphError according to the injected reader contract.
   * @throws std::bad_alloc when parsing or detached-value allocation fails.
   * @throws std::exception for reader failures not represented by GraphError.
   * @note Node identity override and graph publication remain caller-owned.
   */
  NodeDefinition read_node_document(const std::string& document_text) const;

  /**
   * @brief Emits one detached node through the injected writer.
   *
   * @param definition Persistent node fields borrowed for this call.
   * @return Owned representation text.
   * @throws GraphError according to the injected writer contract.
   * @throws std::bad_alloc when emission or output allocation fails.
   * @throws std::exception for writer failures not represented by GraphError.
   * @note The writer retains no reference to the definition after return.
   */
  std::string write_node_document(const NodeDefinition& definition) const;

 private:
  /**
   * @brief Retained immutable complete/node document reader.
   * @note Non-null after successful construction and released with the service.
   */
  std::shared_ptr<const GraphDocumentReader> reader_;

  /**
   * @brief Retained immutable complete/node document writer.
   * @note Non-null after successful construction and released with the service.
   */
  std::shared_ptr<const GraphDocumentWriter> writer_;
};

}  // namespace ps
