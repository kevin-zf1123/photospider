#pragma once
#include "common/file_io.hpp"
#include "photospider/execution/execution.hpp"
namespace ps::tools::psx {
void export_results(const ExecutionResult& results, const fs::path& directory,
                    const Files& files, const CancellationToken& cancellation,
                    const ResourceBudget& budget);
}  // namespace ps::tools::psx
