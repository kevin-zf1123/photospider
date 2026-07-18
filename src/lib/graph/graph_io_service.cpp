#include "graph/graph_io_service.hpp"

#include <yaml-cpp/yaml.h>

#include <exception>
#include <fstream>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "graph/graph_definition_yaml.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"            // NOLINT(build/include_subdir)
#include "graph/in_memory_graph_document_adapter.hpp"  // NOLINT(build/include_subdir)
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
#include "graph/graph_io_service_test_access.hpp"  // NOLINT(build/include_subdir)
#endif

namespace ps {

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
namespace testing {
namespace {

/**
 * @brief Exact private save-failure plan visible to graph-state workers.
 *
 * @note The process-local plan is protected by g_save_failure_mutex and is
 *       consumed only by an exact destination-path and stage match. It owns a
 *       copied path so the arming caller need not outlive the worker call.
 */
struct GraphIoSaveFailurePlan {
  /** @brief Exact destination path allowed to consume this plan. */
  std::filesystem::path yaml_path;
  /** @brief Exact save boundary allowed to consume this plan. */
  GraphIoSaveFailureStage stage;
};

/**
 * @brief Serializes process-local save-failure plan access.
 * @throws Nothing for static initialization.
 * @note The mutex lives for the process lifetime and is never held while a
 *       stream is mutated or an injected exception is thrown.
 */
std::mutex g_save_failure_mutex;

/**
 * @brief Armed destination-scoped failure plan, or no active checkpoint.
 * @throws Nothing for empty optional initialization.
 * @note Every read, replacement, and reset is protected by
 *       g_save_failure_mutex.
 */
std::optional<GraphIoSaveFailurePlan> g_save_failure_plan;

/**
 * @brief Process-local count proving the armed checkpoint was reached.
 * @throws Nothing for initialization and mutex-protected scalar updates.
 * @note Every access is protected by g_save_failure_mutex. The one-shot
 *       checkpoint limits this value to zero or one per arm.
 */
std::size_t g_save_failure_hit_count = 0;

}  // namespace

/** @copydoc ps::testing::arm_graph_io_save_failure */
void arm_graph_io_save_failure(const std::filesystem::path& yaml_path,
                               GraphIoSaveFailureStage stage) {
  GraphIoSaveFailurePlan plan{yaml_path, stage};
  std::lock_guard<std::mutex> lock(g_save_failure_mutex);
  g_save_failure_plan = std::move(plan);
  g_save_failure_hit_count = 0;
}

/** @copydoc ps::testing::clear_graph_io_save_failure */
void clear_graph_io_save_failure() {
  std::lock_guard<std::mutex> lock(g_save_failure_mutex);
  g_save_failure_plan.reset();
  g_save_failure_hit_count = 0;
}

/** @copydoc ps::testing::graph_io_save_failure_hit_count */
std::size_t graph_io_save_failure_hit_count() {
  std::lock_guard<std::mutex> lock(g_save_failure_mutex);
  return g_save_failure_hit_count;
}

/** @copydoc ps::testing::inject_graph_io_save_failure */
void inject_graph_io_save_failure(std::ios& stream,
                                  const std::filesystem::path& yaml_path,
                                  GraphIoSaveFailureStage stage) {
  {
    std::lock_guard<std::mutex> lock(g_save_failure_mutex);
    if (!g_save_failure_plan || g_save_failure_plan->stage != stage ||
        g_save_failure_plan->yaml_path != yaml_path) {
      return;
    }
    g_save_failure_plan.reset();
    ++g_save_failure_hit_count;
  }
  if (stage == GraphIoSaveFailureStage::BeforeDestinationOpenBadAlloc) {
    throw std::bad_alloc{};
  }
  stream.setstate(stage == GraphIoSaveFailureStage::AfterClose
                      ? std::ios::failbit
                      : std::ios::badbit);
}

}  // namespace testing
#endif

namespace {

/**
 * @brief Converts one parsed YAML root into a detached graph definition.
 *
 * @param yaml_root Parsed graph-document representation owned by yaml-cpp.
 * @param yaml_path Source path used only for diagnostic context.
 * @return Fully owned graph definition.
 * @throws std::bad_alloc If conversion or diagnostic allocation exhausts
 *         memory.
 * @throws GraphError with `GraphErrc::InvalidYaml` for YAML conversion and
 *         definition-schema rejection.
 * @throws GraphError with the original category for explicit non-schema graph
 *         failures.
 * @throws GraphError with `GraphErrc::Unknown` for unexpected conversion
 *         failures.
 * @note This boundary deliberately translates definition translator
 *       InvalidParameter details into the public document category.
 */
GraphDefinition convert_graph_document(const YAML::Node& yaml_root,
                                       const std::filesystem::path& yaml_path) {
  try {
    return graph_definition_from_yaml(yaml_root);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const YAML::Exception& error) {
    throw GraphError(GraphErrc::InvalidYaml,
                     "Invalid graph definition in YAML file " +
                         yaml_path.string() + ": " + error.what());
  } catch (const GraphError& error) {
    if (error.code() != GraphErrc::InvalidParameter) {
      throw;
    }
    throw GraphError(GraphErrc::InvalidYaml,
                     "Invalid graph schema in YAML file " + yaml_path.string() +
                         ": " + error.what());
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Unknown,
                     "Unexpected graph conversion failure in YAML file " +
                         yaml_path.string() + ": " + error.what());
  } catch (...) {
    throw GraphError(
        GraphErrc::Unknown,
        "Unknown graph conversion failure in YAML file " + yaml_path.string());
  }
}

/**
 * @brief Applies a detached definition with product-path schema translation.
 *
 * @param graph Graph replaced only after complete staging and validation.
 * @param definition Detached graph document to apply.
 * @param yaml_path Source path used only for diagnostic context.
 * @return Nothing.
 * @throws std::bad_alloc if staging, topology preparation, or diagnostics
 *         cannot allocate.
 * @throws GraphError with `GraphErrc::InvalidYaml` for definition schema
 *         rejection, `GraphErrc::MissingDependency` or `GraphErrc::Cycle` for
 *         topology rejection, and `GraphErrc::Unknown` for unexpected apply
 *         failures.
 * @note InMemoryGraphDocumentAdapter invokes GraphModel::replace_nodes()
 *       exactly once, after every Node has been staged.
 */
void apply_graph_document(GraphModel& graph, const GraphDefinition& definition,
                          const std::filesystem::path& yaml_path) {
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
                     "Invalid graph schema in YAML file " + yaml_path.string() +
                         ": " + error.what());
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Unknown,
                     "Unexpected graph apply failure for YAML file " +
                         yaml_path.string() + ": " + error.what());
  } catch (...) {
    throw GraphError(
        GraphErrc::Unknown,
        "Unknown graph apply failure for YAML file " + yaml_path.string());
  }
}

}  // namespace

