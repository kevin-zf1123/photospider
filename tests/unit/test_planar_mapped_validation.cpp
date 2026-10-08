#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "execution/execution_test_hooks.hpp"
#include "support/fmt_handoff.hpp"

namespace {
using namespace ps;                   // NOLINT(build/namespaces)
using namespace ps::handoff_testing;  // NOLINT(build/namespaces)

std::shared_ptr<OperationRegistry> registry_for(
    PlanarMappedValidationCallback callback, DataMovementViewPolicy policy,
    bool different_layout = false) {
  auto registry = std::make_shared<OperationRegistry>();
  auto operation = probe_operation(
      std::make_shared<Probe>(), true, policy,
      [different_layout](OperationOutputSpecialization& output) {
        if (different_layout)
          output.metadata.planar_layout->order = ImagePlaneOrder::Continuous;
        output.static_dependency_pieces->front().inputs[0].roles |=
            static_cast<std::uint32_t>(DependencyRole::Validation);
      });
  operation.validate_planar_mapped = std::move(callback);
  take(registry->register_operation(std::move(operation)));
  take(registry->freeze());
  return registry;
}

void exits() {
  const auto image = probe_image();
  const auto document = probe_document(image);
  for (const auto policy :
       {DataMovementViewPolicy::RequireView,
        DataMovementViewPolicy::Materialize, DataMovementViewPolicy::Auto}) {
    // Callback return, ordinary exception, allocation exception crossed with
    // cancellation, currentness and both, through the public execution entry.
    for (unsigned event = 0; event < 4; ++event) {
      for (unsigned failure = 0; failure < 3; ++failure) {
        CancellationSource stop;
        GraphContext graph(document);
        auto registry = registry_for(
            [&](const PlanarMappedValidationInvocation&) -> Status {
              if (event & 1)
                stop.cancel();
              if (event & 2)
                static_cast<void>(graph.replace(document));
              if (failure == 1)
                throw std::runtime_error("injected validator exception");
              if (failure == 2)
                throw std::bad_alloc();
              return Status{ErrorCode::OperationFailed, "injected failure"};
            },
            policy);
        auto compiled = take(Compiler(registry).compile(graph));
        const auto expected = event & 1      ? ErrorCode::Cancelled
                              : event & 2    ? ErrorCode::Stale
                              : failure == 2 ? ErrorCode::ResourceExhausted
                                             : ErrorCode::OperationFailed;
        ExecutionContext execution(registry);
        auto result = execution.execute(compiled.plan, probe_bindings(image),
                                        stop.token());
        require(!result.ok() && result.status().code == expected,
                "host exit precedence");
      }
    }
  }
}

void successful_copy_and_view() {
  const auto image = probe_image();
  for (const auto policy :
       {DataMovementViewPolicy::RequireView,
        DataMovementViewPolicy::Materialize, DataMovementViewPolicy::Auto}) {
    unsigned validations = 0;
    const auto caller = std::this_thread::get_id();
    auto registry = registry_for(
        [&](const PlanarMappedValidationInvocation&) {
          require(std::this_thread::get_id() != caller,
                  "validator ran on caller instead of CPU worker");
          ++validations;
          return Status::success();
        },
        policy);
    GraphContext graph(probe_document(image));
    auto compiled = take(Compiler(registry).compile(graph));
    ExecutionContext execution(registry);
    auto result = take(execution.execute(compiled.plan, probe_bindings(image)));
    require(validations == 1, "exact piece not validated once");
    const auto region = Region::whole(image.descriptor().shape);
    auto input = take(image.acquire(region));
    auto output = take(result.tensors.at("result").acquire(region));
    for (unsigned y = 0; y < 3; ++y)
      for (unsigned x = 0; x < 5; ++x)
        for (unsigned c = 0; c < 2; ++c) {
          auto a = take(input.row_run({y, x, c}));
          auto b = take(output.row_run({y, x, c}));
          require(*a.data == *b.data, "validated transfer changed bits");
        }
  }
}

void auto_copy_fallback() {
  const auto image = probe_image();
  unsigned validations = 0;
  auto registry = registry_for(
      [&](const PlanarMappedValidationInvocation&) {
        ++validations;
        return Status::success();
      },
      DataMovementViewPolicy::Auto, true);
  GraphContext graph(probe_document(image));
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionContext execution(registry);
  auto result = take(execution.execute(compiled.plan, probe_bindings(image)));
  const auto& output_image = result.tensors.at("result");
  require(validations == 1 &&
              output_image.config().order == ImagePlaneOrder::Continuous &&
              result.diagnostics.result_copy_bytes == 30,
          "Auto did not validate and take unavailable-view copy fallback");
  const auto region = Region::whole(image.descriptor().shape);
  auto input = take(image.acquire(region));
  auto output = take(output_image.acquire(region));
  for (unsigned y = 0; y < 3; ++y)
    for (unsigned x = 0; x < 5; ++x)
      for (unsigned c = 0; c < 2; ++c)
        require(*take(input.row_run({y, x, c})).data ==
                    *take(output.row_run({y, x, c})).data,
                "Auto copied different bits");
}

void queue_resource_bound() {
  const auto image = probe_image();
  unsigned validations = 0;
  auto registry = registry_for(
      [&](const PlanarMappedValidationInvocation&) {
        ++validations;
        return Status::success();
      },
      DataMovementViewPolicy::RequireView);
  GraphContext graph(probe_document(image));
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Queue] = 0;
  ExecutionContext execution(registry, config);
  const auto result = execution.execute(compiled.plan, probe_bindings(image));
  require(!result.ok() &&
              result.status().code == ErrorCode::ResourceExhausted &&
              validations == 0,
          "mapped validator bypassed queue resource admission");
  require(take(execution.resource_budget())
                  .statistics()
                  .live[ResourceKind::Queue] == 0,
          "failed validation queue reservation leaked");
}

