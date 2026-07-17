#pragma once

#include <functional>
#include <string>
#include <vector>

#include "compute/compute_cache_policy.hpp"
#include "graph/graph_model.hpp"

namespace ps::compute {

/**
 * @brief Owns callback-scoped resolved image-input pointers for one node.
 *
 * The vector preserves `Node::image_inputs` destination-slot identity. A
 * disconnected slot is represented by nullptr rather than being removed, so
 * later executor and public ROI adapters observe the same input indexes as the
 * graph topology.
 *
 * @throws std::bad_alloc when vector storage allocation fails.
 * @note Pointers borrow upstream NodeOutput values supplied by the lookup and
 * must not outlive those cache or request-local output owners.
 */
struct ResolvedNodeInputs {
  /** @brief Image outputs in exact destination-slot order, including nulls. */
  std::vector<const NodeOutput*> image_inputs;
};

/**
 * @brief Resolves node parameter and image inputs without compressing slots.
 *
 * Parameter values are copied into `Node::runtime_parameters`; image values
 * remain borrowed pointers. The resolver owns no graph, cache, or output
 * storage and delegates source selection to a caller-provided lookup.
 *
 * @note A resolver instance has no state; all methods are thread-safe when the
 * supplied node, lookup, and referenced outputs are externally synchronized.
 */
class NodeInputResolver {
 public:
  /** @brief Callable mapping an upstream node id to a borrowed output. */
  using OutputLookup = std::function<const NodeOutput*(int)>;

  /**
   * @brief Resolves effective parameters and ordered image-input slots.
   * @param node Node receiving a copied runtime parameter map and input-size
   * hint.
   * @param lookup Source-output lookup valid throughout this call.
   * @param missing_context Human-readable prefix for dependency diagnostics.
   * @return Borrowed image pointers indexed exactly like node.image_inputs;
   * disconnected slots contain nullptr.
   * @throws GraphError when a connected source output or named parameter is
   * unavailable.
   * @throws std::bad_alloc when copied parameter/vector storage cannot grow.
   * @note The node's static parameters remain unchanged. `last_input_size_hp`
   * uses the first connected input with a positive extent.
   */
  static ResolvedNodeInputs resolve(Node& node, const OutputLookup& lookup,
                                    const std::string& missing_context);

  /**
   * @brief Resolves parameters and image slots through independent lookups.
   *
   * @param node Node receiving request-effective runtime parameters.
   * @param image_lookup Lookup used only for declared image-input edges.
   * @param parameter_lookup Lookup used only for parameter-input edges.
   * @param missing_context Human-readable dependency diagnostic prefix.
   * @return Borrowed image pointers aligned with node.image_inputs.
   * @throws GraphError when a connected source output or named parameter is
   * unavailable.
   * @throws std::bad_alloc from parameter/vector copying.
   * @note Dirty RT execution uses this overload so exact HP-stabilized
   * parameter values do not replace RT-domain image inputs from the same node.
   */
  static ResolvedNodeInputs resolve(Node& node,
                                    const OutputLookup& image_lookup,
                                    const OutputLookup& parameter_lookup,
                                    const std::string& missing_context);

  /**
   * @brief Selects the reusable output exposed by one upstream node.
   * @param node Upstream graph node to inspect.
   * @param mode Requested cache-read mode; current HP authority ignores the
   * mode after the caller selects this boundary.
   * @return Borrowed reusable output, or nullptr when no HP authority exists.
   * @throws Nothing.
   * @note The pointer remains valid only while the node's cache is unchanged.
   */
  static const NodeOutput* output_from_node(const Node& node,
                                            CacheReadMode mode);
};

}  // namespace ps::compute
