#include "kernel/services/compute-service/compute_cache_policy.hpp"

namespace ps::compute {

bool ComputeCachePolicy::has_reusable_output(const Node& node) {
  return node.cached_output_high_precision.has_value() ||
         node.cached_output.has_value();
}

NodeOutput* ComputeCachePolicy::reusable_output(Node& node) {
  if (node.cached_output_high_precision)
    return &*node.cached_output_high_precision;
  if (node.cached_output)
    return &*node.cached_output;
  return nullptr;
}

const NodeOutput* ComputeCachePolicy::reusable_output(const Node& node) {
  if (node.cached_output_high_precision)
    return &*node.cached_output_high_precision;
  if (node.cached_output)
    return &*node.cached_output;
  return nullptr;
}

NodeOutput* ComputeCachePolicy::interactive_output(Node& node) {
  if (node.cached_output_real_time)
    return &*node.cached_output_real_time;
  return reusable_output(node);
}

const NodeOutput* ComputeCachePolicy::interactive_output(const Node& node) {
  if (node.cached_output_real_time)
    return &*node.cached_output_real_time;
  return reusable_output(node);
}

NodeOutput& ComputeCachePolicy::ensure_target_output(Node& node,
                                                     ComputeIntent intent) {
  if (intent == ComputeIntent::RealTimeUpdate) {
    if (!node.cached_output_real_time)
      node.cached_output_real_time = NodeOutput{};
    return *node.cached_output_real_time;
  }
  if (!node.cached_output_high_precision)
    node.cached_output_high_precision = NodeOutput{};
  return *node.cached_output_high_precision;
}

std::optional<const NodeOutput*> ComputeCachePolicy::select_output(
    const Node& node, CacheReadMode mode) {
  const NodeOutput* output = mode == CacheReadMode::InteractivePreferred
                                 ? interactive_output(node)
                                 : reusable_output(node);
  if (!output)
    return std::nullopt;
  return output;
}

bool ComputeCachePolicy::can_read_disk_cache(bool disable_disk_cache,
                                             bool force_recache) {
  return !disable_disk_cache && !force_recache;
}

void ComputeCachePolicy::clear_for_recompute(Node& node, bool clear_legacy) {
  if (clear_legacy)
    node.cached_output.reset();
  node.cached_output_high_precision.reset();
}

}  // namespace ps::compute
