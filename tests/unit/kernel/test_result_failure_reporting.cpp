#include <atomic>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "photospider/plugin/result_program.hpp"
#include "plugin/failure_latch.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
int reporting() {
  ResourceBudget root;
  auto allocator = root.allocator();
  ResultProgramMetadata metadata;
  const std::map<std::string, ParameterValue> parameters;
  const ResultProgramQuery query(metadata, parameters);
  const ResultObjectInputs inputs;
  for (bool latch : {false, true}) {
    ResultProgramPhase phase{
        query, inputs, allocator,
        root,  {},     std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok)};
    if (latch)
      phase.failure_latch = std::make_shared<plugin_internal::FailureLatch>();
    Status observed;
    unsigned calls = 0;
    phase.failure_observer = [&](const Status& status) {
      observed = status;
      ++calls;
    };
    phase.record_failure(Status::success());
    PS_CHECK(calls == 0 && phase.failure->load() == ErrorCode::Ok);
    phase.record_failure({ErrorCode::BackendUnavailable, "first backend"});
    PS_CHECK(calls == 1 && observed.message == "first backend");
    if (latch)
      PS_CHECK(phase.failure_latch->backend_retry_allowed());
    phase.failure_observer = [](const Status&) {
      throw std::runtime_error("observer");
    };
    phase.record_failure({ErrorCode::BackendUnavailable, "second backend"});
    PS_CHECK(phase.failure->load() == ErrorCode::BackendUnavailable);
    if (latch) {
      PS_CHECK(!phase.failure_latch->backend_retry_allowed());
      PS_CHECK(phase.failure_latch->snapshot().message == "first backend");
    }
    phase.record_failure({ErrorCode::Cancelled, "later cancellation"});
    PS_CHECK(phase.failure->load() == ErrorCode::BackendUnavailable);
  }
  return 0;
}
}  // namespace
int main() {
  return reporting();
}
