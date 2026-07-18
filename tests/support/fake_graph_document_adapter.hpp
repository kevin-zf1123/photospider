#pragma once

/**
 * @file fake_graph_document_adapter.hpp
 * @brief Defines a deterministic format-neutral graph-document test fake.
 */

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "graph/graph_document_reader.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_document_writer.hpp"  // NOLINT(build/include_subdir)

namespace ps::testing {

/**
 * @class FakeGraphDocumentAdapter
 * @brief Records graph-document calls and delegates detached-value callbacks.
 *
 * Callback selection and observation publication are protected by one mutex.
 * Each operation copies its configured callback and records its complete input
 * before invoking test code without the mutex held. Tests can therefore block,
 * re-enter their own synchronization, or throw deterministically without
 * deadlocking observation reads.
 *
 * @note One shared instance may be retained through both immutable contract
 *       bases and called by independent graph-state lanes. Callback mutation
 *       should finish before concurrent product operations begin.
 */
class FakeGraphDocumentAdapter final : public GraphDocumentReader,
                                       public GraphDocumentWriter {
 public:
  /** @brief Complete-graph read callback signature. */
  using ReadCallback =
      std::function<GraphDefinition(const std::filesystem::path&)>;

  /** @brief Single-node document read callback signature. */
  using ReadNodeCallback = std::function<NodeDefinition(const std::string&)>;

  /** @brief Complete-graph write callback signature. */
  using WriteCallback =
      std::function<void(const std::filesystem::path&, const GraphDefinition&)>;

  /** @brief Single-node document write callback signature. */
  using WriteNodeCallback = std::function<std::string(const NodeDefinition&)>;

  /**
   * @brief Operation kind attached to one ordered observation.
   * @throws Nothing for value construction and comparison.
   */
  enum class Operation {
    /** @brief Complete graph was requested from a source path. */
    Read,
    /** @brief Single node was requested from document text. */
    ReadNode,
    /** @brief Complete graph was submitted to a destination path. */
    Write,
    /** @brief Single node was submitted for document emission. */
    WriteNode,
  };

  /**
   * @brief Deep-owned input snapshot for one observed contract call.
   *
   * @throws std::bad_alloc if copied paths, text, or definitions allocate.
   * @note Only fields relevant to `operation` are populated. No caller-owned
   *       references escape the contract call.
   */
  struct Observation {
    /** @brief Contract operation that published this observation. */
    Operation operation = Operation::Read;

    /** @brief Source/destination path for complete-graph operations. */
    std::filesystem::path path;

    /** @brief Borrowed text copied for `ReadNode`. */
    std::string document_text;

    /** @brief Complete detached definition copied for `Write`. */
    std::optional<GraphDefinition> graph_definition;

    /** @brief Detached definition copied for `WriteNode`. */
    std::optional<NodeDefinition> node_definition;
  };

  /**
   * @brief Replaces the callback used by complete-graph reads.
   *
   * @param callback Callable copied into fake ownership; empty disables reads.
   * @return Nothing.
   * @throws std::bad_alloc if callback storage allocation fails.
   * @throws std::system_error if callback synchronization fails.
   * @note Existing in-flight calls retain their already copied callback.
   */
  void set_read_callback(ReadCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    read_callback_ = std::move(callback);
  }

  /**
   * @brief Replaces the callback used by single-node reads.
   *
   * @param callback Callable copied into fake ownership; empty disables reads.
   * @return Nothing.
   * @throws std::bad_alloc if callback storage allocation fails.
   * @throws std::system_error if callback synchronization fails.
   * @note Existing in-flight calls retain their already copied callback.
   */
  void set_read_node_callback(ReadNodeCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    read_node_callback_ = std::move(callback);
  }

  /**
   * @brief Replaces the callback used by complete-graph writes.
   *
   * @param callback Callable copied into fake ownership; empty disables writes.
   * @return Nothing.
   * @throws std::bad_alloc if callback storage allocation fails.
   * @throws std::system_error if callback synchronization fails.
   * @note Existing in-flight calls retain their already copied callback.
   */
  void set_write_callback(WriteCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    write_callback_ = std::move(callback);
  }

  /**
   * @brief Replaces the callback used by single-node writes.
   *
   * @param callback Callable copied into fake ownership; empty disables writes.
   * @return Nothing.
   * @throws std::bad_alloc if callback storage allocation fails.
   * @throws std::system_error if callback synchronization fails.
   * @note Existing in-flight calls retain their already copied callback.
   */
  void set_write_node_callback(WriteNodeCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    write_node_callback_ = std::move(callback);
  }

  /**
   * @brief Copies all observations in mutex publication order.
   *
   * @return Deep-owned ordered observation snapshot.
   * @throws std::bad_alloc if snapshot copying allocates and fails.
   * @throws std::system_error if observation synchronization fails.
   * @note The returned vector is independent of subsequent fake operations.
   */
  std::vector<Observation> observations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return observations_;
  }

  /**
   * @brief Removes every recorded observation.
   *
   * @return Nothing.
   * @throws std::system_error if observation synchronization fails.
   * @note Configured callbacks and in-flight callback copies remain unchanged.
   */
  void clear_observations() {
    std::lock_guard<std::mutex> lock(mutex_);
    observations_.clear();
  }

  /** @copydoc GraphDocumentReader::read */
  GraphDefinition read(const std::filesystem::path& path) const override {
    ReadCallback callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      observations_.push_back(
          Observation{Operation::Read, path, {}, std::nullopt, std::nullopt});
      callback = read_callback_;
    }
    if (!callback) {
      throw std::logic_error("Fake graph document read callback is not set.");
    }
    return callback(path);
  }

  /** @copydoc GraphDocumentReader::read_node */
  NodeDefinition read_node(const std::string& document_text) const override {
    ReadNodeCallback callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      observations_.push_back(Observation{Operation::ReadNode,
                                          {},
                                          document_text,
                                          std::nullopt,
                                          std::nullopt});
      callback = read_node_callback_;
    }
    if (!callback) {
      throw std::logic_error(
          "Fake graph node document read callback is not set.");
    }
    return callback(document_text);
  }

  /** @copydoc GraphDocumentWriter::write */
  void write(const std::filesystem::path& path,
             const GraphDefinition& definition) const override {
    WriteCallback callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      observations_.push_back(
          Observation{Operation::Write, path, {}, definition, std::nullopt});
      callback = write_callback_;
    }
    if (!callback) {
      throw std::logic_error("Fake graph document write callback is not set.");
    }
    callback(path, definition);
  }

  /** @copydoc GraphDocumentWriter::write_node */
  std::string write_node(const NodeDefinition& definition) const override {
    WriteNodeCallback callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      observations_.push_back(
          Observation{Operation::WriteNode, {}, {}, std::nullopt, definition});
      callback = write_node_callback_;
    }
    if (!callback) {
      throw std::logic_error(
          "Fake graph node document write callback is not set.");
    }
    return callback(definition);
  }

 private:
  /** @brief Serializes callback selection and observation publication. */
  mutable std::mutex mutex_;

  /** @brief Callback copied for each complete-graph read. */
  ReadCallback read_callback_;

  /** @brief Callback copied for each single-node read. */
  ReadNodeCallback read_node_callback_;

  /** @brief Callback copied for each complete-graph write. */
  WriteCallback write_callback_;

  /** @brief Callback copied for each single-node write. */
  WriteNodeCallback write_node_callback_;

  /**
   * @brief Deep-owned inputs in mutex acquisition order.
   * @note Mutable because immutable contract calls publish test observations.
   */
  mutable std::vector<Observation> observations_;
};

}  // namespace ps::testing
