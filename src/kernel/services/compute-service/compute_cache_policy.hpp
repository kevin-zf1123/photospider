#pragma once

#include <optional>

#include "node.hpp"
#include "ps_types.hpp"

namespace ps::compute {

enum class CacheReadMode {
  ReusableOnly,
  InteractivePreferred,
};

class ComputeCachePolicy {
 public:
  static bool has_reusable_output(const Node& node);
  static NodeOutput* reusable_output(Node& node);
  static const NodeOutput* reusable_output(const Node& node);

  static NodeOutput* interactive_output(Node& node);
  static const NodeOutput* interactive_output(const Node& node);

  static NodeOutput& ensure_target_output(Node& node, ComputeIntent intent);
  static std::optional<const NodeOutput*> select_output(const Node& node,
                                                        CacheReadMode mode);

  static bool can_read_disk_cache(bool disable_disk_cache, bool force_recache);
  static void clear_for_recompute(Node& node);
};

}  // namespace ps::compute
