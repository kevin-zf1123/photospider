#pragma once

#include <filesystem>

#include "photospider/execution/execution.hpp"

namespace ps::tools {
/** @brief Write a complete CPU image through the psx results-v2 exporter.
 * @param image Sealed Result with one physical span per selected tile.
 * @param tile_extent Positive power of two defining the source grid.
 * @param directory New destination directory published after all writes.
 * @param budget Root for temporary storage, coverage and copying.
 * @return Success, or a resource, validation or filesystem failure.
 * The caller retains the Result until return. Concurrent exports require
 * distinct destinations. The file reference is directory/output-0.bin and
 * its schema and tile layout are recorded in directory/manifest.json.
 */
Status export_tiled_image(const ResultRef& image, std::uint64_t tile_extent,
                          const std::filesystem::path& directory,
                          const ResourceBudget& budget);
/** @brief Publish the selected named outputs of a completed Plan in PSXTILE2
 * format.
 * @param plan Exact Plan that produced results, including sparse selections.
 * @param results Successful execution whose owners are borrowed through return.
 * @param directory New directory for manifest.json and the named output files.
 * @param budget Root for bounded export scratch. Separate destinations are
 * safe for concurrent callers. Failure removes the unpublished staging tree.
 */
Status export_job_results(const ExecutionPlan& plan,
                          const ExecutionResult& results,
                          const std::filesystem::path& directory,
                          const ResourceBudget& budget);
}  // namespace ps::tools
