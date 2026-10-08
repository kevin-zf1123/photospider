#include "00-foundation/basic_result.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_analysis_histogram_out_of_range(OperationRegistry* registry) {
  return registry->register_operation(basic_result::operation(
      "analysis.histogram_out_of_range", basic_result::Kind::Outside));
}
}  // namespace ps::plugin_internal
