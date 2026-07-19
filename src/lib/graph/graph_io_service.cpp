#include "graph/graph_io_service.hpp"

/**
 * @file graph_io_service.cpp
 * @brief Implements format-neutral graph-document/model orchestration.
 */

#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "graph/in_memory_graph_document_adapter.hpp"  // NOLINT(build/include_subdir)

namespace ps {
namespace {

/**
 * @brief Applies a detached definition with stable document-schema mapping.
 *
 * @param graph Graph replaced only after complete staging and validation.
 * @param definition Detached graph document to apply.
 * @param document_path Source path used only for diagnostic context.
 * @return Nothing.
 * @throws std::bad_alloc if staging, topology preparation, or diagnostics
 *         cannot allocate.
 * @throws GraphError with `GraphErrc::InvalidYaml` for detached-definition
 *         schema rejection, `GraphErrc::MissingDependency` or
 *         `GraphErrc::Cycle` for topology rejection, and
 *         `GraphErrc::Unknown` for unexpected apply failures.
 * @note `InMemoryGraphDocumentAdapter` invokes `GraphModel::replace_nodes()`
 *       exactly once after every `Node` has been staged. `InvalidYaml` remains
 *       the stable public representation category although this service is
 *       format-neutral.
 */
void apply_graph_document(GraphModel& graph, const GraphDefinition& definition,
                          const std::filesystem::path& document_path) {
  try {
    InMemoryGraphDocumentAdapter adapter;
    adapter.apply(graph, definition);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& error) {
    if (error.code() != GraphErrc::InvalidParameter) {
      throw;
    }
    throw GraphError(GraphErrc::InvalidYaml,
                     "Invalid graph document schema in " +
                         document_path.string() + ": " + error.what());
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Unknown,
                     "Unexpected graph document apply failure for " +
                         document_path.string() + ": " + error.what());
  } catch (...) {
    throw GraphError(
        GraphErrc::Unknown,
        "Unknown graph document apply failure for " + document_path.string());
  }
}

}  // namespace

/** @copydoc GraphIOService::GraphIOService */
GraphIOService::GraphIOService(
    std::shared_ptr<const GraphDocumentReader> reader,
    std::shared_ptr<const GraphDocumentWriter> writer)
    : reader_(std::move(reader)), writer_(std::move(writer)) {
  if (!reader_ || !writer_) {
    throw std::invalid_argument(
        "Graph document reader and writer must not be null.");
  }
}

/** @copydoc GraphIOService::load */
void GraphIOService::load(GraphModel& graph,
                          const std::filesystem::path& document_path) const {
  if (document_path.empty()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph document source path must not be empty.");
  }
  const GraphDefinition definition = reader_->read(document_path);
  apply_graph_document(graph, definition, document_path);
}

/** @copydoc GraphIOService::save */
void GraphIOService::save(const GraphModel& graph,
                          const std::filesystem::path& document_path) const {
  const InMemoryGraphDocumentAdapter adapter;
  writer_->write(document_path, adapter.capture(graph));
}

/** @copydoc GraphIOService::read_node_document */
NodeDefinition GraphIOService::read_node_document(
    const std::string& document_text) const {
  return reader_->read_node(document_text);
}

/** @copydoc GraphIOService::write_node_document */
std::string GraphIOService::write_node_document(
    const NodeDefinition& definition) const {
  return writer_->write_node(definition);
}

}  // namespace ps
