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

#include "photospider/core/resource_allocator.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
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
  std::uint64_t count;
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
  if (shared.mode == 4 || shared.mode == 7) {
    if ((begin != 0 && begin != work.count - 1) || end <= begin ||
        end > work.count)
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
  auto output = multi_result::output(
      "value", multi_result::schema(ElementType::Int64, {1}));
  output.region_rule = OperationRegionRule::Whole;
  operation.traits.outputs = {output};
  struct Program {
    Shared* shared;
    explicit Program(Shared* state) : shared(state) {}
    Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
      using multi_result::check;
      using multi_result::take;
      if (!phase.cpu_parallel)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::Internal, "no service"});
      if (shared->mode == 6) {
        const auto* budget = resource_internal::metadata_budget();
        if (!budget)
          return Result<ResultProgramPoll>(
              Status{ErrorCode::Internal, "missing callback root"});
        static_cast<void>(budget->consume({UINT64_MAX}));
      }
      auto made = phase.allocator.allocate(257 * 8);
      if (!made.ok())
        return Result<ResultProgramPoll>(made.status());
      auto scratch = made.take_value();
      const auto* service = phase.cpu_parallel;
      const bool large = shared->mode == 4 || shared->mode == 7;
      std::uint64_t count = 257;
      if (large) {
        const auto issued = phase.resources.statistics().issued.work;
        if (issued > UINT64_MAX - 4096)
          return Result<ResultProgramPoll>(
              Status{ErrorCode::ResourceExhausted, "range preparation"});
        // Leave fuel for sealing and publishing after the synchronous range.
        count = shared->mode == 7 ? UINT64_MAX : UINT64_MAX - issued - 4096;
      }
      Work work{shared, reinterpret_cast<std::uint64_t*>(scratch.data()),
                service, count};
      const int before = std::fegetround();
      std::fesetround(FE_DOWNWARD);
      const int code =
          service->run(service->context, count, large ? count - 1 : 7,
                       shared->grant, block, &work);
      const bool restored = std::fegetround() == FE_DOWNWARD;
      std::fesetround(before);
      if (!restored)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::Internal, "FP restore"});
      if (!code && !large) {
        for (unsigned i = 0; i < 257; ++i)
          if (work.output[i] != i * 17 + 3)
            return Result<ResultProgramPoll>(
                Status{ErrorCode::Internal, "range coverage"});
      }
      auto builder = take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key));
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(phase.resources, 1, {}))));
      const std::int64_t value = 257;
      check(builder.publish_tensor(
          0, Region::whole({1}),
          {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)},
          take(ResultRelation::cartesian(phase.resources, 1, {})),
          {true, true, true, true}));
      return Result<ResultProgramPoll>(
          ResultPublication{take(builder.seal()), true});
    }
  };
  operation.start_result = [shared](const auto&, const auto& allocator) {
    return ResultContinuation::make<Program>(allocator, shared);
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
    if (mode == 2 || mode == 6 || mode == 7 || exhausted) {
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
      const auto& output = result.value().results.at("output");
      std::int64_t value = 0;
      PS_CHECK(output
                   .read_tensor(multi_result::take(output.descriptor()), 0, {0},
                                &value, sizeof(value))
                   .ok() &&
               value == 257);
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
  if (exhausted || mode == 7)
    PS_CHECK(shared.blocks == 0);
  auto resources = context.resource_budget().take_value().statistics();
  PS_CHECK(resources.live[ResourceKind::Queue] == 0);
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(run(1, 0, 0, 1) == 0);
  PS_CHECK(run(4, 1, 0, 1) == 0);
  PS_CHECK(run(4, 0, 1, 1) == 0);
  PS_CHECK(run(4, 1, 2, 1) == 0);
  PS_CHECK(run(4, 1, 3, 1) == 0);
  PS_CHECK(run(4, 0, 0, 8) == 0);
  PS_CHECK(run(4, 2, 0, 1) == 0);
  PS_CHECK(run(4, 3, 0, 1) == 0);
  PS_CHECK(run(4, 4, 0, 1) == 0);
  PS_CHECK(run(4, 7, 0, 1) == 0);
  PS_CHECK(run(4, 5, 0, 1) == 0);
  PS_CHECK(run(4, 6, 0, 1) == 0);
  PS_CHECK(run(4, 0, 0, 1, true) == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
