#include "compute/node_input_resolver.hpp"

#include <string>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Resolves one node's runtime parameters and image-input slot table.
 * @param node Node updated with request-effective parameters and size hint.
 * @param lookup Borrowed upstream output lookup.
 * @param missing_context Diagnostic prefix for unresolved dependencies.
 * @return Image pointers aligned one-for-one with declared input slots.
 * @throws GraphError when a connected image or parameter output is missing.
 * @throws YAML::Exception or std::bad_alloc from parameter/vector copying.
 * @note Disconnected image slots remain nullptr so later execution preserves
 * graph input indexes instead of compacting the vector.
 */
ResolvedNodeInputs NodeInputResolver::resolve(
    Node& node, const OutputLookup& lookup,
    const std::string& missing_context) {
  return resolve(node, lookup, lookup, missing_context);
}

/**
 * @brief Resolves parameter and image edges through domain-specific lookups.
 * @param node Node updated with request-effective parameters and size hint.
 * @param image_lookup Lookup for image-input source outputs.
 * @param parameter_lookup Lookup for parameter-input source outputs.
 * @param missing_context Diagnostic prefix for unresolved dependencies.
 * @return Image pointers aligned one-for-one with declared image slots.
 * @throws GraphError when a connected image or parameter output is missing.
 * @throws YAML::Exception or std::bad_alloc from parameter/vector copying.
 * @note Keeping lookups separate prevents an HP-stabilized image payload from
 * crossing into RT execution merely because the same node also exports data.
 */
ResolvedNodeInputs NodeInputResolver::resolve(
    Node& node, const OutputLookup& image_lookup,
    const OutputLookup& parameter_lookup, const std::string& missing_context) {
  node.runtime_parameters = node.parameters ? YAML::Clone(node.parameters)
                                            : YAML::Node(YAML::NodeType::Map);

  for (const auto& p_input : node.parameter_inputs) {
    if (p_input.from_node_id < 0)
      continue;
    const NodeOutput* upstream = parameter_lookup(p_input.from_node_id);
    if (!upstream) {
      throw GraphError(GraphErrc::MissingDependency,
                       missing_context +
                           ": parameter input not ready for node " +
                           std::to_string(node.id));
    }
    auto it = upstream->data.find(p_input.from_output_name);
    if (it == upstream->data.end()) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Node " + std::to_string(p_input.from_node_id) +
                           " did not produce output '" +
                           p_input.from_output_name + "'");
    }
    node.runtime_parameters[p_input.to_parameter_name] = it->second;
  }

  ResolvedNodeInputs resolved;
  resolved.image_inputs.resize(node.image_inputs.size(), nullptr);
  for (std::size_t index = 0; index < node.image_inputs.size(); ++index) {
    const ImageInput& i_input = node.image_inputs[index];
    if (i_input.from_node_id < 0) {
      continue;
    }
    const NodeOutput* upstream = image_lookup(i_input.from_node_id);
    if (!upstream) {
      throw GraphError(GraphErrc::MissingDependency,
                       missing_context + ": image input not ready for node " +
                           std::to_string(node.id));
    }
    resolved.image_inputs[index] = upstream;
  }

  node.last_input_size_hp.reset();
  for (const NodeOutput* input : resolved.image_inputs) {
    if (input && input->image_buffer.width > 0 &&
        input->image_buffer.height > 0) {
      node.last_input_size_hp =
          cv::Size(input->image_buffer.width, input->image_buffer.height);
      break;
    }
  }

  return resolved;
}

/**
 * @brief Returns the current reusable output selected by cache policy.
 * @param node Upstream node whose HP cache authority is inspected.
 * @param mode Read mode retained for API symmetry; current selection is HP.
 * @return Borrowed reusable output, or nullptr when none exists.
 * @throws Nothing.
 * @note The returned pointer does not extend the cache value's lifetime.
 */
const NodeOutput* NodeInputResolver::output_from_node(const Node& node,
                                                      CacheReadMode mode) {
  (void)mode;
  return ComputeCachePolicy::reusable_output(node);
}

}  // namespace ps::compute
