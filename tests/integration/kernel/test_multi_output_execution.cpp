#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::binding;
using multi_result::Counts;
using multi_result::number;
using multi_result::run;
using multi_result::take;

ExecutionBinding binding_samples(const ResourceBudget& root,
                                 const std::string& name,
                                 const SchemaTemplate& schema,
                                 const std::vector<double>& samples) {
  auto builder =
      take(ResultBuilder::start(root, schema, "sample.input", {}, 1));
  const auto type = schema.tensors[0].descriptor.element_type;
  for (std::size_t index = 0; index < samples.size(); ++index) {
    std::array<std::uint8_t, 8> bytes{};
    multi_result::write_number(bytes.data(), type, samples[index]);
    multi_result::check(builder.publish_tensor(
        0, Region({{index, 1}}),
        ByteView(bytes.data(), multi_result::width(type))));
  }
  return {name, take(builder.seal())};
}

OperationDefinition independent(const std::shared_ptr<Counts>& counts) {
  OperationDefinition op;
  op.key = "test.independent";
  op.traits.input_count = 2;
  op.traits.input_schema.resize(2);
  for (auto& input : op.traits.input_schema)
    input.tensor_key = "number";
  op.traits.outputs = {multi_result::output("left"),
                       multi_result::output("right")};
  for (std::uint32_t i = 0; i < 2; ++i)
    op.traits.outputs[i].input_indices = std::vector<std::uint32_t>{i};
  multi_result::install_fixed(op, {0, 1}, {0, 0}, counts);
  return op;
}

