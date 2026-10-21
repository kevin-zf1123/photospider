#include "psc/options.hpp"

#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "common/file_io.hpp"
#include "common/json.hpp"

namespace ps::tools::psc {
std::size_t terminal(const Invocation& o) {
  auto n = o.requested_revision.value_or(o.optimizers.size());
  if (n > o.optimizers.size())
    throw ToolError("requested IR revision is beyond the optimizer pipeline",
                    2);
  for (const auto& saved : o.snapshots) {
    if (saved.first > n)
      throw ToolError("--save-ir would extend the main pipeline", 2);
    if (saved.second == "-")
      throw ToolError("only the primary output may use stdout", 2);
  }
  for (auto released : o.releases) {
    bool saved = o.emit != EmitKind::Plan && released == n;
    for (const auto& snapshot : o.snapshots)
      saved = saved || snapshot.first == released;
    if (!saved)
      throw ToolError("released IR must be requested as an output", 2);
  }
  return n;
}
void resolve_optimizers(Invocation* o, const OptimizerRegistry& registry) {
  for (auto& invocation : o->optimizers)
    if (!registry.find(invocation.name))
      throw ToolError("unknown optimizer: " + invocation.name, 2);
  for (const auto& stage : o->optimizer_options) {
    if (!stage.first || stage.first > o->optimizers.size())
      throw ToolError("optimizer option stage is out of range", 2);
    auto& invocation = o->optimizers[stage.first - 1];
    const auto& defaults = registry.find(invocation.name)->option_schema;
    for (const auto& option : stage.second) {
      auto found = defaults.find(option.first);
      if (found == defaults.end())
        throw ToolError("unknown option for optimizer " + invocation.name +
                            ": " + option.first,
                        2);
      if (std::holds_alternative<std::string>(found->second.default_value)) {
        invocation.options[option.first] = option.second;
      } else {
        try {
          JsonDocument value(option.second);
          if (std::holds_alternative<std::int64_t>(found->second.default_value))
            invocation.options[option.first] = value.root().i64();
          else if (std::holds_alternative<double>(found->second.default_value))
            invocation.options[option.first] = value.root().real();
          else
            invocation.options[option.first] = value.root().boolean();
        } catch (const ToolError& e) {
          throw ToolError("optimizer option " + option.first + ": " + e.what(),
                          2);
        }
      }
    }
  }
  for (auto& invocation : o->optimizers) {
    auto resolved =
        registry.find(invocation.name)->resolve_options(invocation.options);
    if (!resolved.ok())
      throw ToolError(resolved.status().message, 2);
    invocation.options = resolved.take_value();
  }
}
std::string option_type_name(const ParameterValue& value) {
  if (std::holds_alternative<std::int64_t>(value))
    return "i64";
  if (std::holds_alternative<double>(value))
    return "f64";
  if (std::holds_alternative<bool>(value))
    return "bool";
  return "string";
}
std::string option_value_text(const ParameterValue& value) {
  if (const auto* number = std::get_if<std::int64_t>(&value))
    return std::to_string(*number);
  if (const auto* number = std::get_if<double>(&value)) {
    std::uint64_t bits;
    std::memcpy(&bits, number, sizeof(bits));
    std::ostringstream stream;
    stream << "f64_bits:" << std::hex << std::setfill('0') << std::setw(16)
           << bits;
    return stream.str();
  }
  if (const auto* flag = std::get_if<bool>(&value))
    return *flag ? "true" : "false";
  JsonWriter writer;
  auto text = writer.finish(writer.str(std::get<std::string>(value)));
  text.pop_back();
  return text;
}
Invocation parse_options(int argc, char** argv, bool* json_diagnostics) {
  auto parsed = parse_arguments(
      argc, argv, json_diagnostics,
      {"--print-pipeline", "--list-optimizers", "--list-planners", "--verbose"},
      {"-x", "--emit", "--planner", "--backend", "--tile-extent", "--max-tasks",
       "--max-dependency-edges", "--help-optimizer", "--help-planner",
       "--error-limit"},
      {"-O", "--opt-option", "--save-ir", "--release-ir-after-save"}, true);
  Invocation o;
  static_cast<CommonOptions&>(o) = std::move(parsed.common);
  if (o.has("--error-limit"))
    unsigned_number(o.get("--error-limit"), "--error-limit");
  for (const auto& item : parsed.repeated) {
    const auto& arg = item.first;
    const auto& value = item.second;
    if (arg == "-O") {
      o.optimizers.push_back({value, {}});
    } else if (arg == "--release-ir-after-save") {
      if (!o.releases.insert(unsigned_number(value, arg)).second)
        throw ToolError("duplicate release", 2);
    } else if (arg == "--save-ir") {
      auto split = value.find('=');
      if (split == std::string::npos || split + 1 == value.size())
        throw ToolError("expected N=path for --save-ir", 2);
      o.snapshots.emplace_back(unsigned_number(value.substr(0, split), arg),
                               value.substr(split + 1));
    } else {
      auto colon = value.find(':');
      auto split = value.find('=');
      if (colon == std::string::npos || split == std::string::npos ||
          colon + 1 >= split)
        throw ToolError("expected N:key=value for --opt-option", 2);
      auto index = unsigned_number(value.substr(0, colon), arg);
      if (!index || !o.optimizer_options[index]
                         .emplace(value.substr(colon + 1, split - colon - 1),
                                  value.substr(split + 1))
                         .second)
        throw ToolError("invalid or duplicate optimizer option", 2);
    }
  }
  o.input_type = o.get("-x");
  if (!o.input_type.empty() && o.input_type != "dag" && o.input_type != "ir")
    throw ToolError("-x requires dag or ir", 2);
  auto emit = o.get("--emit", "plan");
  if (emit == "ir") {
    o.emit = EmitKind::IR;
  } else if (emit.rfind("ir:", 0) == 0) {
    o.emit = EmitKind::IR;
    o.requested_revision = unsigned_number(emit.substr(3), "--emit");
  } else if (emit != "plan") {
    throw ToolError("--emit requires plan, ir, or ir:N", 2);
  }
  const std::set<std::string> planning_options = {"--planner",
                                                  "--backend",
                                                  "--tile-extent",
                                                  "--max-tasks",
                                                  "--max-dependency-edges",
                                                  "--outputs",
                                                  "--requests"};
  if (o.has("--check")) {
    if (o.has("--emit") || o.has("-o") || !o.optimizers.empty() ||
        !o.optimizer_options.empty() || !o.snapshots.empty() ||
        !o.releases.empty())
      throw ToolError("--check conflicts with optimization or output options",
                      2);
  }
  if (o.has("--check") || o.emit == EmitKind::IR) {
    for (const auto& key : planning_options)
      if (o.has(key))
        throw ToolError("IR/check target conflicts with " + key, 2);
  }
  o.planning.planner = o.get("--planner", o.planning.planner);
  auto backend = o.get("--backend", "cpu");
  if (backend != "cpu" && backend != "gpu")
    throw ToolError("--backend requires cpu or gpu", 2);
  o.planning.execution_mode =
      backend == "cpu" ? ExecutionMode::CpuExact : ExecutionMode::NativeGpu;
  if (o.has("--tile-extent"))
    o.planning.tile_extent =
        unsigned_number(o.get("--tile-extent"), "--tile-extent");
  if (!o.planning.tile_extent ||
      (o.planning.tile_extent & (o.planning.tile_extent - 1)))
    throw ToolError("tile extent must be a positive power of two", 2);
  if (o.has("--max-tasks"))
    o.planning.maximum_tasks =
        unsigned_number(o.get("--max-tasks"), "--max-tasks");
  if (o.has("--max-dependency-edges"))
    o.planning.maximum_dependency_edges = unsigned_number(
        o.get("--max-dependency-edges"), "--max-dependency-edges");
  if (!o.planning.maximum_tasks || !o.planning.maximum_dependency_edges)
    throw ToolError("planning limits must be positive", 2);
  if (o.has("--outputs"))
    o.planning.output_names = output_names(o.get("--outputs"));
  bool information = o.has("--help") || o.has("--version") ||
                     o.has("--list-optimizers") || o.has("--help-optimizer") ||
                     o.has("--list-operations") || o.has("--list-planners") ||
                     o.has("--help-planner");
  require_input(o, information);
  terminal(o);
  return o;
}
std::string output_path(const Invocation& o, std::size_t last) {
  if (!o.output.empty())
    return o.output;
  if (o.input == "-")
    throw ToolError("stdin requires explicit -o", 2);
  auto stem = fs::path(o.input).stem().string();
  return stem + (o.emit == EmitKind::Plan
                     ? ".psxp"
                     : ".ir" + std::to_string(last) + ".psir");
}
}  // namespace ps::tools::psc
