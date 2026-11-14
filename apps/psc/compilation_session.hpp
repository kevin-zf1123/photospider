#pragma once
#include "common/diagnostics.hpp"
#include "common/file_io.hpp"
#include "psc/options.hpp"
namespace ps::tools::psc {
int compile(const Invocation& options, const Compiler& compiler, Files* files,
            Diagnostics* diagnostics);
void print_pipeline(const Invocation& options,
                    const OptimizerRegistry& registry,
                    const PlannerRegistry& planners, std::size_t last,
                    Files* files);
}  // namespace ps::tools::psc
