#include "adapters/yaml/yaml_graph_document_adapter.hpp"

/**
 * @file yaml_graph_document_adapter.cpp
 * @brief Implements YAML graph-document parsing and direct filesystem writes.
 */

#include <yaml-cpp/yaml.h>

#include <exception>
#include <fstream>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "adapters/yaml/graph_definition_yaml.hpp"  // NOLINT(build/include_subdir)
#if defined(PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
#include "adapters/yaml/yaml_graph_document_adapter_test_access.hpp"  // NOLINT(build/include_subdir)
#endif

namespace ps {

#if defined(PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
namespace testing {
namespace {

/**
 * @brief Exact private YAML save-failure plan visible to graph-state workers.
 *
 * @note The process-local plan is protected by g_save_failure_mutex and is
 *       consumed only by an exact destination-path and stage match. It owns a
 *       copied path so the arming caller need not outlive the writer call.
 */
struct YamlGraphDocumentSaveFailurePlan {
  /** @brief Exact destination path allowed to consume this plan. */
  std::filesystem::path document_path;
  /** @brief Exact save boundary allowed to consume this plan. */
  YamlGraphDocumentSaveFailureStage stage;
};

/**
 * @brief Serializes process-local YAML save-failure plan access.
 * @throws Nothing for static initialization.
 * @note The mutex is never held while a stream is mutated or an injected
 *       exception is thrown.
 */
std::mutex g_save_failure_mutex;

/**
 * @brief Armed destination-scoped failure plan, or no active checkpoint.
 * @throws Nothing for empty optional initialization.
 * @note Every read, replacement, and reset is protected by
 *       g_save_failure_mutex.
 */
std::optional<YamlGraphDocumentSaveFailurePlan> g_save_failure_plan;

/**
 * @brief Process-local count proving the armed checkpoint was reached.
 * @throws Nothing for initialization and mutex-protected scalar updates.
 * @note Every access is protected by g_save_failure_mutex. The one-shot
 *       checkpoint limits this value to zero or one per arm.
 */
std::size_t g_save_failure_hit_count = 0;

}  // namespace

/** @copydoc ps::testing::arm_yaml_graph_document_save_failure */
void arm_yaml_graph_document_save_failure(
    const std::filesystem::path& document_path,
    YamlGraphDocumentSaveFailureStage stage) {
  YamlGraphDocumentSaveFailurePlan plan{document_path, stage};
  std::lock_guard<std::mutex> lock(g_save_failure_mutex);
  g_save_failure_plan = std::move(plan);
  g_save_failure_hit_count = 0;
}

/** @copydoc ps::testing::clear_yaml_graph_document_save_failure */
void clear_yaml_graph_document_save_failure() {
  std::lock_guard<std::mutex> lock(g_save_failure_mutex);
  g_save_failure_plan.reset();
  g_save_failure_hit_count = 0;
}

/** @copydoc ps::testing::yaml_graph_document_save_failure_hit_count */
std::size_t yaml_graph_document_save_failure_hit_count() {
  std::lock_guard<std::mutex> lock(g_save_failure_mutex);
  return g_save_failure_hit_count;
}

/** @copydoc ps::testing::inject_yaml_graph_document_save_failure */
void inject_yaml_graph_document_save_failure(
    std::ios& stream, const std::filesystem::path& document_path,
    YamlGraphDocumentSaveFailureStage stage) {
  {
    std::lock_guard<std::mutex> lock(g_save_failure_mutex);
    if (!g_save_failure_plan || g_save_failure_plan->stage != stage ||
        g_save_failure_plan->document_path != document_path) {
      return;
    }
    g_save_failure_plan.reset();
    ++g_save_failure_hit_count;
  }
  if (stage ==
      YamlGraphDocumentSaveFailureStage::BeforeDestinationOpenBadAlloc) {
    throw std::bad_alloc{};
  }
  stream.setstate(stage == YamlGraphDocumentSaveFailureStage::AfterClose
                      ? std::ios::failbit
                      : std::ios::badbit);
}

}  // namespace testing
#endif

namespace adapters::yaml {
namespace {

/**
 * @brief Converts one parsed YAML root into a detached graph definition.
 *
 * @param yaml_root Parsed graph-document representation owned by yaml-cpp.
 * @param path Source path used only for diagnostic context.
 * @return Fully owned graph definition.
 * @throws std::bad_alloc if conversion or diagnostic allocation is exhausted.
 * @throws GraphError with `GraphErrc::InvalidYaml` for YAML conversion and
 *         definition-schema rejection, the original category for explicit
 *         non-schema graph failures, or `GraphErrc::Unknown` for unexpected
 *         conversion failures.
 * @note Definition-only endpoint validation remains owned by the in-memory
 *       adapter so every input format shares it.
 */
GraphDefinition convert_graph_document(const YAML::Node& yaml_root,
                                       const std::filesystem::path& path) {
  try {
    return internal::graph_definition_from_yaml(yaml_root);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const YAML::Exception& error) {
    throw GraphError(GraphErrc::InvalidYaml,
                     "Invalid graph definition in YAML file " + path.string() +
                         ": " + error.what());
  } catch (const GraphError& error) {
    if (error.code() != GraphErrc::InvalidParameter) {
      throw;
    }
    throw GraphError(GraphErrc::InvalidYaml,
                     "Invalid graph schema in YAML file " + path.string() +
                         ": " + error.what());
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Unknown,
                     "Unexpected graph conversion failure in YAML file " +
                         path.string() + ": " + error.what());
  } catch (...) {
    throw GraphError(
        GraphErrc::Unknown,
        "Unknown graph conversion failure in YAML file " + path.string());
  }
}

/**
 * @brief Converts one node mapping with the public node-document category.
 *
 * @param yaml_node Parsed node mapping owned by yaml-cpp.
 * @return Fully owned node definition.
 * @throws std::bad_alloc if conversion or diagnostic allocation is exhausted.
 * @throws GraphError with `GraphErrc::InvalidYaml` for every recoverable
 *         parsing, representation, or schema failure.
 * @note Required node identity override and topology validation remain
 *       caller-owned.
 */
NodeDefinition convert_node_document(const YAML::Node& yaml_node) {
  try {
    return internal::node_definition_from_yaml(yaml_node);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& error) {
    throw GraphError(GraphErrc::InvalidYaml, error.what());
  } catch (const YAML::Exception& error) {
    throw GraphError(GraphErrc::InvalidYaml, error.what());
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::InvalidYaml, error.what());
  } catch (...) {
    throw GraphError(GraphErrc::InvalidYaml,
                     "Unknown YAML node conversion failure.");
  }
}

/**
 * @brief Emits one graph definition before destination access begins.
 *
 * @param definition Detached persistent graph definition.
 * @return Independent YAML sequence.
 * @throws std::bad_alloc if YAML or diagnostic allocation is exhausted.
 * @throws GraphError with `GraphErrc::Io` for every recoverable emission
 *         failure.
 * @note Completing this helper is the direct-writer pre-open boundary.
 */
YAML::Node emit_graph_document(const GraphDefinition& definition) {
  try {
    return internal::graph_definition_to_yaml(definition);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(
        GraphErrc::Io,
        std::string("Failed to emit YAML graph document: ") + error.what());
  } catch (...) {
    throw GraphError(GraphErrc::Io,
                     "Unknown YAML graph document emission failure.");
  }
}

}  // namespace

