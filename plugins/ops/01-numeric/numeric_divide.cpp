#include "01-numeric/fixed_pointwise.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_divide(OperationRegistry* registry) {
  return registry->register_operation(numeric_ops::fixed_pointwise(
      "numeric.divide", numeric_ops::Pointwise::Divide));
}
}  // namespace ps::plugin_internal
