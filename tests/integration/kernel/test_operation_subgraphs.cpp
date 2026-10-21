#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::number;
using multi_result::run;
using multi_result::take;
using Counts = std::array<std::atomic<unsigned>, 3>;

OperationDefinition definition(const std::string& key, unsigned size,
                               const std::shared_ptr<Counts>& counts,
                               bool fail = false) {
  OperationDefinition op;
  op.key = key;
  op.traits.outputs = {multi_result::output(
      "value", multi_result::schema(ElementType::Float64, {size}))};
  op.prepare_kernel =
      [counts, fail](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    const auto invalid = [] {
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid subgraph kernel signature"});
    };
    const unsigned kind = kernel.symbol == "source"      ? 0
                          : kernel.symbol == "increment" ? 1
                          : kernel.symbol == "pair"      ? 2
                                                         : 3;
    if (kind == 3 || kernel.contract != "subgraph-v1" ||
        placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host ||
        query.reads.size() != kind || query.writes.size() != 1 ||
        !query.task_parameters.empty() ||
        query.scratch != std::array<std::uint64_t, 3>{})
      return invalid();
    const auto scalar = [](const KernelView& view) {
      return view.domain == physical::Domain::Host && view.bytes == 8 &&
             view.member == 0 && view.layout.byte_offset == 0 &&
             view.layout.rank == 1 && view.layout.shape[0] == 1 &&
             view.layout.strides[0] == 8 &&
             view.layout.element_type == ElementType::Float64;
    };
    if (!scalar(query.writes[0]))
      return invalid();
    for (const auto& view : query.reads)
      if (!scalar(view))
        return invalid();
    if (kind == 2 &&
        (query.reads[0].port != 0 || query.reads[0].tile_index != 0 ||
         query.reads[1].port != 1 || query.reads[1].tile_index != 2))
      return invalid();
    struct State {
      unsigned kind;
      double initial;
      std::shared_ptr<Counts> counts;
      bool fail;
    };
    const auto base = query.parameters.find("base");
    const double initial = kind == 0 ? (base == query.parameters.end()
                                            ? 10.0
                                            : std::get<double>(base->second)) +
                                           query.writes[0].tile_index
                           : kind == 1 ? 1.0
                                       : 0.0;
    ps_prepared_kernel_v8 prepared{};
    prepared.struct_size = sizeof(prepared);
    prepared.abi_version = 8;
    prepared.context =
        new State{kind, initial, counts,
                  fail && kind == 1 && query.writes[0].tile_index == 1};
    prepared.release = [](const void* raw) {
      delete static_cast<const State*>(raw);
    };
    prepared.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                             ps_kernel_status_v8* status) {
      const auto& state =
          *static_cast<const State*>(invocation->prepared_context);
      ++(*state.counts)[state.kind];
      if (state.fail) {
        *status =
            kernel_failure(ErrorCode::OperationFailed, "last tile failed");
        return;
      }
      double value = state.initial;
      for (unsigned i = 0; i < state.kind; ++i) {
        const auto& view = invocation->inputs[i];
        double input;
        std::memcpy(&input,
                    static_cast<const std::uint8_t*>(view.allocation) +
                        view.byte_offset,
                    sizeof(input));
        value += input * (state.kind == 2 && i == 0 ? 10 : 1);
      }
      const auto& output = invocation->outputs[0];
      std::memcpy(
          static_cast<std::uint8_t*>(output.allocation) + output.byte_offset,
          &value, sizeof(value));
      *status = {};
    };
    return Result<ps_prepared_kernel_v8>(prepared);
  };
  return op;
}

