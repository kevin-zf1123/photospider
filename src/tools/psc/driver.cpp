#include "psc/driver.hpp"

#include <iostream>

#include "common/environment.hpp"
#include "common/process.hpp"
#include "psc/compilation_session.hpp"

namespace ps::tools::psc {
namespace {
void print_planner(const PlannerDefinition& definition) {
  std::cout << definition.name << '@' << definition.version << ": "
            << definition.description << '\n';
  std::cout << "IR forms:";
  for (auto form : definition.supported_ir_forms)
    std::cout << (form == IRForm::DependencyPaths ? " dependency_paths"
                                                  : " control_flow");
  std::cout << '\n';
}
void list_planners(const Invocation& o, const PlannerRegistry& registry) {
  if (o.has("--help-planner")) {
    const auto* definition = registry.find(o.get("--help-planner"));
    if (!definition)
      throw ToolError("unknown planner: " + o.get("--help-planner"), 2);
    print_planner(*definition);
    std::cout << "Planning options: --backend cpu|gpu --tile-extent N "
                 "--outputs a,b --requests FILE --max-tasks N "
                 "--max-dependency-edges N\n";
    return;
  }
  for (const auto& definition : registry.definitions())
    print_planner(definition);
}
void list_optimizers(const Invocation& o, const OptimizerRegistry& registry) {
  if (o.has("--help-optimizer")) {
    auto* found = registry.find(o.get("--help-optimizer"));
    if (!found)
      throw ToolError("unknown optimizer", 2);
    std::cout << found->name << '@' << found->version << ": "
              << found->description << '\n';
    std::cout << "IR forms:";
    for (auto form : found->supported_ir_forms)
      std::cout << (form == IRForm::DependencyPaths ? " dependency_paths"
                                                    : " control_flow");
    std::cout << '\n';
    if (found->option_schema.empty())
      std::cout << "Options: none\n";
    for (const auto& option : found->option_schema) {
      const auto& schema = option.second;
      std::cout << "  " << option.first
                << " type=" << option_type_name(schema.default_value)
                << " default=" << option_value_text(schema.default_value);
      if (schema.minimum)
        std::cout << " min=" << option_value_text(*schema.minimum);
      if (schema.maximum)
        std::cout << " max=" << option_value_text(*schema.maximum);
      if (!schema.choices.empty()) {
        std::cout << " choices:";
        for (const auto& choice : schema.choices)
          std::cout << ' ' << option_value_text(choice);
      }
      if (!schema.description.empty())
        std::cout << " (" << schema.description << ')';
      std::cout << '\n';
    }
    return;
  }
  for (const auto& definition : registry.definitions()) {
    std::cout << definition.name << '@' << definition.version << "\t"
              << definition.description << "\tforms:";
    for (auto form : definition.supported_ir_forms)
      std::cout << (form == IRForm::DependencyPaths ? " dependency_paths"
                                                    : " control_flow");
    std::cout << '\n';
  }
}
}  // namespace
int run(int argc, char** argv, Diagnostics* diagnostics) {
  auto o = parse_options(argc, argv, &diagnostics->json);
  configure_diagnostics(o, diagnostics);
  if (o.has("--help")) {
    std::cout << "psc <input|-> [options]\n";
    print_common_help();
    std::cout
        << "psc options:\n"
           "  -x dag|ir --emit plan|ir|ir:N\n"
           "  -O NAME --opt-option N:key=value --list-optimizers "
           "--help-optimizer NAME\n"
           "  --save-ir N=PATH --release-ir-after-save N\n"
           "  --planner NAME --list-planners --help-planner NAME\n"
           "  --backend cpu|gpu --tile-extent N --max-tasks N "
           "--max-dependency-edges N\n"
           "  --print-pipeline\n"
           "  -v --verbose       Report optimizer stages and IR releases\n"
           "  --error-limit N    Limit collected error reports "
           "(default: 20; 0: unlimited)\n"
           "  -Wall -Wextra -Werror -Wno-error\n"
           "  -Wunused-optimizer -Wno-unused-optimizer\n"
           "  -Werror=unused-optimizer -Wno-error=unused-optimizer\n"
           "  Only warning group: unused-optimizer (enabled by default).\n"
           "    Reports optimizer stages beyond the selected ir:N endpoint.\n"
           "    Other warning groups are rejected.\n";
    return 0;
  }
  if (o.has("--version")) {
    print_version("psc");
    return 0;
  }
  auto planners = make_default_planner_registry();
  if (o.has("--list-planners") || o.has("--help-planner")) {
    list_planners(o, *planners);
    return 0;
  }
  auto optimizers = make_default_optimizer_registry();
  if (o.has("--list-optimizers") || o.has("--help-optimizer")) {
    list_optimizers(o, *optimizers);
    return 0;
  }
  resolve_optimizers(&o, *optimizers);
  if (!o.has("--check") && o.emit == EmitKind::Plan &&
      !planners->find(o.planning.planner))
    throw ToolError("unknown planner: " + o.planning.planner, 2);
  Files files;
  if (o.has("--print-pipeline")) {
    print_pipeline(o, *optimizers, *planners, terminal(o), &files);
    return 0;
  }
  diagnostics->stage = "contracts";
  auto started = std::chrono::steady_clock::now();
  auto registry = load_operations(o.libraries, &files);
  diagnostics->timing("contracts", started);
  if (o.has("--list-operations")) {
    list_operations(*registry);
    return 0;
  }
  Compiler compiler(registry, optimizers, planners);
  return compile(o, compiler, &files, diagnostics);
}
}  // namespace ps::tools::psc
