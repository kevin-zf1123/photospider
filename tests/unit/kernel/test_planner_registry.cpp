#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/planner.hpp"
#include "support/kernel_registry_fixture.hpp"
#include "support/test_support.hpp"

namespace {
// Independent backend for the scalar fixture using public physical tables.
// It constructs tasks directly and never invokes operation lowering.
ps::Status scalar_plan(const ps::ValidatedIR&, ps::PlanBuilder& builder,
                       const ps::PlanningOptions& options) {
  using namespace ps::physical;  // NOLINT(build/namespaces)
  auto& p = builder.program();
  Layout layout;
  layout.element_type = ps::ElementType::Float64;
  layout.strides[0] = 8;
  p.layouts.push_back(layout);
  p.placements.push_back({});
  for (Id vi = 0; vi < p.values.size(); ++vi) {
    auto& value = p.values[vi];
    const auto& schema = *p.schemas[value.schema];
    if (schema.tensors.size() != 1 ||
        schema.tensors[0].descriptor.element_type != ps::ElementType::Float64 ||
        schema.tensors[0].sample_shape() != std::vector<std::uint64_t>{1})
      return {ps::ErrorCode::InvalidArgument, "scalar fixture required"};
    value.tiles = {static_cast<Id>(p.tiles.size())};
    TileDesc tile;
    tile.value = vi;
    tile.layout = 0;
    tile.bytes = 8;
    p.tiles.push_back(tile);
  }
  for (Id ci = 0; ci < p.calls.size(); ++ci) {
    const auto& binding = builder.calls()[ci];
    TaskDesc task;
    task.call = ci;
    task.placement = 0;
    task.kernel = static_cast<Id>(p.kernels.size());
    p.kernels.push_back({binding.operation,
                         binding.inputs.empty() ? "constant" : "sum",
                         "scalar-v1"});
    for (Id port = 0; port < binding.inputs.size(); ++port)
      task.reads.push_back({port, p.values[binding.inputs[port]].tiles[0], 0});
    const auto ti = static_cast<Id>(p.tasks.size());
    for (auto output : binding.outputs) {
      auto tile = p.values[output].tiles[0];
      p.tiles[tile].producer = ti;
      task.writes.push_back(tile);
    }
    p.tasks.push_back(std::move(task));
    p.calls[ci].tasks.push_back(ti);
    p.blocks[p.calls[ci].block].tasks.push_back(ti);
  }
  ValidationLimits limits;
  limits.maximum_edges = options.maximum_dependency_edges;
  limits.maximum_work = options.maximum_work;
  return finalize_program(&p, limits);
}
}  // namespace

