#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "photospider/execution/execution.hpp"
#include "support/fixed_operation_fixture.hpp"
#include "support/status_check.hpp"

namespace {
struct Probe {
  std::mutex mutex;
  std::condition_variable changed;
  bool barrier = false, hold_rejection = false, rejected = false;
  bool release = false, return_ok = false, timed_out = false;
  bool rejection_held = false;
  unsigned entered = 0, active = 0, peak = 0;
};
ps::OperationDefinition operation(const std::shared_ptr<Probe>& probe) {
  auto definition = ps::test::fixed_fixture::operation("budgeted", 0);
  definition.lower = [](const ps::OperationLowering& query)
      -> ps::Result<ps::OperationProgram> {
    ps::OperationProgramBuilder builder(query);
    for (const auto& member : query.requests[0].members)
      for (auto tile : member.second) {
        auto task = builder.add_task("charge", "work-v9");
        if (!task.ok())
          return ps::Result<ps::OperationProgram>(task.status());
        auto status = builder.write_output(task.value(), 0, member.first, tile);
        if (!status.ok())
          return ps::Result<ps::OperationProgram>(status);
      }
    auto result = builder.finish();
    if (!result.ok())
      return result;
    auto program = result.take_value();
    for (auto& kernel : program.program.kernels)
      kernel.abi_version = 9;
    auto status = ps::physical::finalize_program(&program.program);
    if (!status.ok())
      return ps::Result<ps::OperationProgram>(status);
    return ps::Result<ps::OperationProgram>(std::move(program));
  };
  definition.prepare_kernel_v9 =
      [probe](const auto&, const auto&, const auto&, const auto&,
              const auto*) -> ps::Result<ps_prepared_kernel_v9> {
    ps_prepared_kernel_v9 result{};
    result.struct_size = sizeof(result);
    result.abi_version = 9;
    result.context = new std::shared_ptr<Probe>(probe);
    result.release = [](const void* p) {
      delete static_cast<const std::shared_ptr<Probe>*>(p);
    };
    result.invoke_cpu = [](const ps_kernel_invocation_v9* inv,
                           ps_kernel_status_v8* status) {
      auto& probe = **static_cast<const std::shared_ptr<Probe>*>(
          inv->views.prepared_context);
      {
        std::unique_lock<std::mutex> lock(probe.mutex);
        if (probe.barrier) {
          ++probe.entered;
          ++probe.active;
          probe.peak = std::max(probe.peak, probe.active);
          probe.changed.notify_all();
          if (!probe.changed.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return probe.entered >= 2; })) {
            probe.timed_out = true;
            --probe.active;
            *status = ps::kernel_failure(ps::ErrorCode::OperationFailed,
                                         "concurrency barrier timeout");
            return;
          }
        }
      }
      *status = inv->work.consume(inv->work.context, 10, 2);
      {
        std::unique_lock<std::mutex> lock(probe.mutex);
        if (probe.barrier)
          --probe.active;
        if (status->code && probe.hold_rejection) {
          probe.rejected = true;
          probe.rejection_held = true;
          probe.changed.notify_all();
          if (!probe.changed.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return probe.release; })) {
            probe.timed_out = true;
            probe.rejection_held = false;
            *status = ps::kernel_failure(ps::ErrorCode::OperationFailed,
                                         "rejection release timeout");
            return;
          }
          probe.rejection_held = false;
        }
        if (status->code && probe.return_ok) {
          *status = {};
          return;
        }
      }
      if (status->code)
        return;
      for (unsigned i = 0; i < inv->views.output_count; ++i) {
        const auto& output = inv->views.outputs[i];
        auto* data = static_cast<std::uint8_t*>(output.allocation);
        for (std::uint64_t x = 0; x < output.layout->shape[0]; ++x)
          data[output.byte_offset + x] = 17;
      }
    };
    return ps::Result<ps_prepared_kernel_v9>(result);
  };
  return definition;
}
int malformed_preparation() {
  ps_prepared_kernel_v9 raw{};
  raw.struct_size = sizeof(raw);
  raw.abi_version = 9;
  raw.invoke_cpu = [](const ps_kernel_invocation_v9*,
                      ps_kernel_status_v8* status) { *status = {}; };
  auto prepared = ps::PreparedKernel::adopt(raw, {});
  PS_REQUIRE_OK(prepared);
  ps_kernel_invocation_v8 legacy{};
  prepared.value().bind(&legacy);
  PS_CHECK(prepared.value().invoke(legacy).code ==
           PS_KERNEL_INVALID_ARGUMENT_V8);
  ps_kernel_invocation_v9 inv{};
  prepared.value().bind(&inv);
  PS_CHECK(prepared.value().invoke(inv).code == PS_KERNEL_INVALID_ARGUMENT_V8);
  inv.struct_size--;
  PS_CHECK(prepared.value().invoke(inv).reason ==
           PS_KERNEL_REASON_MALFORMED_ENVELOPE_V8);
  raw.struct_size--;
  PS_CHECK(!ps::PreparedKernel::adopt(raw, {}).ok());
  raw.struct_size = sizeof(raw);
  raw.abi_version = 8;
  PS_CHECK(!ps::PreparedKernel::adopt(raw, {}).ok());
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(malformed_preparation() == 0);
  auto registry = std::make_shared<ps::OperationRegistry>();
  auto probe = std::make_shared<Probe>();
  PS_REQUIRE_OK(registry->register_operation(operation(probe)));
  PS_REQUIRE_OK(registry->freeze());
  ps::WorkflowDocument doc;
  doc.nodes = {{1, "budgeted", {}, {}}, {2, "budgeted", {}, {}}};
  doc.outputs = {{"one", 1, "out0"}, {"two", 2, "out0"}};
  ps::GraphContext graph(doc);
  ps::PlanningOptions planning;
  planning.tile_extent = 2;
  auto compiled = ps::Compiler(registry).compile(graph, planning);
  PS_REQUIRE_OK(compiled);
  for (const auto& kernel : compiled.value().plan->program().kernels)
    PS_CHECK(kernel.abi_version == 9);
  ps::ExecutionContextConfig config;
  config.cpu_workers = 2;
  ps::ExecutionContext context(registry, config);
  auto prepared = context.prepare(*compiled.value().plan);
  PS_REQUIRE_OK(prepared);
  ps::ExecutionOptions options;
  options.maximum_parallelism = 1;
  options.maximum_work = UINT64_MAX;
  auto first = context.submit(prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(first);
  auto succeeded = first.value().join();
  PS_REQUIRE_OK(succeeded);
  PS_CHECK(succeeded.value().diagnostics.completed_tasks == 4);
  const auto issued = succeeded.value().diagnostics.issued_work;
  PS_CHECK(issued.work >= 40 && issued.stages == 8);
  auto second = context.submit(prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(second);
  auto repeated = second.value().join();
  PS_REQUIRE_OK(repeated);
  PS_CHECK(repeated.value().diagnostics.issued_work.work == issued.work);
  PS_CHECK(repeated.value().diagnostics.issued_work.stages == issued.stages);
  auto low = options;
  low.maximum_refinement_stages = 7;
  auto failed_run = context.submit(prepared.value(), {}, {}, low);
  PS_REQUIRE_OK(failed_run);
  auto failed = failed_run.value().join();
  PS_CHECK(!failed.ok());
  PS_CHECK(failed.status().code == ps::ErrorCode::ResourceExhausted);
  PS_CHECK(failed.status().reason == ps::FailureReason::StageLimit);
  PS_CHECK(failed.status().detail.origin == ps::FailureOrigin::Resource);
  PS_CHECK(failed.status().detail.scope == ps::FailureScope::Run);
  PS_CHECK(failed.status().detail.node_id != 0 &&
           failed.status().detail.task_id);
  auto observation = failed_run.value().observation().snapshot();
  PS_CHECK(observation.issued_work.stages == 6);
  PS_CHECK(observation.abort_drain && observation.abort_drain->finished);
  low = options;
  low.maximum_work = issued.work - 1;
  auto work_run = context.submit(prepared.value(), {}, {}, low);
  PS_REQUIRE_OK(work_run);
  auto work_failed = work_run.value().join();
  PS_CHECK(!work_failed.ok() &&
           work_failed.status().reason == ps::FailureReason::WorkLimit);
  PS_CHECK(work_run.value().observation().snapshot().issued_work.work <=
           low.maximum_work);
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->barrier = true;
    probe->entered = probe->active = probe->peak = 0;
  }
  auto concurrent_low = context.submit(prepared.value(), {}, {}, low);
  auto concurrent_ok = context.submit(prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(concurrent_low);
  PS_REQUIRE_OK(concurrent_ok);
  PS_CHECK(!concurrent_low.value().join().ok());
  PS_REQUIRE_OK(concurrent_ok.value().join());
  PS_CHECK(probe->peak == 2 && probe->active == 0 && !probe->timed_out);
  for (bool cancel : {false, true}) {
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      probe->barrier = true;
      probe->entered = probe->active = probe->peak = 0;
      probe->hold_rejection = true;
      probe->rejected = probe->release = probe->rejection_held = false;
      probe->timed_out = false;
      probe->return_ok = true;
    }
    auto parallel = options;
    parallel.maximum_parallelism = 2;
    parallel.maximum_refinement_stages = 3;
    auto run = context.submit(prepared.value(), {}, {}, parallel);
    PS_REQUIRE_OK(run);
    bool rejected;
    {
      std::unique_lock<std::mutex> lock(probe->mutex);
      rejected = probe->changed.wait_for(lock, std::chrono::seconds(5),
                                         [&] { return probe->rejected; });
    }
    bool aborted = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto snapshot = run.value().observation().snapshot();
      if (snapshot.abort_drain) {
        aborted = true;
        break;
      }
      std::this_thread::yield();
    }
    bool held;
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      held = probe->rejection_held && !probe->timed_out;
    }
    if (cancel)
      run.value().request_cancel();
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      probe->release = true;
    }
    probe->changed.notify_all();
    auto terminal = run.value().join();
    PS_CHECK(rejected && aborted && held && probe->peak == 2 &&
             probe->active == 0 && !probe->timed_out);
    PS_CHECK(!terminal.ok());
    PS_CHECK(
        terminal.status().code ==
        (cancel ? ps::ErrorCode::Cancelled : ps::ErrorCode::ResourceExhausted));
    const auto issued_parallel =
        run.value().observation().snapshot().issued_work;
    PS_CHECK(issued_parallel.stages == 2);
    PS_CHECK(issued_parallel.work == issued.work - 30);
  }
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->barrier = probe->hold_rejection = probe->return_ok = false;
  }
  ps::ResourceLimits limits;
  limits.maximum_stages = 12;
  config.managed_resources = limits;
  ps::ExecutionContext shared_root(registry, config);
  auto shared_prepared = shared_root.prepare(*compiled.value().plan);
  PS_REQUIRE_OK(shared_prepared);
  auto a = shared_root.submit(shared_prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(a);
  PS_REQUIRE_OK(a.value().join());
  auto b = shared_root.submit(shared_prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(b);
  auto shared_failure = b.value().join();
  PS_CHECK(!shared_failure.ok() &&
           shared_failure.status().reason == ps::FailureReason::StageLimit);
  auto root = shared_root.resource_budget();
  PS_REQUIRE_OK(root);
  PS_CHECK(root.value().statistics().issued.stages == 12);
  std::cout << "Run v9 work/stage isolation, cumulative admission, abort drain "
               "and ABI checks PASS\n";
  return 0;
}
