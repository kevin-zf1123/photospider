#include "01-numeric/fixed_ordered.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_variance(OperationRegistry* registry) {
  return registry->register_operation(numeric_ops::fixed_ordered(
      "numeric.variance", numeric_ops::Ordered::Variance));
}
}  // namespace ps::plugin_internal
