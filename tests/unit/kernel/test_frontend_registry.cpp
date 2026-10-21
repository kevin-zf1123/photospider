#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "photospider/compiler/frontend.hpp"
#include "support/kernel_registry_fixture.hpp"
#include "support/test_support.hpp"

namespace {
// Independent frontend for a one-constant document, using public IR builders.
ps::Result<ps::IRProgram> lower_scalar(
    const ps::WorkflowDocument& source,
    const ps::OperationRegistry& operations) {
  if (source.nodes.size() != 1 || source.outputs.size() != 1 ||
      !source.inputs.empty() || source.nodes[0].operation != "core.constant" ||
      !source.nodes[0].inputs.empty() ||
      source.outputs[0].node_id != source.nodes[0].id ||
      source.outputs[0].port != "value")
    return ps::Result<ps::IRProgram>(
        {ps::ErrorCode::InvalidArgument, "scalar fixture required"});
  auto traits = operations.find_traits(source.nodes[0].operation);
  if (!traits.ok())
    return ps::Result<ps::IRProgram>(traits.status());
  ps::IRProgram program;
  ps::SemanticNode call;
  call.id = 10;
  call.operation = source.nodes[0].operation;
  call.parameters = source.nodes[0].parameters;
  call.outputs = {{traits.value().outputs[0].key, {}, 20}};
  program.calls.push_back(std::move(call));
  program.entry = 1;
  program.blocks = {{1, {}, {10}, ps::IRReturn{{20}}}};
  program.returns = {{source.outputs[0].name, {}}};
  return ps::Result<ps::IRProgram>(std::move(program));
}
ps::WorkflowDocument document() {
  ps::WorkflowDocument source;
  source.nodes = {{1, "core.constant", {}, {{"value", 7.0}}}};
  source.outputs = {{"result", 1, "value"}};
  return source;
}
}  // namespace

