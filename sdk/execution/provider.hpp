#pragma once
#include <memory>
#include <mutex>
#include <utility>

#include "photospider/execution/execution.hpp"
#include "photospider/plugin/executor_module_api.h"
namespace ps::executor_sdk {
struct Environment {
  std::shared_ptr<OperationRegistry> operations;
  ExecutionContextConfig config;
  ResourceBudget budget;
  std::shared_ptr<const void> module;
};
// Kept separately from the submission so a completed Run never retains
// input Results or the observer merely to preserve its terminal failure.
struct ObserverDelivery {
  mutable std::mutex mutex;
  Status failure;
  Status deliver(const std::function<void(const CallRecord&)>& observer,
                 const CallRecord& record);
  Status outcome() const;
  bool on_current_thread() const noexcept;
};
struct Submission : std::enable_shared_from_this<Submission> {
  Submission(ExecutionPlan p, ExecutionBindings b, CancellationToken c,
             ExecutionOptions o)
      : plan(std::move(p)),
        bindings(std::move(b)),
        cancellation(std::move(c)),
        options(std::move(o)) {}
  ExecutionPlan plan;
  ExecutionBindings bindings;
  CancellationToken cancellation;
  ExecutionOptions options;
  std::shared_ptr<ObserverDelivery> delivery;
};
struct PhysicalHostServices {
  const Environment& (*environment)(const void*);
  const ExecutionPlan& (*plan)(const void*);
  const Submission& (*submission)(const void*);
  void (*result)(void*, ExecutionResult&&);
};
void bind_plan_services(ps_executor_plan_v1* plan);
Status validate_bindings(const ExecutionPlan&, const ExecutionBindings&);
Status validate_exports(const ExecutionPlan&, const CallRecord&);
Status validate_results(const ExecutionPlan&, const ExecutionResult&);
void bind_data_services(ps_executor_host_v1* host);
const ps_executor_module_v2& physical_api();
}  // namespace ps::executor_sdk
