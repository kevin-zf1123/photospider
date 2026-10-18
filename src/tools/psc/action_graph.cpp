#include <iostream>

#include "psc/compilation_session.hpp"
#include "psc/dag_io.hpp"

namespace ps::tools::psc {
void print_pipeline(const Invocation& o, const OptimizerRegistry& registry,
                    const PlannerRegistry& planners, std::size_t last,
                    Files* files) {
  auto type = o.input_type;
  if (type.empty()) {
    if (o.input == "-")
      throw ToolError("--print-pipeline with stdin requires -x", 2);
    // Identification only: no parser, registry or static hook is invoked.
    type = inspect_input(files->read(o.input));
  }
  std::cout << "input: " << o.input << " [" << type << "]\n";
  for (const auto& library : o.libraries)
    std::cout << "contracts: " << library << " [not loaded]\n";
  if (o.has("--resources"))
    std::cout << "resources: " << o.get("--resources") << " [not loaded]\n";
  std::cout << (type == "dag" ? "frontend -> validate -> IR0\n"
                              : "read IR -> validate -> IR0\n");
  for (std::size_t i = 0; i <= last; ++i) {
    for (const auto& snapshot : o.snapshots)
      if (snapshot.first == i)
        std::cout << "  save IR" << i << " -> " << snapshot.second << '\n';
    if (i < last) {
      std::cout << "  optimizer[" << i + 1 << "]: " << o.optimizers[i].name
                << '@' << registry.find(o.optimizers[i].name)->version
                << " -> validate -> IR" << i + 1 << '\n';
      for (const auto& option : o.optimizers[i].options)
        std::cout << "    " << option.first << '='
                  << option_value_text(option.second) << '\n';
    }
  }
  std::cout << "terminal stage: IR" << last << '\n';
  if (!o.has("--check")) {
    if (o.emit == EmitKind::Plan) {
      const auto* planner = planners.find(o.planning.planner);
      if (!planner)
        throw ToolError("unknown planner: " + o.planning.planner, 2);
      std::cout << "planner: " << planner->name << '@' << planner->version
                << " -> validate Plan -> ";
    } else {
      std::cout << "main IR -> ";
    }
    std::cout << output_path(o, last) << '\n';
  }
  if (o.has("--check") || o.emit == EmitKind::IR)
    std::cout << "planner: not scheduled\n";
}
}  // namespace ps::tools::psc
