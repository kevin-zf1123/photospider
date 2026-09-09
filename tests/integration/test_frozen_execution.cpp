#include <memory>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto operations = make_default_operation_registry();
  Compiler compiler(operations);
  ExecutionContext execution(operations);
  FrozenExecution frozen;
  {
    GraphContext graph(test::addition_document(2, 3));
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
  auto tile = frozen.for_region("sum", Region::whole({1}));
  PS_CHECK(tile.ok());
  PS_CHECK(execution.execute(tile.value()).ok());
  CancellationSource source;
  source.cancel();
  PS_CHECK(execution.execute(frozen, source.token()).status().code ==
           ErrorCode::Cancelled);
  PS_CHECK(execution.execute(FrozenExecution{}).status().code ==
           ErrorCode::Stale);
  ExecutionContext foreign(make_default_operation_registry());
  PS_CHECK(foreign.execute(frozen).status().code == ErrorCode::Stale);
  PS_CHECK(!frozen.for_region("missing", Region::whole({1})).ok());
  return 0;
}