/**
 * @brief Transactionally loads YAML nodes into an existing graph.
 *
 * @param graph Graph whose topology is replaced after complete validation.
 * @param yaml_path Source YAML sequence path.
 * @return Nothing.
 * @throws std::bad_alloc if parsing, diagnostics, definition conversion, or
 *         temporary storage exhausts memory.
 * @throws GraphError with `GraphErrc::InvalidParameter` for an empty path,
 *         `GraphErrc::Io` for an inaccessible source,
 *         `GraphErrc::InvalidYaml` for parser/root/duplicate-id/node-schema
 *         rejection, `GraphErrc::MissingDependency` or `GraphErrc::Cycle`
 *         for topology rejection, and `GraphErrc::Unknown` for unexpected
 *         ingestion failures.
 * @note Replacement occurs only after every definition node has converted and
 *       topology validation has succeeded, so failed load preserves the
 *       complete prior graph and runtime state. BUILD_TESTING may compile
 *       immutable YAML-tag failpoints immediately before real definition
 *       conversion; production builds compile out the probes and expose no
 *       callable test seam.
 */
void GraphIOService::load(GraphModel& graph,
                          const std::filesystem::path& yaml_path) const {
  if (yaml_path.empty()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph YAML source path must not be empty.");
  }
  YAML::Node config;
  try {
    config = YAML::LoadFile(yaml_path.string());
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const YAML::BadFile& e) {
    throw GraphError(GraphErrc::Io, "Failed to load YAML file " +
                                        yaml_path.string() + ": " + e.what());
  } catch (const YAML::Exception& e) {
    throw GraphError(
        GraphErrc::InvalidYaml,
        "Failed to parse YAML file " + yaml_path.string() + ": " + e.what());
  } catch (const std::exception& e) {
    throw GraphError(GraphErrc::Unknown, "Unexpected YAML load failure for " +
                                             yaml_path.string() + ": " +
                                             e.what());
  } catch (...) {
    throw GraphError(GraphErrc::Unknown,
                     "Unknown YAML load failure for " + yaml_path.string());
  }
  const GraphDefinition definition = convert_graph_document(config, yaml_path);
  apply_graph_document(graph, definition, yaml_path);
}

