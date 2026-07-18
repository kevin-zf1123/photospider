#pragma once

/**
 * @file yaml_graph_document_adapter_test_access.hpp
 * @brief Declares BUILD_TESTING-only YAML writer stream checkpoints.
 */

#include <cstddef>
#include <filesystem>
#include <iosfwd>

namespace ps::testing {

/**
 * @brief Save-stream checkpoint available only to YAML adapter tests.
 *
 * @throws Nothing for value construction and comparison.
 * @note Production builds do not compile this test-access contract.
 */
enum class YamlGraphDocumentSaveFailureStage {
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
 * @brief Arms one destination-scoped YAML save-stream checkpoint.
 *
 * @param document_path Exact destination path that may consume the checkpoint.
 * @param stage Checkpoint whose real stream operation must complete first.
 * @return Nothing.
 * @throws std::bad_alloc if copying the destination path exhausts memory.
 * @throws std::system_error if checkpoint synchronization fails.
 * @note One process-local plan exists at a time. It is consumed at most once
 *       and only by an exact destination-path and stage match.
 */
void arm_yaml_graph_document_save_failure(
    const std::filesystem::path& document_path,
    YamlGraphDocumentSaveFailureStage stage);

/**
 * @brief Clears the process-local YAML save-stream failure checkpoint.
 *
 * @return Nothing.
 * @throws std::system_error if checkpoint synchronization fails.
 * @note Clearing also resets the observable hit count to zero. Callers invoke
 *       this only while no writer is expected to consume the plan.
 */
void clear_yaml_graph_document_save_failure();

/**
 * @brief Returns how many times the current YAML checkpoint was reached.
 *
 * @return Zero before a hit or one after the armed one-shot checkpoint fires.
 * @throws std::system_error if checkpoint synchronization fails.
 * @note The count resets whenever callers arm or clear the checkpoint.
 */
std::size_t yaml_graph_document_save_failure_hit_count();

/**
 * @brief Applies an armed failure at one real YAML save-stream boundary.
 *
 * @param stream Real destination stream whose state is marked failed.
 * @param document_path Exact destination path owned by the active save.
 * @param stage Pre-open boundary or output stage that has just completed.
 * @return Nothing.
 * @throws std::bad_alloc if the exact pre-open resource checkpoint matches.
 * @throws std::ios_base::failure only when the stream exception mask requests
 *         throwing for the injected state.
 * @throws std::system_error if checkpoint synchronization fails.
 * @note Recoverable stages mutate the real stream state. The pre-open caller
 *       checks that state before opening, while the resource stage throws only
 *       after atomically consuming the one-shot plan.
 */
void inject_yaml_graph_document_save_failure(
    std::ios& stream, const std::filesystem::path& document_path,
    YamlGraphDocumentSaveFailureStage stage);

}  // namespace ps::testing
