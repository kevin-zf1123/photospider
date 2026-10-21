#include "01-numeric/fixed_pointwise.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_minimum(OperationRegistry* registry) {
  return registry->register_operation(numeric_ops::fixed_pointwise(
      "numeric.minimum", numeric_ops::Pointwise::Minimum));
}
}  // namespace ps::plugin_internal
