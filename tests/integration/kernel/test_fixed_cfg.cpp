#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/execution/execution.hpp"
#include "support/fixed_operation_fixture.hpp"
#include "support/status_check.hpp"

namespace {
ps::SchemaTemplate scalar() {
  auto schema = ps::test::fixed_fixture::schema();
  schema.tensors[0].descriptor.shape = {1};
  return schema;
}
ps::OperationDefinition operation(const std::string& name, unsigned inputs,
                                  std::uint8_t constant) {
  auto result = ps::test::fixed_fixture::operation(name, inputs);
  result.traits.outputs[0].result_schema = scalar();
  result.prepare_kernel = [constant](const auto&, const auto&,
                                     const auto& query, const auto&,
                                     const ps_native_gpu_service_v2*) {
    if (query.reads.size() != query.inputs.size() || query.writes.size() != 1 ||
        query.writes[0].port != 0 || query.writes[0].layout.shape[0] != 1)
      return ps::Result<ps_prepared_kernel_v8>(
          {ps::ErrorCode::TypeMismatch, "invalid scalar invocation"});
    ps_prepared_kernel_v8 kernel{};
    kernel.struct_size = sizeof(kernel);
    kernel.abi_version = 8;
    kernel.context = new std::uint8_t(constant);
    kernel.release = [](const void* context) {
      delete static_cast<const std::uint8_t*>(context);
    };
    kernel.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      std::uint8_t value =
          *static_cast<const std::uint8_t*>(invocation->prepared_context);
      if (invocation->input_count)
        value = static_cast<const std::uint8_t*>(
                    invocation->inputs[0]
                        .allocation)[invocation->inputs[0].byte_offset] -
                1;
      static_cast<std::uint8_t*>(invocation->outputs[0].allocation)[0] = value;
      *status = {};
    };
    return ps::Result<ps_prepared_kernel_v8>(kernel);
  };
  return result;
}
ps::SemanticNode call(std::uint64_t id, const std::string& operation,
                      std::vector<ps::IRValueId> inputs, ps::IRValueId output) {
  ps::SemanticNode node;
  node.id = id;
  node.operation = operation;
  node.inputs = std::move(inputs);
  node.outputs = {{"out0", {}, output}};
  return node;
}
int sample(const ps::ResultRef& value) {
  auto descriptor = value.descriptor();
  if (!descriptor.ok())
    return -1;
  std::uint8_t byte = 0;
  if (!value.read_tensor(descriptor.value(), 0, {0}, &byte, 1).ok())
    return -1;
  return byte;
}

