#include "graph/graph_definition_yaml.hpp"

/**
 * @file graph_definition_yaml.cpp
 * @brief Implements private YAML translation for detached graph definitions.
 */

#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/parameter_value_adapter.hpp"

namespace ps {

namespace {

/**
 * @brief Serializes image-input declarations in their stable input order.
 *
 * @param inputs Persistent image edges to serialize.
 * @return YAML sequence using the established graph schema.
 * @throws YAML::Exception when yaml-cpp assignment fails.
 * @throws std::bad_alloc when YAML storage cannot allocate.
 * @note Declaration order is preserved and no input storage is retained.
 */
YAML::Node image_inputs_to_yaml(const std::vector<ImageInput>& inputs) {
  YAML::Node result(YAML::NodeType::Sequence);
  for (const ImageInput& input : inputs) {
    YAML::Node item;
    item["from_node_id"] = input.from_node_id;
    if (input.from_output_name != "image") {
      item["from_output_name"] = input.from_output_name;
    }
    result.push_back(item);
  }
  return result;
}

/**
 * @brief Serializes parameter-input declarations in effective-merge order.
 *
 * @param inputs Persistent parameter edges to serialize.
 * @return YAML sequence using the established graph schema.
 * @throws YAML::Exception when yaml-cpp assignment fails.
 * @throws std::bad_alloc when YAML storage cannot allocate.
 * @note Effective-merge order is preserved and no input storage is retained.
 */
YAML::Node parameter_inputs_to_yaml(const std::vector<ParameterInput>& inputs) {
  YAML::Node result(YAML::NodeType::Sequence);
  for (const ParameterInput& input : inputs) {
    YAML::Node item;
    item["from_node_id"] = input.from_node_id;
    item["from_output_name"] = input.from_output_name;
    item["to_parameter_name"] = input.to_parameter_name;
    result.push_back(item);
  }
  return result;
}

/**
 * @brief Serializes persistent output-port descriptors.
 *
 * @param outputs Output descriptors to serialize.
 * @return YAML sequence preserving descriptor order and optional presence.
 * @throws YAML::Exception when yaml-cpp assignment fails.
 * @throws std::invalid_argument when an output parameter has an unknown kind.
 * @throws std::bad_alloc when recursive YAML storage cannot allocate.
 * @note An engaged null ParameterValue emits an explicit YAML null; an empty
 *       optional omits output_parameters.
 */
YAML::Node output_ports_to_yaml(const std::vector<OutputPort>& outputs) {
  YAML::Node result(YAML::NodeType::Sequence);
  for (const OutputPort& output : outputs) {
    YAML::Node item;
    item["output_id"] = output.output_id;
    item["output_type"] = output.output_type;
    if (output.output_parameters.has_value()) {
      item["output_parameters"] =
          core::parameter_value_to_yaml(*output.output_parameters);
    }
    result.push_back(item);
  }
  return result;
}

/**
 * @brief Serializes persistent external-cache descriptors.
 *
 * @param caches Cache descriptors to serialize.
 * @return YAML sequence preserving descriptor order.
 * @throws YAML::Exception when yaml-cpp assignment fails.
 * @throws std::bad_alloc when YAML storage cannot allocate.
 * @note No descriptor storage or filesystem ownership is retained.
 */
YAML::Node caches_to_yaml(const std::vector<CacheEntry>& caches) {
  YAML::Node result(YAML::NodeType::Sequence);
  for (const CacheEntry& cache : caches) {
    YAML::Node item;
    item["cache_type"] = cache.cache_type;
    item["location"] = cache.location;
    result.push_back(item);
  }
  return result;
}

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Injects deterministic failures at real definition conversion.
 *
 * @param yaml_node Sequence item about to become a NodeDefinition.
 * @return Nothing.
 * @throws std::bad_alloc when yaml_node carries the resource probe tag.
 * @throws std::runtime_error when yaml_node carries the unexpected-failure
 *         probe tag.
 * @note The immutable YAML tags keep the test probe thread-safe. Production
 *       builds compile this helper and every call site out.
 */
void throw_if_graph_load_failure_probe(const YAML::Node& yaml_node) {
  if (yaml_node.Tag() == "!photospider-test-reload-bad-alloc") {
    throw std::bad_alloc{};
  }
  if (yaml_node.Tag() == "!photospider-test-load-unknown") {
    throw std::runtime_error("injected unexpected graph load failure");
  }
}
#endif

}  // namespace

/** @copydoc node_definition_from_yaml */
NodeDefinition node_definition_from_yaml(const YAML::Node& yaml_node) {
  NodeDefinition definition;
  definition.id = yaml_node["id"].as<int>();
  definition.name = yaml_node["name"].as<std::string>("");
  definition.type = yaml_node["type"].as<std::string>("");
  definition.subtype = yaml_node["subtype"].as<std::string>("");
  definition.preserved = yaml_node["preserved"].as<bool>(false);

  if (const YAML::Node inputs = yaml_node["image_inputs"]; inputs.IsDefined()) {
    for (const YAML::Node& item : inputs) {
      ImageInput input;
      input.from_node_id = item["from_node_id"].as<int>(-1);
      input.from_output_name =
          item["from_output_name"].as<std::string>("image");
      definition.image_inputs.push_back(std::move(input));
    }
  }

  if (const YAML::Node inputs = yaml_node["parameter_inputs"];
      inputs.IsDefined()) {
    for (const YAML::Node& item : inputs) {
      ParameterInput input;
      input.from_node_id = item["from_node_id"].as<int>(-1);
      input.from_output_name = item["from_output_name"].as<std::string>("");
      input.to_parameter_name = item["to_parameter_name"].as<std::string>("");
      definition.parameter_inputs.push_back(std::move(input));
    }
  }

  if (const YAML::Node parameters = yaml_node["parameters"];
      parameters.IsDefined()) {
    try {
      definition.parameters = core::parameter_map_from_yaml(parameters);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const YAML::Exception&) {
      throw;
    } catch (const std::invalid_argument& error) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Invalid parameters for node " +
                           std::to_string(definition.id) + ": " + error.what());
    }
  }

  if (const YAML::Node outputs = yaml_node["outputs"]; outputs.IsDefined()) {
    for (const YAML::Node& item : outputs) {
      OutputPort output;
      output.output_id = item["output_id"].as<int>(-1);
      output.output_type = item["output_type"].as<std::string>("");
      const YAML::Node output_parameters = item["output_parameters"];
      if (output_parameters.IsDefined()) {
        try {
          output.output_parameters =
              core::parameter_value_from_yaml(output_parameters);
        } catch (const std::bad_alloc&) {
          throw;
        } catch (const YAML::Exception&) {
          throw;
        } catch (const std::invalid_argument& error) {
          throw GraphError(GraphErrc::InvalidParameter,
                           "Invalid output parameters for node " +
                               std::to_string(definition.id) + ": " +
                               error.what());
        }
      }
      definition.outputs.push_back(std::move(output));
    }
  }

  if (const YAML::Node caches = yaml_node["caches"]; caches.IsDefined()) {
    for (const YAML::Node& item : caches) {
      CacheEntry cache;
      cache.cache_type = item["cache_type"].as<std::string>("");
      cache.location = item["location"].as<std::string>("");
      definition.caches.push_back(std::move(cache));
    }
  }

  return definition;
}

