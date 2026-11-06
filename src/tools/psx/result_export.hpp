#pragma once
#include "common/file_io.hpp"
#include "photospider/execution/execution.hpp"
namespace ps::tools::psx {
inline constexpr std::uint64_t kDefaultExportBufferBytes = 8 * 1024 * 1024;
void export_results(const ExecutionPlan& plan, const ExecutionResult& results,
                    const fs::path& directory, const Files& files,
                    const CancellationToken& cancellation,
                    const ResourceBudget& budget,
                    std::uint64_t buffer_limit = kDefaultExportBufferBytes);
}  // namespace ps::tools::psx
