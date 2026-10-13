#include "01-numeric/finite_elementwise.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_minimum(OperationRegistry* registry) {
  return registry->register_operation(numeric_ops::finite_elementwise(
      "numeric.minimum", numeric_ops::FiniteElementwise::Minimum));
}
}  // namespace ps::plugin_internal
