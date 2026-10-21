#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::run;
using multi_result::take;
struct Probe {
  std::atomic<unsigned> preparations{0}, cpu{0}, gpu{0};
};
OperationDefinition constant(const std::string& key,
                             const std::shared_ptr<Probe>& probe, bool cpu,
                             bool gpu, bool effect = false,
                             bool metadata_input = false) {
  OperationDefinition op;
  op.key = key;
  op.traits.supports_cpu = cpu;
  op.traits.supports_gpu = gpu;
  op.traits.side_effect_free = !effect;
  op.traits.outputs = {
      multi_result::output("value", multi_result::schema(ElementType::UInt8))};
  if (metadata_input) {
    op.traits.input_count = 1;
    OperationPortConstraint port;
    port.tensor_key = "number";
    port.element_type = static_cast<unsigned>(ElementType::UInt8);
    op.traits.input_schema = {port};
    op.traits.outputs[0].input_indices = std::vector<std::uint32_t>{};
  }
  op.lower = [effect, metadata_input](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    if (effect || !query.requests[0].empty()) {
      physical::Placement placement;
      placement.backend = query.backend;
      auto task = take(
          builder.add_task("constant", "admission-v1", placement, {}, effect));
      if (metadata_input)
        check(builder.read_input_schema(task, 0));
      for (const auto& member : query.requests[0].members)
        for (auto tile : member.second)
          check(builder.write_output(task, 0, member.first, tile));
    }
    return builder.finish();
  };
  op.prepare_kernel =
      [probe](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if ((kernel.symbol != "constant" && kernel.symbol != "increment") ||
        kernel.contract != "admission-v1" ||
        placement.domain != physical::Domain::Host ||
        query.reads.size() != (kernel.symbol == "increment" ? 1U : 0U) ||
        query.writes.size() > 1)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid admission kernel"});
    auto views = query.writes;
    views.insert(views.end(), query.reads.begin(), query.reads.end());
    for (const auto& view : views)
      if (view.bytes != 1 || view.domain != physical::Domain::Host ||
          view.layout.rank != 1 || view.layout.shape[0] != 1 ||
          view.layout.element_type != ElementType::UInt8)
        return Result<ps_prepared_kernel_v8>(
            {ErrorCode::TypeMismatch, "invalid admission output"});
    struct State {
      std::shared_ptr<Probe> probe;
      static void write(const ps_kernel_invocation_v8* invocation) noexcept {
        for (unsigned i = 0; i < invocation->output_count; ++i) {
          const auto& output = invocation->outputs[i];
          const auto value =
              invocation->input_count
                  ? static_cast<const std::uint8_t*>(
                        invocation->inputs[0]
                            .allocation)[invocation->inputs[0].byte_offset] +
                        1
                  : 17;
          static_cast<std::uint8_t*>(output.allocation)[output.byte_offset] =
              value;
        }
      }
    };
    ++probe->preparations;
    ps_prepared_kernel_v8 prepared{};
    prepared.struct_size = sizeof(prepared);
    prepared.abi_version = 8;
    prepared.context = new State{probe};
    prepared.release = [](const void* context) {
      delete static_cast<const State*>(context);
    };
    if (placement.backend == physical::Backend::Cpu) {
      prepared.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                               ps_kernel_status_v8* status) {
        ++static_cast<const State*>(invocation->prepared_context)->probe->cpu;
        State::write(invocation);
        *status = {};
      };
    } else {
      // Host-backed inline completion tests admission without native hardware.
      prepared.submit_gpu = [](const ps_kernel_invocation_v8* invocation,
                               const ps_kernel_completion_v8* completion) {
        ++static_cast<const State*>(invocation->prepared_context)->probe->gpu;
        State::write(invocation);
        const ps_kernel_status_v8 status{};
        completion->finish(completion->context, completion->key, &status);
        ps_kernel_submission_v8 submitted{};
        submitted.accepted = 1;
        return submitted;
      };
    }
    return Result<ps_prepared_kernel_v8>(prepared);
  };
  return op;
}
OperationDefinition staged(const std::string& key,
                           const std::shared_ptr<Probe>& probe,
                           bool supports_cpu) {
  auto op = constant(key, probe, supports_cpu, true);
  op.lower = [](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    if (!query.requests[0].empty()) {
      const auto value =
          take(builder.add_temporary(query.outputs[0].result_schema));
      physical::Placement cpu, gpu;
      gpu.backend = physical::Backend::Gpu;
      for (const auto& member : query.requests[0].members)
        for (auto tile : member.second) {
          const auto first =
              take(builder.add_task("constant", "admission-v1", cpu));
          const auto temporary =
              take(builder.write_temporary(first, value, member.first, tile));
          auto second = builder.add_task("increment", "admission-v1", gpu);
          if (!second.ok())
            return Result<OperationProgram>(second.status());
          check(builder.read_tile(second.value(), temporary));
          check(builder.write_output(second.value(), 0, member.first, tile));
        }
    }
    return builder.finish();
  };
  return op;
}
void check_value(const ResultRef& result) {
  std::uint8_t value = 0;
  check(result.read_tensor(take(result.descriptor()), 0, {0}, &value, 1));
  if (value != 17)
    throw std::runtime_error("wrong admitted output");
}
}  // namespace

