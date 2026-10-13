#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#include "support/kernel_registry_fixture.hpp"

namespace ps::test {
namespace probe_internal {
struct Delay final {
  multi_result::Program identity{0};
  std::int64_t milliseconds;
  bool waited = false;
  explicit Delay(std::int64_t milliseconds) : milliseconds(milliseconds) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (identity.requested && !waited) {
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
}  // namespace probe_internal
inline Status register_delay_probe(OperationRegistry* registry) {
  auto operation = kernel_identity("test.delay");
  operation.traits.cacheable = false;
  operation.traits.outputs[0].continuation_bytes =
      sizeof(probe_internal::Delay);
  operation.traits.parameter_schema = {
      {"milliseconds", OperationParameterType::Int64, true, true, 0, 5000}};
  operation.start_result = [](const ResultProgramQuery& query,
                              const BufferAllocator& allocator) {
    return ResultContinuation::make<probe_internal::Delay>(
        allocator, std::get<std::int64_t>(query.parameters.at("milliseconds")));
  };
  return registry->register_operation(std::move(operation));
}
inline Status register_fallback_probe(OperationRegistry* registry) {
  auto operation = kernel_identity("test.gpu_fallback_probe");
  operation.traits.supports_gpu = true;
  operation.traits.allows_cpu_fallback = true;
  operation.start_result = [](const ResultProgramQuery& query,
                              const BufferAllocator& allocator) {
    if (query.backend == Backend::Gpu)
      return Result<ResultContinuation>(
          Status{ErrorCode::BackendUnavailable, "probe rejects GPU execution"});
    return ResultContinuation::make<multi_result::Program>(allocator, 0);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::test
