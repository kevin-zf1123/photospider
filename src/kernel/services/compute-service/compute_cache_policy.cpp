#include "kernel/services/compute-service/compute_cache_policy.hpp"

namespace ps::compute {

bool ComputeCachePolicy::has_reusable_output(const Node& node) {
  return node.cached_output_high_precision.has_value();
}

NodeOutput* ComputeCachePolicy::reusable_output(Node& node) {
  if (node.cached_output_high_precision)
    return &*node.cached_output_high_precision;
  return nullptr;
}

const NodeOutput* ComputeCachePolicy::reusable_output(const Node& node) {
  if (node.cached_output_high_precision)
    return &*node.cached_output_high_precision;
  return nullptr;
}

std::optional<const NodeOutput*> ComputeCachePolicy::select_output(
    const Node& node, CacheReadMode mode) {
  (void)mode;
  const NodeOutput* output = reusable_output(node);
  if (!output)
    return std::nullopt;
  return output;
}

bool ComputeCachePolicy::can_read_disk_cache(bool disable_disk_cache,
                                             bool force_recache) {
  return !disable_disk_cache && !force_recache;
}

void ComputeCachePolicy::clear_for_recompute(Node& node) {
  node.cached_output_high_precision.reset();
}

}  // namespace ps::compute
