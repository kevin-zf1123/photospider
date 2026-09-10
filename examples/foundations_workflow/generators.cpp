#include <atomic>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "workflow.hpp"  // NOLINT(build/include_subdir)

namespace foundations {
namespace {
Parameters sample(const std::string& expression, std::int64_t count = 3,
                  double step = .5) {
  return {{"expression", expression},
          {"count", count},
          {"start", 0.},
          {"step", step}};
}
ps::Value signal(const std::vector<float>& values) {
  ps::SemanticDescriptor s;
  s.kind = ps::SemanticKind::SampledSignal;
  s.channels = {{"value", "value", "dimensionless"}};
  s.sample_step = 1;
  s.sample_axis_unit = "dimensionless";
  return array(values, {}, facets(s));
}
}  // namespace
void expressions() {
  auto coefficients = array<double>({1});
  exact<float>(output(operation("numeric.sample_expression", {coefficients},
                                sample("c[0]*x^2"))),
               {0, .25F, 1});
  exact<float>(output(operation("numeric.sample_expression", {coefficients},
                                sample("2*x+1", 5, .25))),
               {1, 1.5F, 2, 2.5F, 3});
  exact<float>(
      output(evaluate(
          {coefficients, signal({.25F})},
          {{1,
            "numeric.sample_expression",
            {ps::WorkflowInputReference{1}},
            sample("x^2")},
           {2,
            "lut.apply_1d",
            {ps::WorkflowInputReference{2}, ps::WorkflowNodeOutput{1, "value"}},
            {{"out_of_domain", std::string("reject")}}}})),
      {.125F});
  rejected(operation("numeric.sample_expression", {coefficients},
                     sample("min(1e300*1e300,0)")),
           ps::ErrorCode::OperationFailed);
  rejected(
      operation("numeric.sample_expression", {coefficients}, sample("c[1]")),
      ps::ErrorCode::InvalidArgument);
  std::cout << "expression-lut x_squared=[0,.25,1] query_.25=.125 "
               "count5=[1,1.5,2,2.5,3] invalid_AST_and_nonfinite=rejected "
               "oracle=passed\n";
}
void generator_gain() {
  // Public registry wrappers count callback entry without altering operation
  // traits.
  auto base = ps::make_default_operation_registry();
  auto registry = std::make_shared<ps::OperationRegistry>();
  std::atomic<unsigned> producers{0}, consumers{0};
  for (const char* key : {"numeric.sample_expression", "image.exposure_gain"}) {
    const auto name = std::string(key);
    auto status = registry->register_operation(
        {name, take(base->find_traits(name)),
         [&, name](const ps::OperationInvocation& call) {
           if (name == "numeric.sample_expression")
             ++producers;
           else
             ++consumers;
           return base->invoke(name, call);
         }});
    require(status.ok(), status.message);
  }
  require(registry->freeze().ok(), "registry freeze failed");
  const auto image =
      array<float>({-2, 3, 4, .5F}, {1, 1, 4}, facets(ps::rgba_semantics()));
  const auto coefficient = array<double>({1});
  auto doc = document(
      {image, coefficient},
      {{1,
        "numeric.sample_expression",
        {ps::WorkflowInputReference{2}},
        sample("c[0]*2", 1, 1)},
       {2,
        "image.exposure_gain",
        {ps::WorkflowInputReference{1}, ps::WorkflowNodeOutput{1, "value"}},
        {}}});
  auto bound = bindings({image, coefficient});
  ps::InputSnapshotStore snapshots;
  bound.inputs[0].snapshot =
      std::make_shared<ps::InputSnapshot>(take(snapshots.import_value(image)));
  bound.inputs[0].value = {};
  ps::GraphContext graph(doc);
  ps::Compiler compiler(registry);
  auto plan = take(compiler.compile(graph)).plan;
  ps::ExecutionContext execution(registry, {4, false, 32, 1024 * 1024, 65536});
  auto run = [&](double c) {
    auto b = bound;
    b.inputs[1].value = array<double>({c});
    return execution.execute(plan, b);
  };
  for (double c : {.5, 1., 2., 8.})
    exact<float>(output(run(c)),
                 {static_cast<float>(-4 * c), static_cast<float>(6 * c),
                  static_cast<float>(8 * c), .5F});
  auto a = std::async(std::launch::async, [&] { return run(3); });
  auto b = std::async(std::launch::async, [&] { return run(4); });
  exact<float>(output(a.get()), {-12, 18, 24, .5F});
  exact<float>(output(b.get()), {-16, 24, 32, .5F});
  auto before = producers.load();
  auto warm = take(run(2));
  require(warm.diagnostics.cache_hits > 0 && producers == before,
          "warm generator recomputed");
  const auto callbacks = consumers.load();
  rejected(run(10), ps::ErrorCode::OperationFailed);
  require(consumers == callbacks, "invalid fresh gain entered callback");
  // Prime the generator's valid signal value 20, then reject it as bounded
  // gain.
  auto producer_doc = doc;
  producer_doc.nodes.resize(1);
  producer_doc.outputs = {{"result", 1, "value"}};
  ps::GraphContext producer_graph(producer_doc);
  auto producer_plan = take(compiler.compile(producer_graph)).plan;
  auto bad = bound;
  bad.inputs[1].value = array<double>({10});
  exact<float>(output(execution.execute(producer_plan, bad)), {20});
  before = producers.load();
  rejected(execution.execute(plan, bad), ps::ErrorCode::OperationFailed);
  require(producers == before && consumers == callbacks,
          "cached invalid gain crossed callback boundary");
  bad.inputs[1].value =
      array<double>({std::numeric_limits<double>::quiet_NaN()});
  rejected(execution.execute(plan, bad), ps::ErrorCode::OperationFailed);
  require(consumers == callbacks, "NaN coefficients entered gain callback");
  std::cout << "generator-gain plan_reused=passed "
               "sequential_and_concurrent=passed warm_cache_hits="
            << warm.diagnostics.cache_hits
            << " fresh_and_cached_invalid_callback_entries=0 oracle=passed\n";
}
}  // namespace foundations
