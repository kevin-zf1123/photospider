#include "00-foundation/basic_result.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_field_smoothstep(OperationRegistry* registry) {
  return registry->register_operation(basic_result::operation(
      "field.smoothstep", basic_result::Kind::Smoothstep));
}
}  // namespace ps::plugin_internal
