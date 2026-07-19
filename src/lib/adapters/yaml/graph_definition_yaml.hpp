#pragma once

/**
 * @file graph_definition_yaml.hpp
 * @brief Declares YAML-adapter translation for detached graph definitions.
 */

#include <yaml-cpp/yaml.h>

#include "graph/graph_definition.hpp"  // NOLINT(build/include_subdir)

namespace ps::adapters::yaml::internal {

/**
 * @brief Converts one YAML node mapping into a detached node definition.
 *
 * @param yaml_node Mapping containing persistent graph-node fields.
 * @return Deep-owned definition that retains no alias to yaml_node.
 * @throws YAML::Exception when a field has an incompatible representation or
 *         scalar conversion, including numeric overflow.
 * @throws GraphError with `GraphErrc::InvalidParameter` when a static or
 *         output parameter value cannot be normalized.
 * @throws std::bad_alloc when strings, recursive values, or vectors cannot
 *         allocate.
 * @note Definition-only endpoint validation is deferred to
 *       InMemoryGraphDocumentAdapter so every input format shares it.
 */
NodeDefinition node_definition_from_yaml(const YAML::Node& yaml_node);

/**
 * @brief Converts one detached node definition into a YAML mapping.
 *
 * @param definition Persistent node value to serialize.
 * @return Independent YAML mapping using the established graph schema.
 * @throws YAML::Exception when yaml-cpp cannot construct or assign a value.
 * @throws std::invalid_argument when a recursive ParameterValue reports an
 *         unknown kind.
 * @throws std::bad_alloc when YAML/container storage cannot allocate.
 * @note Runtime state cannot be emitted because NodeDefinition contains none.
 */
YAML::Node node_definition_to_yaml(const NodeDefinition& definition);

/**
 * @brief Converts a YAML sequence into an ordered graph definition.
 *
 * @param yaml_root Complete YAML root expected to be a node sequence.
 * @return Deep-owned definition preserving source sequence order.
 * @throws GraphError with `GraphErrc::InvalidParameter` when yaml_root is not a
 *         sequence or a parameter document cannot be normalized.
 * @throws YAML::Exception when a node field cannot be represented.
 * @throws std::bad_alloc when definition storage cannot allocate.
 * @throws std::runtime_error from deterministic test-only conversion probes
 *         compiled under PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING.
 * @note Duplicate ids and parameter-edge endpoint names are intentionally
 *       validated by InMemoryGraphDocumentAdapter.
 */
GraphDefinition graph_definition_from_yaml(const YAML::Node& yaml_root);

/**
 * @brief Converts a complete graph definition into a YAML sequence.
 *
 * @param definition Ordered detached definition to serialize.
 * @return Independent YAML sequence in definition order.
 * @throws YAML::Exception when yaml-cpp cannot construct or assign a value.
 * @throws std::invalid_argument when a recursive ParameterValue reports an
 *         unknown kind.
 * @throws std::bad_alloc when YAML/container storage cannot allocate.
 * @note GraphIO captures definitions in ascending node-id order, so the product
 *       save path remains deterministic.
 */
YAML::Node graph_definition_to_yaml(const GraphDefinition& definition);

}  // namespace ps::adapters::yaml::internal