int binding_tiles() {
  auto definition = ps::test::fixed_fixture::operation("tiles", 1);
  definition.lower = [](const ps::OperationLowering& query) {
    ps::OperationProgramBuilder builder(query);
    for (unsigned tile = 0; tile < 2; ++tile) {
      auto task = builder.add_task("increment", "increment-v1");
      if (!task.ok())
        return ps::Result<ps::OperationProgram>(task.status());
      auto status = builder.read_input(task.value(), 0, 0, tile);
      if (status.ok())
        status = builder.write_output(task.value(), 0, 0, tile);
      if (!status.ok())
        return ps::Result<ps::OperationProgram>(status);
    }
    return builder.finish();
  };
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(std::move(definition)));
  PS_REQUIRE_OK(registry->freeze());
  auto schema = ps::test::fixed_fixture::schema();
  ps::WorkflowDocument document;
  document.inputs = {
      {1, "input", std::make_shared<const ps::SchemaTemplate>(schema)}};
  document.nodes = {{1, "tiles", {ps::WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "out0"}};
  ps::GraphContext graph(document);
  ps::Compiler compiler(registry);
  auto ir = compiler.analyze(graph.snapshot());
  PS_REQUIRE_OK(ir);
  ps::PlanningOptions planning;
  planning.tile_extent = 2;
  ps::ExecutionContext context(registry);
  ps::ResourceBudget root;
  for (bool dense : {false, true}) {
    planning.input_layouts["input"] =
        dense ? ps::physical::InputLayout::DenseHost
              : ps::physical::InputLayout::DirectTiles;
    auto plan = compiler.plan(ir.value(), planning);
    PS_REQUIRE_OK(plan);
    auto encoded = ps::CompilerArtifact::export_plan(plan.value());
    PS_REQUIRE_OK(encoded);
    auto loaded = ps::CompilerArtifact::import_plan(registry, encoded.value());
    PS_REQUIRE_OK(loaded);
    PS_CHECK(loaded.value().digest().value == plan.value().digest().value);
    auto prepared = context.prepare(loaded.value());
    PS_REQUIRE_OK(prepared);
    for (bool separate : {false, true}) {
      auto made = ps::ResultBuilder::start(root, schema, "input", {}, 2);
      PS_REQUIRE_OK(made);
      auto builder = made.take_value();
      const unsigned chunks = separate ? 2 : 1;
      const unsigned size = separate ? 2 : 4;
      for (unsigned chunk = 0; chunk < chunks; ++chunk) {
        auto allocated = root.allocator().allocate(size);
        PS_REQUIRE_OK(allocated);
        auto buffer = allocated.take_value();
        for (unsigned i = 0; i < size; ++i)
          buffer.data()[i] = 2;
        PS_REQUIRE_OK(builder.publish_tensor(
            0, ps::Region({{chunk * size, size}}), {0, {1}, {chunk * size}},
            std::move(buffer).freeze()));
      }
      auto input = builder.seal();
      PS_REQUIRE_OK(input);
      ps::ExecutionBindings bindings;
      bindings.inputs = {{"input", input.take_value()}};
      auto run = context.submit(prepared.value(), std::move(bindings));
      if (separate == dense) {
        PS_CHECK(run.status().code == ps::ErrorCode::TypeMismatch);
        continue;
      }
      PS_REQUIRE_OK(run);
      auto result = run.value().join();
      PS_REQUIRE_OK(result);
      PS_CHECK(sample(result.value().results.at("result")) == 3);
      PS_CHECK(result.value().diagnostics.completed_tasks == (dense ? 4 : 2));
    }
  }
  return 0;
}
// A padded leading offset is allowed: the compiler fixes strides and capacity
// relative to the bound allocation base, and submit retains that base owner.
int dense_boundaries() {
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->freeze());
  ps::Compiler compiler(registry);
  ps::ExecutionContext context(registry);
  auto shape = ps::test::fixed_fixture::schema();
  shape.tensors[0].descriptor.shape = {4, 4};
  auto schema = std::make_shared<const ps::SchemaTemplate>(shape);
  ps::IRProgram source;
  source.entry = 1;
  source.input_declarations = {{1, "input", schema}};
  source.input_bindings = {{1, 10}};
  source.returns = {{"result", schema}};
  ps::PlanningOptions planning;
  planning.tile_extent = 2;
  planning.input_layouts["input"] = ps::physical::InputLayout::DenseHost;
  for (bool edge : {false, true}) {
    source.blocks = {{1, {}, {}, ps::IRReturn{{10}}}};
    if (edge)
      source.blocks = {{1, {}, {}, ps::IRJump{{2, {10}}}},
                       {2, {{20, schema}}, {}, ps::IRReturn{{20}}}};
    auto ir = compiler.bind_ir(source);
    PS_REQUIRE_OK(ir);
    auto compiled = compiler.plan(ir.value(), planning);
    PS_REQUIRE_OK(compiled);
    PS_CHECK(compiled.value().program().tasks.size() == 4);
    PS_CHECK(compiled.value().program().calls.empty());
    auto encoded = ps::CompilerArtifact::export_plan(compiled.value());
    PS_REQUIRE_OK(encoded);
    auto loaded = ps::CompilerArtifact::import_plan(registry, encoded.value());
    PS_REQUIRE_OK(loaded);
    PS_CHECK(loaded.value().digest().value == compiled.value().digest().value);
    ps::TileRequest partial;
    auto selected_tiles =
        ps::TileSet::from_indices(ps::ResourceBudget{}, {1, 3});
    PS_REQUIRE_OK(selected_tiles);
    partial.members.emplace(0, selected_tiles.take_value());
    for (bool selected : {false, true}) {
      auto narrowed = loaded.value().select_outputs(
          {{"result", selected
                          ? partial
                          : loaded.value().output_requests().at("result")}});
      PS_REQUIRE_OK(narrowed);
      PS_CHECK(narrowed.value().program().tasks.size() ==
               (selected && !edge ? 2 : 4));
      auto prepared = context.prepare(narrowed.value());
      PS_REQUIRE_OK(prepared);
      ps::ResourceBudget budget;
      auto made = ps::ResultBuilder::start(budget, shape, "input", {}, 2);
      PS_REQUIRE_OK(made);
      auto builder = made.take_value();
      auto allocated = budget.allocator().allocate(24);
      PS_REQUIRE_OK(allocated);
      auto buffer = allocated.take_value();
      for (unsigned i = 0; i < 16; ++i)
        buffer.data()[8 + i] = static_cast<std::uint8_t>(i + 1);
      auto original = std::move(buffer).freeze();
      PS_REQUIRE_OK(builder.publish_tensor(0, ps::Region({{0, 4}, {0, 4}}),
                                           {8, {4, 1}, {0, 0}}, original));
      auto input = builder.seal();
      PS_REQUIRE_OK(input);
      ps::ExecutionBindings bindings;
      bindings.inputs = {{"input", input.take_value()}};
      unsigned records = 0;
      ps::ExecutionOptions options;
      options.on_call_retired = [&](const auto&) { ++records; };
      auto submitted = context.submit(prepared.value(), bindings, {}, options);
      PS_REQUIRE_OK(submitted);
      auto result = submitted.value().join();
      PS_REQUIRE_OK(result);
      PS_CHECK(records == 0 && result.value().diagnostics.retired_calls == 0);
      PS_CHECK(result.value().diagnostics.completed_tasks ==
               (selected && !edge ? 2 : 4));
      const auto& returned = result.value().results.at("result");
      auto descriptor = returned.descriptor();
      PS_REQUIRE_OK(descriptor);
      std::vector<const ps::CpuStorage*> owners;
      for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = selected ? 2 : 0; col < 4; ++col) {
          std::uint8_t byte = 0;
          PS_REQUIRE_OK(returned.read_tensor(descriptor.value(), 0, {row, col},
                                             &byte, 1));
          PS_CHECK(byte == row * 4 + col + 1);
        }
      auto snapshot = returned.capture();
      PS_REQUIRE_OK(snapshot);
      for (unsigned row = 0; row < 4; row += 2)
        for (unsigned col = selected ? 2 : 0; col < 4; col += 2) {
          auto window = snapshot.value().acquire_tensor(
              descriptor.value(), 0, ps::Region({{row, 2}, {col, 2}}));
          PS_REQUIRE_OK(window);
          auto view = window.value().affine_view();
          PS_REQUIRE_OK(view);
          PS_CHECK(view.value().storage().get() != original.get());
          PS_CHECK(view.value().storage()->bytes().size() == 4);
          for (auto owner : owners)
            PS_CHECK(owner != view.value().storage().get());
          owners.push_back(view.value().storage().get());
        }
    }
    auto empty = compiled.value().select_outputs({});
    PS_REQUIRE_OK(empty);
    if (!edge)
      PS_CHECK(empty.value().program().tasks.empty());
  }
  auto ir = compiler.bind_ir(source);
  PS_REQUIRE_OK(ir);
  planning.maximum_tasks = 3;
  PS_CHECK(compiler.plan(ir.value(), planning).status().code ==
           ps::ErrorCode::ResourceExhausted);
  planning.maximum_tasks = 4;
  auto planners = std::make_shared<ps::PlannerRegistry>();
  auto gpu_boundary =
      *ps::make_default_planner_registry()->find("static-tiles");
  auto base = gpu_boundary.plan;
  gpu_boundary.name = "gpu-boundary";
  gpu_boundary.plan = [base](const auto& input, auto& builder,
                             const auto& options) {
    auto status = base(input, builder, options);
    if (!status.ok())
      return status;
    auto& p = builder.program();
    p.placements[p.tasks[0].placement].backend = ps::physical::Backend::Gpu;
    return ps::physical::finalize_program(&p);
  };
  PS_REQUIRE_OK(planners->add(std::move(gpu_boundary)));
  planners->freeze();
  ps::Compiler custom(registry, {}, planners);
  planning.planner = "gpu-boundary";
  auto rejected = custom.plan(ir.value(), planning);
  PS_CHECK(rejected.status().code == ps::ErrorCode::InvalidArgument);
  PS_CHECK(rejected.status().message.find(
               "GPU COPY requires declared Shared staging") !=
           std::string::npos);
  planning.planner = "static-tiles";
  planning.input_layouts["missing"] = ps::physical::InputLayout::DenseHost;
  PS_CHECK(compiler.plan(ir.value(), planning).status().code ==
           ps::ErrorCode::InvalidArgument);
  return 0;
}
int dense_loop_and_fanout() {
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(operation("seed", 0, 3)));
  PS_REQUIRE_OK(registry->register_operation(operation("decrement", 1, 0)));
  PS_REQUIRE_OK(registry->register_operation(
      ps::test::fixed_fixture::operation("increment", 1)));
  PS_REQUIRE_OK(registry->freeze());
  auto counter = std::make_shared<const ps::SchemaTemplate>(scalar());
  auto data = std::make_shared<const ps::SchemaTemplate>(
      ps::test::fixed_fixture::schema());
  ps::IRProgram source;
  source.entry = 1;
  source.input_declarations = {{1, "data", data}};
  source.input_bindings = {{1, 5}};
  source.calls = {call(1, "seed", {}, 10), call(2, "decrement", {20}, 30),
                  call(3, "increment", {5}, 40), call(4, "increment", {5}, 41)};
  source.blocks = {{1, {}, {1}, ps::IRJump{{2, {10}}}},
                   {2,
                    {{20, counter}},
                    {2, 3, 4},
                    ps::IRConditionalJump{30, {2, {30}}, {3, {40, 41}}}},
                   {3, {{50, data}, {51, data}}, {}, ps::IRReturn{{50, 51}}}};
  source.returns = {{"a", data}, {"b", data}};
  ps::Compiler compiler(registry);
  auto ir = compiler.bind_ir(source);
  PS_REQUIRE_OK(ir);
  ps::PlanningOptions options;
  options.input_layouts["data"] = ps::physical::InputLayout::DenseHost;
  auto plan = compiler.plan(ir.value(), options);
  PS_REQUIRE_OK(plan);
  PS_CHECK(plan.value().program().tasks.size() == 5);
  PS_CHECK(plan.value().program().calls[1].tasks.size() == 1);
  PS_CHECK(plan.value().program().calls[2].tasks.size() == 2);
  PS_CHECK(plan.value().program().calls[3].tasks.size() == 1);
  auto made = ps::ResultBuilder::start(ps::ResourceBudget{}, *data, "data");
  PS_REQUIRE_OK(made);
  auto builder = made.take_value();
  const std::uint8_t samples[] = {7, 9, 11, 13};
  PS_REQUIRE_OK(
      builder.publish_tensor(0, ps::Region::whole({4}), {samples, 4}));
  auto input = builder.seal();
  PS_REQUIRE_OK(input);
  ps::ExecutionBindings bindings;
  bindings.inputs = {{"data", input.take_value()}};
  ps::ExecutionContext context(registry);
  auto prepared = context.prepare(plan.value());
  PS_REQUIRE_OK(prepared);
  auto run = context.submit(prepared.value(), bindings);
  PS_REQUIRE_OK(run);
  auto result = run.value().join();
  PS_REQUIRE_OK(result);
  PS_CHECK(result.value().diagnostics.activations == 5);
  PS_CHECK(result.value().diagnostics.completed_tasks == 13);
  PS_CHECK(result.value().diagnostics.retired_calls == 10);
  PS_CHECK(sample(result.value().results.at("a")) == 8);
  PS_CHECK(sample(result.value().results.at("b")) == 8);
  return 0;
}
}  // namespace

