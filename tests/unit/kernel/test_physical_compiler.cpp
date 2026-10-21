#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/artifact.hpp"
#include "photospider/compiler/compiler.hpp"
#include "photospider/plugin/fixed_invocation.hpp"
#include "photospider/plugin/operation_program.hpp"
#include "support/fixed_operation_fixture.hpp"
#include "support/status_check.hpp"

using namespace ps::test::fixed_fixture;  // NOLINT(build/namespaces)

int main() {
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(operation("zero", 0)));
  PS_REQUIRE_OK(registry->register_operation(operation("one", 1)));
  PS_REQUIRE_OK(registry->register_operation(operation("two", 2)));
  PS_REQUIRE_OK(registry->register_operation(operation("effect", 0, true)));
  PS_REQUIRE_OK(registry->register_operation(operation("pair", 0, false, 2)));
  auto aligned = operation("aligned", 1);
  const auto lower = aligned.lower;
  aligned.lower = [lower](const ps::OperationLowering& query) {
    auto graph = lower(query);
    if (!graph.ok())
      return graph;
    auto result = graph.take_value();
    auto& program = result.program;
    for (auto id : program.values[program.inputs[0]].tiles)
      program.tiles[id].alignment = 64;
    return ps::Result<ps::OperationProgram>(std::move(result));
  };
  PS_REQUIRE_OK(registry->register_operation(std::move(aligned)));
  auto shape_only = operation("shape-only", 1);
  shape_only.lower = [](const ps::OperationLowering& query) {
    ps::OperationProgramBuilder builder(query);
    for (const auto& member : query.requests[0].members)
      for (auto tile : member.second) {
        auto task = builder.add_task("increment", "increment-v1");
        if (!task.ok())
          return ps::Result<ps::OperationProgram>(task.status());
        auto status = builder.read_input_schema(task.value(), 0);
        if (status.ok())
          status = builder.read_input_schema(task.value(), 0);
        if (status.ok())
          status = builder.write_output(task.value(), 0, member.first, tile);
        if (!status.ok())
          return ps::Result<ps::OperationProgram>(status);
      }
    return builder.finish();
  };
  PS_REQUIRE_OK(registry->register_operation(std::move(shape_only)));
  PS_REQUIRE_OK(registry->freeze());
  ps::Compiler compiler(registry);
  ps::WorkflowDocument document;
  document.nodes = {
      {1, "zero", {}, {}},
      {2, "one", {ps::WorkflowNodeOutput{1, "out0"}}, {}},
      {3, "zero", {}, {}},
      {4, "one", {ps::WorkflowNodeOutput{3, "out0"}}, {}},
      {5,
       "two",
       {ps::WorkflowNodeOutput{2, "out0"}, ps::WorkflowNodeOutput{4, "out0"}},
       {}},
      {6, "zero", {}, {}}};
  document.outputs = {{"result", 5, "out0"}};
  ps::GraphContext context(document);
  auto ir = compiler.analyze(context.snapshot());
  PS_REQUIRE_OK(ir);
  auto compiled = compiler.plan(ir.value());
  PS_REQUIRE_OK(compiled);
  const auto& plan = compiled.value();
  const auto& p = plan.program();
  PS_CHECK(p.blocks.size() == 1 && p.calls.size() == 6 && p.tasks.size() == 5);
  PS_CHECK(p.calls[5].tasks.empty());
  PS_CHECK(preparations == 0);
  for (std::size_t i = 0; i < 6; ++i) {
    PS_CHECK(p.calls[i].source_call == i + 1);
    PS_CHECK(p.calls[i].ordinal == i);
  }
  auto optimized = compiler.optimize(ir.value());
  PS_REQUIRE_OK(optimized);
  auto optimized_plan = compiler.plan(optimized.value());
  PS_REQUIRE_OK(optimized_plan);
  PS_CHECK(optimized_plan.value().program().blocks.size() == 1);
  for (std::size_t i = 0; i < p.calls.size(); ++i)
    PS_CHECK(optimized_plan.value().program().calls[i].source_call ==
             p.calls[i].source_call);

  // A static schema dependency preserves the semantic call and metadata while
  // leaving the upstream sample producer elided and carrying no payload edge.
  ps::WorkflowDocument metadata_document;
  metadata_document.nodes = {
      {1, "zero", {}, {}},
      {2, "shape-only", {ps::WorkflowNodeOutput{1, "out0"}}, {}}};
  metadata_document.outputs = {{"result", 2, "out0"}};
  ps::GraphContext metadata_graph(metadata_document);
  auto metadata_ir = compiler.analyze(metadata_graph.snapshot());
  PS_REQUIRE_OK(metadata_ir);
  auto metadata_plan = compiler.plan(metadata_ir.value());
  PS_REQUIRE_OK(metadata_plan);
  const auto& metadata_program = metadata_plan.value().program();
  PS_CHECK(metadata_program.tasks.size() == 1 &&
           metadata_program.calls[0].tasks.empty());
  PS_CHECK(metadata_program.tasks[0].reads.empty() &&
           metadata_program.tasks[0].predecessors.empty());
  PS_CHECK(metadata_program.tasks[0].schema_reads ==
           metadata_plan.value().calls()[1].inputs);
  auto metadata_text = ps::CompilerArtifact::export_plan(metadata_plan.value());
  PS_REQUIRE_OK(metadata_text);
  auto metadata_loaded =
      ps::CompilerArtifact::import_plan(registry, metadata_text.value());
  PS_REQUIRE_OK(metadata_loaded);
  PS_CHECK(metadata_loaded.value().program().tasks[0].schema_reads ==
           metadata_program.tasks[0].schema_reads);

  // A consumer cannot silently strengthen an incoming allocation contract.
  auto misaligned_document = document;
  misaligned_document.nodes[1].operation = "aligned";
  ps::GraphContext misaligned_context(misaligned_document);
  auto misaligned_ir = compiler.analyze(misaligned_context.snapshot());
  PS_REQUIRE_OK(misaligned_ir);
  auto misaligned_plan = compiler.plan(misaligned_ir.value());
  PS_CHECK(!misaligned_plan.ok());
  PS_CHECK(misaligned_plan.status().code == ps::ErrorCode::InvalidArgument);

  // Selection operates on saved tables and retains all call records.
  auto count = lowerings.load();
  auto empty = plan.select_outputs({});
  PS_REQUIRE_OK(empty);
  PS_CHECK(empty.value().program().tasks.empty());
  PS_CHECK(empty.value().program().calls.size() == 6);
  PS_CHECK(lowerings == count && preparations == 0);
  PS_CHECK(!empty.value().select_outputs(plan.output_requests()).ok());

  // Artifacts preserve static tables and do not execute lowerers or kernels.
  auto encoded_ir = ps::CompilerArtifact::export_ir(ir.value());
  PS_REQUIRE_OK(encoded_ir);
  PS_CHECK(encoded_ir.value().find("psir 5\n") == 0);
  auto loaded_ir =
      ps::CompilerArtifact::import_ir(compiler, encoded_ir.value());
  PS_REQUIRE_OK(loaded_ir);
  PS_CHECK(loaded_ir.value().digest().value == ir.value().digest().value);
  auto encoded = ps::CompilerArtifact::export_plan(plan);
  PS_REQUIRE_OK(encoded);
  auto manifest = ps::CompilerArtifact::inspect(encoded.value());
  PS_REQUIRE_OK(manifest);
  PS_CHECK(manifest.value().format_version == 5);
  auto loaded = ps::CompilerArtifact::import_plan(registry, encoded.value());
  PS_REQUIRE_OK(loaded);
  PS_CHECK(loaded.value().digest().value == plan.digest().value);
  auto reencoded = ps::CompilerArtifact::export_plan(loaded.value());
  PS_REQUIRE_OK(reencoded);
  PS_CHECK(reencoded.value() == encoded.value());
  PS_CHECK(lowerings == count && preparations == 0);
  auto empty_text = ps::CompilerArtifact::export_plan(empty.value());
  PS_REQUIRE_OK(empty_text);
  auto empty_loaded =
      ps::CompilerArtifact::import_plan(registry, empty_text.value());
  PS_REQUIRE_OK(empty_loaded);
  PS_CHECK(empty_loaded.value().program().tasks.empty());
  PS_CHECK(empty_loaded.value().outputs().empty());
  auto invalid_text = encoded.value();
  invalid_text.replace(0, 6, "psxp 4");
  PS_CHECK(!ps::CompilerArtifact::inspect(invalid_text).ok());
  invalid_text = encoded.value();
  const auto mode = invalid_text.find("mode cpu_exact");
  PS_CHECK(mode != std::string::npos);
  invalid_text.replace(mode, std::strlen("mode cpu_exact"), "mode native_gpu");
  // NativeGpu is a static lowering preference, so CPU-only tasks remain
  // legal. The saved mode still participates in physical Plan identity.
  auto native_mode = ps::CompilerArtifact::import_plan(registry, invalid_text);
  PS_REQUIRE_OK(native_mode);
  PS_CHECK(native_mode.value().digest().value != plan.digest().value);
  const auto& native_program = native_mode.value().program();
  PS_CHECK(native_program.tasks.size() == p.tasks.size());
  for (const auto& task : native_program.tasks)
    PS_CHECK(native_program.placements[task.placement].backend ==
             ps::physical::Backend::Cpu);
  PS_CHECK(lowerings == count && preparations == 0);
  invalid_text = encoded.value();
  auto reservation = invalid_text.find("reservation [");
  PS_CHECK(reservation != std::string::npos);
  auto number = reservation + std::strlen("reservation [ ");
  auto end_number = invalid_text.find(' ', number);
  invalid_text.replace(number, end_number - number, "0");
  PS_CHECK(!ps::CompilerArtifact::import_plan(registry, invalid_text).ok());

  // Actual fixed kernels receive exclusive preallocated output views.
  std::vector<ps_kernel_layout_v8> layouts;
  for (const auto& layout : p.layouts)
    layouts.push_back(ps::kernel_layout(layout));
  std::vector<std::unique_ptr<std::uint8_t[]>> allocations;
  for (const auto& tile : p.tiles)
    allocations.push_back(std::make_unique<std::uint8_t[]>(tile.bytes));
  std::vector<bool> complete(p.tasks.size());
  std::size_t remaining = p.tasks.size();
  while (remaining) {
    bool progressed = false;
    for (std::size_t ti = 0; ti < p.tasks.size(); ++ti) {
      if (complete[ti])
        continue;
      const auto& task = p.tasks[ti];
      bool ready = true;
      for (auto predecessor : task.predecessors)
        ready &= complete[predecessor];
      if (!ready)
        continue;
      auto kernel = plan.calls()[task.call].prepared->prepare_kernel(
          p, static_cast<ps::physical::Id>(ti),
          plan.calls()[task.call].outputs);
      PS_REQUIRE_OK(kernel);
      std::vector<ps_kernel_const_view_v8> inputs;
      for (const auto& read : task.reads) {
        const auto& tile = p.tiles[read.tile];
        inputs.push_back({allocations[read.tile].get(), tile.bytes,
                          p.layouts[read.layout].byte_offset,
                          &layouts[read.layout], 0, read.port, tile.member, 0,
                          tile.tile_index});
      }
      std::vector<ps_kernel_mutable_view_v8> outputs;
      for (auto id : task.writes) {
        const auto& tile = p.tiles[id];
        outputs.push_back({allocations[id].get(), tile.bytes, 0,
                           &layouts[tile.layout], 0, 0, tile.member, 0,
                           tile.tile_index});
      }
      ps_kernel_invocation_v8 invocation{};
      invocation.inputs = inputs.data();
      invocation.input_count = static_cast<std::uint32_t>(inputs.size());
      invocation.outputs = outputs.data();
      invocation.output_count = static_cast<std::uint32_t>(outputs.size());
      kernel.value().bind(&invocation);
      PS_REQUIRE_OK(ps::kernel_status(kernel.value().invoke(invocation)));
      complete[ti] = true;
      --remaining;
      progressed = true;
    }
    PS_CHECK(progressed);
  }
  auto returned = p.blocks[0].return_values[0];
  auto tile = p.values[returned].tiles[0];
  for (unsigned i = 0; i < 4; ++i)
    PS_CHECK(allocations[tile][i] == 5);

  auto effect_document = document;
  effect_document.nodes.push_back({7, "effect", {}, {}});
  ps::GraphContext effects(effect_document);
  auto effect_ir = compiler.analyze(effects.snapshot());
  PS_REQUIRE_OK(effect_ir);
  auto effect_plan = compiler.plan(effect_ir.value());
  PS_REQUIRE_OK(effect_plan);
  auto effect_only = effect_plan.value().select_outputs({});
  PS_REQUIRE_OK(effect_only);
  PS_CHECK(effect_only.value().program().tasks.size() == 1);
  PS_CHECK(effect_only.value().program().tasks[0].kind ==
           ps::physical::TaskKind::Effect);
  PS_CHECK(effect_only.value().program().calls.size() == 7);

  ps::WorkflowDocument paired;
  paired.nodes = {{1, "pair", {}, {}}};
  paired.outputs = {{"a", 1, "out0"}, {"b", 1, "out1"}};
  ps::GraphContext pair_context(paired);
  auto pair_ir = compiler.analyze(pair_context.snapshot());
  PS_REQUIRE_OK(pair_ir);
  auto pair = compiler.plan(pair_ir.value());
  PS_REQUIRE_OK(pair);
  PS_CHECK(pair.value().program().calls.size() == 1);
  PS_CHECK(pair.value().program().tasks.size() == 1);
  PS_CHECK(pair.value().program().tasks[0].writes.size() == 2);
  auto selected = pair.value().select_outputs(
      {{"a", pair.value().output_requests().at("a")}});
  PS_REQUIRE_OK(selected);
  PS_CHECK(selected.value().program().tasks[0].writes.size() == 2);
  PS_CHECK(selected.value().outputs().size() == 1);
  // Table order is serialization identity, not dependency execution order.
  auto planners = std::make_shared<ps::PlannerRegistry>();
  auto reversed = *ps::make_default_planner_registry()->find("static-tiles");
  const auto base_planner = reversed.plan;
  reversed.name = "reversed";
  reversed.plan = [base_planner](const ps::ValidatedIR& input,
                                 ps::PlanBuilder& builder,
                                 const ps::PlanningOptions& options) {
    auto status = base_planner(input, builder, options);
    if (!status.ok())
      return status;
    auto& program = builder.program();
    auto remap = [&](ps::physical::Id id) {
      return id == ps::physical::none
                 ? id
                 : static_cast<ps::physical::Id>(program.tasks.size() - 1 - id);
    };
    std::reverse(program.tasks.begin(), program.tasks.end());
    for (auto& task : program.tasks) {
      for (auto& dependency : task.explicit_dependencies)
        dependency = remap(dependency);
      task.predecessors.clear();
      task.successors.clear();
    }
    for (auto& tile : program.tiles)
      tile.producer = remap(tile.producer);
    for (auto& call : program.calls) {
      call.effect_task = remap(call.effect_task);
      for (auto& task : call.tasks)
        task = remap(task);
    }
    for (auto& block : program.blocks)
      for (auto& task : block.tasks)
        task = remap(task);
    // Exercise signed affine read encoding and opaque bytes/scratch tables.
    for (auto& task : program.tasks) {
      task.parameters = {0, 127, 128, 255};
      task.scratch[0] = 17;
    }
    auto signed_layout = program.layouts.front();
    signed_layout.byte_offset = 3;
    signed_layout.strides[0] = -1;
    program.layouts.push_back(signed_layout);
    return ps::physical::finalize_program(&program);
  };
  PS_REQUIRE_OK(planners->add(std::move(reversed)));
  auto large_parameters =
      *ps::make_default_planner_registry()->find("static-tiles");
  large_parameters.name = "large-parameters";
  large_parameters.plan = [base_planner](const ps::ValidatedIR& input,
                                         ps::PlanBuilder& builder,
                                         const ps::PlanningOptions& options) {
    auto status = base_planner(input, builder, options);
    if (!status.ok())
      return status;
    builder.program().tasks[0].parameters.assign(16777216, 0);
    return ps::physical::finalize_program(&builder.program());
  };
  PS_REQUIRE_OK(planners->add(std::move(large_parameters)));
  planners->freeze();
  ps::Compiler reverse_compiler(registry, {}, planners);
  ps::PlanningOptions reverse_options;
  reverse_options.planner = "reversed";
  auto reverse_plan = reverse_compiler.plan(ir.value(), reverse_options);
  PS_REQUIRE_OK(reverse_plan);
  PS_CHECK(reverse_plan.value().program().tasks[0].predecessors[0] > 0);
  auto reverse_text = ps::CompilerArtifact::export_plan(reverse_plan.value());
  PS_REQUIRE_OK(reverse_text);
  count = lowerings;
  auto prepared_count = preparations.load();
  auto reverse_loaded =
      ps::CompilerArtifact::import_plan(registry, reverse_text.value());
  PS_REQUIRE_OK(reverse_loaded);
  PS_CHECK(reverse_loaded.value().digest().value ==
           reverse_plan.value().digest().value);
  auto reverse_reencoded =
      ps::CompilerArtifact::export_plan(reverse_loaded.value());
  PS_REQUIRE_OK(reverse_reencoded);
  PS_CHECK(reverse_reencoded.value() == reverse_text.value());
  PS_CHECK(lowerings == count && preparations == prepared_count);

  // Parameter bytes consume the work budget, independently of edge counts.
  reverse_options.planner = "large-parameters";
  auto large_plan = reverse_compiler.plan(pair_ir.value(), reverse_options);
  PS_REQUIRE_OK(large_plan);
  auto large_text = ps::CompilerArtifact::export_plan(large_plan.value());
  PS_REQUIRE_OK(large_text);
  auto large_loaded =
      ps::CompilerArtifact::import_plan(registry, large_text.value());
  PS_REQUIRE_OK(large_loaded);
  PS_CHECK(large_loaded.value().digest().value ==
           large_plan.value().digest().value);

  // A completed builder transfers its tables exactly once.
  ps::OperationLowering query;
  query.operation = "zero";
  query.outputs.push_back(
      {std::make_shared<const ps::SchemaTemplate>(schema())});
  query.requests.resize(1);
  ps::OperationProgramBuilder builder(query);
  auto built = builder.finish();
  PS_REQUIRE_OK(built);
  PS_CHECK(!builder.finish().ok());
  PS_CHECK(!builder.add_task("increment", "increment-v1").ok());
  PS_CHECK(!builder.add_copy().ok());
  PS_CHECK(!builder.read_input_schema(0, 0).ok());

  query.inputs = query.outputs;
  ps::OperationProgramBuilder transfer(query);
  auto copy = transfer.add_copy();
  PS_REQUIRE_OK(copy);
  PS_REQUIRE_OK(transfer.read_input(copy.value(), 0, 0, 0));
  PS_REQUIRE_OK(transfer.write_output(copy.value(), 0, 0, 0));
  auto copied = transfer.finish();
  PS_REQUIRE_OK(copied);
  PS_CHECK(copied.value().program.tasks.size() == 1);
  PS_CHECK(copied.value().program.tasks[0].kind ==
           ps::physical::TaskKind::Copy);
  PS_CHECK(copied.value().program.kernels.empty());
  ps::OperationProgramBuilder incomplete(query);
  PS_REQUIRE_OK(incomplete.add_copy());
  PS_CHECK(!incomplete.finish().ok());
  return 0;
}
