#include "graph/in_memory_graph_document_adapter.hpp"

/**
 * @file in_memory_graph_document_adapter.cpp
 * @brief Implements detached graph-definition/model translation.
 */

#include <string>
#include <utility>
#include <vector>

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"         // NOLINT(build/include_subdir)

namespace ps {

namespace {

/**
 * @brief Validates definition-only schema rules for one parameter edge.
 *
 * @param node_id Node identifier used for diagnostic context.
 * @param input Parameter edge whose required names are checked.
 * @return Nothing.
 * @throws GraphError with `GraphErrc::InvalidParameter` when either required
 *         endpoint name is empty.
 * @throws std::bad_alloc if diagnostic construction cannot allocate.
 * @note Topology identity validation remains owned by
 *       GraphModel::replace_nodes().
 */
void validate_parameter_input(int node_id, const ParameterInput& input) {
  if (input.from_output_name.empty() || input.to_parameter_name.empty()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Parameter input for node " + std::to_string(node_id) +
                         " is missing required fields.");
  }
}

}  // namespace

/** @copydoc InMemoryGraphDocumentAdapter::apply */
void InMemoryGraphDocumentAdapter::apply(
    GraphModel& graph, const GraphDefinition& definition) const {
  GraphModel::NodeMap staged_nodes;
  staged_nodes.reserve(definition.nodes.size());
  for (const NodeDefinition& node_definition : definition.nodes) {
    Node node = materialize_node(node_definition);
    const int node_id = node.id;
    const auto inserted = staged_nodes.emplace(node_id, std::move(node));
    if (!inserted.second) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Duplicate node id " + std::to_string(node_id) +
                           " in graph definition.");
    }
  }
  graph.replace_nodes(std::move(staged_nodes));
}

/** @copydoc InMemoryGraphDocumentAdapter::capture */
GraphDefinition InMemoryGraphDocumentAdapter::capture(
    const GraphModel& graph) const {
  GraphDefinition definition;
  const std::vector<int> node_ids = graph.node_ids();
  definition.nodes.reserve(node_ids.size());
  for (int node_id : node_ids) {
    definition.nodes.push_back(capture_node(graph.node(node_id)));
  }
  return definition;
}

/** @copydoc InMemoryGraphDocumentAdapter::materialize_node */
Node InMemoryGraphDocumentAdapter::materialize_node(
    const NodeDefinition& definition) const {
  for (const ParameterInput& input : definition.parameter_inputs) {
    validate_parameter_input(definition.id, input);
  }

  Node node;
  node.id = definition.id;
  node.name = definition.name;
  node.type = definition.type;
  node.subtype = definition.subtype;
  node.image_inputs = definition.image_inputs;
  node.parameter_inputs = definition.parameter_inputs;
  node.parameters = definition.parameters;
  node.outputs = definition.outputs;
  node.caches = definition.caches;
  node.preserved = definition.preserved;
  return node;
}

/** @copydoc InMemoryGraphDocumentAdapter::capture_node */
NodeDefinition InMemoryGraphDocumentAdapter::capture_node(
    const Node& node) const {
  NodeDefinition definition;
  definition.id = node.id;
  definition.name = node.name;
  definition.type = node.type;
  definition.subtype = node.subtype;
  definition.image_inputs = node.image_inputs;
  definition.parameter_inputs = node.parameter_inputs;
  definition.parameters = node.parameters;
  definition.outputs = node.outputs;
  definition.caches = node.caches;
  definition.preserved = node.preserved;
  return definition;
}

}  // namespace ps
