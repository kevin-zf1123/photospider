#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "photospider/compiler/planner.hpp"
#include "support/kernel_registry_fixture.hpp"
#include "support/test_support.hpp"

namespace {
// Independent backend for the scalar fixture. Uses only installed public types,
// emits full scalar grants, and never calls the built-in planner or mapper.
ps::Status scalar_plan(const ps::ValidatedIR&, ps::PlanBuilder& builder,
                       const ps::PlanningOptions&) {
  std::map<std::size_t, std::pair<std::size_t, std::size_t>> locations;
  for (std::size_t bi = 0; bi < builder.blocks().size(); ++bi) {
    auto& code = builder.instructions(bi);
    for (auto si : builder.blocks()[bi].calls) {
      const auto& step = builder.steps()[si];
      if (!step.shared_stages.empty() || step.tile_grids.size() != 1 ||
          step.tile_grids[0].tile_count() != 1)
        return {ps::ErrorCode::InvalidArgument, "scalar fixture required"};
      ps::PlanTileInstruction instruction;
      instruction.step_index = si;
      auto tile = ps::TileSet::range(ps::ResourceBudget{}, 0, 1);
      if (!tile.ok())
        return tile.status();
      instruction.publication.members.emplace(0, tile.value());
      auto samples = step.tile_grids[0].coverage(tile.value());
      if (!samples.ok())
        return samples.status();
      instruction.tensor_outputs = samples.value();
      instruction.publication_samples = {samples.value()};
      for (std::uint32_t port = 0; port < step.inputs.size(); ++port) {
        const auto& input = step.inputs[port];
        auto input_samples =
            builder.input_grids(input).at(0).coverage(tile.value());
        if (!input_samples.ok())
          return input_samples.status();
        instruction.inputs.push_back(
            {port, 0, false, tile.value(), input_samples.take_value()});
        if (const auto* source = std::get_if<ps::PlanStepInput>(&input)) {
          const auto location = locations.at(source->step_index);
          if (location.first == bi)
            instruction.dependencies.push_back(location.second);
        }
      }
      std::sort(instruction.dependencies.begin(),
                instruction.dependencies.end());
      locations.emplace(si, std::make_pair(bi, code.size()));
      code.push_back(std::move(instruction));
    }
  }
  return ps::Status::success();
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
  for (const auto& name : {"missing-output", "bad-step", "bad-dependency",
                           "bad-publication", "bad-read", "descriptor-samples",
                           "over-budget", "throws", "allocates", "fails"}) {
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
           auto status = scalar_plan(input, builder, options);
           if (!status.ok())
             return status;
           for (std::size_t bi = 0; bi < builder.blocks().size(); ++bi) {
             auto& code = builder.instructions(bi);
             if (mode == "missing-output") {
               code.clear();
               continue;
             }
             if (code.empty())
               continue;
             auto& instruction = code.front();
             if (mode == "bad-step")
               instruction.step_index = SIZE_MAX;
             if (mode == "bad-dependency")
               instruction.dependencies = {SIZE_MAX};
             if (mode == "bad-publication")
               instruction.publication.members.clear();
             if (mode == "over-budget")
               instruction.dependencies = {0, 1, 2};
             for (auto& item : code) {
               if (item.inputs.empty())
                 continue;
               if (mode == "bad-read")
                 item.inputs.front().tensor_slot = UINT32_MAX;
               if (mode == "descriptor-samples") {
                 item.inputs.front().descriptor_only = true;
                 item.inputs.front().tiles = {};
               }
             }
           }
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
  auto result = execution.execute(plan.value());
  PS_REQUIRE_OK(result);
  PS_CHECK(ps::test::named_scalar(result.value(), "sum") == 9);
  auto encoded = ps::CompilerArtifact::export_plan(plan.value());
  PS_REQUIRE_OK(encoded);
  auto loaded = ps::CompilerArtifact::import_plan(operations, encoded.value());
  PS_REQUIRE_OK(loaded);
  auto narrowed =
      loaded.value().select_outputs(loaded.value().output_requests());
  PS_REQUIRE_OK(narrowed);
  auto replay = execution.execute(narrowed.value());
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
  options.maximum_instructions = 1;
  auto limit = compiler.plan(ir.value(), options);
  PS_CHECK(!limit.ok() &&
           limit.status().code == ps::ErrorCode::ResourceExhausted);
  options.maximum_instructions = 4194304;
  for (const auto& name : {"missing-output", "bad-step", "bad-dependency",
                           "bad-publication", "bad-read", "descriptor-samples",
                           "throws", "allocates", "fails", "over-budget"}) {
    options.planner = name;
    options.maximum_dependency_edges =
        std::string(name) == "over-budget" ? 1 : 16777216;
    auto failed = compiler.plan(ir.value(), options);
    const std::string mode = name;
    const auto expected = mode == "allocates" || mode == "over-budget"
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
