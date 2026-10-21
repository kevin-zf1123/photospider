#include "01-numeric/fixed_pointwise.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_subtract(OperationRegistry* registry) {
  return registry->register_operation(numeric_ops::fixed_pointwise(
      "numeric.subtract", numeric_ops::Pointwise::Subtract));
}
}  // namespace ps::plugin_internal
