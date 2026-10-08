#include "01-numeric/finite_elementwise.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_numeric_abs(OperationRegistry* registry) {
  return registry->register_operation(numeric_ops::finite_elementwise(
      "numeric.abs", numeric_ops::FiniteElementwise::Abs));
}
}  // namespace ps::plugin_internal