int independent_outputs() {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  PS_REQUIRE_OK(registry->register_operation(independent(counts)));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "a"),
                     multi_result::declaration(2, "b")};
  document.nodes = {{1,
                     "test.independent",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"left", 1, "left"}, {"right", 1, "right"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_REQUIRE_OK(compiled);
  const auto& plan = compiled.value().plan.value();
  PS_CHECK(plan.program().tasks.size() == 2);
  for (const auto& task : plan.program().tasks) {
    const auto& write = plan.program().tiles[task.writes[0]];
    const auto port = write.value == plan.calls()[0].outputs[0] ? 0U : 1U;
    PS_CHECK(task.reads.size() == 1 && task.reads[0].port == port &&
             plan.program().tiles[task.reads[0].tile].value ==
                 plan.calls()[0].inputs[port]);
  }
  ExecutionContext execution(registry);
  const auto root = take(execution.resource_budget());
  const ExecutionBindings original{
      {binding(root, "a", 7), binding(root, "b", 11)}};
  auto prepared = execution.prepare(plan);
  PS_REQUIRE_OK(prepared);
  auto first_run = execution.submit(prepared.value(), original);
  PS_REQUIRE_OK(first_run);
  auto first = first_run.value().join();
  PS_REQUIRE_OK(first);
  PS_CHECK(number(first.value().results.at("left")) == 7 &&
           number(first.value().results.at("right")) == 11);
  PS_CHECK((*counts)[0] == 1 && (*counts)[1] == 1);
  auto changed_run = execution.submit(
      prepared.value(), {{binding(root, "a", 13), original.inputs[1]}});
  PS_REQUIRE_OK(changed_run);
  auto changed = changed_run.value().join();
  PS_REQUIRE_OK(changed);
  PS_CHECK(number(changed.value().results.at("left")) == 13 &&
           number(changed.value().results.at("right")) == 11);
  PS_CHECK((*counts)[0] == 2 && (*counts)[1] == 2);
  PS_CHECK(number(first.value().results.at("left")) == 7);
  PS_CHECK(first.value().results.at("left").object_id() !=
           changed.value().results.at("left").object_id());

  // Canonical declaration ordering must preserve original operation ports.
  auto renamed = document;
  renamed.inputs[0].id = 20;
  renamed.inputs[1].id = 10;
  renamed.nodes[0].id = 99;
  renamed.nodes[0].inputs = {WorkflowInputReference{20},
                             WorkflowInputReference{10}};
  renamed.outputs = {{"right", 99, "right"}};
  GraphContext renamed_graph(renamed);
  auto renamed_plan = take(compiler.compile(renamed_graph)).plan.value();
  PS_CHECK(renamed_plan.program().tasks.size() == 1 &&
           renamed_plan.program().calls[0].source_call == 99);
  const auto& read = renamed_plan.program().tasks[0].reads[0];
  PS_CHECK(read.port == 1 && renamed_plan.input_declarations()[0].name == "b" &&
           renamed_plan.program().tiles[read.tile].value ==
               renamed_plan.program().inputs[0]);
  auto renamed_result = run(execution, renamed_plan, original);
  PS_REQUIRE_OK(renamed_result);
  PS_CHECK(number(renamed_result.value().results.at("right")) == 11);
  PS_CHECK((*counts)[0] == 2 && (*counts)[1] == 3);

  auto chain = document;
  chain.nodes.push_back(
      {2,
       "test.independent",
       {WorkflowNodeOutput{1, "left"}, WorkflowNodeOutput{1, "right"}},
       {}});
  chain.outputs = {{"right", 2, "right"}};
  GraphContext chain_graph(chain);
  auto chain_plan = take(compiler.compile(chain_graph)).plan.value();
  PS_CHECK(chain_plan.program().tasks.size() == 2);
  for (const auto& call : chain_plan.calls())
    PS_CHECK(chain_plan.program().values[call.outputs[0]].tiles.empty());
  const auto& consumer =
      chain_plan.program().tasks[chain_plan.program().calls[1].tasks[0]];
  PS_CHECK(consumer.predecessors.size() == 1 &&
           consumer.predecessors[0] == chain_plan.program().calls[0].tasks[0]);
  auto chain_result = run(execution, chain_plan, original);
  PS_REQUIRE_OK(chain_result);
  PS_CHECK(number(chain_result.value().results.at("right")) == 11);
  PS_CHECK((*counts)[0] == 2 && (*counts)[1] == 5);
  return 0;
}

int projected_inputs() {
  for (bool managed : {false, true}) {
    for (unsigned outputs : {1U, 2U}) {
      auto registry = std::make_shared<OperationRegistry>();
      auto unwanted_calls = std::make_shared<std::atomic<unsigned>>(0);
      OperationDefinition unwanted;
      unwanted.key = "test.unwanted";
      unwanted.traits.outputs[0] = multi_result::output("value");
      multi_result::install_fixed(unwanted, {-1}, {0});
      unwanted.prepare_kernel = [unwanted_calls](const auto&, const auto&,
                                                 const auto&, const auto&,
                                                 const auto*) {
        ++*unwanted_calls;
        return Result<ps_prepared_kernel_v8>(
            {ErrorCode::OperationFailed, "unrequested input prepared"});
      };
      PS_REQUIRE_OK(registry->register_operation(std::move(unwanted)));
      auto counts = std::make_shared<Counts>();
      auto projected = independent(counts);
      projected.key = "test.projected";
      projected.traits.outputs = {multi_result::output("pass")};
      projected.traits.outputs[0].input_indices = std::vector<std::uint32_t>{0};
      std::vector<int> sources{0};
      std::vector<double> constants{0};
      if (outputs == 2) {
        projected.traits.outputs.push_back(multi_result::output("constant"));
        projected.traits.outputs[1].input_indices =
            std::vector<std::uint32_t>{};
        sources.push_back(-1);
        constants.push_back(29);
      }
      multi_result::install_fixed(projected, sources, constants, counts);
      const auto lower = projected.lower;
      projected.lower = [lower](const OperationLowering& query) {
        if (query.inputs.size() != 2 || !query.inputs[0].result_schema ||
            !query.inputs[1].result_schema ||
            query.inputs[0].result_schema->tensors[0].key != "number" ||
            query.inputs[1].result_schema->tensors[0].key != "number")
          return Result<OperationProgram>(
              {ErrorCode::TypeMismatch, "projected metadata order changed"});
        return lower(query);
      };
      PS_REQUIRE_OK(registry->register_operation(std::move(projected)));
      PS_REQUIRE_OK(registry->freeze());
      WorkflowDocument document;
      document.inputs = {multi_result::declaration(1, "source")};
      document.nodes = {
          {1, "test.unwanted", {}, {}},
          {2,
           "test.projected",
           {WorkflowInputReference{1}, WorkflowNodeOutput{1, "value"}},
           {}}};
      document.outputs = {{"pass", 2, "pass"}};
      if (outputs == 2)
        document.outputs.push_back({"constant", 2, "constant"});
      GraphContext graph(document);
      auto compiled = Compiler(registry).compile(graph);
      PS_REQUIRE_OK(compiled);
      const auto& plan = compiled.value().plan.value();
      PS_CHECK(plan.program().calls[0].tasks.empty());
      ExecutionContextConfig config;
      config.cpu_workers = 1;
      if (managed)
        config.managed_resources = ResourceLimits{};
      ExecutionContext context(registry, config);
      auto root = take(context.resource_budget());
      auto result = run(context, plan, {{binding(root, "source", 7)}});
      PS_REQUIRE_OK(result);
      PS_CHECK(!*unwanted_calls && (*counts)[0] == 1);
      PS_CHECK(number(result.value().results.at("pass")) == 7);
      if (outputs == 2) {
        PS_CHECK((*counts)[1] == 1 &&
                 number(result.value().results.at("constant")) == 29);
        auto only_constant = plan.select_outputs(
            {{"constant", plan.output_requests().at("constant")}});
        PS_REQUIRE_OK(only_constant);
        PS_CHECK(only_constant.value().program().tasks.size() == 1 &&
                 only_constant.value().program().tasks[0].reads.empty());
        for (double input : {17., 23.}) {
          auto constant = run(context, only_constant.value(),
                              {{binding(root, "source", input)}});
          PS_REQUIRE_OK(constant);
          PS_CHECK(number(constant.value().results.at("constant")) == 29);
        }
        PS_CHECK((*counts)[0] == 1 && (*counts)[1] == 3 && !*unwanted_calls);
      }
    }
  }
  return 0;
}

int projected_shapes_and_permutation() {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  auto op = independent(counts);
  const auto left_schema = multi_result::schema(ElementType::Float64, {2});
  const auto right_schema = multi_result::schema(ElementType::Int64, {3});
  op.traits.outputs[0].result_schema = left_schema;
  op.traits.outputs[1].result_schema = right_schema;
  const auto lower = op.lower;
  op.lower = [lower](const OperationLowering& query) {
    if (query.inputs.size() != 2 ||
        query.inputs[0].result_schema->tensors[0].descriptor.element_type !=
            ElementType::Float64 ||
        query.inputs[1].result_schema->tensors[0].descriptor.element_type !=
            ElementType::Int64)
      return Result<OperationProgram>(
          {ErrorCode::TypeMismatch, "original metadata order"});
    return lower(query);
  };
  PS_REQUIRE_OK(registry->register_operation(std::move(op)));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "a", left_schema),
                     multi_result::declaration(2, "b", right_schema)};
  document.nodes = {{1,
                     "test.independent",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"left", 1, "left"}, {"right", 1, "right"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.tile_extent = 1;
  ResourceBudget selections;
  const TileRequest left_request{{{0, take(TileSet::range(selections, 1, 2))}}};
  const TileRequest right_request{
      {{0, take(TileSet::range(selections, 2, 3))}}};
  planning.output_requests = {{"left", left_request}, {"right", right_request}};
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_REQUIRE_OK(compiled);
  const auto& plan = compiled.value().plan.value();
  PS_CHECK(plan.program().tasks.size() == 2);
  for (const auto& task : plan.program().tasks) {
    const auto& input = plan.program().tiles[task.reads[0].tile];
    PS_CHECK(input.tile_index == task.reads[0].port + 1);
  }
  ExecutionContext execution(registry);
  const auto root = take(execution.resource_budget());
  const auto a = binding_samples(root, "a", left_schema, {7, 8});
  const auto b = binding_samples(root, "b", right_schema, {11, 12, 13});
  auto result = run(execution, plan, {{b, a}});
  PS_REQUIRE_OK(result);
  const auto& left = result.value().results.at("left");
  const auto& right = result.value().results.at("right");
  PS_CHECK(left.schema().tensors[0].sample_shape() ==
           std::vector<std::uint64_t>{2});
  PS_CHECK(right.schema().tensors[0].sample_shape() ==
           std::vector<std::uint64_t>{3});
  PS_CHECK(number(left, {1}) == 8);
  std::int64_t integer = 0;
  PS_REQUIRE_OK(
      right.read_tensor(right.descriptor().value(), 0, {2}, &integer, 8));
  PS_CHECK(integer == 13);
  PS_CHECK(left.descriptor().value().tensor_coverage(0) ==
           take(Footprint::from_regions({2}, {Region({{1, 1}})})));
  PS_CHECK(right.descriptor().value().tensor_coverage(0) ==
           take(Footprint::from_regions({3}, {Region({{2, 1}})})));
  PS_CHECK(
      !right.read_tensor(right.descriptor().value(), 0, {1}, &integer, 8).ok());
  auto selected =
      plan.select_outputs({{"left", plan.output_requests().at("left")}});
  PS_REQUIRE_OK(selected);
  auto only_left = run(execution, selected.value(), {{b, a}});
  PS_REQUIRE_OK(only_left);
  PS_CHECK((*counts)[0] == 2 && (*counts)[1] == 1);
  auto wrong =
      run(execution, plan, {{binding(root, "a", 7, right_schema, 1), b}});
  PS_CHECK(!wrong.ok() && wrong.status().code == ErrorCode::TypeMismatch);
  PS_CHECK((*counts)[0] == 2 && (*counts)[1] == 1);
  return 0;
}

int module_outputs() {
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->load_plugin(PS_MULTI_OUTPUT_FIXTURE));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.c_results", {}, {}}};
  document.outputs = {{"first", 1, "first"}, {"second", 1, "second"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  PS_REQUIRE_OK(compiled);
  ExecutionContext execution(registry);
  auto prepared = execution.prepare(compiled.value().plan.value());
  PS_REQUIRE_OK(prepared);
  for (unsigned i = 0; i < 2; ++i) {
    auto submitted = execution.submit(prepared.value());
    PS_REQUIRE_OK(submitted);
    auto result = submitted.value().join();
    PS_REQUIRE_OK(result);
    PS_CHECK(number(result.value().results.at("first")) == 10);
    PS_CHECK(number(result.value().results.at("second")) == 11);
    PS_CHECK(result.value().diagnostics.completed_tasks == 2);
  }
  // Artifact rebinding and output selection preserve the original port index.
  auto text = CompilerArtifact::export_plan(compiled.value().plan.value());
  PS_REQUIRE_OK(text);
  auto imported = CompilerArtifact::import_plan(registry, text.value());
  PS_REQUIRE_OK(imported);
  auto second = imported.value().select_outputs(
      {{"second", imported.value().output_requests().at("second")}});
  PS_REQUIRE_OK(second);
  PS_CHECK(second.value().program().tasks.size() == 1);
  auto selected = run(execution, second.value());
  PS_REQUIRE_OK(selected);
  PS_CHECK(selected.value().results.size() == 1 &&
           number(selected.value().results.at("second")) == 11 &&
           selected.value().diagnostics.completed_tasks == 1);
  return 0;
}
int distinct_member_grids() {
  auto schema = multi_result::schema(ElementType::Float64, {2, 4});
  schema.id = "test.member_grids";
  schema.tensors[0].batch_axes = {2, 2};
  schema.tensors[0].layout.spatial = true;
  schema.tensors[0].layout.channel_axis.reset();
  auto second = schema.tensors[0];
  second.key = "second";
  second.batch_axes = {1, 2};
  second.descriptor.shape = {1, 3};
  schema.tensors.push_back(second);
  PS_REQUIRE_OK(schema.validate());
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition op;
  op.key = "test.member_grids";
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);
  op.traits.input_schema[0].result_schema_id = schema.id;
  op.traits.input_schema[0].result_schema_version = schema.version;
  op.traits.outputs = {multi_result::output("value")};
  op.lower = [](const OperationLowering& query) -> Result<OperationProgram> {
    OperationProgramBuilder builder(query);
    auto task = builder.add_task("members", "members-v1");
    if (!task.ok())
      return Result<OperationProgram>(task.status());
    for (unsigned member = 0; member < 2; ++member) {
      auto status =
          builder.read_input(task.value(), 0, member, member ? 5 : 31);
      if (!status.ok())
        return Result<OperationProgram>(status);
    }
    auto status = builder.write_output(task.value(), 0, 0, 0);
    if (!status.ok())
      return Result<OperationProgram>(status);
    return builder.finish();
  };
  op.prepare_kernel = [](const auto&, const auto&, const auto&, const auto&,
                         const auto*) -> Result<ps_prepared_kernel_v8> {
    ps_prepared_kernel_v8 kernel{};
    kernel.struct_size = sizeof(kernel);
    kernel.abi_version = 8;
    kernel.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      if (invocation->input_count != 2 || invocation->output_count != 1) {
        *status = kernel_failure(ErrorCode::Internal, "member view count");
        return;
      }
      double values[2]{};
      for (unsigned member = 0; member < 2; ++member) {
        const auto& input = invocation->inputs[member];
        std::memcpy(&values[member],
                    static_cast<const std::uint8_t*>(input.allocation) +
                        input.byte_offset,
                    sizeof(double));
      }
      const double value = 10 * values[0] + values[1];
      const auto& output = invocation->outputs[0];
      std::memcpy(
          static_cast<std::uint8_t*>(output.allocation) + output.byte_offset,
          &value, sizeof(value));
      *status = {};
    };
    return Result<ps_prepared_kernel_v8>(kernel);
  };
  PS_REQUIRE_OK(registry->register_operation(std::move(op)));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "source", schema)};
  document.nodes = {{1, "test.member_grids", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.tile_extent = 1;
  auto compiled = Compiler(registry).compile(graph, planning);
  PS_REQUIRE_OK(compiled);
  ExecutionContext context(registry);
  auto prepared = context.prepare(compiled.value().plan.value());
  PS_REQUIRE_OK(prepared);
  const auto root = take(context.resource_budget());
  ResultRef retained;
  for (unsigned round = 0; round < 2; ++round) {
    auto builder = take(ResultBuilder::start(root, schema, "member.samples"));
    for (unsigned member = 0; member < 2; ++member) {
      const double value = (member ? 105 : 31) + round * (member ? 2000 : 1000);
      const Region region = member ? Region({{0, 1}, {1, 1}, {0, 1}, {2, 1}})
                                   : Region({{1, 1}, {1, 1}, {1, 1}, {3, 1}});
      PS_REQUIRE_OK(builder.publish_tensor(
          member, region,
          ByteView(reinterpret_cast<const std::uint8_t*>(&value), 8)));
    }
    auto source = builder.seal();
    PS_REQUIRE_OK(source);
    auto submitted =
        context.submit(prepared.value(), {{{"source", source.take_value()}}});
    PS_REQUIRE_OK(submitted);
    auto completed = submitted.value().join();
    PS_REQUIRE_OK(completed);
    const auto& output = completed.value().results.at("value");
    PS_CHECK(number(output) == (round ? 12415 : 415));
    if (!round)
      retained = output;
  }
  PS_CHECK(number(retained) == 415);
  return 0;
}
int binding_preflight() {
  auto registry = std::make_shared<OperationRegistry>();
  auto counts = std::make_shared<Counts>();
  PS_REQUIRE_OK(registry->register_operation(independent(counts)));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "a"),
                     multi_result::declaration(2, "b")};
  document.nodes = {{1,
                     "test.independent",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {}}};
  document.outputs = {{"left", 1, "left"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_REQUIRE_OK(compiled);
  auto wrong_schema = multi_result::schema();
  wrong_schema.id = "test.other";
  auto changed = document;
  changed.inputs[1].result_schema =
      std::make_shared<const SchemaTemplate>(wrong_schema);
  GraphContext changed_graph(changed);
  auto changed_plan = compiler.compile(changed_graph);
  PS_REQUIRE_OK(changed_plan);
  PS_CHECK(changed_plan.value().plan->digest().value !=
           compiled.value().plan->digest().value);
  auto unknown_version = document;
  ++unknown_version.schema_version;
  PS_CHECK(compiler.compile(GraphContext(unknown_version)).status().code ==
           ErrorCode::InvalidArgument);
  auto missing_schema = document;
  missing_schema.inputs[1].result_schema.reset();
  PS_CHECK(compiler.compile(GraphContext(missing_schema)).status().code ==
           ErrorCode::InvalidArgument);
  ResourceBudget foreign;
  const auto a = binding(foreign, "a", 7);
  const auto b = binding(foreign, "b", 11);
  const auto wrong = binding(foreign, "b", 11, wrong_schema);
  auto open =
      take(ResultBuilder::start(foreign, multi_result::schema(), "open"));
  auto failed =
      take(ResultBuilder::start(foreign, multi_result::schema(), "failed"));
  failed.fail({ErrorCode::OperationFailed, "input failed"});
  for (std::uint64_t capacity : {8U, 0U}) {
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Referenced] = capacity;
    ExecutionContext context(registry, config);
    auto root = take(context.resource_budget());
    auto prepared = context.prepare(compiled.value().plan.value());
    PS_REQUIRE_OK(prepared);
    const auto before = (*counts)[0].load();
    if (capacity) {
      const std::vector<ExecutionBindings> malformed{
          {},
          {{a}},
          {{a, b, b}},
          {{{"a", a.result}, {"a", b.result}}},
          {{{"unknown", a.result}, b}},
          {{{"a", {}}, b}}};
      for (const auto& inputs : malformed) {
        auto rejected = context.submit(prepared.value(), inputs);
        PS_CHECK(rejected.status().code == ErrorCode::InvalidArgument);
        PS_CHECK((*counts)[0] == before && (*counts)[1] == 0);
        PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 0);
      }
      for (const auto& incomplete : {open.reference(), failed.reference()}) {
        auto rejected =
            context.submit(prepared.value(), {{a, {"b", incomplete}}});
        PS_CHECK(!rejected.ok());
        PS_CHECK((*counts)[0] == before && (*counts)[1] == 0);
        PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 0);
      }
      auto rejected = context.submit(prepared.value(), {{a, wrong}});
      PS_CHECK(rejected.status().code == ErrorCode::TypeMismatch);
      PS_CHECK((*counts)[0] == before && (*counts)[1] == 0);
      PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 0);
    }
    {
      auto submitted = context.submit(prepared.value(), {{a, b}});
      if (!capacity) {
        PS_CHECK(submitted.status().code == ErrorCode::ResourceExhausted);
        PS_CHECK((*counts)[0] == before);
      } else {
        PS_REQUIRE_OK(submitted);
        auto completed = submitted.value().join();
        PS_REQUIRE_OK(completed);
        PS_CHECK(number(completed.value().results.at("left")) == 7);
        PS_CHECK((*counts)[0] == before + 1 && (*counts)[1] == 0);
        PS_CHECK(root.statistics().peak[ResourceKind::Referenced] == 8);
      }
    }
    PS_CHECK(root.statistics().live[ResourceKind::Referenced] == 0);
  }
  return 0;
}
}  // namespace

int main() {
  return independent_outputs() || projected_inputs() ||
         projected_shapes_and_permutation() || module_outputs() ||
         distinct_member_grids() || binding_preflight();
}
