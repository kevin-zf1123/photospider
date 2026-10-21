#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include "photospider/execution/execution.hpp"
#include "support/fixed_operation_fixture.hpp"

namespace {
struct Counts {
  std::atomic<unsigned> checked{0}, destroyed{0};
};
struct State {
  ps::ResourceIdentity identity;
  std::shared_ptr<Counts> counts;
  std::int64_t mode;
  ~State() { ++counts->destroyed; }
};
int shared_input_record_admission() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(
      test::fixed_fixture::operation("repeat-input", 2)));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  document.inputs = {
      {1, "input",
       std::make_shared<const SchemaTemplate>(test::fixed_fixture::schema())}};
  document.nodes = {{1,
                     "repeat-input",
                     {WorkflowInputReference{1}, WorkflowInputReference{1}},
                     {}}};
  document.outputs = {{"out", 1, "out0"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto ir = compiler.analyze(graph.snapshot());
  PS_REQUIRE_OK(ir);
  PlanningOptions options;
  options.tile_extent = 4;
  options.maximum_records = 16;
  auto exact = compiler.plan(ir.value(), options);
  PS_REQUIRE_OK(exact);
  PS_CHECK(exact.value().program().tiles.size() == 2);
  options.maximum_records = 15;
  auto failed = compiler.plan(ir.value(), options);
  PS_CHECK(!failed.ok() &&
           failed.status().code == ErrorCode::ResourceExhausted);
  return 0;
}
int preparation_admission() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto operation = test::fixed_fixture::operation("prepare-budget", 0);
  auto prepare = operation.prepare_kernel;
  unsigned calls = 0;
  operation.prepare_kernel = [&](const auto& kernel, const auto& placement,
                                 const auto& query, const auto& resources,
                                 const auto* native) {
    ++calls;
    return prepare(kernel, placement, query, resources, native);
  };
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(std::move(operation)));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "prepare-budget", {}, {}}};
  document.outputs = {{"out", 1, "out0"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  PS_REQUIRE_OK(compiled);
  auto plan = *compiled.value().plan;
  PS_CHECK(&plan.program() == &compiled.value().plan->program());
  auto selected = plan.select_outputs({});
  PS_REQUIRE_OK(selected);
  PS_CHECK(selected.value().program().tasks.empty());
  PS_CHECK(!plan.program().tasks.empty());
  ResourceBudget retained;
  {
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Host] = 4 * 1024 * 1024;
    config.managed_resources->capacity[ResourceKind::Metadata] =
        4 * 1024 * 1024;
    ExecutionContext context(registry, config);
    retained = context.resource_budget().take_value();
    const auto before = retained.statistics().live;
    for (auto kind : {ResourceKind::Host, ResourceKind::Metadata}) {
      ResourceCapacity capacity;
      capacity[kind] = retained.available_capacity()[kind] -
                       ResourceBudget::lease_metadata_bytes();
      if (kind == ResourceKind::Metadata)
        capacity[ResourceKind::Host] = capacity[kind];
      auto blocked = retained.reserve(capacity);
      PS_REQUIRE_OK(blocked);
      auto failed = context.prepare(plan);
      PS_CHECK(!failed.ok() &&
               failed.status().code == ErrorCode::ResourceExhausted);
      PS_CHECK(calls == 0);
    }
    PS_CHECK(retained.statistics().live.values == before.values);
    {
      auto ready = context.prepare(plan);
      PS_REQUIRE_OK(ready);
      PS_CHECK(calls > 0 && &ready.value().plan().program() == &plan.program());
      PS_CHECK(retained.statistics().live[ResourceKind::Metadata] >
               before[ResourceKind::Metadata]);
    }
    PS_CHECK(retained.statistics().live.values == before.values);
  }
  for (auto live : retained.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int cpu_environment() {
  struct Restore {
    std::fenv_t saved{};
    bool active = std::fegetenv(&saved) == 0;
    ~Restore() {
      if (active)
        std::fesetenv(&saved);
    }
  } restore;
  PS_CHECK(restore.active);
  PS_CHECK(std::fesetround(FE_UPWARD) == 0);
  PS_CHECK(std::feclearexcept(FE_ALL_EXCEPT) == 0);
  for (unsigned mode : {0, 1, 2}) {
    ps_prepared_kernel_v8 raw{};
    raw.struct_size = sizeof(raw);
    raw.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    raw.context = &mode;
    raw.release = [](const void*) {};
    raw.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                        ps_kernel_status_v8* status) {
      if (std::fegetround() != FE_UPWARD || std::fesetround(FE_DOWNWARD) != 0 ||
          std::feraiseexcept(FE_INVALID) != 0) {
        *status = ps::kernel_failure(ps::ErrorCode::Internal);
        return;
      }
      switch (*static_cast<const unsigned*>(invocation->prepared_context)) {
        case 1:
          throw std::runtime_error("CPU callback exception");
        case 2:
          throw std::bad_alloc();
        default:
          *status = {};
      }
    };
    auto adopted = ps::PreparedKernel::adopt(raw, {});
    PS_REQUIRE_OK(adopted);
    auto kernel = adopted.take_value();
    ps_kernel_invocation_v8 invocation{};
    kernel.bind(&invocation);
    auto status = ps::kernel_status(kernel.invoke(invocation));
    PS_CHECK(status.code == (mode == 0   ? ps::ErrorCode::Ok
                             : mode == 1 ? ps::ErrorCode::OperationFailed
                                         : ps::ErrorCode::ResourceExhausted));
    if (mode == 1)
      PS_CHECK(status.reason == ps::FailureReason::HostException &&
               status.message == "CPU callback exception");
    PS_CHECK(std::fegetround() == FE_UPWARD);
    PS_CHECK(std::fetestexcept(FE_INVALID) == 0);
  }
  return 0;
}
}  // namespace

