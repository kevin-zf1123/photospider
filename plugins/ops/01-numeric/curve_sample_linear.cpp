#include "00-foundation/basic_result.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_curve_sample_linear(OperationRegistry* registry) {
  return registry->register_operation(basic_result::operation(
      "curve.sample_linear", basic_result::Kind::Linear));
}
}  // namespace ps::plugin_internal