int main() {
  PS_CHECK(binding_tiles() == 0);
  PS_CHECK(dense_boundaries() == 0);
  PS_CHECK(dense_loop_and_fanout() == 0);
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(operation("seed3", 0, 3)));
  PS_REQUIRE_OK(registry->register_operation(operation("seed1", 0, 1)));
  PS_REQUIRE_OK(registry->register_operation(operation("seed2", 0, 2)));
  PS_REQUIRE_OK(registry->register_operation(operation("decrement", 1, 0)));
  PS_REQUIRE_OK(registry->freeze());
  ps::Compiler compiler(registry);
  auto schema = std::make_shared<const ps::SchemaTemplate>(scalar());
  ps::IRProgram source;
  source.entry = 1;
  source.calls = {call(1, "seed3", {}, 10), call(2, "seed1", {}, 11),
                  call(3, "seed2", {}, 12), call(4, "decrement", {20}, 30)};
  source.blocks = {{1, {}, {1, 2, 3}, ps::IRJump{{2, {10, 11, 12}}}},
                   {2,
                    {{20, schema}, {21, schema}, {22, schema}},
                    {4},
                    ps::IRConditionalJump{30, {2, {30, 22, 21}}, {3, {}}}},
                   {3, {}, {}, ps::IRReturn{{21, 22, 11}}}};
  source.returns = {{"x", schema}, {"y", schema}, {"original", schema}};
  auto ir = compiler.bind_ir(source);
  PS_REQUIRE_OK(ir);
  auto plan = compiler.plan(ir.value());
  PS_REQUIRE_OK(plan);
  ps::ExecutionContextConfig config;
  config.cpu_workers = 2;
  ps::ExecutionContext context(registry, config);
  auto prepared = context.prepare(plan.value());
  PS_REQUIRE_OK(prepared);
  auto root = context.resource_budget().take_value();
  bool metadata_within_capacity = true;
  std::vector<std::uint64_t> calls, activations;
  ps::ExecutionOptions options;
  options.on_call_retired = [&](const ps::CallRecord& record) {
    calls.push_back(record.source_call);
    activations.push_back(record.activation);
    const auto statistics = root.statistics();
    metadata_within_capacity &= statistics.allocated_metadata_bytes <=
                                statistics.live[ps::ResourceKind::Metadata];
  };
  auto submitted = context.submit(prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(submitted);
  auto finished = submitted.value().join();
  PS_REQUIRE_OK(finished);
  PS_CHECK(sample(finished.value().results.at("x")) == 1);
  PS_CHECK(sample(finished.value().results.at("y")) == 2);
  PS_CHECK(sample(finished.value().results.at("original")) == 1);
  PS_CHECK(calls == std::vector<std::uint64_t>({1, 2, 3, 4, 4, 4}));
  PS_CHECK(activations == std::vector<std::uint64_t>({1, 1, 1, 2, 3, 4}));
  PS_CHECK(finished.value().diagnostics.activations == 5);
  PS_CHECK(metadata_within_capacity);

  options.maximum_block_activations = 2;
  auto bounded = context.submit(prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(bounded);
  PS_CHECK(bounded.value().join().status().code ==
           ps::ErrorCode::ResourceExhausted);

  // Bind a real owning Result; preflight retains its fixed affine storage.
  ps::WorkflowDocument input_document;
  input_document.inputs = {{1, "input", schema}};
  input_document.nodes = {
      {1, "decrement", {ps::WorkflowInputReference{1}}, {}}};
  input_document.outputs = {{"result", 1, "out0"}};
  ps::GraphContext graph(input_document);
  auto input_ir = compiler.analyze(graph.snapshot());
  PS_REQUIRE_OK(input_ir);
  auto input_plan = compiler.plan(input_ir.value());
  PS_REQUIRE_OK(input_plan);
  auto input_prepared = context.prepare(input_plan.value());
  PS_REQUIRE_OK(input_prepared);
  ps::ExecutionBindings bindings;
  bindings.inputs = {{"input", finished.value().results.at("y")}};
  auto input_run = context.submit(input_prepared.value(), bindings);
  PS_REQUIRE_OK(input_run);
  auto input_result = input_run.value().join();
  PS_REQUIRE_OK(input_result);
  PS_CHECK(sample(input_result.value().results.at("result")) == 1);
  bindings.inputs.push_back(bindings.inputs[0]);
  PS_CHECK(context.submit(input_prepared.value(), bindings).status().code ==
           ps::ErrorCode::InvalidArgument);

  // Structurally valid artifacts must still satisfy the linked kernel's
  // invocation contract before any task receives payload pointers.
  auto planners = std::make_shared<ps::PlannerRegistry>();
  auto malformed = *ps::make_default_planner_registry()->find("static-tiles");
  auto base = malformed.plan;
  malformed.name = "missing-read";
  malformed.plan = [base](const auto& input, auto& builder,
                          const auto& options) {
    auto status = base(input, builder, options);
    if (!status.ok())
      return status;
    auto& p = builder.program();
    for (auto& task : p.tasks)
      if (p.kernels[task.kernel].operation == "decrement")
        task.reads.clear();
    return ps::physical::finalize_program(&p);
  };
  PS_REQUIRE_OK(planners->add(std::move(malformed)));
  planners->freeze();
  ps::Compiler custom(registry, {}, planners);
  ps::PlanningOptions bad_options;
  bad_options.planner = "missing-read";
  auto bad_plan = custom.plan(input_ir.value(), bad_options);
  PS_REQUIRE_OK(bad_plan);
  auto serialized = ps::CompilerArtifact::export_plan(bad_plan.value());
  PS_REQUIRE_OK(serialized);
  auto decoded =
      ps::CompilerArtifact::import_plan(registry, serialized.value());
  PS_REQUIRE_OK(decoded);
  PS_CHECK(context.prepare(decoded.value()).status().code ==
           ps::ErrorCode::TypeMismatch);
  return 0;
}
