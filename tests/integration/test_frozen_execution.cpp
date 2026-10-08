#include <memory>
#include <string>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto operations = make_default_operation_registry();
  Compiler compiler(operations);
  ExecutionContext execution(operations);
  const auto root = execution.resource_budget().take_value();
  const auto baseline = root.statistics().live;
  {
    GraphContext graph(test::addition_document(2, 3));
    auto compiled = compiler.compile(graph).take_value();
    auto owner = execution.freeze(compiled.plan).take_value();
    const auto captured = root.statistics().live.values;
    PS_CHECK(root.statistics().live[ResourceKind::Metadata] >
             baseline[ResourceKind::Metadata]);
    auto copy = owner;
    owner = {};
    PS_CHECK(root.statistics().live.values == captured);
    copy = {};
    PS_CHECK(root.statistics().live.values == baseline.values);
  }
  FrozenExecution frozen;
  {
    auto document = test::addition_document(2, 3);
    document.outputs.push_back({"left", 1, "value"});
    GraphContext graph(document);
    auto compiled = compiler.compile(graph);
    PS_CHECK(compiled.ok());
    auto captured = execution.freeze(compiled.value().plan);
    PS_CHECK(captured.ok());
    frozen = captured.take_value();
    graph.replace(test::addition_document(7, 11));
    PS_CHECK(execution.execute(compiled.value().plan).status().code ==
             ErrorCode::Stale);
    auto result = execution.execute(frozen);
    PS_CHECK(result.ok() && test::named_scalar(result.value(), "sum") == 5);
    PS_CHECK(execution.freeze(compiled.value().plan).status().code ==
             ErrorCode::Stale);
  }
  auto result = execution.execute(frozen);
  PS_CHECK(result.ok() && test::named_scalar(result.value(), "sum") == 5);
  auto retained = frozen;
  frozen = {};
  PS_CHECK(!frozen.valid() && !frozen.plan().current());
  result = execution.execute(retained);
  PS_CHECK(result.ok() && result.value().results.size() == 2 &&
           test::named_scalar(result.value(), "sum") == 5);
  frozen = retained;
  auto tile = frozen.for_region("sum", Region::whole({1}));
  PS_CHECK(tile.ok());
  PS_CHECK(execution.execute(tile.value()).ok());
  auto left = frozen.for_region("left", Region::whole({1}));
  PS_CHECK(left.ok() && frozen.plan().outputs().size() == 2 &&
           tile.value().plan().outputs().count("sum") == 1 &&
           tile.value().plan().outputs().count("left") == 0);
  auto selected = execution.execute(left.value());
  PS_CHECK(selected.ok() && test::named_scalar(selected.value(), "left") == 2 &&
           selected.value().results.count("sum") == 0);
  CancellationSource source;
  source.cancel();
  PS_CHECK(execution.execute(frozen, source.token()).status().code ==
           ErrorCode::Cancelled);
  PS_CHECK(execution.execute(FrozenExecution{}).status().code ==
           ErrorCode::Stale);
  ExecutionContext foreign(make_default_operation_registry());
  PS_CHECK(foreign.execute(frozen).status().code == ErrorCode::Stale);
  PS_CHECK(!frozen.for_region("missing", Region::whole({1})).ok());
  auto handle = execution.open_demand(frozen.plan(), {}).take_value();
  auto pinned = handle.freeze().take_value();
  DemandQuery query{{"sum", Footprint::all({1}).take_value()}};
  auto first = handle.request(query);
  PS_CHECK(first.ok() && first.value().generation == 1);
  auto replaced = handle.replace_bindings({});
  PS_CHECK(replaced.ok() && replaced.value().generation == 2);
  // A latest-request predicate must not change an independently pinned plan.
  PS_CHECK(pinned.plan().current());
  auto old = execution.execute(pinned);
  PS_CHECK(old.ok() && test::named_scalar(old.value(), "sum") == 5);
  auto next = handle.request(query);
  PS_CHECK(next.ok() && next.value().generation == 2);
  PS_CHECK(handle.cancel());
  PS_CHECK(execution.execute(pinned).ok());
  return 0;
}