int main() {
  auto operations = ps::test::kernel_registry();
  ps::Compiler defaults(operations);
  ps::GraphContext graph(document());
  auto implicit_ir = defaults.analyze(graph.snapshot());
  auto explicit_ir = defaults.analyze(graph.snapshot(), {}, "dag");
  PS_REQUIRE_OK(implicit_ir);
  PS_REQUIRE_OK(explicit_ir);
  PS_CHECK(implicit_ir.value().form() == ps::IRForm::DependencyPaths);
  PS_CHECK(implicit_ir.value().digest().value ==
           explicit_ir.value().digest().value);
  PS_CHECK(implicit_ir.value().provenance().producer == "dag");
  PS_CHECK(implicit_ir.value().provenance().version == "1");
  PS_CHECK(defaults.frontends().frozen());
  PS_CHECK(defaults.frontends().find("dag") != nullptr);

  auto registry = std::make_shared<ps::FrontendRegistry>();
  int calls = 0;
  ps::FrontendDefinition custom{
      "scalar", "3", "Scalar document frontend",
      [&](const auto& source, const auto& environment) {
        ++calls;
        return lower_scalar(source, environment);
      }};
  PS_REQUIRE_OK(registry->add(custom));
  PS_CHECK(!registry->add(custom).ok());
  for (int kind = 0; kind < 5; ++kind) {
    auto invalid = custom;
    invalid.name = "invalid";
    if (kind == 0)
      invalid.name.clear();
    if (kind == 1)
      invalid.version.clear();
    if (kind == 2)
      invalid.lower = {};
    if (kind == 3)
      invalid.name = std::string(1025, 'x');
    if (kind == 4)
      invalid.version = std::string(129, 'v');
    PS_CHECK(!registry->add(std::move(invalid)).ok());
  }
  bool rejected_mutable = false;
  try {
    ps::Compiler compiler(operations, {}, {}, registry);
  } catch (const std::invalid_argument&) {
    rejected_mutable = true;
  }
  PS_CHECK(rejected_mutable);
  for (const auto& name : {"invalid-ir", "throws", "allocates", "fails"}) {
    PS_REQUIRE_OK(registry->add(
        {name, "1", "Failure fixture",
         [name](const ps::WorkflowDocument&,
                const ps::OperationRegistry&) -> ps::Result<ps::IRProgram> {
           const std::string mode = name;
           if (mode == "throws")
             throw std::runtime_error("fixture exception");
           if (mode == "allocates")
             throw std::bad_alloc();
           if (mode == "fails")
             return ps::Result<ps::IRProgram>(
                 {ps::ErrorCode::TypeMismatch, "fixture source error"});
           return ps::Result<ps::IRProgram>(ps::IRProgram{});
         }}));
  }
  PS_REQUIRE_OK(
      registry->add({"stale", "1", "Replace source during lowering",
                     [&](const auto& source, const auto& environment) {
                       auto result = lower_scalar(source, environment);
                       graph.replace(document());
                       return result;
                     }}));
  registry->freeze();
  PS_CHECK(!registry->add({"later", "1", "", lower_scalar}).ok());
  ps::Compiler compiler(operations, {}, {}, registry);
  registry.reset();
  auto ir = compiler.analyze(graph.snapshot(), {}, "scalar");
  PS_REQUIRE_OK(ir);
  PS_CHECK(calls == 1);
  PS_CHECK(ir.value().form() == ps::IRForm::ControlFlow);
  PS_CHECK(ir.value().provenance().producer == "scalar");
  PS_CHECK(ir.value().provenance().version == "3");
  PS_CHECK(ir.value().provenance().parent_digest.empty());
  PS_CHECK(ir.value().revision() == graph.revision());
  auto plan = compiler.plan(ir.value());
  PS_REQUIRE_OK(plan);
  ps::ExecutionContext execution(operations);
  auto executed = multi_result::run(execution, plan.value());
  PS_REQUIRE_OK(executed);
  PS_CHECK(ps::test::named_scalar(executed.value(), "result") == 7);
  auto encoded = ps::CompilerArtifact::export_ir(ir.value());
  PS_REQUIRE_OK(encoded);
  auto imported = ps::CompilerArtifact::import_ir(compiler, encoded.value());
  PS_REQUIRE_OK(imported);
  PS_CHECK(imported.value().provenance().producer == "scalar");
  PS_CHECK(imported.value().provenance().version == "3");
  PS_REQUIRE_OK(compiler.bind_ir(ir.value().program()));
  PS_CHECK(calls == 1);
  auto compiled = compiler.compile(graph, {}, {}, {}, "scalar");
  PS_REQUIRE_OK(compiled);
  PS_CHECK(calls == 2);
  PS_CHECK(compiled.value().ir_versions.front().ir->provenance().producer ==
           "scalar");
  auto unknown = compiler.analyze(graph.snapshot(), {}, "unknown");
  PS_CHECK(!unknown.ok() && unknown.status().code == ps::ErrorCode::NotFound);
  auto unknown_compile = compiler.compile(graph, {}, {}, {}, "unknown");
  PS_CHECK(!unknown_compile.ok() &&
           unknown_compile.status().code == ps::ErrorCode::NotFound);
  PS_CHECK(calls == 2);
  auto invalid_snapshot = compiler.analyze({}, {}, "scalar");
  PS_CHECK(!invalid_snapshot.ok() &&
           invalid_snapshot.status().code == ps::ErrorCode::Stale);
  PS_CHECK(calls == 2);
  for (const auto& name : {"invalid-ir", "throws", "allocates", "fails"}) {
    auto failed = compiler.analyze(graph.snapshot(), {}, name);
    const std::string mode = name;
    const auto expected = mode == "invalid-ir" ? ps::ErrorCode::InvalidArgument
                          : mode == "allocates"
                              ? ps::ErrorCode::ResourceExhausted
                          : mode == "fails" ? ps::ErrorCode::TypeMismatch
                                            : ps::ErrorCode::Internal;
    if (failed.ok() || failed.status().code != expected)
      std::cerr << mode << ": " << failed.status().message << '\n';
    PS_CHECK(!failed.ok() && failed.status().code == expected);
    PS_CHECK(failed.status().message.find("frontend " + mode + "@1") !=
             std::string::npos);
  }
  auto stale = compiler.analyze(graph.snapshot(), {}, "stale");
  PS_CHECK(!stale.ok() && stale.status().code == ps::ErrorCode::Stale);
  PS_CHECK(!ir.value().current());
  PS_CHECK(imported.value().current());
  const auto before = calls;
  ps::GraphSnapshot expired;
  {
    ps::GraphContext temporary(document());
    expired = temporary.snapshot();
  }
  auto rejected = compiler.analyze(expired, {}, "scalar");
  PS_CHECK(!rejected.ok() && rejected.status().code == ps::ErrorCode::Stale);
  PS_CHECK(calls == before);
  return 0;
}
