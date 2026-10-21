#include "01-numeric/fixed_pointwise.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_multiply(OperationRegistry* registry) {
  return registry->register_operation(numeric_ops::fixed_pointwise(
      "numeric.multiply", numeric_ops::Pointwise::Multiply));
}
}  // namespace ps::plugin_internal
