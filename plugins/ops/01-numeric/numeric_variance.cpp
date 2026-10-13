#include "01-numeric/ordered_reduction.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_numeric_variance(OperationRegistry* registry) {
  return registry->register_operation(
      numeric_ops::ordered_reduction("numeric.variance", true));
}
}  // namespace ps::plugin_internal
