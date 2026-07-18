#pragma once

/**
 * @file in_memory_graph_document_adapter.hpp
 * @brief Declares the detached graph-definition/model adapter.
 */

#include "graph/graph_definition.hpp"  // NOLINT(build/include_subdir)

namespace ps {

class GraphModel;
class Node;

/**
 * @brief Translates detached persistent definitions and private graph models.
 *
 * The adapter owns no graph, parser tree, file, cache, or thread. Complete
 * apply operations stage every Node in temporary storage and publish exactly
 * once through GraphModel::replace_nodes(). Capture copies only persistent
 * fields and never returns aliases to graph-owned storage.
 *
 * @note Callers must provide the existing graph-state serialization when the
 *       target model is already published. The adapter retains no references
 *       after a method returns.
 */
class InMemoryGraphDocumentAdapter {
 public:
  /**
   * @brief Stages and transactionally applies one complete graph definition.
   *
   * @param graph Model replaced only after definition and topology validation.
   * @param definition Detached ordered definition to apply.
   * @return Nothing.
   * @throws GraphError with `GraphErrc::InvalidParameter` for duplicate node
   *         ids or empty parameter-edge endpoint names,
   *         `GraphErrc::MissingDependency` for absent topology dependencies,
   *         or `GraphErrc::Cycle` for cyclic topology.
   * @throws std::bad_alloc when staging, validation, or topology construction
   *         cannot allocate.
   * @note Every definition node is materialized before the single
   *       GraphModel::replace_nodes() publication. Any failure preserves the
   *       prior nodes, topology, generation, and runtime state.
   */
  void apply(GraphModel& graph, const GraphDefinition& definition) const;

  /**
   * @brief Captures a deterministic persistent definition from one graph.
   *
   * @param graph Serialized model whose persistent node fields are copied.
   * @return Detached definition ordered by ascending node id.
   * @throws std::bad_alloc when copied strings, recursive parameters, or
   *         vectors cannot allocate.
   * @note Runtime parameters, computed outputs, revisions, ROI/LUT state,
   *       timing, dirty state, and cache results are excluded.
   */
  GraphDefinition capture(const GraphModel& graph) const;

  /**
   * @brief Materializes one private Node from a persistent definition.
   *
   * @param definition Detached node definition to validate and copy.
   * @return Node with persistent fields copied and all runtime fields at their
   *         defaults.
   * @throws GraphError with `GraphErrc::InvalidParameter` when a parameter
   *         edge has an empty source-output or destination-parameter name.
   * @throws std::bad_alloc when copied persistent storage cannot allocate.
   * @note This helper performs no topology validation and retains no reference
   *       to the definition.
   */
  Node materialize_node(const NodeDefinition& definition) const;

  /**
   * @brief Captures the persistent fields of one private Node.
   *
   * @param node Graph-owned node to copy while caller serialization is held.
   * @return Detached node definition containing no runtime state.
   * @throws std::bad_alloc when copied persistent storage cannot allocate.
   * @note The returned value owns every nested string and parameter value.
   */
  NodeDefinition capture_node(const Node& node) const;
};

}  // namespace ps
