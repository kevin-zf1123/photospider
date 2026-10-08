#include "01-numeric/expression_result.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_lut_apply_1d(OperationRegistry* registry) {
  return registry->register_operation(
      expression_result::operation("lut.apply_1d", true));
}
}  // namespace ps::plugin_internal