int main() {
  auto registry = std::make_shared<OperationRegistry>();
  auto gpu_probe = std::make_shared<Probe>();
  auto cpu_probe = std::make_shared<Probe>();
  auto both_probe = std::make_shared<Probe>();
  auto consumer_probe = std::make_shared<Probe>();
  auto effect_probe = std::make_shared<Probe>();
  auto staged_probe = std::make_shared<Probe>();
  check(registry->register_operation(staged("staged", staged_probe, true)));
  check(registry->register_operation(
      staged("invalid-stage", staged_probe, false)));
  check(registry->register_operation(constant("gpu", gpu_probe, false, true)));
  check(registry->register_operation(constant("cpu", cpu_probe, true, false)));
  check(registry->register_operation(
      constant("gpu-effect", gpu_probe, false, true, true)));
  check(registry->register_operation(constant("both", both_probe, true, true)));
  check(registry->register_operation(
      constant("metadata", consumer_probe, true, true, false, true)));
  check(registry->register_operation(
      constant("metadata-effect", effect_probe, true, true, true, true)));
  PS_CHECK(
      registry->register_operation(constant("invalid", gpu_probe, false, false))
          .code == ErrorCode::InvalidArgument);
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "gpu", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  PS_CHECK(compiler.compile(graph).status().code ==
           ErrorCode::BackendUnavailable);
  PlanningOptions native;
  native.execution_mode = ExecutionMode::NativeGpu;
  auto gpu_plan = take(compiler.compile(graph, native)).plan.value();
  const auto& binding = gpu_plan.calls()[0];
  PS_CHECK(binding.prepared
               ->lower({gpu_plan.output_requests().at("value")}, 128,
                       physical::Backend::Cpu, {})
               .status()
               .code == ErrorCode::BackendUnavailable);
  ExecutionContextConfig disabled_config;
  disabled_config.maximum_gpu_submissions = 0;
  ExecutionContext disabled(registry, disabled_config);
  PS_CHECK(disabled.prepare(gpu_plan).status().code ==
           ErrorCode::BackendUnavailable);
  PS_CHECK(!gpu_probe->preparations && !gpu_probe->cpu && !gpu_probe->gpu);

  document.nodes[0].operation = "both";
  GraphContext both_graph(document);
  auto both_gpu = take(compiler.compile(both_graph, native)).plan.value();
  PS_CHECK(disabled.prepare(both_gpu).status().code ==
           ErrorCode::BackendUnavailable);
  PS_CHECK(!both_probe->preparations && !both_probe->cpu && !both_probe->gpu);
  auto both_cpu = take(compiler.compile(both_graph)).plan.value();
  check_value(take(run(disabled, both_cpu)).results.at("value"));
  PS_CHECK(both_probe->cpu == 1 && !both_probe->gpu);
  ExecutionContext enabled(registry);
  check_value(take(run(enabled, both_gpu)).results.at("value"));
  PS_CHECK(both_probe->cpu == 1 && both_probe->gpu == 1);

  // Native placement is chosen per operation before preparation. CPU-only
  // calls remain CPU tasks in a mixed Plan and after artifact round-trip.
  WorkflowDocument mixed;
  mixed.nodes = {{1, "cpu", {}, {}}, {2, "both", {}, {}}};
  mixed.outputs = {{"host", 1, "value"}, {"native", 2, "value"}};
  GraphContext mixed_graph(mixed);
  auto mixed_plan = take(compiler.compile(mixed_graph, native)).plan.value();
  auto imported = take(CompilerArtifact::import_plan(
      registry, take(CompilerArtifact::export_plan(mixed_plan))));
  for (const auto* plan : {&mixed_plan, &imported}) {
    PS_CHECK(plan->program().tasks.size() == 2);
    const auto& physical = plan->program();
    PS_CHECK(physical.placements[physical.tasks[0].placement].backend ==
             physical::Backend::Cpu);
    PS_CHECK(physical.placements[physical.tasks[1].placement].backend ==
             physical::Backend::Gpu);
    auto result = take(run(enabled, *plan));
    check_value(result.results.at("host"));
    check_value(result.results.at("native"));
    PS_CHECK(result.diagnostics.backends[0].submitted == 1 &&
             result.diagnostics.backends[1].submitted == 1);
  }
  PS_CHECK(cpu_probe->cpu == 2 && !cpu_probe->gpu && both_probe->cpu == 1 &&
           both_probe->gpu == 3);

  // A single call may have CPU preparation feeding a GPU task. Placement
  // remains static through artifact import; the private value is not exported.
  WorkflowDocument stages;
  stages.nodes = {{1, "staged", {}, {}}};
  stages.outputs = {{"value", 1, "value"}};
  GraphContext stages_graph(stages);
  auto stage_plan = take(compiler.compile(stages_graph, native)).plan.value();
  auto stage_import = take(CompilerArtifact::import_plan(
      registry, take(CompilerArtifact::export_plan(stage_plan))));
  for (const auto* plan : {&stage_plan, &stage_import}) {
    PS_CHECK(plan->program().calls.size() == 1 &&
             plan->program().tasks.size() == 2);
    auto result = take(run(enabled, *plan));
    const auto& output = result.results.at("value");
    std::uint8_t value = 0;
    check(output.read_tensor(take(output.descriptor()), 0, {0}, &value, 1));
    PS_CHECK(value == 18);
    PS_CHECK(result.diagnostics.backends[0].submitted == 1 &&
             result.diagnostics.backends[1].submitted == 1 &&
             result.diagnostics.retired_calls == 1);
  }
  PS_CHECK(staged_probe->cpu == 2 && staged_probe->gpu == 2);
  PS_CHECK(compiler.compile(stages_graph).status().code ==
           ErrorCode::InvalidArgument);
  stages.nodes[0].operation = "invalid-stage";
  GraphContext invalid_stage(stages);
  PS_CHECK(compiler.compile(invalid_stage, native).status().code ==
           ErrorCode::InvalidArgument);

  WorkflowDocument metadata;
  metadata.nodes = {{1, "gpu", {}, {}},
                    {2, "metadata", {WorkflowNodeOutput{1, "value"}}, {}}};
  metadata.outputs = {{"value", 2, "value"}};
  GraphContext metadata_graph(metadata);
  auto metadata_cpu = take(compiler.compile(metadata_graph)).plan.value();
  PS_CHECK(metadata_cpu.program().calls[0].tasks.empty());
  check_value(take(run(disabled, metadata_cpu)).results.at("value"));
  auto metadata_gpu =
      take(compiler.compile(metadata_graph, native)).plan.value();
  check_value(take(run(enabled, metadata_gpu)).results.at("value"));
  PS_CHECK(!gpu_probe->preparations && !gpu_probe->gpu);

  metadata.nodes.push_back(
      {3, "metadata-effect", {WorkflowNodeOutput{1, "value"}}, {}});
  GraphContext effect_graph(metadata);
  for (bool optimized : {false, true}) {
    auto effect_plan = take(optimized ? compiler.compile(effect_graph, {}, {},
                                                         {{"optimize", {}}})
                                      : compiler.compile(effect_graph))
                           .plan.value();
    const auto before = effect_probe->cpu.load();
    check_value(take(run(disabled, effect_plan)).results.at("value"));
    auto empty = take(effect_plan.select_outputs({}));
    PS_CHECK(take(run(disabled, empty)).results.empty());
    PS_CHECK(effect_probe->cpu == before + 2 && !gpu_probe->preparations);
  }

  metadata.nodes.push_back({4, "gpu-effect", {}, {}});
  GraphContext unavailable(metadata);
  PS_CHECK(compiler.compile(unavailable).status().code ==
           ErrorCode::BackendUnavailable);
  metadata.inputs = {multi_result::declaration(1, "wrong")};
  metadata.nodes = {{2, "metadata", {WorkflowInputReference{1}}, {}}};
  GraphContext wrong(metadata);
  PS_CHECK(compiler.compile(wrong).status().code == ErrorCode::TypeMismatch);
  return 0;
}
