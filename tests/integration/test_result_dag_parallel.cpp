#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Control {
  std::mutex mutex;
  std::condition_variable changed;
  bool overlap = true, left_entered = false, right_done = false;
  bool check_release = true, cancel = false;
  unsigned backed = 0;
  bool allocation_failed = false;
  std::weak_ptr<const CpuStorage> source;
  CancellationSource cancellation;
  std::atomic<unsigned> sources{0}, active{0}, peak{0};
};
struct Program {
  Control* control;
  unsigned mode;
  bool requested = false;
  Program(Control* control, unsigned mode) : control(control), mode(mode) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (mode && !requested) {
      requested = true;
      ResultProgramNeed need;
      for (unsigned port = 0; port < (mode == 3 ? 2U : 1U); ++port)
        need.tensors.push_back({port, 0, take(Footprint::all({1})), 1});
      return Result<ResultProgramPoll>(std::move(need));
    }
    const auto active = ++control->active;
    auto peak = control->peak.load();
    while (peak < active &&
           !control->peak.compare_exchange_weak(peak, active)) {
    }
    struct Retire {
      Control* control;
      ~Retire() { --control->active; }
    } retire{control};
    double number = 2;
    if (mode) {
      check(phase.read_tensor(0, 0, {0}, &number, sizeof(number)));
      if (mode == 3) {
        double right = 0;
        check(phase.read_tensor(1, 0, {0}, &right, sizeof(right)));
        number += right;
        if (control->check_release && !control->source.expired())
          return Result<ResultProgramPoll>(Status{
              ErrorCode::OperationFailed, "source retained after last use"});
      } else {
        std::unique_lock<std::mutex> lock(control->mutex);
        if (mode == 1) {
          control->left_entered = true;
          control->changed.notify_all();
          if (control->overlap &&
              !control->changed.wait_for(lock, std::chrono::seconds(2),
                                         [&] { return control->right_done; }))
            return Result<ResultProgramPoll>(Status{
                ErrorCode::OperationFailed, "independent right did not run"});
        } else {
          if (control->overlap &&
              !control->changed.wait_for(lock, std::chrono::seconds(2),
                                         [&] { return control->left_entered; }))
            return Result<ResultProgramPoll>(Status{
                ErrorCode::OperationFailed, "independent left did not run"});
          control->right_done = true;
          if (control->cancel)
            control->cancellation.cancel();
          control->changed.notify_all();
        }
        if (control->source.expired())
          return Result<ResultProgramPoll>(
              Status{ErrorCode::OperationFailed, "source released during use"});
        number += mode;
      }
    } else {
      ++control->sources;
    }
    check(phase.consume_work(1));
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    auto relation_lease = take(phase.resources.reserve(ResourceCapacity::host(
        2 * sizeof(ResultRelation), 2 * sizeof(ResultRelation))));
    std::vector<ResultRelation> relations;
    relations.reserve(2);
    for (unsigned port = 0; port < (mode == 3 ? 2U : mode ? 1U : 0U); ++port)
      relations.push_back(take(ResultRelation::cartesian(
          phase.resources, 1,
          {port, 1, 0, 1, ResultSupportTarget::Tensor, 0})));
    auto relation =
        relations.empty()
            ? take(ResultRelation::cartesian(phase.resources, 1, {}))
            : take(ResultRelation::unite(phase.resources, relations));
    auto made = phase.resources.allocator().allocate(sizeof(number));
    if (!made.ok()) {
      std::lock_guard<std::mutex> lock(control->mutex);
      control->allocation_failed = true;
      control->changed.notify_all();
      return Result<ResultProgramPoll>(made.status());
    }
    auto buffer = made.take_value();
    if ((mode == 1 || mode == 2) && control->overlap) {
      std::unique_lock<std::mutex> lock(control->mutex);
      ++control->backed;
      control->changed.notify_all();
      if (!control->changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return control->backed == 2 || control->allocation_failed ||
                   control->cancellation.token().cancelled();
          }))
        return Result<ResultProgramPoll>(
            Status{ErrorCode::OperationFailed, "parallel backing barrier"});
      if (control->cancellation.token().cancelled())
        return Result<ResultProgramPoll>(Status{ErrorCode::Cancelled, {}});
      if (control->allocation_failed)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::ResourceExhausted, {}});
    }
    std::memcpy(buffer.data(), &number, sizeof(number));
    auto storage = std::move(buffer).freeze();
    if (!mode)
      control->source = storage;
    check(builder.publish_tensor(0, Region::whole({1}), {0, {8}},
                                 std::move(storage), std::move(relation),
                                 {true, true, true, true}));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
