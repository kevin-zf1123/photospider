#include "01-numeric/numeric_algorithms.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_numeric_divide(OperationRegistry* registry) {
  return registry->register_operation(
      numeric_ops::finite_arithmetic("numeric.divide", 3));
}
}  // namespace ps::plugin_internal
