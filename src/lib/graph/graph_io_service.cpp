#include "graph/graph_io_service.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
#include "graph/graph_io_service_test_access.hpp"  // NOLINT(build/include_subdir)
#endif

namespace ps {

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
namespace testing {
namespace {

/**
 * @brief Calling-thread failure stage consumed by the next matching save.
 * @throws Nothing for thread-local initialization and optional reset.
 * @note Thread-local ownership prevents unrelated concurrent saves from
 *       observing or consuming the armed private test checkpoint.
 */
thread_local std::optional<GraphIoSaveFailureStage> g_save_failure_stage;

/**
 * @brief Calling-thread count proving the armed checkpoint was reached.
 * @throws Nothing for initialization and scalar updates.
 * @note The one-shot checkpoint limits this value to zero or one per arm.
 */
thread_local std::size_t g_save_failure_hit_count = 0;

}  // namespace

/** @copydoc ps::testing::arm_graph_io_save_failure */
void arm_graph_io_save_failure(GraphIoSaveFailureStage stage) noexcept {
  g_save_failure_stage = stage;
  g_save_failure_hit_count = 0;
}

/** @copydoc ps::testing::clear_graph_io_save_failure */
void clear_graph_io_save_failure() noexcept {
  g_save_failure_stage.reset();
  g_save_failure_hit_count = 0;
}

/** @copydoc ps::testing::graph_io_save_failure_hit_count */
std::size_t graph_io_save_failure_hit_count() noexcept {
  return g_save_failure_hit_count;
}

/** @copydoc ps::testing::inject_graph_io_save_failure */
void inject_graph_io_save_failure(std::ios& stream,
                                  GraphIoSaveFailureStage stage) {
  if (!g_save_failure_stage || *g_save_failure_stage != stage) {
    return;
  }
  g_save_failure_stage.reset();
  ++g_save_failure_hit_count;
  stream.setstate(stage == GraphIoSaveFailureStage::AfterClose
                      ? std::ios::failbit
                      : std::ios::badbit);
}

}  // namespace testing
#endif

namespace {

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Injects resource exhaustion at real graph-node YAML conversion.
 *
 * @param node_yaml Sequence item returned by YAML::LoadFile.
 * @return Nothing.
 * @throws std::bad_alloc when node_yaml carries the private test probe tag.
 * @note This helper has translation-unit-local linkage and is compiled only
 * when BUILD_TESTING is enabled. The immutable YAML tag makes the probe
 * deterministic and thread-safe without adding an installed or exported API.
 */
void throw_if_graph_load_bad_alloc_probe(const YAML::Node& node_yaml) {
  if (node_yaml.Tag() == "!photospider-test-reload-bad-alloc") {
    throw std::bad_alloc{};
  }
}
#endif

}  // namespace

/**
 * @brief Transactionally loads YAML nodes into an existing graph.
 *
 * @param graph Graph whose topology is replaced after complete validation.
 * @param yaml_path Source YAML sequence path.
 * @return Nothing.
 * @throws std::bad_alloc if parsing, node construction, or temporary storage
 * exhausts memory.
 * @throws GraphError for other I/O, YAML-root, duplicate-id, node, or topology
 * validation failures.
 * @note Replacement occurs only after every temporary node has parsed, so a
 * failed load cannot expose partial topology. BUILD_TESTING may compile an
 * immutable YAML tag failpoint immediately before Node::from_yaml; production
 * builds compile out the probe and expose no callable test seam.
 */
void GraphIOService::load(GraphModel& graph,
                          const std::filesystem::path& yaml_path) const {
  YAML::Node config;
  try {
    config = YAML::LoadFile(yaml_path.string());
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& e) {
    throw GraphError(GraphErrc::Io, "Failed to load YAML file " +
                                        yaml_path.string() + ": " + e.what());
  }
  if (!config.IsSequence()) {
    throw GraphError(GraphErrc::InvalidYaml,
                     "YAML root is not a sequence of nodes.");
  }
  GraphModel::NodeMap loaded_nodes;
  for (const auto& node_yaml : config) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    throw_if_graph_load_bad_alloc_probe(node_yaml);
#endif
    Node node = Node::from_yaml(node_yaml);
    if (loaded_nodes.count(node.id)) {
      throw GraphError(
          GraphErrc::InvalidYaml,
          "Duplicate node id " + std::to_string(node.id) + " in graph YAML.");
    }
    loaded_nodes[node.id] = std::move(node);
  }
  graph.replace_nodes(std::move(loaded_nodes));
}

/**
 * @brief Serializes the graph's current nodes to a YAML sequence.
 *
 * @param graph Graph to serialize.
 * @param yaml_path Destination YAML path.
 * @return Nothing.
 * @throws std::bad_alloc if path, YAML, or stream storage exhausts memory.
 * @throws GraphError with `GraphErrc::Io` if the destination cannot be opened
 *         or reports a write, flush, or close failure.
 * @throws std::exception for YAML node serialization failures outside those
 *         stream phases.
 * @note This operation does not create parent directories or mutate graph
 *       state. It writes directly to `yaml_path`, so a post-open failure may
 *       leave a created, truncated, or partially written destination; no
 *       atomic replacement or rollback is provided. BUILD_TESTING may compile
 *       thread-local one-shot stream-state probes after each real output phase;
 *       production builds expose no probe or alternate writer.
 */
void GraphIOService::save(const GraphModel& graph,
                          const std::filesystem::path& yaml_path) const {
  YAML::Node root(YAML::NodeType::Sequence);
  for (int id : graph.node_ids()) {
    root.push_back(graph.node(id).to_yaml());
  }

  std::ofstream fout(yaml_path);
  if (!fout) {
    throw GraphError(GraphErrc::Io,
                     "Failed to open file for writing: " + yaml_path.string());
  }
  fout << root;
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
  testing::inject_graph_io_save_failure(
      fout, testing::GraphIoSaveFailureStage::AfterWrite);
#endif
  if (!fout) {
    throw GraphError(GraphErrc::Io,
                     "Failed to write YAML file: " + yaml_path.string());
  }
  fout.flush();
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
  testing::inject_graph_io_save_failure(
      fout, testing::GraphIoSaveFailureStage::AfterFlush);
#endif
  if (!fout) {
    throw GraphError(GraphErrc::Io,
                     "Failed to flush YAML file: " + yaml_path.string());
  }
  fout.close();
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_IO_TESTING)
  testing::inject_graph_io_save_failure(
      fout, testing::GraphIoSaveFailureStage::AfterClose);
#endif
  if (!fout) {
    throw GraphError(GraphErrc::Io,
                     "Failed to close YAML file: " + yaml_path.string());
  }
}

}  // namespace ps
