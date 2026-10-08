#include "00-foundation/basic_result.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_analysis_histogram(OperationRegistry* registry) {
  return registry->register_operation(basic_result::operation(
      "analysis.histogram", basic_result::Kind::Histogram));
}
}  // namespace ps::plugin_internal
