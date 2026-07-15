#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>

namespace ps::testing {

/**
 * @brief Save-stream checkpoint available only to GraphIOService tests.
 *
 * @throws Nothing for value construction and comparison.
 * @note Production builds do not compile this test-access contract.
 */
enum class GraphIoSaveFailureStage {
  /** @brief Injects a recoverable failure before destination open. */
  BeforeDestinationOpen,
  /** @brief Throws resource exhaustion before destination open. */
  BeforeDestinationOpenBadAlloc,
  /** @brief Injects a bad stream state after YAML write completes. */
  AfterWrite,
  /** @brief Injects a bad stream state after destination flush completes. */
  AfterFlush,
  /** @brief Injects a failed stream state after destination close completes. */
  AfterClose,
};

/**
 * @brief Arms one destination-scoped save-stream failure checkpoint.
 *
 * @param yaml_path Exact destination path that may consume the checkpoint.
 * @param stage Checkpoint whose real stream operation must complete first.
 * @return Nothing.
 * @throws std::bad_alloc If copying the destination path exhausts memory.
 * @throws std::system_error If checkpoint synchronization fails.
 * @note One process-local plan exists at a time. It is consumed at most once
 *       and only by an exact destination-path and stage match.
 */
void arm_graph_io_save_failure(const std::filesystem::path& yaml_path,
                               GraphIoSaveFailureStage stage);

/**
 * @brief Clears the process-local save-stream failure checkpoint.
 *
 * @return Nothing.
 * @throws std::system_error If checkpoint synchronization fails.
 * @note Clearing also resets the observable hit count to zero. Callers must
 *       invoke this only while no save is expected to consume the plan.
 */
void clear_graph_io_save_failure();

/**
 * @brief Returns how many times the current checkpoint was reached.
 *
 * @return Zero before a hit or one after the armed one-shot checkpoint fires.
 * @throws std::system_error If checkpoint synchronization fails.
 * @note The count belongs to the current process-local plan lifecycle and is
 *       reset whenever callers arm or clear the checkpoint.
 */
std::size_t graph_io_save_failure_hit_count();

/**
 * @brief Applies an armed failure at one real save-stream boundary.
 *
 * @param stream Real destination stream whose state is marked failed.
 * @param yaml_path Exact destination path owned by the active save.
 * @param stage Pre-open boundary or output stage that has just completed.
 * @return Nothing.
 * @throws std::bad_alloc If the exact pre-open resource checkpoint matches.
 * @throws std::ios_base::failure only if the stream already has an exception
 *         mask that requests throwing for the injected state.
 * @throws std::system_error If checkpoint synchronization fails.
 * @note Recoverable stages change stream state without replacing its buffer.
 *       At the pre-open boundary the caller checks that state before opening,
 *       so an existing destination is untouched. The resource stage throws
 *       only after atomically consuming the exact one-shot plan.
 */
void inject_graph_io_save_failure(std::ios& stream,
                                  const std::filesystem::path& yaml_path,
                                  GraphIoSaveFailureStage stage);

}  // namespace ps::testing
