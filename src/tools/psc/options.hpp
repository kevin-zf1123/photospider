#pragma once
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/options.hpp"
#include "photospider/compiler/compiler.hpp"

namespace ps::tools::psc {
enum class EmitKind { Plan, IR };
struct Invocation final : CommonOptions {
  EmitKind emit = EmitKind::Plan;
  std::optional<std::size_t> requested_revision;
  std::string input_type;
  PlanningOptions planning;
  std::vector<OptimizerInvocation> optimizers;
  std::map<std::size_t, std::map<std::string, std::string>> optimizer_options;
  std::vector<std::pair<std::size_t, std::string>> snapshots;
  std::set<std::size_t> releases;
};
Invocation parse_options(int argc, char** argv, bool* json_diagnostics);
void resolve_optimizers(Invocation* options, const OptimizerRegistry& registry);
std::size_t terminal(const Invocation& options);
std::string option_value_text(const ParameterValue& value);
std::string option_type_name(const ParameterValue& value);
std::string output_path(const Invocation& options, std::size_t last);
}  // namespace ps::tools::psc
