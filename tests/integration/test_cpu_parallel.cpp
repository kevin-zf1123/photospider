#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/execution/resource_allocator.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Shared {
  std::atomic<unsigned> active{0}, peak{0}, blocks{0};
  std::atomic<bool> paired{false};
  unsigned mode = 0;
  unsigned grant = 0;
  CancellationSource cancellation;
};
struct Work {
  Shared* shared;
  std::uint64_t* output;
  const ps_cpu_parallel_service_v1* service;
};
int block(void* user, std::uint64_t begin, std::uint64_t end,
          std::uint32_t slot) {
  auto& work = *static_cast<Work*>(user);
  auto& shared = *work.shared;
  const auto active = ++shared.active;
  auto peak = shared.peak.load();
  while (peak < active && !shared.peak.compare_exchange_weak(peak, active)) {
  }
  if (active > 1)
    shared.paired = true;
  int code = 0;
  if (std::fegetround() != FE_TONEAREST ||
      slot >= (shared.grant ? shared.grant : work.service->maximum_workers))
    code = 1;
  if (shared.mode == 1) {
    // Force overlap for the multi-worker fixture, with a finite timeout.
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!shared.paired && std::chrono::steady_clock::now() < until)
      std::this_thread::yield();
    if (!shared.paired)
      code = 1;
  }
  if (shared.mode == 2)
    code = 4;
  if (shared.mode == 5)
    shared.cancellation.cancel();
  if (shared.mode == 3) {
    // The ignored invalid service call must still fail the enclosing callback.
    (void)work.service->run(work.service->context, 1, 1, 1, block, user);
  }
  if (shared.mode == 4) {
    if ((begin != 0 && begin != UINT64_MAX - 1) || end <= begin)
      code = 1;
  } else {
    for (auto i = begin; i < end; ++i)
      work.output[i] = i * 17 + 3;
  }
  ++shared.blocks;
  --shared.active;
  return code;
}
std::shared_ptr<OperationRegistry> registry(Shared* shared) {
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "parallel.probe";
  operation.traits.cacheable = false;
  operation.traits.workspace_bytes = 257 * 8;
  operation.traits.outputs[0].output_element_type = ElementType::Int64;
  operation.traits.outputs[0].shape_rule = OperationShapeRule::Fixed;
  operation.traits.outputs[0].fixed_output_shape = {1};
  operation.callback = [shared](const OperationInvocation& call) {
    if (!call.cpu_parallel)
      return Result<Value>(Status{ErrorCode::Internal, "no service"});
    if (shared->mode == 6) {
      const auto* budget = resource_internal::metadata_budget();
      if (!budget)
        return Result<Value>(
            Status{ErrorCode::Internal, "missing callback root"});
      // A swallowed resource failure must still prevent successful publication.
      static_cast<void>(budget->consume({UINT64_MAX}));
    }
    auto made = call.allocator.allocate(257 * 8);
    if (!made.ok())
      return Result<Value>(made.status());
    auto scratch = made.take_value();
    Work work{shared, reinterpret_cast<std::uint64_t*>(scratch.data()),
              call.cpu_parallel};
    const auto* service = call.cpu_parallel;
    const int before = std::fegetround();
    std::fesetround(FE_DOWNWARD);
    const bool large = shared->mode == 4;
    const int code =
        service->run(service->context, large ? UINT64_MAX : 257,
                     large ? UINT64_MAX - 1 : 7, shared->grant, block, &work);
    const bool restored = std::fegetround() == FE_DOWNWARD;
    std::fesetround(before);
    if (!restored)
      return Result<Value>(Status{ErrorCode::Internal, "FP restore"});
    // Deliberately ignore worker and nested errors; host sticky failures win.
    if (!code && !large) {
      for (unsigned i = 0; i < 257; ++i)
        if (work.output[i] != i * 17 + 3)
          return Result<Value>(Status{ErrorCode::Internal, "range coverage"});
    }
    auto made_output = MutableValue::allocate(
        {ElementType::Int64, {1}}, Region::whole({1}), call.allocator);
    if (!made_output.ok())
      return Result<Value>(made_output.status());
    auto output = made_output.take_value();
    const std::int64_t value = 257;
    std::memcpy(output.data(), &value, 8);
    return std::move(output).publish();
  };
  if (!registry->register_operation(std::move(operation)).ok() ||
      !registry->freeze().ok())
    throw std::runtime_error("registration failed");
  return registry;
}
int run(unsigned workers, unsigned mode, unsigned grant, unsigned simultaneous,
        bool exhausted = false) {
  Shared shared;
  shared.mode = mode;
  shared.grant = grant;
  auto operations = registry(&shared);
  WorkflowDocument document;
  document.nodes = {{1, "parallel.probe", {}, {}}};
  document.outputs = {{"output", 1, "value"}};
  GraphContext graph(document);
  auto compiled = Compiler(operations).compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContextConfig config;
  config.cpu_workers = workers;
  config.gpu_enabled = false;
  config.managed_resources = ResourceLimits{};
  if (exhausted)
    config.managed_resources->maximum_work = 0;
  else if (mode == 6)
    config.managed_resources->maximum_work = 1000000;
  ExecutionContext context(operations, config);
  std::vector<std::future<Result<ExecutionResult>>> runs;
  for (unsigned i = 0; i < simultaneous; ++i)
    runs.push_back(std::async(std::launch::async, [&] {
      return context.execute(compiled.value().plan, {},
                             shared.cancellation.token());
    }));
  for (auto& future : runs) {
    PS_CHECK(future.wait_for(std::chrono::seconds(10)) ==
             std::future_status::ready);
    auto result = future.get();
    if (mode == 2 || mode == 6 || exhausted) {
      PS_CHECK(!result.ok());
      PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
    } else if (mode == 5) {
      PS_CHECK(!result.ok());
      PS_CHECK(result.status().code == ErrorCode::Cancelled);
    } else if (mode == 3) {
      PS_CHECK(!result.ok());
      PS_CHECK(result.status().code == ErrorCode::InvalidArgument);
    } else {
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
    }
  }
  PS_CHECK(shared.active == 0);
  PS_CHECK(shared.peak <= workers);
  if (mode == 1)
    PS_CHECK(shared.peak > 1);
  if (grant && simultaneous == 1)
    PS_CHECK(shared.peak <= grant);
  if (grant == 1 && simultaneous == 1)
    PS_CHECK(shared.peak == 1);
  if (mode == 4)
    PS_CHECK(shared.blocks == 2);
  if (exhausted)
    PS_CHECK(shared.blocks == 0);
  auto resources = context.resource_budget().take_value().statistics();
  PS_CHECK(resources.live[ResourceKind::Queue] == 0);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(run(1, 0, 0, 1) == 0);
  PS_CHECK(run(4, 1, 0, 1) == 0);
  PS_CHECK(run(4, 0, 1, 1) == 0);
  PS_CHECK(run(4, 1, 2, 1) == 0);
  PS_CHECK(run(4, 1, 3, 1) == 0);
  PS_CHECK(run(4, 0, 0, 8) == 0);
  PS_CHECK(run(4, 2, 0, 1) == 0);
  PS_CHECK(run(4, 3, 0, 1) == 0);
  PS_CHECK(run(4, 4, 0, 1) == 0);
  PS_CHECK(run(4, 5, 0, 1) == 0);
  PS_CHECK(run(4, 6, 0, 1) == 0);
  PS_CHECK(run(4, 0, 0, 1, true) == 0);
  return 0;
}
