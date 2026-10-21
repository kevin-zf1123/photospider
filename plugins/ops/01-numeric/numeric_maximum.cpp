#include "01-numeric/fixed_pointwise.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_maximum(OperationRegistry* registry) {
  return registry->register_operation(numeric_ops::fixed_pointwise(
      "numeric.maximum", numeric_ops::Pointwise::Maximum));
}
}  // namespace ps::plugin_internal
