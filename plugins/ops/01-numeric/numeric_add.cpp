#include "01-numeric/fixed_pointwise.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_add(OperationRegistry* registry) {
  return registry->register_operation(
      numeric_ops::fixed_pointwise("numeric.add", numeric_ops::Pointwise::Add));
}
}  // namespace ps::plugin_internal
