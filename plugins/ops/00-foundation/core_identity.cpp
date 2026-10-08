#include "00-foundation/tensor_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_core_identity(OperationRegistry* registry) {
  return registry->register_operation(tensor_ops::identity("core.identity"));
}
}  // namespace ps::plugin_internal