int private_chain(bool cyclic, bool fail = false) {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  auto op = definition("test.chain", 2, counts, fail);
  op.lower = [cyclic, fail](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    if (query.requests[0].empty())
      return builder.finish();
    auto a = take(builder.add_temporary(query.outputs[0].result_schema));
    auto b = take(builder.add_temporary(
        std::make_shared<const SchemaTemplate>(multi_result::schema())));
    auto seed = take(builder.add_task("source", "subgraph-v1"));
    auto middle = take(builder.add_task("increment", "subgraph-v1"));
    auto last = take(builder.add_task("increment", "subgraph-v1"));
    auto a0 = take(builder.write_temporary(seed, a, 0, 0));
    auto b0 = take(builder.write_temporary(middle, b, 0, 0));
    auto a1 = take(builder.write_temporary(last, a, 0, 1));
    check(builder.read_tile(middle, a0));
    check(builder.read_tile(last, b0));
    if (cyclic)
      check(builder.depends_on(seed, last));
    for (const auto& member : query.requests[0].members)
      for (auto tile : member.second) {
        auto copy = take(builder.add_copy());
        check(builder.read_tile(copy, tile ? a1 : a0));
        check(builder.write_output(copy, 0, member.first, tile));
        // Complete the first exported tile before the later task fails.
        if (fail && tile == 0)
          check(builder.depends_on(last, copy));
      }
    return builder.finish();
  };
  check(registry->register_operation(std::move(op)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.chain", {}, {}}};
  document.outputs = {{"first", 1, "value"}, {"last", 1, "value"}};
  GraphContext graph(document);
  ResourceBudget root;
  const TileRequest first{{{0, take(TileSet::range(root, 0, 1))}}};
  const TileRequest last{{{0, take(TileSet::range(root, 1, 2))}}};
  PlanningOptions options;
  options.tile_extent = 1;
  options.output_requests = {{"first", first}, {"last", last}};
  auto compiled = Compiler(registry).compile(graph, options);
  if (cyclic) {
    PS_CHECK(!compiled.ok() &&
             compiled.status().code == ErrorCode::InvalidArgument);
    PS_CHECK(!(*counts)[0] && !(*counts)[1]);
    return 0;
  }
  PS_REQUIRE_OK(compiled);
  auto plan = compiled.value().plan.value();
  PS_CHECK(plan.program().tasks.size() == 5);
  auto encoded = take(CompilerArtifact::export_plan(plan));
  plan = take(CompilerArtifact::import_plan(registry, encoded));
  ExecutionContext context(registry);
  if (fail) {
    std::vector<CallRecord> retired;
    ExecutionOptions execution;
    execution.on_call_retired = [&](const CallRecord& record) {
      retired.push_back(record);
    };
    auto prepared = take(context.prepare(plan));
    auto submitted = take(context.submit(prepared, {}, {}, execution));
    auto failed = submitted.join();
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::OperationFailed &&
             failed.status().message == "last tile failed");
    PS_CHECK(retired.size() == 1 && retired[0].outcome == CallOutcome::Failed &&
             retired[0].values.empty());
    PS_CHECK((*counts)[0] == 1 && (*counts)[1] == 2);
    return 0;
  }
  auto result = take(run(context, plan));
  PS_CHECK(number(result.results.at("first")) == 10);
  PS_CHECK(number(result.results.at("last"), {1}) == 12);
  PS_CHECK((*counts)[0] == 1 && (*counts)[1] == 2);
  const auto first_coverage =
      take(result.results.at("first").descriptor()).tensor_coverage(0);
  const auto last_coverage =
      take(result.results.at("last").descriptor()).tensor_coverage(0);
  PS_CHECK(first_coverage.contains({0}) && !first_coverage.contains({1}));
  PS_CHECK(last_coverage.contains({1}) && !last_coverage.contains({0}));
  PS_CHECK(result.results.at("first").object_id() !=
           result.results.at("last").object_id());
  auto selected = take(plan.select_outputs({{"first", first}}));
  PS_CHECK(selected.program().tasks.size() == 2);
  auto partial = take(run(context, selected));
  PS_CHECK(number(partial.results.at("first")) == 10);
  PS_CHECK((*counts)[0] == 2 && (*counts)[1] == 2);
  return 0;
}

int aliased_input_ports(bool distinct) {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  auto source = definition("test.source", 3, counts);
  source.traits.parameter_schema = {{"base", OperationParameterType::Float64}};
  source.lower = [](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    for (const auto& member : query.requests[0].members)
      for (auto tile : member.second) {
        auto task = take(builder.add_task("source", "subgraph-v1"));
        check(builder.write_output(task, 0, member.first, tile));
      }
    return builder.finish();
  };
  check(registry->register_operation(std::move(source)));
  auto pair = definition("test.pair", 1, counts);
  pair.traits.input_count = 2;
  pair.traits.input_schema.resize(2);
  for (auto& port : pair.traits.input_schema) {
    port.tensor_key = "number";
    port.element_type = static_cast<unsigned>(ElementType::Float64);
    port.rank = 1;
  }
  pair.lower = [](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    if (!query.requests[0].empty()) {
      auto task = take(builder.add_task("pair", "subgraph-v1"));
      check(builder.read_input(task, 0, 0, 0));
      check(builder.read_input(task, 1, 0, 2));
      check(builder.write_output(task, 0, 0, 0));
    }
    return builder.finish();
  };
  check(registry->register_operation(std::move(pair)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.source", {}, {{"base", 10.0}}},
                    {2, "test.source", {}, {{"base", 20.0}}},
                    {3,
                     "test.pair",
                     {WorkflowNodeOutput{1, "value"},
                      WorkflowNodeOutput{distinct ? 2U : 1U, "value"}},
                     {}}};
  document.outputs = {{"pair", 3, "value"}};
  GraphContext graph(document);
  PlanningOptions options;
  options.tile_extent = 1;
  auto plan = take(Compiler(registry).compile(graph, options)).plan.value();
  PS_CHECK(plan.program().tasks.size() == 3);
  const auto call = std::find_if(
      plan.program().calls.begin(), plan.program().calls.end(),
      [](const auto& candidate) { return candidate.source_call == 3; });
  PS_CHECK(call != plan.program().calls.end() && call->tasks.size() == 1);
  const auto& task = plan.program().tasks[call->tasks[0]];
  PS_CHECK(task.reads.size() == 2 && task.reads[0].port == 0 &&
           task.reads[1].port == 1);
  PS_CHECK(plan.program().tiles[task.reads[0].tile].tile_index == 0 &&
           plan.program().tiles[task.reads[1].tile].tile_index == 2);
  PS_CHECK((plan.program().tiles[task.reads[0].tile].value !=
            plan.program().tiles[task.reads[1].tile].value) == distinct);
  ExecutionContext context(registry);
  for (unsigned i = 1; i <= 2; ++i) {
    auto result = take(run(context, plan));
    PS_CHECK(number(result.results.at("pair")) == (distinct ? 122 : 112));
    PS_CHECK((*counts)[0] == 2 * i && (*counts)[2] == i);
  }
  return 0;
}
}  // namespace

int main() {
  return private_chain(false) || private_chain(true) ||
         private_chain(false, true) || aliased_input_ports(false) ||
         aliased_input_ports(true);
}
