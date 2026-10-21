#include "01-numeric/fixed_pointwise.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_abs(OperationRegistry* registry) {
  return registry->register_operation(
      numeric_ops::fixed_pointwise("numeric.abs", numeric_ops::Pointwise::Abs));
}
}  // namespace ps::plugin_internal
