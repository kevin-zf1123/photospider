#pragma once

/**
 * @file yaml_graph_document_adapter.hpp
 * @brief Declares the configured YAML graph-document filesystem adapter.
 */

#include <string>

#include "graph/graph_document_reader.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_document_writer.hpp"  // NOLINT(build/include_subdir)

namespace ps::adapters::yaml {

/**
 * @class YamlGraphDocumentAdapter
 * @brief Translates YAML files/text and detached graph definitions.
 *
 * This adapter owns graph-document yaml-cpp parsing/emission and direct
 * filesystem destination writes. It owns no graph, topology, session,
 * graph-state lane, cache policy, or path-selection/copy policy. Returned
 * definitions and text are deep-owned and retain no yaml-cpp aliases.
 *
 * @note One immutable instance may be shared as both the
 *       `GraphDocumentReader` and `GraphDocumentWriter` across independent
 *       graph-state lanes. All production operations use call-local parser and
 *       stream state; BUILD_TESTING-only failure hooks synchronize their own
 *       process-local observation state.
 */
class YamlGraphDocumentAdapter final : public GraphDocumentReader,
                                       public GraphDocumentWriter {
 public:
  /**
   * @brief Reads one complete YAML graph document from a filesystem path.
   *
   * @param path Source path selected by graph-session lifecycle policy.
   * @return Detached deep-owned graph definition.
   * @throws GraphError with `GraphErrc::InvalidParameter` for an empty path,
   *         `GraphErrc::Io` for an inaccessible source,
   *         `GraphErrc::InvalidYaml` for parser/root/node-schema rejection,
   *         or `GraphErrc::Unknown` for unexpected non-resource failure.
   * @throws std::bad_alloc when parser, detached-value, path, or diagnostic
   *         allocation is exhausted.
   * @note Duplicate identifiers and topology validation remain owned by
   *       `GraphIOService` and `InMemoryGraphDocumentAdapter`.
   */
  GraphDefinition read(const std::filesystem::path& path) const override;

  /**
   * @brief Parses one YAML node mapping into a detached node definition.
   *
   * @param document_text YAML mapping text borrowed for this call.
   * @return Detached deep-owned node definition.
   * @throws GraphError with `GraphErrc::InvalidYaml` for parsing,
   *         representation, or node-schema rejection.
   * @throws std::bad_alloc when parser, detached-value, or diagnostic
   *         allocation is exhausted.
   * @note Node identity override and topology publication remain caller-owned.
   */
  NodeDefinition read_node(const std::string& document_text) const override;

  /**
   * @brief Emits and directly writes one complete YAML graph document.
   *
   * @param path Destination path supplied by the graph-document caller.
   * @param definition Detached definition captured before this call.
   * @return Nothing.
   * @throws GraphError with `GraphErrc::Io` for recoverable YAML emission,
   *         destination preparation/open, write, flush, or close failure.
   * @throws std::bad_alloc when YAML, path, stream, or diagnostic allocation is
   *         exhausted.
   * @note YAML emission completes before destination open. The adapter creates
   *       no parent directory and writes directly rather than atomically
   *       replacing the destination. Pre-open failure preserves existing
   *       bytes; post-open failure may leave created, truncated, or partial
   *       output.
   */
  void write(const std::filesystem::path& path,
             const GraphDefinition& definition) const override;

  /**
   * @brief Emits one detached node definition as owned YAML text.
   *
   * @param definition Persistent node fields borrowed for this call.
   * @return Owned YAML mapping text.
   * @throws GraphError with `GraphErrc::InvalidYaml` for recoverable YAML
   *         emission failure.
   * @throws std::bad_alloc when YAML or output string allocation is exhausted.
   * @note Runtime state cannot be emitted because `NodeDefinition` contains
   *       none.
   */
  std::string write_node(const NodeDefinition& definition) const override;
};

}  // namespace ps::adapters::yaml
