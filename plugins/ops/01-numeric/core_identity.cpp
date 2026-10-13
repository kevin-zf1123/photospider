#include "common/tensor_program.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal {
Status register_core_identity(OperationRegistry* registry) {
  return registry->register_operation(tensor_ops::identity("core.identity"));
}
}  // namespace ps::plugin_internal
