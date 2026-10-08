#include "01-numeric/numeric_algorithms.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_numeric_subtract(OperationRegistry* registry) {
  return registry->register_operation(
      numeric_ops::finite_arithmetic("numeric.subtract", 1));
}
}  // namespace ps::plugin_internal
