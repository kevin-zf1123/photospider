#pragma once

#include <functional>
#include <string>
#include <vector>

#include "compute/compute_cache_policy.hpp"
#include "graph/graph_model.hpp"

namespace ps::compute {

struct ResolvedNodeInputs {
  std::vector<const NodeOutput*> image_inputs;
};

class NodeInputResolver {
 public:
  using OutputLookup = std::function<const NodeOutput*(int)>;

  static ResolvedNodeInputs resolve(Node& node, const OutputLookup& lookup,
                                    const std::string& missing_context);

  static const NodeOutput* output_from_node(const Node& node,
                                            CacheReadMode mode);
};

}  // namespace ps::compute
