#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "graph/graph_io_service.hpp"           // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"                // NOLINT(build/include_subdir)
#include "host/embedded_host_dependencies.hpp"  // NOLINT(build/include_subdir)
#include "photospider/core/graph_error.hpp"
#include "photospider/host/host.hpp"
#include "providers/configured_image_artifact_codec.hpp"  // NOLINT(build/include_subdir)
#include "support/cache_test_dependencies.hpp"
#include "support/fake_graph_document_adapter.hpp"

namespace ps {
namespace {

/**
 * @class ScopedInjectionDirectory
 * @brief Owns one unique filesystem root for composition-injection tests.
 *
 * @note Destruction uses the error-code cleanup overload so a fixture cleanup
 *       failure cannot mask an assertion.
 */
class ScopedInjectionDirectory final {
 public:
  /**
   * @brief Creates one empty unique temporary directory.
   *
   * @throws std::filesystem::filesystem_error if cleanup or creation fails.
   * @throws std::bad_alloc if path construction fails.
   */
  ScopedInjectionDirectory()
      : root_(std::filesystem::temp_directory_path() /
              ("photospider_graph_document_injection_" +
               std::to_string(std::chrono::high_resolution_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  /**
   * @brief Removes the owned directory without throwing.
   * @throws Nothing.
   */
  ~ScopedInjectionDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /**
   * @brief Prevents duplicate cleanup ownership.
   * @param other Owner that retains its directory.
   * @throws Nothing because construction is unavailable.
   */
  ScopedInjectionDirectory(const ScopedInjectionDirectory& other) = delete;

  /**
   * @brief Prevents replacement of cleanup ownership.
   * @param other Owner that retains its directory.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedInjectionDirectory& operator=(const ScopedInjectionDirectory& other) =
      delete;

  /**
   * @brief Returns the owned root.
   * @return Stable path reference valid for this fixture's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Unique temporary root removed by this fixture. */
  std::filesystem::path root_;
};

/**
 * @brief Builds one detached source-node graph definition.
 *
 * @param name Persistent node name copied into the definition.
 * @return Complete one-node graph definition with id 1.
 * @throws std::bad_alloc if strings or definition storage cannot allocate.
 * @note The definition owns no YAML/parser or GraphModel state.
 */
GraphDefinition make_graph_definition(const std::string& name) {
  NodeDefinition node;
  node.id = 1;
  node.name = name;
  node.type = "test_source";
  node.subtype = "constant";
  node.parameters["width"] = 16;
  node.parameters["height"] = 16;

  GraphDefinition definition;
  definition.nodes.push_back(std::move(node));
  return definition;
}

/**
 * @brief Writes arbitrary bytes needed only by generic session selection.
 *
 * @param path Destination whose parent directory is created first.
 * @param contents Exact bytes written to the file.
 * @return Nothing.
 * @throws std::filesystem::filesystem_error if parent creation fails.
 * @throws std::runtime_error if destination open/write/close is unsuccessful.
 * @note The fake reader must receive this path without parsing its deliberately
 *       invalid YAML bytes.
 */
void write_fixture(const std::filesystem::path& path,
                   const std::string& contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Failed to open injection fixture.");
  }
  output << contents;
  output.close();
  if (!output) {
    throw std::runtime_error("Failed to write injection fixture.");
  }
}

/**
 * @brief Counts one operation kind in an ordered fake snapshot.
 *
 * @param observations Deep-owned observation snapshot.
 * @param operation Operation kind to count.
 * @return Number of matching observations.
 * @throws Nothing.
 */
std::size_t count_operations(
    const std::vector<testing::FakeGraphDocumentAdapter::Observation>&
        observations,
    testing::FakeGraphDocumentAdapter::Operation operation) noexcept {
  std::size_t count = 0;
  for (const auto& observation : observations) {
    if (observation.operation == operation) {
      ++count;
    }
  }
  return count;
}

}  // namespace

/**
 * @brief Verifies construction rejects absent required neutral dependencies.
 */
TEST(GraphDocumentInjection, RejectsMissingReaderOrWriterBeforeUse) {
  auto fake = std::make_shared<testing::FakeGraphDocumentAdapter>();
  EXPECT_THROW(internal::create_embedded_host_with_dependencies(
                   providers::make_configured_image_artifact_codec(), nullptr,
                   fake, fake),
               std::invalid_argument);
  EXPECT_THROW(
      GraphIOService(nullptr, std::shared_ptr<const GraphDocumentWriter>(fake)),
      std::invalid_argument);
  EXPECT_THROW(
      GraphIOService(std::shared_ptr<const GraphDocumentReader>(fake), nullptr),
      std::invalid_argument);
  EXPECT_THROW(internal::create_embedded_host_with_dependencies(
                   providers::make_configured_image_artifact_codec(),
                   testing::make_yaml_cache_metadata_codec(), nullptr, fake),
               std::invalid_argument);
  EXPECT_THROW(internal::create_embedded_host_with_dependencies(
                   providers::make_configured_image_artifact_codec(),
                   testing::make_yaml_cache_metadata_codec(), fake, nullptr),
               std::invalid_argument);
  EXPECT_TRUE(fake->observations().empty());
}

/**
 * @brief Verifies GraphIO delegates detached load/save/node operations.
 */
TEST(GraphDocumentInjection,
     GraphIoUsesFakeContractsAndRetainsTheirSharedOwner) {
  ScopedInjectionDirectory directory;
  GraphModel graph(directory.root() / "cache");
  std::weak_ptr<testing::FakeGraphDocumentAdapter> retained;

  {
    auto fake = std::make_shared<testing::FakeGraphDocumentAdapter>();
    retained = fake;
    fake->set_read_callback([](const std::filesystem::path&) {
      return make_graph_definition("loaded-without-yaml");
    });
    fake->set_read_node_callback([](const std::string& document_text) {
      NodeDefinition definition = make_graph_definition(document_text).nodes[0];
      definition.id = 99;
      return definition;
    });
    fake->set_write_callback(
        [](const std::filesystem::path&, const GraphDefinition&) {});
    fake->set_write_node_callback([](const NodeDefinition& definition) {
      return "node:" + definition.name;
    });

    GraphIOService io(fake, fake);
    fake.reset();
    ASSERT_FALSE(retained.expired());

    io.load(graph, directory.root() / "virtual.graph");
    ASSERT_TRUE(graph.has_node(1));
    EXPECT_EQ(graph.node(1).name, "loaded-without-yaml");

    const NodeDefinition parsed = io.read_node_document("replacement");
    EXPECT_EQ(parsed.id, 99);
    EXPECT_EQ(parsed.name, "replacement");
    EXPECT_EQ(io.write_node_document(parsed), "node:replacement");

    io.save(graph, directory.root() / "captured.graph");

    auto live = retained.lock();
    ASSERT_NE(live, nullptr);
    const auto observations = live->observations();
    ASSERT_EQ(observations.size(), 4U);
    EXPECT_EQ(observations[0].operation,
              testing::FakeGraphDocumentAdapter::Operation::Read);
    EXPECT_EQ(observations[1].operation,
              testing::FakeGraphDocumentAdapter::Operation::ReadNode);
    EXPECT_EQ(observations[2].operation,
              testing::FakeGraphDocumentAdapter::Operation::WriteNode);
    EXPECT_EQ(observations[3].operation,
              testing::FakeGraphDocumentAdapter::Operation::Write);
    ASSERT_TRUE(observations[3].graph_definition.has_value());
    ASSERT_EQ(observations[3].graph_definition->nodes.size(), 1U);
    EXPECT_EQ(observations[3].graph_definition->nodes[0].name,
              "loaded-without-yaml");
  }

  EXPECT_TRUE(retained.expired());
}

/**
 * @brief Verifies a fake read failure preserves the published graph.
 */
TEST(GraphDocumentInjection,
     GraphIoPreservesPublishedGraphWhenInjectedReaderFails) {
  ScopedInjectionDirectory directory;
  GraphModel graph(directory.root() / "cache");
  auto fake = std::make_shared<testing::FakeGraphDocumentAdapter>();
  fake->set_read_callback([](const std::filesystem::path&) {
    return make_graph_definition("stable");
  });
  fake->set_write_callback(
      [](const std::filesystem::path&, const GraphDefinition&) {});
  GraphIOService io(fake, fake);
  io.load(graph, directory.root() / "initial.graph");

  fake->set_read_callback([](const std::filesystem::path&) -> GraphDefinition {
    throw GraphError(GraphErrc::Io, "injected reader failure");
  });

  try {
    io.load(graph, directory.root() / "broken.graph");
    FAIL() << "Injected reader failure did not propagate.";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::Io);
  }
  ASSERT_TRUE(graph.has_node(1));
  EXPECT_EQ(graph.node(1).name, "stable");
}

/**
 * @brief Verifies public Host lifecycle and node methods use the injected fake.
 */
TEST(GraphDocumentInjection,
     EmbeddedHostUsesOneRetainedFakeForGraphAndNodeDocuments) {
  ScopedInjectionDirectory directory;
  const GraphSessionId session{"fake-session"};
  write_fixture(directory.root() / session.value / "content.yaml",
                "not: [valid YAML");

  auto fake = std::make_shared<testing::FakeGraphDocumentAdapter>();
  std::weak_ptr<testing::FakeGraphDocumentAdapter> retained = fake;
  fake->set_read_callback([](const std::filesystem::path& path) {
    if (path.filename() == "broken.document") {
      throw GraphError(GraphErrc::Io, "injected Host reader failure");
    }
    if (path.filename() == "content.yaml") {
      return make_graph_definition("initial-from-fake");
    }
    return make_graph_definition("reloaded-from-fake");
  });
  fake->set_read_node_callback([](const std::string& document_text) {
    NodeDefinition definition = make_graph_definition(document_text).nodes[0];
    definition.id = 999;
    return definition;
  });
  fake->set_write_callback(
      [](const std::filesystem::path&, const GraphDefinition&) {});
  fake->set_write_node_callback([](const NodeDefinition& definition) {
    return "fake-node:" + definition.name;
  });

  auto host = internal::create_embedded_host_with_dependencies(
      providers::make_configured_image_artifact_codec(),
      testing::make_yaml_cache_metadata_codec(), fake, fake);
  fake.reset();
  ASSERT_FALSE(retained.expired());

  GraphLoadRequest request;
  request.session = session;
  request.root_dir = directory.root().string();
  const Result<GraphSessionId> loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  Result<GraphInspectionView> inspection = host->inspect_graph(session);
  ASSERT_TRUE(inspection.status.ok) << inspection.status.message;
  ASSERT_EQ(inspection.value.nodes.size(), 1U);
  EXPECT_EQ(inspection.value.nodes[0].name, "initial-from-fake");

  const Result<std::string> emitted = host->get_node_yaml(session, NodeId{1});
  ASSERT_TRUE(emitted.status.ok) << emitted.status.message;
  EXPECT_EQ(emitted.value, "fake-node:initial-from-fake");

  const VoidResult replaced =
      host->set_node_yaml(session, NodeId{1}, "updated-through-fake");
  ASSERT_TRUE(replaced.status.ok) << replaced.status.message;
  inspection = host->inspect_graph(session);
  ASSERT_TRUE(inspection.status.ok) << inspection.status.message;
  ASSERT_EQ(inspection.value.nodes.size(), 1U);
  EXPECT_EQ(inspection.value.nodes[0].id.value, 1);
  EXPECT_EQ(inspection.value.nodes[0].name, "updated-through-fake");

  const VoidResult reloaded = host->reload_graph(
      session, (directory.root() / "reload.document").string());
  ASSERT_TRUE(reloaded.status.ok) << reloaded.status.message;
  inspection = host->inspect_graph(session);
  ASSERT_TRUE(inspection.status.ok) << inspection.status.message;
  ASSERT_EQ(inspection.value.nodes.size(), 1U);
  EXPECT_EQ(inspection.value.nodes[0].name, "reloaded-from-fake");

  const VoidResult failed_reload = host->reload_graph(
      session, (directory.root() / "broken.document").string());
  EXPECT_FALSE(failed_reload.status.ok);
  EXPECT_EQ(checked_graph_error_code(failed_reload.status), GraphErrc::Io);
  inspection = host->inspect_graph(session);
  ASSERT_TRUE(inspection.status.ok) << inspection.status.message;
  ASSERT_EQ(inspection.value.nodes.size(), 1U);
  EXPECT_EQ(inspection.value.nodes[0].name, "reloaded-from-fake");

  const VoidResult saved =
      host->save_graph(session, (directory.root() / "saved.document").string());
  ASSERT_TRUE(saved.status.ok) << saved.status.message;

  auto live = retained.lock();
  ASSERT_NE(live, nullptr);
  const auto before_missing = live->observations();
  const VoidResult missing_reload =
      host->reload_graph(GraphSessionId{"missing"},
                         (directory.root() / "unused.document").string());
  const VoidResult missing_save =
      host->save_graph(GraphSessionId{"missing"},
                       (directory.root() / "unused-save.document").string());
  EXPECT_EQ(checked_graph_error_code(missing_reload.status),
            GraphErrc::NotFound);
  EXPECT_EQ(checked_graph_error_code(missing_save.status), GraphErrc::NotFound);
  EXPECT_EQ(live->observations().size(), before_missing.size());

  const auto observations = live->observations();
  EXPECT_EQ(
      count_operations(observations,
                       testing::FakeGraphDocumentAdapter::Operation::Read),
      3U);
  EXPECT_EQ(
      count_operations(observations,
                       testing::FakeGraphDocumentAdapter::Operation::ReadNode),
      1U);
  EXPECT_EQ(
      count_operations(observations,
                       testing::FakeGraphDocumentAdapter::Operation::WriteNode),
      1U);
  EXPECT_EQ(
      count_operations(observations,
                       testing::FakeGraphDocumentAdapter::Operation::Write),
      1U);

  live.reset();
  host.reset();
  EXPECT_TRUE(retained.expired());
}

}  // namespace ps