/** @copydoc YamlGraphDocumentAdapter::read */
GraphDefinition YamlGraphDocumentAdapter::read(
    const std::filesystem::path& path) const {
  if (path.empty()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "Graph document source path must not be empty.");
  }

  YAML::Node root;
  try {
    root = YAML::LoadFile(path.string());
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const YAML::BadFile& error) {
    throw GraphError(GraphErrc::Io, "Failed to load YAML file " +
                                        path.string() + ": " + error.what());
  } catch (const YAML::Exception& error) {
    throw GraphError(
        GraphErrc::InvalidYaml,
        "Failed to parse YAML file " + path.string() + ": " + error.what());
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::Unknown, "Unexpected YAML load failure for " +
                                             path.string() + ": " +
                                             error.what());
  } catch (...) {
    throw GraphError(GraphErrc::Unknown,
                     "Unknown YAML load failure for " + path.string());
  }
  return convert_graph_document(root, path);
}

/** @copydoc YamlGraphDocumentAdapter::read_node */
NodeDefinition YamlGraphDocumentAdapter::read_node(
    const std::string& document_text) const {
  try {
    return convert_node_document(YAML::Load(document_text));
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError&) {
    throw;
  } catch (const YAML::Exception& error) {
    throw GraphError(GraphErrc::InvalidYaml, error.what());
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::InvalidYaml, error.what());
  } catch (...) {
    throw GraphError(GraphErrc::InvalidYaml,
                     "Unknown YAML node parsing failure.");
  }
}

/** @copydoc YamlGraphDocumentAdapter::write */
void YamlGraphDocumentAdapter::write(const std::filesystem::path& path,
                                     const GraphDefinition& definition) const {
  const YAML::Node root = emit_graph_document(definition);

  std::ofstream output;
#if defined(PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
  testing::inject_yaml_graph_document_save_failure(
      output, path,
      testing::YamlGraphDocumentSaveFailureStage::
          BeforeDestinationOpenBadAlloc);
  testing::inject_yaml_graph_document_save_failure(
      output, path,
      testing::YamlGraphDocumentSaveFailureStage::BeforeDestinationOpen);
  if (!output) {
    throw GraphError(GraphErrc::Io,
                     "Failed to prepare YAML destination: " + path.string());
  }
#endif
  output.open(path);
  if (!output) {
    throw GraphError(GraphErrc::Io,
                     "Failed to open file for writing: " + path.string());
  }
  output << root;
#if defined(PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
  testing::inject_yaml_graph_document_save_failure(
      output, path, testing::YamlGraphDocumentSaveFailureStage::AfterWrite);
#endif
  if (!output) {
    throw GraphError(GraphErrc::Io,
                     "Failed to write YAML file: " + path.string());
  }
  output.flush();
#if defined(PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
  testing::inject_yaml_graph_document_save_failure(
      output, path, testing::YamlGraphDocumentSaveFailureStage::AfterFlush);
#endif
  if (!output) {
    throw GraphError(GraphErrc::Io,
                     "Failed to flush YAML file: " + path.string());
  }
  output.close();
#if defined(PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
  testing::inject_yaml_graph_document_save_failure(
      output, path, testing::YamlGraphDocumentSaveFailureStage::AfterClose);
#endif
  if (!output) {
    throw GraphError(GraphErrc::Io,
                     "Failed to close YAML file: " + path.string());
  }
}

/** @copydoc YamlGraphDocumentAdapter::write_node */
std::string YamlGraphDocumentAdapter::write_node(
    const NodeDefinition& definition) const {
  try {
    const YAML::Node root = internal::node_definition_to_yaml(definition);
    std::stringstream output;
    output << root;
    return output.str();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError&) {
    throw;
  } catch (const std::exception& error) {
    throw GraphError(GraphErrc::InvalidYaml, error.what());
  } catch (...) {
    throw GraphError(GraphErrc::InvalidYaml,
                     "Unknown YAML node emission failure.");
  }
}

}  // namespace adapters::yaml
}  // namespace ps
