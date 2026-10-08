#include "08-transform/radius_result.hpp"
#include "08-transform/stmap_result.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_dependency_sampling(OperationRegistry* registry) {
  auto status = registry->register_operation(stmap_result::operation());
  if (!status.ok())
    return status;
  for (bool scatter : {false, true}) {
    status = registry->register_operation(radius_result::operation(scatter));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
