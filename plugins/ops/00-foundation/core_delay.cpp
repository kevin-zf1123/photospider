#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "00-foundation/tensor_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
struct Delay final {
  tensor_ops::Identity identity{true};
  std::int64_t milliseconds;
  bool waited = false;
  explicit Delay(std::int64_t milliseconds) : milliseconds(milliseconds) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (identity.started && !waited) {
      for (std::int64_t elapsed = 0; elapsed < milliseconds; ++elapsed) {
        if (phase.query.cancellation.cancelled())
          return Result<ResultProgramPoll>(
              Status{ErrorCode::Cancelled, "delay was cancelled"});
        auto active = phase.consume_work(1);
        if (!active.ok())
          return Result<ResultProgramPoll>(active);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      waited = true;
    }
    return identity.poll(phase);
  }
};
}  // namespace
Status register_core_delay(OperationRegistry* registry) {
  auto operation = tensor_ops::identity("core.delay", true);
  operation.traits.cacheable = false;
  operation.traits.outputs[0].continuation_bytes = sizeof(Delay);
  operation.traits.parameter_schema = {
      {"milliseconds", OperationParameterType::Int64, true, true, 0, 5000}};
  operation.start_result = [](const ResultProgramQuery& query,
                              const BufferAllocator& allocator) {
    return ResultContinuation::make<Delay>(
        allocator, std::get<std::int64_t>(query.parameters.at("milliseconds")));
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
