#include <memory>
#include <utility>

#include "photospider/plugin/operation_program.hpp"
#include "support/fixed_operation_fixture.hpp"
#include "support/status_check.hpp"

namespace {
ps::OperationLowering query() {
  ps::OperationLowering result;
  result.operation = "bounded";
  const auto schema = std::make_shared<const ps::SchemaTemplate>(
      ps::test::fixed_fixture::schema());
  result.inputs = {{schema}};
  result.outputs = {{schema}};
  result.requests.resize(1);
  result.tile_extent = 1;
  return result;
}
}  // namespace
int main() {
  // The aggregate table limit must reject a write before finish even though
  // every individual table remains smaller than the limit.
  auto q = query();
  q.limits.maximum_records = 12;
  ps::OperationProgramBuilder aggregate(q);
  auto task = aggregate.add_task("copy", "v1");
  PS_REQUIRE_OK(task);
  PS_REQUIRE_OK(aggregate.read_input(task.value(), 0, 0, 0));
  PS_CHECK(aggregate.write_output(task.value(), 0, 0, 0).code ==
           ps::ErrorCode::ResourceExhausted);
  PS_CHECK(aggregate.finish().status().code ==
           ps::ErrorCode::ResourceExhausted);

  // Repeated incoming tiles share a record, but each task's read consumes a
  // reference. Exhaustion is sticky and cannot produce a truncated program.
  q = query();
  q.limits.maximum_edges = 13;
  ps::OperationProgramBuilder repeated(q);
  auto a = repeated.add_task("copy", "v1");
  auto b = repeated.add_task("copy", "v1");
  auto c = repeated.add_task("copy", "v1");
  PS_REQUIRE_OK(a);
  PS_REQUIRE_OK(b);
  PS_REQUIRE_OK(c);
  PS_REQUIRE_OK(repeated.read_input(a.value(), 0, 0, 0));
  PS_REQUIRE_OK(repeated.read_input(b.value(), 0, 0, 0));
  PS_CHECK(repeated.read_input(c.value(), 0, 0, 0).code ==
           ps::ErrorCode::ResourceExhausted);
  PS_CHECK(repeated.read_input_schema(a.value(), 0).code ==
           ps::ErrorCode::ResourceExhausted);
  PS_CHECK(repeated.finish().status().code == ps::ErrorCode::ResourceExhausted);

  // A tightly bounded fan-out succeeds: shared input records are counted once.
  q = query();
  q.limits.maximum_records = 28;
  q.limits.maximum_edges = 26;
  auto selected = ps::TileSet::from_indices(ps::ResourceBudget{}, {0, 1, 2, 3});
  PS_REQUIRE_OK(selected);
  q.requests[0].members.emplace(0, selected.take_value());
  ps::OperationProgramBuilder fanout(q);
  for (unsigned i = 0; i < 4; ++i) {
    auto next = fanout.add_task("copy", "v1");
    PS_REQUIRE_OK(next);
    PS_REQUIRE_OK(fanout.read_input(next.value(), 0, 0, 0));
    PS_REQUIRE_OK(fanout.write_output(next.value(), 0, 0, i));
  }
  auto program = fanout.finish();
  PS_REQUIRE_OK(program);
  PS_CHECK(program.value().program.tiles.size() == 5);
  PS_CHECK(program.value().program.values[0].tiles.size() == 1);

  // Derived live-ins still consume the final reference budget.
  q = query();
  q.limits.maximum_edges = 15;
  ps::OperationProgramBuilder derived(q);
  for (unsigned i = 0; i < 2; ++i) {
    auto next = derived.add_task("copy", "v1");
    PS_REQUIRE_OK(next);
    PS_REQUIRE_OK(derived.read_input(next.value(), 0, 0, 0));
    PS_REQUIRE_OK(derived.write_output(next.value(), 0, 0, i));
  }
  PS_CHECK(derived.finish().status().code == ps::ErrorCode::ResourceExhausted);

  q = query();
  q.limits.maximum_records = 12;
  ps::OperationProgramBuilder transfer_builder(q);
  auto transfer = transfer_builder.add_copy();
  PS_REQUIRE_OK(transfer);
  PS_REQUIRE_OK(transfer_builder.read_input(transfer.value(), 0, 0, 0));
  PS_REQUIRE_OK(transfer_builder.write_output(transfer.value(), 0, 0, 0));
  PS_REQUIRE_OK(transfer_builder.finish());

  q = query();
  q.limits.maximum_records = 7;
  ps::OperationProgramBuilder temporary(q);
  PS_CHECK(temporary.add_temporary(q.inputs[0].result_schema).status().code ==
           ps::ErrorCode::ResourceExhausted);

  q = query();
  q.limits.maximum_edges = 9;
  ps::OperationProgramBuilder schema(q);
  auto constant = schema.add_task("constant", "v1");
  PS_REQUIRE_OK(constant);
  PS_REQUIRE_OK(schema.read_input_schema(constant.value(), 0));
  PS_REQUIRE_OK(schema.read_input_schema(constant.value(), 0));
  PS_REQUIRE_OK(schema.write_output(constant.value(), 0, 0, 0));
  PS_REQUIRE_OK(schema.finish());

  q = query();
  q.limits.maximum_records = 5;
  ps::OperationProgramBuilder initial(q);
  PS_CHECK(initial.add_task("constant", "v1").status().code ==
           ps::ErrorCode::ResourceExhausted);
  return 0;
}
