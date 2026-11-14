#include "psx/options.hpp"

#include <string>
#include <utility>

#include "common/file_io.hpp"

namespace ps::tools::psx {
Invocation parse_options(int argc, char** argv, bool* json_diagnostics) {
  auto parsed = parse_arguments(
      argc, argv, json_diagnostics, {},
      {"--bindings", "--threads", "--parallelism", "--max-live-bytes",
       "--max-work", "--max-stages", "--export-buffer-bytes", "--executor"},
      {"--load-executor"});
  Invocation o;
  static_cast<CommonOptions&>(o) = std::move(parsed.common);
  for (const auto& item : parsed.repeated)
    o.executor_modules.push_back(item.second);
  require_input(
      o, o.has("--help") || o.has("--version") || o.has("--list-operations"));
  if (o.has("--check") && o.has("-o"))
    throw ToolError("--check conflicts with output options", 2);
  if (o.output == "-")
    throw ToolError("psx requires a result directory", 2);
  o.context.cpu_workers = positive_count(o, "--threads", 0);
  o.execution.maximum_parallelism = positive_count(o, "--parallelism", 0);
  if (o.context.cpu_workers > 4096 || o.execution.maximum_parallelism > 4096)
    throw ToolError("--threads and --parallelism must be in [1,4096]", 2);
  if (o.has("--max-live-bytes"))
    o.context.maximum_live_bytes =
        unsigned_number(o.get("--max-live-bytes"), "--max-live-bytes");
  if (!o.context.maximum_live_bytes)
    throw ToolError("--max-live-bytes must be positive", 2);
  if (o.has("--export-buffer-bytes")) {
    o.export_buffer_bytes = unsigned_number(o.get("--export-buffer-bytes"),
                                            "--export-buffer-bytes");
    if (!o.export_buffer_bytes)
      throw ToolError("--export-buffer-bytes must be positive", 2);
  }
  if (o.has("--max-work")) {
    o.execution.maximum_work =
        unsigned_number(o.get("--max-work"), "--max-work");
    if (!o.execution.maximum_work)
      throw ToolError("--max-work must be positive", 2);
  }
  if (o.has("--max-stages"))
    o.execution.maximum_refinement_stages =
        unsigned_number(o.get("--max-stages"), "--max-stages");
  if (o.has("--outputs"))
    output_names(o.get("--outputs"));
  return o;
}
std::string output_path(const Invocation& o) {
  if (!o.output.empty())
    return o.output;
  if (o.input == "-")
    throw ToolError("stdin requires explicit -o", 2);
  return fs::path(o.input).stem().string() + ".results";
}
}  // namespace ps::tools::psx