int main() {
  PS_CHECK(cpu_environment() == 0);
  PS_CHECK(preparation_admission() == 0);
  PS_CHECK(shared_input_record_admission() == 0);
  ps::ResourceBudget budget;
  const std::uint8_t bytes[] = {42, 7};
  auto imported = ps::ImmutableResource::import({bytes, sizeof(bytes)}, budget);
  PS_REQUIRE_OK(imported);
  auto resource = imported.take_value();
  std::weak_ptr<const ps::CpuStorage> storage = resource.storage();
  const auto identity = resource.identity();
  auto supplied = ps::ResourceSet::create({resource}, budget);
  PS_REQUIRE_OK(supplied);
  auto resources = supplied.take_value();
  auto counts = std::make_shared<Counts>();
  auto definition = ps::test::fixed_fixture::operation("resource", 0);
  definition.traits.requires_metadata_specialization = true;
  definition.traits.parameter_schema = {
      {"mode", ps::OperationParameterType::Int64}};
  definition.prepare_static = [identity, counts](const auto&,
                                                 const auto& parameters) {
    ps::OperationPreparation result;
    result.resources = {identity};
    auto state = std::make_shared<State>();
    state->identity = identity;
    state->counts = counts;
    state->mode = std::get<std::int64_t>(parameters.at("mode"));
    result.state = std::move(state);
    result.outputs.resize(1);
    result.outputs[0].metadata.result_schema =
        std::make_shared<const ps::SchemaTemplate>(
            ps::test::fixed_fixture::schema());
    return ps::Result<ps::OperationPreparation>(std::move(result));
  };
  definition.validate_resources =
      [](const void* value, const ps::ResourceSet& resources) -> ps::Status {
    const auto& state = *static_cast<const State*>(value);
    ++state.counts->checked;
    std::fesetround(FE_DOWNWARD);
    if (state.mode == 2)
      throw std::runtime_error("resource parser failure");
    if (state.mode == 1)
      return {ps::ErrorCode::TypeMismatch, "resource format rejected"};
    auto resource = resources.find(state.identity);
    if (!resource.ok())
      return resource.status();
    if (resources.size() != 1 || resource.value().bytes()[0] != 42)
      return {ps::ErrorCode::TypeMismatch, "unexpected resource bytes"};
    return ps::Status::success();
  };
  auto registry = std::make_shared<ps::OperationRegistry>();
  auto compiler_definition = definition;
  PS_REQUIRE_OK(registry->register_operation(std::move(definition)));
  PS_REQUIRE_OK(registry->freeze());
  std::fesetround(FE_UPWARD);
  auto missing =
      registry->prepare_operation("resource", {}, {{"mode", INT64_C(0)}});
  PS_CHECK(!missing.ok() && missing.status().code == ps::ErrorCode::NotFound);
  PS_CHECK(counts->checked == 0 && counts->destroyed == 1);
  for (std::int64_t mode : {1, 2}) {
    auto failed = registry->prepare_operation("resource", {}, {{"mode", mode}},
                                              resources);
    PS_CHECK(!failed.ok());
    PS_CHECK(failed.status().code == (mode == 1
                                          ? ps::ErrorCode::TypeMismatch
                                          : ps::ErrorCode::OperationFailed));
    PS_CHECK(std::fegetround() == FE_UPWARD);
  }
  PS_CHECK(counts->checked == 2 && counts->destroyed == 3);
  auto result = registry->prepare_operation("resource", {},
                                            {{"mode", INT64_C(0)}}, resources);
  PS_REQUIRE_OK(result);
  auto prepared = result.take_value();
  PS_CHECK(counts->checked == 3 && std::fegetround() == FE_UPWARD);
  auto empty =
      prepared->lower({ps::TileRequest{}}, 4, ps::physical::Backend::Cpu, {});
  PS_REQUIRE_OK(empty);
  PS_CHECK(empty.value().program.tasks.empty() && counts->checked == 3);
  ps::TileRequest request;
  auto tiles = ps::TileSet::range(budget, 0, 1);
  PS_REQUIRE_OK(tiles);
  request.members[0] = tiles.take_value();
  auto lowered = prepared->lower({request}, 4, ps::physical::Backend::Cpu, {});
  PS_REQUIRE_OK(lowered);
  auto kernel = prepared->prepare_kernel(lowered.value().program, 0,
                                         lowered.value().outputs);
  PS_REQUIRE_OK(kernel);
  PS_CHECK(counts->checked == 3);
  resource = {};
  resources = {};
  registry.reset();
  prepared.reset();
  PS_CHECK(!storage.expired() && counts->destroyed == 3);
  kernel = ps::Result<ps::PreparedKernel>(ps::PreparedKernel{});
  PS_CHECK(storage.expired() && counts->destroyed == 4);
  auto reimported =
      ps::ImmutableResource::import({bytes, sizeof(bytes)}, budget);
  PS_REQUIRE_OK(reimported);
  auto rebound = ps::ResourceSet::create({reimported.value()}, budget);
  PS_REQUIRE_OK(rebound);
  auto compiler_registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(
      compiler_registry->register_operation(std::move(compiler_definition)));
  PS_REQUIRE_OK(compiler_registry->freeze());
  ps::WorkflowDocument document;
  document.nodes = {{1, "resource", {}, {{"mode", INT64_C(0)}}}};
  document.outputs = {{"out", 1, "out0"}};
  ps::GraphContext graph(document);
  ps::Compiler compiler(compiler_registry);
  auto ir = compiler.analyze(graph.snapshot(), rebound.value());
  PS_REQUIRE_OK(ir);
  ps::PlanningOptions options;
  options.tile_extent = 4;
  options.output_requests["out"] = {};
  auto plan = compiler.plan(ir.value(), options);
  PS_REQUIRE_OK(plan);
  auto artifact = ps::CompilerArtifact::export_plan(plan.value());
  PS_REQUIRE_OK(artifact);
  const auto before_import = counts->checked.load();
  auto restored = ps::CompilerArtifact::import_plan(
      compiler_registry, artifact.value(), rebound.value());
  PS_REQUIRE_OK(restored);
  PS_CHECK(counts->checked == before_import + 1);
  auto unresolved =
      ps::CompilerArtifact::import_plan(compiler_registry, artifact.value());
  PS_CHECK(!unresolved.ok());
  ps::ExecutionContext context(compiler_registry);
  auto executable = context.prepare(restored.value());
  PS_REQUIRE_OK(executable);
  auto run = context.submit(executable.value(), {});
  PS_REQUIRE_OK(run);
  PS_REQUIRE_OK(run.value().join());
  PS_CHECK(counts->checked == before_import + 1);
  std::fesetround(FE_TONEAREST);
  return 0;
}
