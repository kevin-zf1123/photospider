#pragma once
#include <string>
#include <vector>

#include "common/options.hpp"
#include "photospider/execution/execution.hpp"
#include "psx/result_export.hpp"
namespace ps::tools::psx {
struct Invocation final : CommonOptions {
  std::vector<std::string> executor_modules;
  ExecutionContextConfig context;
  ExecutionOptions execution;
  std::uint64_t export_buffer_bytes = kDefaultExportBufferBytes;
};
Invocation parse_options(int argc, char** argv, bool* json_diagnostics);
std::string output_path(const Invocation& options);
}  // namespace ps::tools::psx
