#include "common/basic_result.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_curve_sample_linear(OperationRegistry* registry) {
  return registry->register_operation(basic_result::operation(
      "curve.sample_linear", basic_result::Kind::Linear));
}
}  // namespace ps::plugin_internal
