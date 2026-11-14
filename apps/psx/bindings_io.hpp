#pragma once
#include <string>

#include "common/file_io.hpp"
#include "photospider/execution/execution.hpp"
namespace ps::tools::psx {
ExecutionBindings read_bindings(const std::string& manifest,
                                const ExecutionPlan& plan, Files* files,
                                const ResourceBudget& root);
}  // namespace ps::tools::psx
