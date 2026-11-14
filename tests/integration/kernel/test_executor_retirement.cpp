#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "support/kernel_registry_fixture.hpp"
#include "support/test_support.hpp"

namespace {
std::shared_ptr<ps::OperationRegistry> registry() {
  auto ops = ps::test::kernel_registry(false);
  for (const char* name : {"export.f64", "export.u8", "prepare.fail"}) {
    ps::OperationDefinition op;
    op.key = name;
    auto scalar =
        multi_result::schema(op.key == "export.u8" ? ps::ElementType::UInt8
                                                   : ps::ElementType::Float64);
    op.traits.outputs[0] = multi_result::output("value", scalar);
    ps::test::install_scalar(op, false);
    auto lower = op.lower;
    op.lower = [lower](const auto& q) {
      auto result = lower(q);
      if (!result.ok())
        return result;
      auto subgraph = result.take_value();
      subgraph.program.calls[0].returns = subgraph.outputs;
      return ps::Result<ps::OperationProgram>(std::move(subgraph));
    };
    if (op.key == "prepare.fail") {
      op.prepare_kernel = [](const auto&, const auto&, const auto&, const auto&,
                             const auto*) {
        ps::Status status{ps::ErrorCode::OperationFailed, "prepare failed",
                          ps::FailureReason::InvalidDomain};
        status.detail.domain_code = 77;
        return ps::Result<ps_prepared_kernel_v8>(std::move(status));
      };
    }
    multi_result::check(ops->register_operation(std::move(op)));
  }
  ps::OperationDefinition consume;
  consume.key = "consume.f64";
  consume.traits.input_count = 1;
  consume.traits.input_schema.resize(1);
  consume.traits.input_schema[0].element_type_mask = 127;
  consume.traits.outputs[0] = multi_result::output(
      "value", multi_result::schema(ps::ElementType::UInt8));
  consume.lower = [](const ps::OperationLowering& q) {
    ps::OperationProgramBuilder builder(q);
    auto task = builder.add_task("consume", "retirement-fixture");
    if (!task.ok())
      return ps::Result<ps::OperationProgram>(task.status());
    auto status = builder.read_input(task.value(), 0, 0, 0);
    if (status.ok())
      status = builder.write_output(task.value(), 0, 0, 0);
    if (!status.ok())
      return ps::Result<ps::OperationProgram>(status);
    return builder.finish();
  };
  multi_result::check(ops->register_operation(std::move(consume)));
  multi_result::check(ops->freeze());
  return ops;
}
}  // namespace
int main() {
  ps::ExecutionContextConfig sizing;
  const auto default_limit = ps::ResultGrowthLimits{}.maximum_bytes;
  PS_CHECK(sizing.result_growth_limits().maximum_bytes == default_limit);
  sizing.maximum_live_bytes = 2 * default_limit;
  PS_CHECK(sizing.result_growth_limits().maximum_bytes == 2 * default_limit);
  auto operations = registry();
  ps::WorkflowDocument document;
  document.nodes = {{1, "core.constant", {}, {{"value", 4.0}}},
                    {2, "core.constant", {}, {{"value", 5.0}}}};
  document.outputs = {{"one", 1, "value"}, {"two", 2, "value"}};
  ps::GraphContext graph(document);
  auto compiled = ps::Compiler(operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  const auto& plan = *compiled.value().plan;
  const auto& program = plan.program();
  PS_CHECK(program.tasks.size() == 2);
  const auto call = program.tasks[0].call;
  PS_CHECK(program.calls[call].source_call == 1);
  PS_CHECK(program.tasks[1].call != call);

  ps::ExecutorRegistry providers;
  PS_REQUIRE_OK(providers.load_plugin(PS_RETIREMENT_MODULE));
  PS_CHECK(providers.load_plugin(PS_OBSOLETE_MODULE).code ==
           ps::ErrorCode::TypeMismatch);
  PS_CHECK(providers.load_plugin(PS_BAD_EXECUTOR_MODULE).code ==
           ps::ErrorCode::InvalidArgument);
  providers.freeze();
  for (bool observer : {false, true}) {
    for (unsigned mode = 0; mode != 8; ++mode) {
      ps::ExecutionContextConfig config;
      config.cpu_workers = mode + 1;
      ps::ExecutionContext context(operations, config, providers, "retirement");
      auto prepared = context.prepare(plan);
      PS_REQUIRE_OK(prepared);
      unsigned calls = 0;
      std::optional<ps::CallRecord> observed;
      ps::ExecutionOptions options;
      if (observer) {
        options.on_call_retired = [&](const ps::CallRecord& record) {
          ++calls;
          observed = record;
        };
      }
      auto run = context.submit(prepared.value(), {}, {}, options);
      if (mode) {
        PS_CHECK(!run.ok() &&
                 run.status().code == ps::ErrorCode::InvalidArgument);
        PS_CHECK(calls == 0 && !observed);
        continue;
      }
      PS_REQUIRE_OK(run);
      ps::ExecutionTaskTiming task_timing;
      ps::ExecutionCallTiming call_timing;
      PS_CHECK(run.value().observation().copy_task_timings(&task_timing, 1) <=
               1);
      PS_CHECK(run.value().observation().copy_call_timings(&call_timing, 1) <=
               1);
      auto failure = run.value().join();
      PS_CHECK(!failure.ok() &&
               failure.status().code == ps::ErrorCode::OperationFailed &&
               failure.status().reason == ps::FailureReason::InvalidDomain &&
               failure.status().detail.domain_code == 77 &&
               failure.status().detail.task_id == 0);
      PS_CHECK(calls == (observer ? 1U : 0U));
      if (observer) {
        PS_CHECK(observed && observed->sequence == 7 &&
                 observed->activation == 3 && observed->source_call == 1 &&
                 observed->call == call && observed->failed_task == 0 &&
                 observed->outcome == ps::CallOutcome::Failed &&
                 observed->values.empty());
        const auto& status = observed->status;
        PS_CHECK(status.code == ps::ErrorCode::OperationFailed &&
                 status.reason == ps::FailureReason::InvalidDomain &&
                 status.detail.domain_code == 77 &&
                 status.detail.origin == ps::FailureOrigin::Backend &&
                 status.detail.scope == ps::FailureScope::Run &&
                 status.detail.node_id == 1 && status.detail.task_id == 0);
      }
    }
  }
  document.nodes = {{1, "export.f64", {}, {}}, {2, "export.u8", {}, {}}};
  ps::GraphContext exports_graph(document);
  auto exports = ps::Compiler(operations).compile(exports_graph);
  PS_REQUIRE_OK(exports);
  for (unsigned mode = 8; mode <= 12; ++mode) {
    for (bool observer : {false, true}) {
      ps::ExecutionContextConfig config;
      config.cpu_workers = mode + 1;
      ps::ExecutionContext context(operations, config, providers, "retirement");
      auto prepared = context.prepare(*exports.value().plan);
      PS_REQUIRE_OK(prepared);
      unsigned delivered = 0;
      double sample = 0;
      ps::ExecutionOptions options;
      if (observer) {
        options.on_call_retired = [&](const auto& record) {
          ++delivered;
          sample = multi_result::number(record.values.begin()->second);
        };
      }
      auto run = context.submit(prepared.value(), {}, {}, options);
      if (mode == 8) {
        PS_REQUIRE_OK(run);
        PS_CHECK(delivered == (observer ? 1U : 0U));
        if (observer)
          PS_CHECK(sample == 4);
      } else {
        PS_CHECK(!run.ok() &&
                 run.status().code == ps::ErrorCode::InvalidArgument &&
                 delivered == 0);
      }
    }
  }
  document.nodes[0].operation = "prepare.fail";
  ps::GraphContext failure_graph(document);
  auto fails = ps::Compiler(operations).compile(failure_graph);
  PS_REQUIRE_OK(fails);
  ps::ExecutionContextConfig config;
  config.cpu_workers = 14;
  ps::ExecutionContext checking(operations, config, providers, "retirement");
  auto prep = checking.prepare(*fails.value().plan);
  PS_CHECK(!prep.ok() && prep.status().code == ps::ErrorCode::OperationFailed &&
           prep.status().reason == ps::FailureReason::InvalidDomain &&
           prep.status().detail.domain_code == 77);
  config.cpu_workers = 15;
  ps::ExecutionContext throwing(operations, config, providers, "retirement");
  auto ready = throwing.prepare(plan);
  PS_REQUIRE_OK(ready);
  unsigned deliveries = 0;
  ps::ExecutionOptions options;
  options.on_call_retired = [&](const ps::CallRecord&) {
    ++deliveries;
    throw std::runtime_error("stop delivery");
  };
  auto run = throwing.submit(ready.value(), {}, {}, std::move(options));
  PS_REQUIRE_OK(run);
  auto failure = run.value().join();
  PS_CHECK(deliveries == 1 && !failure.ok() &&
           failure.status().code == ps::ErrorCode::OperationFailed &&
           failure.status().detail.origin == ps::FailureOrigin::Observer &&
           failure.status().detail.node_id == 1 &&
           failure.status().detail.call_sequence == 7);
  config.cpu_workers = 16;
  ps::ExecutionContext deferred(operations, config, providers, "retirement");
  auto deferred_plan = deferred.prepare(plan);
  PS_REQUIRE_OK(deferred_plan);
  ps::RunHandle joining;
  ps::ErrorCode inside = ps::ErrorCode::Ok;
  ps::ExecutionOptions deferred_options;
  deferred_options.on_call_retired = [&](const auto&) {
    inside = joining.join().status().code;
  };
  auto deferred_run = deferred.submit(deferred_plan.value(), {}, {},
                                      std::move(deferred_options));
  PS_REQUIRE_OK(deferred_run);
  joining = deferred_run.take_value();
  auto terminal = joining.join();
  PS_CHECK(inside == ps::ErrorCode::InvalidArgument && !terminal.ok() &&
           terminal.status().detail.domain_code == 77);
  // Only the U8 sink has a final output name. The C executor publishes the
  // intermediate F64 value using its physical ID from the first task.
  document.nodes = {
      {1, "export.f64", {}, {}},
      {2, "consume.f64", {ps::WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"two", 2, "value"}};
  ps::GraphContext chain_graph(document);
  auto chain = ps::Compiler(operations).compile(chain_graph);
  PS_REQUIRE_OK(chain);
  config.cpu_workers = 17;
  ps::ExecutionContext independent(operations, config, providers, "retirement");
  auto chain_plan = independent.prepare(*chain.value().plan);
  PS_REQUIRE_OK(chain_plan);
  ps::ResultRef intermediate;
  ps::ExecutionOptions capture;
  capture.on_call_retired = [&](const auto& record) {
    intermediate = record.values.begin()->second;
  };
  auto submission =
      independent.submit(chain_plan.value(), {}, {}, std::move(capture));
  PS_REQUIRE_OK(submission);
  PS_CHECK(multi_result::number(intermediate) == 4);
  return 0;
}