int main() {
  auto operations = ps::test::kernel_registry();
  ps::Compiler defaults(operations);
  ps::GraphContext graph(ps::test::addition_document(4, 5));
  auto ir = defaults.analyze(graph.snapshot());
  PS_REQUIRE_OK(ir);
  auto registry = std::make_shared<ps::PlannerRegistry>();
  int calls = 0;
  ps::PlannerDefinition custom{
      "scalar", "7", "Public scalar fixture backend",
      [&](const auto& input, auto& builder, const auto& options) {
        ++calls;
        return scalar_plan(input, builder, options);
      }};
  PS_REQUIRE_OK(registry->add(custom));
  PS_CHECK(!registry->add(custom).ok());
  for (int kind = 0; kind < 7; ++kind) {
    auto invalid = custom;
    invalid.name = "invalid";
    if (kind == 0)
      invalid.name.clear();
    if (kind == 1)
      invalid.version.clear();
    if (kind == 2)
      invalid.plan = {};
    if (kind == 3)
      invalid.supported_ir_forms.clear();
    if (kind == 4)
      invalid.supported_ir_forms = {static_cast<ps::IRForm>(99)};
    if (kind == 5)
      invalid.supported_ir_forms = {ps::IRForm::ControlFlow,
                                    ps::IRForm::ControlFlow};
    if (kind == 6)
      invalid.name = std::string(1025, 'x');
    PS_CHECK(!registry->add(std::move(invalid)).ok());
  }
  bool rejected_mutable = false;
  try {
    ps::Compiler compiler(operations, {}, registry);
  } catch (const std::invalid_argument&) {
    rejected_mutable = true;
  }
  PS_CHECK(rejected_mutable);
  auto cfg_only = custom;
  cfg_only.name = "cfg-only";
  cfg_only.supported_ir_forms = {ps::IRForm::ControlFlow};
  PS_REQUIRE_OK(registry->add(cfg_only));
  for (const auto& name :
       {"missing-output", "bad-task", "bad-dependency", "bad-write", "bad-read",
        "bad-schema-read", "over-budget", "outer-budget", "throws", "allocates",
        "fails"}) {
    PS_REQUIRE_OK(registry->add(
        {name, "1", "Failure fixture",
         [name](const auto& input, auto& builder, const auto& options) {
           const std::string mode = name;
           if (mode == "throws")
             throw std::runtime_error("fixture exception");
           if (mode == "allocates")
             throw std::bad_alloc();
           if (mode == "fails")
             return ps::Status{ps::ErrorCode::BackendUnavailable,
                               "fixture failure"};
           auto status = scalar_plan(
               input, builder,
               mode == "outer-budget" ? ps::PlanningOptions{} : options);
           if (!status.ok())
             return status;
           auto& p = builder.program();
           if (mode == "missing-output")
             p.tasks.clear();
           else if (mode == "bad-task")
             p.tasks.front().call = ps::physical::none;
           else if (mode == "bad-dependency")
             p.tasks.front().predecessors = {ps::physical::none};
           else if (mode == "bad-write")
             p.tasks.front().writes.clear();
           else if (mode == "bad-read")
             p.tasks.back().reads.front().tile = ps::physical::none;
           else if (mode == "bad-schema-read")
             p.tasks.back().schema_reads = {ps::physical::none};
           return ps::Status::success();
         }}));
  }
  PS_REQUIRE_OK(registry->add(
      {"stale", "1", "Replace source during callback",
       [&](const auto& input, auto& builder, const auto& options) {
         auto status = scalar_plan(input, builder, options);
         graph.replace(ps::test::addition_document(8, 9));
         return status;
       }}));
  registry->freeze();
  PS_CHECK(!registry->add({"later", "1", "", scalar_plan}).ok());
  ps::Compiler compiler(operations, {}, registry);
  ps::PlanningOptions options;
  options.planner = "scalar";
  auto plan = compiler.plan(ir.value(), options);
  PS_REQUIRE_OK(plan);
  PS_CHECK(calls == 1);
  ps::ExecutionContext execution(operations);
  auto result = multi_result::run(execution, plan.value());
  PS_REQUIRE_OK(result);
  PS_CHECK(ps::test::named_scalar(result.value(), "sum") == 9);
  auto encoded = ps::CompilerArtifact::export_plan(plan.value());
  PS_REQUIRE_OK(encoded);
  auto loaded = ps::CompilerArtifact::import_plan(operations, encoded.value());
  PS_REQUIRE_OK(loaded);
  auto narrowed =
      loaded.value().select_outputs(loaded.value().output_requests());
  PS_REQUIRE_OK(narrowed);
  auto replay = multi_result::run(execution, narrowed.value());
  PS_REQUIRE_OK(replay);
  PS_CHECK(ps::test::named_scalar(replay.value(), "sum") == 9);
  PS_CHECK(calls == 1);
  auto compiled = compiler.compile(graph, options);
  PS_REQUIRE_OK(compiled);
  PS_CHECK(calls == 2);
  options.planner = "cfg-only";
  auto unsupported = compiler.plan(ir.value(), options);
  PS_CHECK(!unsupported.ok() &&
           unsupported.status().code == ps::ErrorCode::InvalidArgument);
  PS_CHECK(calls == 2);
  auto cfg = defaults.optimize(ir.value());
  PS_REQUIRE_OK(cfg);
  PS_REQUIRE_OK(compiler.plan(cfg.value(), options));
  PS_CHECK(calls == 3);
  options.planner = "unknown";
  auto unknown = compiler.plan(ir.value(), options);
  PS_CHECK(!unknown.ok() && unknown.status().code == ps::ErrorCode::NotFound);
  ps::Compiler foreign(ps::test::kernel_registry(), {}, registry);
  options.planner = "scalar";
  auto wrong_owner = foreign.plan(ir.value(), options);
  PS_CHECK(!wrong_owner.ok() &&
           wrong_owner.status().code == ps::ErrorCode::Stale);
  PS_CHECK(calls == 3);
  options.maximum_tasks = 1;
  auto limit = compiler.plan(ir.value(), options);
  PS_CHECK(!limit.ok() &&
           limit.status().code == ps::ErrorCode::ResourceExhausted);
  options.maximum_tasks = 4194304;
  for (const auto& name :
       {"missing-output", "bad-task", "bad-dependency", "bad-write", "bad-read",
        "bad-schema-read", "throws", "allocates", "fails", "over-budget",
        "outer-budget"}) {
    options.planner = name;
    options.maximum_dependency_edges = (std::string(name) == "over-budget" ||
                                        std::string(name) == "outer-budget")
                                           ? 1
                                           : 16777216;
    auto failed = compiler.plan(ir.value(), options);
    const std::string mode = name;
    const auto expected =
        mode == "allocates" || mode == "over-budget" || mode == "outer-budget"
            ? ps::ErrorCode::ResourceExhausted
        : mode == "fails" ? ps::ErrorCode::BackendUnavailable
                          : ps::ErrorCode::Internal;
    if (failed.ok() || failed.status().code != expected)
      std::cerr << mode << ": code=" << static_cast<int>(failed.status().code)
                << " " << failed.status().message << '\n';
    PS_CHECK(!failed.ok() && failed.status().code == expected);
    PS_CHECK(failed.status().message.find("planner " + mode + "@1") !=
             std::string::npos);
  }
  options.planner = "stale";
  options.maximum_dependency_edges = 16777216;
  auto stale = compiler.plan(ir.value(), options);
  PS_CHECK(!stale.ok() && stale.status().code == ps::ErrorCode::Stale);
  PS_CHECK(plan.value().digest().value == loaded.value().digest().value);
  return 0;
}
