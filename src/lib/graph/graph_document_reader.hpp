#pragma once

/**
 * @file graph_document_reader.hpp
 * @brief Declares format-neutral graph-document read operations.
 */

#include <filesystem>
#include <string>

#include "graph/graph_definition.hpp"  // NOLINT(build/include_subdir)

namespace ps {

/**
 * @class GraphDocumentReader
 * @brief Reads detached graph definitions without owning graph publication.
 *
 * Implementations translate a selected external representation into
 * deep-owned `GraphDefinition` and `NodeDefinition` values. They own format
 * parsing and source access, but never receive `GraphModel`, publish topology,
 * retain parser aliases, or own session lifecycle policy.
 *
 * @note Callers retain implementations through shared ownership for the
 *       complete service lifetime. Implementations must support independent
 *       const calls from graph-state lanes belonging to different graphs.
 */
class GraphDocumentReader {
 public:
  /**
   * @brief Destroys one reader after every retaining service releases it.
   * @throws Nothing.
   * @note Destruction must not publish graph or session state.
   */
  virtual ~GraphDocumentReader() = default;

  /**
   * @brief Reads one complete detached graph definition.
   *
   * @param path Source path selected by graph-session lifecycle policy.
   * @return Deep-owned complete graph definition.
   * @throws GraphError with the stable graph-document category selected by the
   *         implementation for recoverable source or representation failure.
   * @throws std::bad_alloc when path, parser, diagnostic, or detached-value
   *         allocation is exhausted.
   * @throws std::exception for implementation failures not represented by a
   *         graph-domain category.
   * @note The returned definition must remain valid after parser temporaries
   *       and the reader call are destroyed. The reader retains no reference to
   *       the path or returned definition.
   */
  virtual GraphDefinition read(const std::filesystem::path& path) const = 0;

  /**
   * @brief Parses one detached node definition from owned document text.
   *
   * @param document_text Representation text borrowed for this call.
   * @return Deep-owned persistent node definition.
   * @throws GraphError with the implementation's stable document category for
   *         recoverable representation or schema failure.
   * @throws std::bad_alloc when parser, diagnostic, or detached-value
   *         allocation is exhausted.
   * @throws std::exception for implementation failures not represented by a
   *         graph-domain category.
   * @note Node identity override and topology publication remain caller-owned.
   *       The reader retains no view into `document_text`.
   */
  virtual NodeDefinition read_node(const std::string& document_text) const = 0;
};

}  // namespace ps