/** @copydoc node_definition_to_yaml */
YAML::Node node_definition_to_yaml(const NodeDefinition& definition) {
  YAML::Node result;
  result["id"] = definition.id;
  result["name"] = definition.name;
  result["type"] = definition.type;
  result["subtype"] = definition.subtype;

  if (definition.preserved) {
    result["preserved"] = true;
  }
  if (!definition.image_inputs.empty()) {
    result["image_inputs"] = image_inputs_to_yaml(definition.image_inputs);
  }
  if (!definition.parameter_inputs.empty()) {
    result["parameter_inputs"] =
        parameter_inputs_to_yaml(definition.parameter_inputs);
  }
  result["parameters"] = core::parameter_map_to_yaml(definition.parameters);
  if (!definition.outputs.empty()) {
    result["outputs"] = output_ports_to_yaml(definition.outputs);
  }
  if (!definition.caches.empty()) {
    result["caches"] = caches_to_yaml(definition.caches);
  }
  return result;
}

/** @copydoc graph_definition_from_yaml */
GraphDefinition graph_definition_from_yaml(const YAML::Node& yaml_root) {
  if (!yaml_root.IsSequence()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "YAML root is not a sequence of nodes.");
  }

  GraphDefinition definition;
  definition.nodes.reserve(yaml_root.size());
  for (const YAML::Node& yaml_node : yaml_root) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    throw_if_graph_load_failure_probe(yaml_node);
#endif
    definition.nodes.push_back(node_definition_from_yaml(yaml_node));
  }
  return definition;
}

/** @copydoc graph_definition_to_yaml */
YAML::Node graph_definition_to_yaml(const GraphDefinition& definition) {
  YAML::Node result(YAML::NodeType::Sequence);
  for (const NodeDefinition& node : definition.nodes) {
    result.push_back(node_definition_to_yaml(node));
  }
  return result;
}

}  // namespace ps
