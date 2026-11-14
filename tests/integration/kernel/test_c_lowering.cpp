#include <memory>
#include <string>

#include "photospider/compiler/artifact.hpp"
#include "support/test_support.hpp"

namespace {
ps::WorkflowDocument document(const char* key) {
  ps::WorkflowDocument result;
  result.nodes = {{1, key, {}, {}}};
  result.outputs = {{"out", 1, "value"}};
  return result;
}
int check_execution(const std::shared_ptr<ps::OperationRegistry>& operations,
                    const ps::ExecutionPlan& plan, bool v9) {
  ps::ExecutionContext context(operations);
  auto prepared = context.prepare(plan);
  PS_REQUIRE_OK(prepared);
  auto run = context.submit(prepared.value());
  PS_REQUIRE_OK(run);
  auto result = run.value().join();
  PS_REQUIRE_OK(result);
  PS_CHECK(ps::test::named_scalar(result.value(), "out") == 42);
  if (v9) {
    PS_CHECK(result.value().diagnostics.issued_work.stages == 1);
    PS_CHECK(result.value().diagnostics.completed_tasks == 2);
    ps::ExecutionOptions limits;
    limits.maximum_refinement_stages = 0;
    auto limited = context.submit(prepared.value(), {}, {}, limits);
    PS_REQUIRE_OK(limited);
    auto failure = limited.value().join();
    PS_CHECK(!failure.ok() &&
             failure.status().code == ps::ErrorCode::ResourceExhausted &&
             failure.status().reason == ps::FailureReason::StageLimit);
  }
  return 0;
}
}  // namespace
int main() {
  auto operations = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(operations->load_plugin(PS_LOWERING_MODULE));
  PS_REQUIRE_OK(operations->freeze());
  ps::Compiler compiler(operations);
  for (const char* key : {"fixture.custom-v9", "fixture.backend"}) {
    ps::GraphContext graph(document(key));
    auto compiled = compiler.compile(graph);
    PS_REQUIRE_OK(compiled);
    const auto& plan = *compiled.value().plan;
    const bool v9 = std::string(key) == "fixture.custom-v9";
    PS_CHECK(plan.program().kernels.size() == 1);
    PS_CHECK(plan.program().kernels[0].abi_version == (v9 ? 9U : 8U));
    PS_CHECK(check_execution(operations, plan, v9) == 0);
    auto text = ps::CompilerArtifact::export_plan(plan);
    PS_REQUIRE_OK(text);
    auto imported = ps::CompilerArtifact::import_plan(operations, text.value());
    PS_REQUIRE_OK(imported);
    PS_CHECK(check_execution(operations, imported.value(), v9) == 0);
  }
  ps::GraphContext invalid(document("fixture.bad-abi"));
  auto bad = compiler.compile(invalid);
  PS_CHECK(!bad.ok() && bad.status().code == ps::ErrorCode::InvalidArgument);

  // Lower both placements without requiring a GPU device or executing GPU work.
  auto operation = operations->prepare_operation("fixture.backend", {}, {});
  PS_REQUIRE_OK(operation);
  ps::TileRequest request;
  auto selection = ps::TileSet::range(ps::ResourceBudget{}, 0, 1);
  PS_REQUIRE_OK(selection);
  request.members.emplace(0, selection.take_value());
  auto invalid_gpu =
      operations->prepare_operation("fixture.bad-gpu-v9", {}, {});
  PS_REQUIRE_OK(invalid_gpu);
  auto gpu_v9 = invalid_gpu.value()->lower({request}, 128,
                                           ps::physical::Backend::Gpu, {});
  PS_CHECK(!gpu_v9.ok() &&
           gpu_v9.status().code == ps::ErrorCode::InvalidArgument);
  for (auto backend :
       {ps::physical::Backend::Cpu, ps::physical::Backend::Gpu}) {
    auto program = operation.value()->lower({request}, 128, backend, {});
    PS_REQUIRE_OK(program);
    PS_CHECK(program.value().program.tasks.size() == 1);
    const auto& task = program.value().program.tasks[0];
    const auto& placement = program.value().program.placements[task.placement];
    PS_CHECK(placement.backend == backend);
    PS_CHECK(placement.domain == (backend == ps::physical::Backend::Cpu
                                      ? ps::physical::Domain::Host
                                      : ps::physical::Domain::Device));
  }
  return 0;
}
