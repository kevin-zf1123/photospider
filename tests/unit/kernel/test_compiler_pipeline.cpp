#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/compiler/artifact.hpp"
#include "support/kernel_registry_fixture.hpp"
#include "support/test_support.hpp"

// Frozen catalogs must not be replaced or drained through another owner.
static_assert(!std::is_copy_assignable_v<ps::FrontendRegistry> &&
              !std::is_move_assignable_v<ps::FrontendRegistry> &&
              !std::is_move_constructible_v<ps::FrontendRegistry>);
static_assert(!std::is_copy_assignable_v<ps::OptimizerRegistry> &&
              !std::is_move_assignable_v<ps::OptimizerRegistry> &&
              !std::is_move_constructible_v<ps::OptimizerRegistry>);
static_assert(!std::is_copy_assignable_v<ps::PlannerRegistry> &&
              !std::is_move_assignable_v<ps::PlannerRegistry> &&
              !std::is_move_constructible_v<ps::PlannerRegistry>);

int main() {
  auto operations = ps::test::kernel_registry();
  auto registry = std::make_shared<ps::OptimizerRegistry>();
  std::vector<std::string> trace;
  for (const auto& name : {"a", "b"}) {
    PS_CHECK(registry
                 ->add({name,
                        "1",
                        "identity",
                        {{"label", {std::string{}, {}, {}, {}, "trace label"}}},
                        [&, name](const ps::ValidatedIR& ir, const auto&) {
                          trace.push_back(name);
                          return ps::Result<ps::IRProgram>(ir.program());
                        }})
                 .ok());
  }
  PS_CHECK(registry
               ->add({"bad",
                      "1",
                      "invalid result",
                      {},
                      [](const ps::ValidatedIR& ir, const auto&) {
                        auto program = ir.program();
                        program.entry = 99999;
                        return ps::Result<ps::IRProgram>(std::move(program));
                      }})
               .ok());
  ps::OptimizerDefinition oversized{
      "large",
      std::string(129, 'v'),
      "",
      {},
      [](const ps::ValidatedIR& ir, const auto&) {
        return ps::Result<ps::IRProgram>(ir.program());
      }};
  PS_CHECK(!registry->add(oversized).ok());
  for (const auto& name : {"rename-output", "add-input"}) {
    PS_CHECK(registry
                 ->add({name,
                        "7",
                        "interface violation",
                        {},
                        [name](const ps::ValidatedIR& ir, const auto&) {
                          auto program = ir.program();
                          if (std::string(name) == "rename-output") {
                            program.returns.front().name = "renamed";
                          } else {
                            program.input_declarations.push_back(
                                {999, "extra-input",
                                 program.returns.front().result_schema});
                            program.input_bindings.push_back({999, 99999});
                          }
                          return ps::Result<ps::IRProgram>(std::move(program));
                        }})
                 .ok());
  }
  int bounded_calls = 0;
  ps::OptimizerDefinition bounded{
      "bounded",
      "2",
      "declarative option constraints",
      {{"count",
        {std::int64_t{1},
         ps::ParameterValue{std::int64_t{1}},
         ps::ParameterValue{std::int64_t{4}},
         {std::int64_t{1}, std::int64_t{2}, std::int64_t{4}},
         "number of rounds"}}},
      [&](const ps::ValidatedIR& ir, const auto&) {
        ++bounded_calls;
        return ps::Result<ps::IRProgram>(ir.program());
      },
      {ps::IRForm::ControlFlow}};
  auto bad_schema = bounded;
  bad_schema.name = "invalid-schema";
  bad_schema.option_schema["count"].maximum = std::string{"wrong type"};
  PS_CHECK(!registry->add(std::move(bad_schema)).ok());
  PS_CHECK(registry->add(std::move(bounded)).ok());
  registry->freeze();
  ps::Compiler compiler(operations, registry);
  ps::GraphContext graph(ps::test::addition_document(4.0, 5.0));
  auto plain = compiler.compile(graph);
  PS_REQUIRE_OK(plain);
  PS_CHECK(trace.empty());
  PS_CHECK(plain.value().ir_versions.size() == 1);
  PS_CHECK(plain.value().diagnostics.optimize_us == 0);
  ps::PlanningOptions selected_planner;
  selected_planner.planner = "static-tiles";
  auto explicit_plan =
      compiler.plan(*plain.value().ir_versions[0].ir, selected_planner);
  PS_REQUIRE_OK(explicit_plan);
  auto implicit_text = ps::CompilerArtifact::export_plan(*plain.value().plan);
  auto explicit_text = ps::CompilerArtifact::export_plan(explicit_plan.value());
  PS_REQUIRE_OK(implicit_text);
  PS_REQUIRE_OK(explicit_text);
  PS_CHECK(explicit_text.value() == implicit_text.value());
  selected_planner.planner = "missing";
  auto unknown_plan =
      compiler.plan(*plain.value().ir_versions[0].ir, selected_planner);
  PS_CHECK(!unknown_plan.ok() &&
           unknown_plan.status().code == ps::ErrorCode::NotFound);

  auto ordered =
      compiler.compile(graph, {}, {}, {{"b", {}}, {"a", {}}, {"b", {}}});
  PS_REQUIRE_OK(ordered);
  PS_CHECK(trace == std::vector<std::string>({"b", "a", "b"}));
  PS_CHECK(ordered.value().ir_versions.size() == 4);
  const auto& initial = *ordered.value().ir_versions[0].ir;
  for (const auto& revision : ordered.value().ir_versions)
    PS_CHECK(revision.ir->digest().value == initial.digest().value);
  const auto& transformed = *ordered.value().ir_versions.back().ir;
  auto encoded = ps::CompilerArtifact::export_ir(transformed);
  PS_REQUIRE_OK(encoded);
  auto imported = ps::CompilerArtifact::import_ir(compiler, encoded.value());
  PS_REQUIRE_OK(imported);
  PS_CHECK(imported.value().provenance().producer ==
           transformed.provenance().producer);
  PS_REQUIRE_OK(compiler.plan(imported.value()));
  auto failed = compiler.optimize(initial, {"bad", {}});
  PS_CHECK(!failed.ok() && failed.status().code == ps::ErrorCode::Internal);
  for (const auto& name : {"rename-output", "add-input"}) {
    auto changed = registry->find(name)->transform(initial, {});
    PS_REQUIRE_OK(changed);
    PS_REQUIRE_OK(compiler.bind_ir(std::move(changed).value()));
    auto violation = compiler.optimize(initial, {name, {}});
    PS_CHECK(!violation.ok() &&
             violation.status().code == ps::ErrorCode::Internal);
    PS_CHECK(violation.status().message.find(std::string(name) + "@7") !=
             std::string::npos);
  }
  auto unsupported_form = compiler.optimize(initial, {"bounded", {}});
  PS_CHECK(!unsupported_form.ok() &&
           unsupported_form.status().code == ps::ErrorCode::InvalidArgument);
  PS_CHECK(bounded_calls == 0);
  ps::Compiler defaults(operations);
  auto cfg = defaults.optimize(initial);
  PS_REQUIRE_OK(cfg);
  for (const auto& override_value :
       {ps::ParameterValue{std::int64_t{0}},
        ps::ParameterValue{std::int64_t{3}},
        ps::ParameterValue{std::int64_t{5}}, ps::ParameterValue{true}}) {
    auto rejected = compiler.optimize(cfg.value(),
                                      {"bounded", {{"count", override_value}}});
    PS_CHECK(!rejected.ok() &&
             rejected.status().code == ps::ErrorCode::InvalidArgument);
  }
  PS_CHECK(bounded_calls == 0);
  auto allowed =
      compiler.optimize(cfg.value(), {"bounded", {{"count", std::int64_t{2}}}});
  PS_REQUIRE_OK(allowed);
  PS_CHECK(bounded_calls == 1);
  PS_CHECK(std::get<std::int64_t>(
               allowed.value().provenance().options.at("count")) == 2);
  ps::IRRevision released(1, allowed.value());
  released.ir.reset();
  PS_CHECK(!released.ir && released.index == 1);
  PS_CHECK(released.content_digest().value == allowed.value().digest().value);
  PS_CHECK(released.provenance().parent_digest == cfg.value().digest().value);
  PS_CHECK(released.provenance().producer == "bounded" &&
           released.provenance().version == "2");
  PS_CHECK(std::get<std::int64_t>(released.provenance().options.at("count")) ==
           2);
  PS_REQUIRE_OK(compiler.plan(initial));
  PS_CHECK(
      !compiler.optimize(initial, {"a", {{"label", std::string(8193, 'x')}}})
           .ok());
  ps::ExecutionContext execution(operations);
  auto result = multi_result::run(execution, *plain.value().plan);
  PS_REQUIRE_OK(result);
  PS_CHECK(ps::test::named_scalar(result.value(), "sum") == 9.0);
  return 0;
}
