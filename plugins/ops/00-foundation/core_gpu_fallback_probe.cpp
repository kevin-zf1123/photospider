#include <utility>

#include "00-foundation/tensor_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_core_gpu_fallback_probe(OperationRegistry* registry) {
  auto operation = tensor_ops::identity("core.gpu_fallback_probe");
  operation.traits.supports_gpu = true;
  operation.traits.allows_cpu_fallback = true;
  operation.start_result = [](const ResultProgramQuery& query,
                              const BufferAllocator& allocator) {
    if (query.backend == Backend::Gpu)
      return Result<ResultContinuation>(
          Status{ErrorCode::BackendUnavailable, "probe rejects GPU execution"});
    return ResultContinuation::make<tensor_ops::Identity>(allocator);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