/**
 * @brief Captures and serializes current persistent graph state to YAML.
 *
 * @param graph Graph to serialize.
 * @param yaml_path Destination YAML path.
 * @return Nothing.
 * @throws std::bad_alloc if path, YAML, or stream storage exhausts memory.
 * @throws GraphError with `GraphErrc::Io` if destination preparation/open or
 *         write, flush, or close reports a recoverable failure.
 * @throws std::exception for definition/YAML serialization failures outside
 *         those stream phases.
 * @note This operation does not create parent directories or mutate graph,
 *       topology, runtime, or session-owner state on success or failure. It
 *       writes directly to `yaml_path`: failure before open preserves existing
 *       bytes, while a post-open failure may leave a created, truncated, or
 *       partially written destination. No atomic replacement or destination
 *       rollback is provided. BUILD_TESTING may compile one process-local,
 *       destination-scoped, one-shot probe before destination open or after a
 *       real output phase; production builds expose no probe or alternate
 *       writer.
 */
void GraphIOService::save(const GraphModel& graph,
                          const std::filesystem::path& yaml_path) const {
  const InMemoryGraphDocumentAdapter adapter;
  const GraphDefinition definition = adapter.capture(graph);
  const YAML::Node root = graph_definition_to_yaml(definition);

  std::ofstream fout;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
  testing::inject_graph_io_save_failure(
      fout, yaml_path,
      testing::GraphIoSaveFailureStage::BeforeDestinationOpenBadAlloc);
  testing::inject_graph_io_save_failure(
      fout, yaml_path, testing::GraphIoSaveFailureStage::BeforeDestinationOpen);
  if (!fout) {
    throw GraphError(GraphErrc::Io, "Failed to prepare YAML destination: " +
                                        yaml_path.string());
  }
#endif
  fout.open(yaml_path);
  if (!fout) {
    throw GraphError(GraphErrc::Io,
                     "Failed to open file for writing: " + yaml_path.string());
  }
  fout << root;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
  testing::inject_graph_io_save_failure(
      fout, yaml_path, testing::GraphIoSaveFailureStage::AfterWrite);
#endif
  if (!fout) {
    throw GraphError(GraphErrc::Io,
                     "Failed to write YAML file: " + yaml_path.string());
  }
  fout.flush();
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
  testing::inject_graph_io_save_failure(
      fout, yaml_path, testing::GraphIoSaveFailureStage::AfterFlush);
#endif
  if (!fout) {
    throw GraphError(GraphErrc::Io,
                     "Failed to flush YAML file: " + yaml_path.string());
  }
  fout.close();
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
  testing::inject_graph_io_save_failure(
      fout, yaml_path, testing::GraphIoSaveFailureStage::AfterClose);
#endif
  if (!fout) {
    throw GraphError(GraphErrc::Io,
                     "Failed to close YAML file: " + yaml_path.string());
  }
}

}  // namespace ps
