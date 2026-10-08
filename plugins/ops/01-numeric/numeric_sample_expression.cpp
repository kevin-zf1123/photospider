#include "01-numeric/expression_result.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_numeric_sample_expression(OperationRegistry* registry) {
  return registry->register_operation(
      expression_result::operation("numeric.sample_expression", false));
}
}  // namespace ps::plugin_internal
