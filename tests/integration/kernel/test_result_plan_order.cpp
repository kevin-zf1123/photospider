#include <atomic>
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
  std::vector<unsigned> order;
  bool cancel = false;
  std::weak_ptr<const CpuStorage> source;
  CancellationSource cancellation;
  std::atomic<unsigned> sources{0}, active{0}, peak{0};
};
struct Program {
  Control* control;
  unsigned mode;
  Program(Control* control, unsigned mode) : control(control), mode(mode) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    const auto active = ++control->active;
    auto peak = control->peak.load();
    while (peak < active &&
           !control->peak.compare_exchange_weak(peak, active)) {
    }
    struct Retire {
      Control* control;
      ~Retire() { --control->active; }
    } retire{control};
    {
      std::lock_guard<std::mutex> lock(control->mutex);
      control->order.push_back(mode);
    }
    double number = 2;
    if (mode) {
      check(phase.read_tensor(0, 0, {0}, &number, sizeof(number)));
      if (mode == 3) {
        double right = 0;
        check(phase.read_tensor(1, 0, {0}, &right, sizeof(right)));
        number += right;
        // Compiled producer values remain retained through this Run.
        if (control->source.expired())
          return Result<ResultProgramPoll>(
              Status{ErrorCode::OperationFailed,
                     "source released before downstream completion"});
      } else {
        if (mode == 2 && control->cancel)
          control->cancellation.cancel();
        if (control->source.expired())
          return Result<ResultProgramPoll>(
              Status{ErrorCode::OperationFailed, "source released during use"});
        number += mode;
      }
    } else {
      ++control->sources;
    }
    auto status = phase.consume_work(1);
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key, {}));

    auto made = phase.resources.allocator().allocate(sizeof(number));
    if (!made.ok())
      return Result<ResultProgramPoll>(made.status());
    auto buffer = made.take_value();
    std::memcpy(buffer.data(), &number, sizeof(number));
    auto storage = std::move(buffer).freeze();
    if (!mode)
      control->source = storage;
    check(builder.publish_tensor(0, Region::whole({1}), {0, {8}},
                                 std::move(storage)));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
constexpr std::uint64_t kJoinedPayload = sizeof(Program) + 4 * sizeof(double);
constexpr std::uint64_t kNamedPayload = sizeof(Program) + 3 * sizeof(double);
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
      input.tensor_key = "number";
    }
    operation.traits.outputs = {multi_result::output("value")};
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
  auto result = context.execute(plan.plan.value(), {},
                                control.cancellation.token(), options);
  const bool exhausted = maximum_payload == kJoinedPayload - 1;
  if (exhausted) {
    PS_CHECK(!result.ok() &&
             result.status().code == ErrorCode::ResourceExhausted);
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
    PS_CHECK(result.value().diagnostics.peak_active_tasks == 1);
  }
  PS_CHECK(control.peak == 1 && control.active == 0 && control.sources == 1);
  const std::vector<unsigned> expected =
      named || cancel ? std::vector<unsigned>{0, 1, 2}
                      : std::vector<unsigned>{0, 1, 2, 3};
  PS_CHECK(control.order == expected);
  PS_CHECK(control.source.expired());
  const auto root = take(context.resource_budget());
  const auto stats = root.statistics();
  PS_CHECK(stats.live[ResourceKind::Queue] == 0);
  if (cancel || exhausted) {
    PS_CHECK(stats.live[ResourceKind::Payload] == 0);
  } else {
    PS_CHECK(stats.live[ResourceKind::Payload] == (named ? 16 : 8));
    PS_CHECK(stats.peak[ResourceKind::Payload] ==
             (named ? kNamedPayload : kJoinedPayload));
  }
  if (maximum_payload == kJoinedPayload) {
    control.order.clear();
    control.sources = 0;
    auto held = context.execute(plan.plan.value(), {}, {}, options);
    PS_CHECK(!held.ok() && held.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(control.order == expected && control.sources == 1 &&
             control.active == 0 && control.source.expired());
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 8 &&
             root.statistics().live[ResourceKind::Queue] == 0);
    result = Result<ExecutionResult>(ExecutionResult{});
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
    control.order.clear();
    control.sources = 0;
    auto recovered = context.execute(plan.plan.value(), {}, {}, options);
    if (!recovered.ok())
      std::cerr << recovered.status().message << '\n';
    PS_CHECK(recovered.ok());
    PS_CHECK(multi_result::number(recovered.value().results.at("result")) == 7);
    PS_CHECK(control.order == expected && control.sources == 1 &&
             control.active == 0 && control.source.expired());
    PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0 &&
             root.statistics().live[ResourceKind::Payload] == 8);
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
  PS_CHECK(fork(2, false, kJoinedPayload) == 0);
  PS_CHECK(fork(2, false, kJoinedPayload - 1) == 0);
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
