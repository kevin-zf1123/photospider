#include "01-numeric/fixed_ordered.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_ordered_scan(OperationRegistry* registry) {
  return registry->register_operation(numeric_ops::fixed_ordered(
      "numeric.ordered_scan", numeric_ops::Ordered::Scan));
}
}  // namespace ps::plugin_internal
