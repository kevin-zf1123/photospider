#include "00-foundation/basic_result.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_curve_sample_monotone(OperationRegistry* registry) {
  return registry->register_operation(basic_result::operation(
      "curve.sample_monotone", basic_result::Kind::Monotone));
}
}  // namespace ps::plugin_internal
