#include "01-numeric/finite_elementwise.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_numeric_maximum(OperationRegistry* registry) {
  return registry->register_operation(numeric_ops::finite_elementwise(
      "numeric.maximum", numeric_ops::FiniteElementwise::Maximum));
}
}  // namespace ps::plugin_internal
