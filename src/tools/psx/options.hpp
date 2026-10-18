#pragma once
#include <string>

#include "common/options.hpp"
#include "photospider/execution/execution.hpp"
namespace ps::tools::psx {
struct Invocation final : CommonOptions {
  ExecutionContextConfig context;
  ExecutionOptions execution;
};
Invocation parse_options(int argc, char** argv, bool* json_diagnostics);
std::string output_path(const Invocation& options);
}  // namespace ps::tools::psx
