#pragma once

/**
 * @file graph_document_writer.hpp
 * @brief Declares format-neutral graph-document write operations.
 */

#include <filesystem>
#include <string>

#include "graph/graph_definition.hpp"  // NOLINT(build/include_subdir)

namespace ps {

/**
 * @class GraphDocumentWriter
 * @brief Emits detached graph definitions without owning graph capture.
 *
 * Implementations translate deep-owned `GraphDefinition` and `NodeDefinition`
 * values into a selected external representation. They own format emission and
 * destination access, but never receive `GraphModel`, mutate graph/session
 * state, or retain aliases to caller-owned definitions.
 *
 * @note Callers retain implementations through shared ownership for the
 *       complete service lifetime. Implementations must support independent
 *       const calls from graph-state lanes belonging to different graphs.
 */
class GraphDocumentWriter {
 public:
  /**
   * @brief Destroys one writer after every retaining service releases it.
   * @throws Nothing.
   * @note Destruction must not publish graph or session state.
   */
  virtual ~GraphDocumentWriter() = default;

  /**
   * @brief Writes one complete detached graph definition.
   *
   * @param path Destination selected by the graph-document caller.
   * @param definition Complete persistent definition borrowed for this call.
   * @return Nothing.
   * @throws GraphError with the stable graph-document category selected by the
   *         implementation for recoverable emission or destination failure.
   * @throws std::bad_alloc when path, emitter, diagnostic, or destination
   *         storage allocation is exhausted.
   * @throws std::exception for implementation failures not represented by a
   *         graph-domain category.
   * @note Parent-directory creation, replacement atomicity, and rollback are
   *       implementation contracts. The writer retains no reference to either
   *       argument after return.
   */
  virtual void write(const std::filesystem::path& path,
                     const GraphDefinition& definition) const = 0;

  /**
   * @brief Emits one detached node definition as owned document text.
   *
   * @param definition Persistent node definition borrowed for this call.
   * @return Owned representation text independent of emitter temporaries.
   * @throws GraphError with the implementation's stable document category for
   *         recoverable representation failure.
   * @throws std::bad_alloc when emitter, diagnostic, or output allocation is
   *         exhausted.
   * @throws std::exception for implementation failures not represented by a
   *         graph-domain category.
   * @note The writer retains no reference to `definition`.
   */
  virtual std::string write_node(const NodeDefinition& definition) const = 0;
};

}  // namespace ps
