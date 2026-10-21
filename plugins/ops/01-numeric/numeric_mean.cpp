#include "01-numeric/fixed_ordered.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_mean(OperationRegistry* registry) {
  return registry->register_operation(
      numeric_ops::fixed_ordered("numeric.mean", numeric_ops::Ordered::Mean));
}
}  // namespace ps::plugin_internal