std::shared_ptr<OperationRegistry> registry(Control* control) {
  auto registry = std::make_shared<OperationRegistry>();
  const std::string keys[] = {"source", "left", "right", "join"};
  for (unsigned mode = 0; mode < 4; ++mode) {
    OperationDefinition operation;
    operation.key = keys[mode];
    operation.traits.cacheable = false;
    operation.traits.input_count = mode == 3 ? 2 : mode ? 1 : 0;
    operation.traits.input_schema.resize(operation.traits.input_count);
    for (auto& input : operation.traits.input_schema) {
      input.kind = OperationPortKind::Result;
      input.tensor_key = "number";
    }
    operation.traits.outputs = {multi_result::output("value")};
    operation.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    operation.start_result = [control, mode](const auto&,
                                             const auto& allocator) {
      return ResultContinuation::make<Program>(allocator, control, mode);
    };
    check(registry->register_operation(std::move(operation)));
  }
  check(registry->freeze());
  return registry;
}
int fork(unsigned parallelism, bool cancel = false,
         std::uint64_t maximum_payload = UINT64_MAX, bool named = false,
         ResultRef* retained = nullptr,
         ResourceBudget* retained_root = nullptr) {
  Control control;
  control.overlap = parallelism > 1;
  control.cancel = cancel;
  auto operations = registry(&control);
  WorkflowDocument document;
  document.nodes = {
      {1, "source", {}, {}},
      {2, "left", {WorkflowNodeOutput{1, "value"}}, {}},
      {3, "right", {WorkflowNodeOutput{1, "value"}}, {}},
      {4,
       "join",
       {WorkflowNodeOutput{2, "value"}, WorkflowNodeOutput{3, "value"}},
       {}},
      {5, "source", {}, {}}};
  document.outputs = {{"result", 4, "value"}};
  if (named)
    document.outputs = {{"left", 2, "value"}, {"right", 3, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(operations).compile(graph));
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.result_cache_bytes = 0;
  config.maximum_live_bytes = maximum_payload;
  ExecutionContext context(operations, config);
  ExecutionOptions options;
  options.maximum_parallelism = parallelism;
  options.enable_joint = false;
  auto result =
      context.execute(plan.plan, {}, control.cancellation.token(), options);
  if (maximum_payload == 71) {
    PS_CHECK(!result.ok() &&
             result.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(control.active == 0);
    PS_CHECK(take(context.resource_budget())
                 .statistics()
                 .live[ResourceKind::Queue] == 0);
    return 0;
  } else if (cancel) {
    PS_CHECK(!result.ok() && result.status().code == ErrorCode::Cancelled);
  } else {
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok());
    if (named) {
      PS_CHECK(multi_result::number(result.value().results.at("left")) == 3);
      PS_CHECK(multi_result::number(result.value().results.at("right")) == 4);
    } else {
      PS_CHECK(multi_result::number(result.value().results.at("result")) == 7);
    }
    PS_CHECK(result.value().diagnostics.peak_active_tasks <= parallelism);
    if (parallelism > 1)
      PS_CHECK(control.peak == 2);
    else
      PS_CHECK(control.peak == 1);
  }
  PS_CHECK(control.active == 0 && control.sources == 1);
  PS_CHECK(control.source.expired());
  const auto stats = take(context.resource_budget()).statistics();
  PS_CHECK(stats.live[ResourceKind::Queue] == 0);
  if (!cancel) {
    PS_CHECK(stats.live[ResourceKind::Payload] == (named ? 16 : 8));
    PS_CHECK(stats.peak[ResourceKind::Payload] <= 72);
  }
  if (maximum_payload == 72) {
    control.left_entered = control.right_done = false;
    control.backed = 0;
    control.allocation_failed = false;
    auto held = context.execute(plan.plan, {}, {}, options);
    PS_CHECK(!held.ok() && held.status().code == ErrorCode::ResourceExhausted);
    result = Result<ExecutionResult>(ExecutionResult{});
    control.left_entered = control.right_done = false;
    control.backed = 0;
    control.allocation_failed = false;
    auto recovered = context.execute(plan.plan, {}, {}, options);
    if (!recovered.ok())
      std::cerr << recovered.status().message << '\n';
    PS_CHECK(recovered.ok());
    PS_CHECK(multi_result::number(recovered.value().results.at("result")) == 7);
  }
  if (retained)
    *retained = result.value().results.at("result");
  if (retained_root)
    *retained_root = take(context.resource_budget());
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(fork(2) == 0);
  PS_CHECK(fork(1) == 0);
  PS_CHECK(fork(2, true) == 0);
  PS_CHECK(fork(2, false, 72) == 0);
  PS_CHECK(fork(2, false, 71) == 0);
  PS_CHECK(fork(2, false, UINT64_MAX, true) == 0);
  ResultRef retained;
  ResourceBudget root;
  PS_CHECK(fork(2, false, UINT64_MAX, false, &retained, &root) == 0);
  PS_CHECK(retained.owned_by(root) && multi_result::number(retained) == 7);
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 8);
  retained = {};
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
