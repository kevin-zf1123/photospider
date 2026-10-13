#include "08-transform/stmap_result.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_stmap(OperationRegistry* registry) {
  return registry->register_operation(stmap_result::operation());
}
}  // namespace ps::plugin_internal
