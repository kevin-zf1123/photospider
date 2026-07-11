#include "compute/node_input_resolver.hpp"

#include <string>

#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

ResolvedNodeInputs NodeInputResolver::resolve(
    Node& node, const OutputLookup& lookup,
    const std::string& missing_context) {
  node.runtime_parameters = node.parameters ? YAML::Clone(node.parameters)
                                            : YAML::Node(YAML::NodeType::Map);

  for (const auto& p_input : node.parameter_inputs) {
    if (p_input.from_node_id < 0)
      continue;
    const NodeOutput* upstream = lookup(p_input.from_node_id);
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
  resolved.image_inputs.reserve(node.image_inputs.size());
  for (const auto& i_input : node.image_inputs) {
    if (i_input.from_node_id < 0)
      continue;
    const NodeOutput* upstream = lookup(i_input.from_node_id);
    if (!upstream) {
      throw GraphError(GraphErrc::MissingDependency,
                       missing_context + ": image input not ready for node " +
                           std::to_string(node.id));
    }
    resolved.image_inputs.push_back(upstream);
  }

  if (!resolved.image_inputs.empty()) {
    const auto& first_buf = resolved.image_inputs.front()->image_buffer;
    if (first_buf.width > 0 && first_buf.height > 0) {
      node.last_input_size_hp = cv::Size(first_buf.width, first_buf.height);
    } else {
      node.last_input_size_hp.reset();
    }
  } else {
    node.last_input_size_hp.reset();
  }

  return resolved;
}

const NodeOutput* NodeInputResolver::output_from_node(const Node& node,
                                                      CacheReadMode mode) {
  (void)mode;
  return ComputeCachePolicy::reusable_output(node);
}

}  // namespace ps::compute
