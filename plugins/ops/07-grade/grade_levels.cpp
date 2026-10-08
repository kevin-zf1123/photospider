#include "00-foundation/basic_result.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_grade_levels(OperationRegistry* registry) {
  return registry->register_operation(
      basic_result::operation("grade.levels", basic_result::Kind::Levels));
}
}  // namespace ps::plugin_internal