void stage_resource_bound() {
  const auto image = probe_image();
  unsigned validations = 0;
  auto registry = registry_for(
      [&](const PlanarMappedValidationInvocation&) {
        ++validations;
        return Status::success();
      },
      DataMovementViewPolicy::RequireView);
  GraphContext graph(probe_document(image));
  auto compiled = take(Compiler(registry).compile(graph));
  for (const auto limit : {0U, 1U}) {
    validations = 0;
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->maximum_stages = limit;
    ExecutionContext execution(registry, config);
    if (limit)
      take(execution.execute(compiled.plan, probe_bindings(image)));
    const auto result = execution.execute(compiled.plan, probe_bindings(image));
    require(!result.ok() &&
                result.status().code == ErrorCode::ResourceExhausted &&
                validations == limit,
            "mapped validator bypassed cumulative stage limit");
    const auto statistics = take(execution.resource_budget()).statistics();
    require(statistics.issued.stages == limit &&
                statistics.live[ResourceKind::Queue] == 0,
            "stage rejection changed accounting or leaked queue");
  }
}

std::function<execution_testing::CallbackSubmitAction()> submission;
execution_testing::CallbackSubmitAction submit_action(Backend) {
  return submission();
}

void submission_failures() {
  using execution_testing::CallbackSubmitAction;
  const auto image = probe_image();
  const auto document = probe_document(image);
  for (const auto action :
       {CallbackSubmitAction::Reject, CallbackSubmitAction::ThrowBadAlloc,
        CallbackSubmitAction::ThrowNullDiagnostic}) {
    for (unsigned event = 0; event < 4; ++event) {
      unsigned validations = 0;
      CancellationSource stop;
      GraphContext graph(document);
      auto registry = registry_for(
          [&](const PlanarMappedValidationInvocation&) {
            ++validations;
            return Status::success();
          },
          DataMovementViewPolicy::RequireView);
      auto compiled = take(Compiler(registry).compile(graph));
      ExecutionContext execution(registry);
      submission = [&] {
        if (event & 1)
          stop.cancel();
        if (event & 2)
          static_cast<void>(graph.replace(document));
        return action;
      };
      execution_testing::ExecutionTestHooks hooks;
      hooks.callback_submit_action = submit_action;
      execution_testing::install_execution_test_hooks(&hooks);
      auto result =
          execution.execute(compiled.plan, probe_bindings(image), stop.token());
      execution_testing::install_execution_test_hooks(nullptr);
      const auto expected =
          event & 1   ? ErrorCode::Cancelled
          : event & 2 ? ErrorCode::Stale
          : action == CallbackSubmitAction::ThrowNullDiagnostic
              ? ErrorCode::OperationFailed
              : ErrorCode::ResourceExhausted;
      require(
          !result.ok() && result.status().code == expected && validations == 0,
          "submission failure lost stop priority or invoked validator");
    }
  }
  submission = {};
}

void worker_bound() {
  const auto image = probe_image();
  std::atomic<unsigned> active{0}, peak{0};
  std::mutex mutex;
  std::condition_variable changed;
  unsigned entered = 0;
  auto registry = registry_for(
      [&](const PlanarMappedValidationInvocation&) {
        const auto n = ++active;
        auto old = peak.load();
        while (old < n && !peak.compare_exchange_weak(old, n)) {
        }
        {
          std::unique_lock<std::mutex> lock(mutex);
          ++entered;
          changed.notify_all();
          // Bounded overlap opportunity detects the original caller-thread
          // execution bug. Correct single-worker execution times out once.
          changed.wait_for(lock, std::chrono::milliseconds(150),
                           [&] { return entered >= 2; });
        }
        --active;
        return Status::success();
      },
      DataMovementViewPolicy::RequireView);
  GraphContext graph(probe_document(image));
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_queued_tasks = 2;
  ExecutionContext execution(registry, config);
  const auto run = [&] {
    return execution.execute(compiled.plan, probe_bindings(image));
  };
  auto first = std::async(std::launch::async, run);
  auto second = std::async(std::launch::async, run);
  require(first.get().ok() && second.get().ok(), "concurrent mapped execution");
  require(peak == 1 && entered == 2, "context CPU bound bypassed");
}
}  // namespace

int main() {
  exits();
  successful_copy_and_view();
  auto_copy_fallback();
  queue_resource_bound();
  stage_resource_bound();
  submission_failures();
  worker_bound();
  std::cout << "mapped validator: 36 exit-priority checks, 3 layout policies, "
               "Auto copy fallback, queue resources, worker affinity and "
               "concurrency bound passed\n";
}
