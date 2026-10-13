#include "common/basic_result.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_field_apply_lut_1d(OperationRegistry* registry) {
  return registry->register_operation(
      basic_result::operation("field.apply_lut_1d", basic_result::Kind::Lut));
}
}  // namespace ps::plugin_internal
