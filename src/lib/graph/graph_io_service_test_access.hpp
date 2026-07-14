#pragma once

#include <cstddef>
#include <iosfwd>

namespace ps::testing {

/**
 * @brief Save-stream checkpoint available only to GraphIOService tests.
 *
 * @throws Nothing for value construction and comparison.
 * @note Production builds do not compile this test-access contract.
 */
enum class GraphIoSaveFailureStage {
  /** @brief Injects a bad stream state after YAML write completes. */
  AfterWrite,
  /** @brief Injects a bad stream state after destination flush completes. */
  AfterFlush,
  /** @brief Injects a failed stream state after destination close completes. */
  AfterClose,
};

/**
 * @brief Arms one thread-local save-stream failure checkpoint.
 *
 * @param stage Checkpoint whose real stream operation must complete first.
 * @return Nothing.
 * @throws Nothing.
 * @note The checkpoint is consumed at most once on the calling thread.
 */
void arm_graph_io_save_failure(GraphIoSaveFailureStage stage) noexcept;

/**
 * @brief Clears the calling thread's save-stream failure checkpoint.
 *
 * @return Nothing.
 * @throws Nothing.
 */
void clear_graph_io_save_failure() noexcept;

/**
 * @brief Returns how many times the current checkpoint was reached.
 *
 * @return Zero before a hit or one after the armed one-shot checkpoint fires.
 * @throws Nothing.
 */
std::size_t graph_io_save_failure_hit_count() noexcept;

/**
 * @brief Applies an armed failure after one real save-stream stage.
 *
 * @param stream Real destination stream whose state is marked failed.
 * @param stage Stage that has just completed.
 * @return Nothing.
 * @throws std::ios_base::failure only if the stream already has an exception
 * mask that requests throwing for the injected state.
 * @note The helper changes stream state only; it never throws directly or
 * replaces the stream buffer.
 */
void inject_graph_io_save_failure(std::ios& stream,
                                  GraphIoSaveFailureStage stage);

}  // namespace ps::testing
