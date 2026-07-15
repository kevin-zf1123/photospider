#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "graph/graph_io_service.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"       // NOLINT(build/include_subdir)
#include "photospider/core/graph_error.hpp"
#include "photospider/host/host.hpp"

namespace ps {
namespace {

/**
 * @brief Owns one unique graph-document test directory.
 *
 * @throws std::filesystem::filesystem_error If setup cleanup or directory
 *         creation fails.
 * @note Destruction uses the non-throwing error-code overload so cleanup never
 *       masks a test assertion.
 */
class ScopedGraphDocumentDirectory final {
 public:
  /**
   * @brief Creates an empty unique directory below the platform temp root.
   *
   * @throws std::filesystem::filesystem_error If directory setup fails.
   */
  ScopedGraphDocumentDirectory()
      : root_(std::filesystem::temp_directory_path() /
              ("photospider_graph_document_errors_" +
               std::to_string(std::chrono::high_resolution_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  /** @brief Removes the owned directory without throwing. */
  ~ScopedGraphDocumentDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /**
   * @brief Prevents duplicate cleanup ownership.
   * @param other Owner that retains its directory.
   * @throws Nothing because construction is unavailable.
   */
  ScopedGraphDocumentDirectory(const ScopedGraphDocumentDirectory& other) =
      delete;

  /**
   * @brief Prevents replacement of cleanup ownership.
   * @param other Owner that retains its directory.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedGraphDocumentDirectory& operator=(
      const ScopedGraphDocumentDirectory& other) = delete;

  /**
   * @brief Returns the owned test root.
   * @return Stable path reference valid for this helper's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Unique root removed by this owner. */
  std::filesystem::path root_;
};

/**
 * @brief Writes exact text to a graph-document fixture.
 *
 * @param path Destination whose parent directory is created first.
 * @param contents Complete document contents.
 * @return Nothing.
 * @throws std::filesystem::filesystem_error If parent creation fails.
 * @throws std::ios_base::failure If opening or writing the file fails.
 * @note The stream is configured to report every persistence failure.
 */
void write_document(const std::filesystem::path& path,
                    const std::string& contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream;
  stream.exceptions(std::ios::failbit | std::ios::badbit);
  stream.open(path);
  stream << contents;
  stream.close();
}

/**
 * @brief Configures no-worker schedulers for document-only Host tests.
 *
 * @param host Embedded Host under test.
 * @return Nothing.
 * @throws std::runtime_error If Host rejects the deterministic scheduler.
 * @note The serial scheduler avoids consuming the process worker budget while
 *       preserving the real Kernel graph-load lifecycle.
 */
void configure_document_host(Host& host) {
  HostSchedulerConfig config;
  config.hp_type = "serial_debug";
  config.rt_type = "serial_debug";
  const VoidResult configured = host.configure_scheduler_defaults(config);
  if (!configured.status.ok) {
    throw std::runtime_error("failed to configure graph document Host: " +
                             configured.status.message);
  }
}

/**
 * @brief Builds a graph load request rooted in one test directory.
 *
 * @param root Directory containing session, source, and cache children.
 * @param session Frontend-visible session label.
 * @param source Explicit source path, or empty text for omission semantics.
 * @return Fully owned public Host request.
 * @throws std::bad_alloc If request strings cannot be allocated.
 */
GraphLoadRequest make_load_request(const std::filesystem::path& root,
                                   const std::string& session,
                                   const std::filesystem::path& source) {
  GraphLoadRequest request;
  request.session = GraphSessionId{session};
  request.root_dir = (root / "sessions").string();
  request.yaml_path = source.empty() ? std::string{} : source.string();
  request.cache_root_dir = (root / "cache").string();
  return request;
}

/**
 * @brief Loads a deterministic single-node graph through public Host.
 *
 * @param host Embedded Host that owns the resulting session.
 * @param root Test root containing source, session, and cache children.
 * @param session Frontend-visible session label.
 * @param node_name Name stored in node 1 for preservation assertions.
 * @return Loaded public session id.
 * @throws std::bad_alloc If document/request construction or Host loading
 *         exhausts memory.
 * @throws std::runtime_error If Host rejects the valid fixture.
 * @note The fixture needs no registered operation because graph document
 *       lifecycle validation does not execute nodes.
 */
GraphSessionId load_single_node_graph(Host& host,
                                      const std::filesystem::path& root,
                                      const std::string& session,
                                      const std::string& node_name) {
  const std::filesystem::path source = root / "source" / (session + ".yaml");
  write_document(source, "- id: 1\n  name: " + node_name +
                             "\n  type: fixture\n  subtype: source\n");
  const Result<GraphSessionId> loaded =
      host.load_graph(make_load_request(root, session, source));
  if (!loaded.status.ok) {
    throw std::runtime_error("valid graph document load failed: " +
                             loaded.status.message);
  }
  return loaded.value;
}

/**
 * @brief Asserts one session still exposes exactly the expected node.
 *
 * @param host Host that owns the inspected session.
 * @param session Session whose current graph is copied.
 * @param node_name Expected node 1 name.
 * @return Nothing.
 * @throws std::bad_alloc If Host inspection or assertion diagnostics exhaust
 *         memory.
 * @note The helper checks both identity and contents so an accidental empty or
 *       partially replaced graph cannot satisfy preservation assertions.
 */
void expect_single_node(Host& host, const GraphSessionId& session,
                        const std::string& node_name) {
  const Result<GraphInspectionView> graph = host.inspect_graph(session);
  ASSERT_TRUE(graph.status.ok) << graph.status.message;
  ASSERT_EQ(graph.value.nodes.size(), 1U);
  EXPECT_EQ(graph.value.nodes.front().id.value, 1);
  EXPECT_EQ(graph.value.nodes.front().name, node_name);
}

/**
 * @brief Checks one recoverable reload failure and visible-state preservation.
 *
 * @param host Host that owns the existing graph session.
 * @param session Session to reload.
 * @param source Explicit replacement document path.
 * @param expected_code Exact public graph error category.
 * @param original_node_name Node name that must remain visible.
 * @return Nothing.
 * @throws std::bad_alloc If Host translation or inspection exhausts memory.
 * @note Each invocation completes before the next one, so the same published
 *       session also proves that failed reloads do not poison later requests.
 */
void expect_reload_failure_preserves(Host& host, const GraphSessionId& session,
                                     const std::filesystem::path& source,
                                     GraphErrc expected_code,
                                     const std::string& original_node_name) {
  const VoidResult reloaded = host.reload_graph(session, source.string());
  ASSERT_FALSE(reloaded.status.ok);
  EXPECT_EQ(checked_graph_error_code(reloaded.status), expected_code);
  expect_single_node(host, session, original_node_name);
}

/**
 * @brief Proves an explicit missing source cannot consume stale session data.
 *
 * @throws Nothing when the public error and publication contract hold.
 * @note The pre-created session-local document is valid and would make the
 *       former fallback path succeed, so this is a direct regression probe.
 */
TEST(GraphDocumentLoadErrors,
     ExplicitMissingSourceReturnsIoWithoutPublishingStaleSession) {
  ScopedGraphDocumentDirectory directory;
  constexpr char kSession[] = "explicit_missing";
  write_document(directory.root() / "sessions" / kSession / "content.yaml",
                 "[]\n");
  const std::filesystem::path missing =
      directory.root() / "source" / "missing.yaml";

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> loaded =
      host->load_graph(make_load_request(directory.root(), kSession, missing));
  ASSERT_FALSE(loaded.status.ok);
  EXPECT_EQ(checked_graph_error_code(loaded.status), GraphErrc::Io);

  const Result<std::vector<GraphSessionId>> sessions = host->list_graphs();
  ASSERT_TRUE(sessions.status.ok) << sessions.status.message;
  EXPECT_TRUE(sessions.value.empty());
}

/**
 * @brief Proves an existing but uncopyable explicit source is an IO failure.
 *
 * @throws Nothing when the public error and publication contract hold.
 * @note A directory exists at the source path, forcing the real copy_file
 *       operation to fail without relying on platform permission behavior.
 */
TEST(GraphDocumentLoadErrors,
     UncopyableExplicitSourceReturnsIoWithoutPublishingSession) {
  ScopedGraphDocumentDirectory directory;
  const std::filesystem::path uncopyable =
      directory.root() / "source" / "document_directory";
  std::filesystem::create_directories(uncopyable);

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> loaded = host->load_graph(
      make_load_request(directory.root(), "uncopyable_source", uncopyable));
  ASSERT_FALSE(loaded.status.ok);
  EXPECT_EQ(checked_graph_error_code(loaded.status), GraphErrc::Io);

  const Result<std::vector<GraphSessionId>> sessions = host->list_graphs();
  ASSERT_TRUE(sessions.status.ok) << sessions.status.message;
  EXPECT_TRUE(sessions.value.empty());
}

/**
 * @brief Proves malformed YAML has a document category and no publication.
 *
 * @throws Nothing when the public error and publication contract hold.
 * @note The source exists, isolating parser classification from path IO.
 */
TEST(GraphDocumentLoadErrors,
     SyntaxFailureReturnsInvalidYamlWithoutPublishingSession) {
  ScopedGraphDocumentDirectory directory;
  const std::filesystem::path malformed =
      directory.root() / "source" / "malformed.yaml";
  write_document(malformed, "[unterminated\n");

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> loaded = host->load_graph(
      make_load_request(directory.root(), "syntax_failure", malformed));
  ASSERT_FALSE(loaded.status.ok);
  EXPECT_EQ(checked_graph_error_code(loaded.status), GraphErrc::InvalidYaml);

  const Result<std::vector<GraphSessionId>> sessions = host->list_graphs();
  ASSERT_TRUE(sessions.status.ok) << sessions.status.message;
  EXPECT_TRUE(sessions.value.empty());
}

/**
 * @brief Proves node-schema conversion failures are invalid YAML documents.
 *
 * @throws Nothing when the public error and publication contract hold.
 * @note The YAML sequence parses successfully but omits the required node id,
 *       isolating schema conversion from parser and topology validation.
 */
TEST(GraphDocumentLoadErrors,
     SchemaFailureReturnsInvalidYamlWithoutPublishingSession) {
  ScopedGraphDocumentDirectory directory;
  const std::filesystem::path invalid_schema =
      directory.root() / "source" / "invalid_schema.yaml";
  write_document(invalid_schema,
                 "- name: missing_required_id\n"
                 "  type: fixture\n"
                 "  subtype: source\n");

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> loaded = host->load_graph(
      make_load_request(directory.root(), "schema_failure", invalid_schema));
  ASSERT_FALSE(loaded.status.ok);
  EXPECT_EQ(checked_graph_error_code(loaded.status), GraphErrc::InvalidYaml);

  const Result<std::vector<GraphSessionId>> sessions = host->list_graphs();
  ASSERT_TRUE(sessions.status.ok) << sessions.status.message;
  EXPECT_TRUE(sessions.value.empty());
}

/**
 * @brief Proves semantic node-schema validation remains a document failure.
 *
 * @throws Nothing when the public error and publication contract hold.
 * @note Empty parameter-edge names reach Node::from_yaml's explicit schema
 *       validator, which must not leak its lower-level InvalidParameter code.
 */
TEST(GraphDocumentLoadErrors,
     SemanticSchemaFailureReturnsInvalidYamlWithoutPublishingSession) {
  ScopedGraphDocumentDirectory directory;
  const std::filesystem::path invalid_schema =
      directory.root() / "source" / "invalid_edge_schema.yaml";
  write_document(invalid_schema,
                 "- id: 1\n"
                 "  name: invalid_parameter_edge\n"
                 "  type: fixture\n"
                 "  subtype: source\n"
                 "  parameter_inputs:\n"
                 "    - from_node_id: 2\n"
                 "      from_output_name: ''\n"
                 "      to_parameter_name: ''\n");

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> loaded = host->load_graph(make_load_request(
      directory.root(), "semantic_schema_failure", invalid_schema));
  ASSERT_FALSE(loaded.status.ok);
  EXPECT_EQ(checked_graph_error_code(loaded.status), GraphErrc::InvalidYaml);

  const Result<std::vector<GraphSessionId>> sessions = host->list_graphs();
  ASSERT_TRUE(sessions.status.ok) << sessions.status.message;
  EXPECT_TRUE(sessions.value.empty());
}

/**
 * @brief Proves invalid root shape and duplicate ids share InvalidYaml.
 *
 * @throws Nothing when both structural failures remain unpublished.
 * @note Both fixtures are syntactically valid, separating structural document
 *       validation from parser and node-field conversion failures.
 */
TEST(GraphDocumentLoadErrors,
     StructuralFailuresReturnInvalidYamlWithoutPublishingSessions) {
  ScopedGraphDocumentDirectory directory;
  const std::filesystem::path root_shape =
      directory.root() / "source" / "root_shape.yaml";
  const std::filesystem::path duplicate =
      directory.root() / "source" / "duplicate.yaml";
  write_document(root_shape, "nodes: []\n");
  write_document(duplicate,
                 "- id: 1\n"
                 "  name: first\n"
                 "  type: fixture\n"
                 "  subtype: source\n"
                 "- id: 1\n"
                 "  name: duplicate\n"
                 "  type: fixture\n"
                 "  subtype: source\n");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> invalid_root = host->load_graph(
      make_load_request(directory.root(), "invalid_root", root_shape));
  ASSERT_FALSE(invalid_root.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_root.status),
            GraphErrc::InvalidYaml);
  const Result<GraphSessionId> duplicate_id = host->load_graph(
      make_load_request(directory.root(), "duplicate_id", duplicate));
  ASSERT_FALSE(duplicate_id.status.ok);
  EXPECT_EQ(checked_graph_error_code(duplicate_id.status),
            GraphErrc::InvalidYaml);

  const Result<std::vector<GraphSessionId>> sessions = host->list_graphs();
  ASSERT_TRUE(sessions.status.ok) << sessions.status.message;
  EXPECT_TRUE(sessions.value.empty());
}

/**
 * @brief Proves unresolved graph inputs retain MissingDependency on load.
 *
 * @throws Nothing when the public error and publication contract hold.
 * @note Parsing and node schema conversion both succeed before topology
 *       validation rejects the missing source node.
 */
TEST(GraphDocumentLoadErrors,
     MissingDependencyRetainsExactCodeWithoutPublishingSession) {
  ScopedGraphDocumentDirectory directory;
  const std::filesystem::path missing_dependency =
      directory.root() / "source" / "missing_dependency.yaml";
  write_document(missing_dependency,
                 "- id: 1\n"
                 "  name: broken_child\n"
                 "  type: fixture\n"
                 "  subtype: sink\n"
                 "  image_inputs:\n"
                 "    - from_node_id: 99\n"
                 "      from_output_name: image\n");

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> loaded = host->load_graph(make_load_request(
      directory.root(), "missing_dependency", missing_dependency));
  ASSERT_FALSE(loaded.status.ok);
  EXPECT_EQ(checked_graph_error_code(loaded.status),
            GraphErrc::MissingDependency);

  const Result<std::vector<GraphSessionId>> sessions = host->list_graphs();
  ASSERT_TRUE(sessions.status.ok) << sessions.status.message;
  EXPECT_TRUE(sessions.value.empty());
}

/**
 * @brief Proves cyclic graph topology retains Cycle on initial load.
 *
 * @throws Nothing when the public error and publication contract hold.
 * @note Both dependency endpoints exist, isolating cycle detection from the
 *       missing-dependency category.
 */
TEST(GraphDocumentLoadErrors, CycleRetainsExactCodeWithoutPublishingSession) {
  ScopedGraphDocumentDirectory directory;
  const std::filesystem::path cycle =
      directory.root() / "source" / "cycle.yaml";
  write_document(cycle,
                 "- id: 1\n"
                 "  name: first\n"
                 "  type: fixture\n"
                 "  subtype: transform\n"
                 "  image_inputs:\n"
                 "    - from_node_id: 2\n"
                 "      from_output_name: image\n"
                 "- id: 2\n"
                 "  name: second\n"
                 "  type: fixture\n"
                 "  subtype: transform\n"
                 "  image_inputs:\n"
                 "    - from_node_id: 1\n"
                 "      from_output_name: image\n");

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> loaded =
      host->load_graph(make_load_request(directory.root(), "cycle", cycle));
  ASSERT_FALSE(loaded.status.ok);
  EXPECT_EQ(checked_graph_error_code(loaded.status), GraphErrc::Cycle);

  const Result<std::vector<GraphSessionId>> sessions = host->list_graphs();
  ASSERT_TRUE(sessions.status.ok) << sessions.status.message;
  EXPECT_TRUE(sessions.value.empty());
}

/**
 * @brief Proves an omitted source intentionally creates an empty session.
 *
 * @throws Nothing when the omission and publication contract hold.
 * @note No session-local content document is created before Host load.
 */
TEST(GraphDocumentLoadErrors, OmittedSourceWithoutLocalContentPublishesEmpty) {
  ScopedGraphDocumentDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> loaded = host->load_graph(make_load_request(
      directory.root(), "omitted_empty", std::filesystem::path{}));
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const Result<GraphInspectionView> graph = host->inspect_graph(loaded.value);
  ASSERT_TRUE(graph.status.ok) << graph.status.message;
  EXPECT_TRUE(graph.value.nodes.empty());
}

/**
 * @brief Proves an omitted source consumes existing session-local content.
 *
 * @throws Nothing when the omission and local-document contract hold.
 * @note The fixture bypasses explicit source copying and exercises the
 *       `<root>/<session>/content.yaml` branch directly.
 */
TEST(GraphDocumentLoadErrors, OmittedSourceLoadsExistingLocalContent) {
  ScopedGraphDocumentDirectory directory;
  constexpr char kSession[] = "omitted_local";
  write_document(directory.root() / "sessions" / kSession / "content.yaml",
                 "- id: 7\n"
                 "  name: local_node\n"
                 "  type: fixture\n"
                 "  subtype: source\n");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> loaded = host->load_graph(
      make_load_request(directory.root(), kSession, std::filesystem::path{}));
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const Result<GraphInspectionView> graph = host->inspect_graph(loaded.value);
  ASSERT_TRUE(graph.status.ok) << graph.status.message;
  ASSERT_EQ(graph.value.nodes.size(), 1U);
  EXPECT_EQ(graph.value.nodes.front().id.value, 7);
  EXPECT_EQ(graph.value.nodes.front().name, "local_node");
}

/**
 * @brief Proves explicit source may already be the session content file.
 *
 * @throws Nothing when same-file detection avoids a self-copy IO failure.
 * @note Explicit-path semantics still apply; this case merely requires no
 *       physical copy before parsing the same existing source.
 */
TEST(GraphDocumentLoadErrors, ExplicitSessionContentPathLoadsWithoutSelfCopy) {
  ScopedGraphDocumentDirectory directory;
  constexpr char kSession[] = "explicit_local";
  const std::filesystem::path content =
      directory.root() / "sessions" / kSession / "content.yaml";
  write_document(content,
                 "- id: 8\n"
                 "  name: explicit_local_node\n"
                 "  type: fixture\n"
                 "  subtype: source\n");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> loaded =
      host->load_graph(make_load_request(directory.root(), kSession, content));
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  const Result<GraphInspectionView> graph = host->inspect_graph(loaded.value);
  ASSERT_TRUE(graph.status.ok) << graph.status.message;
  ASSERT_EQ(graph.value.nodes.size(), 1U);
  EXPECT_EQ(graph.value.nodes.front().id.value, 8);
  EXPECT_EQ(graph.value.nodes.front().name, "explicit_local_node");
}

/**
 * @brief Proves resource exhaustion aborts publication and permits retry.
 *
 * @throws Nothing when std::bad_alloc propagates and the retry succeeds.
 * @note The private immutable YAML tag reaches the production conversion path
 *       only in BUILD_TESTING builds; no callable test seam is exported.
 */
TEST(GraphDocumentLoadErrors,
     ResourceExhaustionPropagatesWithoutPublishingAndAllowsRetry) {
  ScopedGraphDocumentDirectory directory;
  const std::filesystem::path source =
      directory.root() / "source" / "resource_exhaustion.yaml";
  write_document(source,
                 "- !photospider-test-reload-bad-alloc\n"
                 "  id: 1\n"
                 "  name: exhausted\n"
                 "  type: fixture\n"
                 "  subtype: source\n");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);
  const GraphLoadRequest request =
      make_load_request(directory.root(), "resource_exhaustion", source);

  EXPECT_THROW((void)host->load_graph(request), std::bad_alloc);
  const Result<std::vector<GraphSessionId>> after_failure = host->list_graphs();
  ASSERT_TRUE(after_failure.status.ok) << after_failure.status.message;
  EXPECT_TRUE(after_failure.value.empty());

  write_document(source,
                 "- id: 1\n"
                 "  name: retry_node\n"
                 "  type: fixture\n"
                 "  subtype: source\n");
  const Result<GraphSessionId> retried = host->load_graph(request);
  ASSERT_TRUE(retried.status.ok) << retried.status.message;
  const Result<GraphInspectionView> graph = host->inspect_graph(retried.value);
  ASSERT_TRUE(graph.status.ok) << graph.status.message;
  ASSERT_EQ(graph.value.nodes.size(), 1U);
  EXPECT_EQ(graph.value.nodes.front().name, "retry_node");
}

/**
 * @brief Proves unexpected ingestion exceptions become Unknown atomically.
 *
 * @throws Nothing when the public classification and publication contract
 *         hold.
 * @note A BUILD_TESTING-only immutable tag raises std::runtime_error from the
 *       real GraphIO conversion stage without exporting a callable seam.
 */
TEST(GraphDocumentLoadErrors,
     UnexpectedFailureReturnsUnknownWithoutPublishingSession) {
  ScopedGraphDocumentDirectory directory;
  const std::filesystem::path source =
      directory.root() / "source" / "unexpected.yaml";
  write_document(source,
                 "- !photospider-test-load-unknown\n"
                 "  id: 1\n"
                 "  name: unexpected\n"
                 "  type: fixture\n"
                 "  subtype: source\n");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const Result<GraphSessionId> loaded = host->load_graph(
      make_load_request(directory.root(), "unexpected", source));
  ASSERT_FALSE(loaded.status.ok);
  EXPECT_EQ(checked_graph_error_code(loaded.status), GraphErrc::Unknown);

  const Result<std::vector<GraphSessionId>> sessions = host->list_graphs();
  ASSERT_TRUE(sessions.status.ok) << sessions.status.message;
  EXPECT_TRUE(sessions.value.empty());
}

/**
 * @brief Proves a duplicate name is lifecycle InvalidParameter, not document
 * IO.
 *
 * @throws Nothing when the original session remains the sole publication.
 * @note The duplicate request uses a missing explicit source; name ownership
 *       must take precedence and must not mutate the original graph.
 */
TEST(GraphDocumentLoadErrors,
     DuplicateSessionPrecedesDocumentPathAndPreservesOriginal) {
  ScopedGraphDocumentDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);
  const GraphSessionId session = load_single_node_graph(
      *host, directory.root(), "duplicate", "original_node");

  const Result<GraphSessionId> duplicate = host->load_graph(
      make_load_request(directory.root(), session.value,
                        directory.root() / "source" / "does_not_exist.yaml"));
  ASSERT_FALSE(duplicate.status.ok);
  EXPECT_EQ(checked_graph_error_code(duplicate.status),
            GraphErrc::InvalidParameter);
  expect_single_node(*host, session, "original_node");
}

/**
 * @brief Proves reload lifecycle validation precedes document classification.
 *
 * @throws Nothing when missing and existing session precedence both hold.
 * @note Empty input is intentionally reused for both calls: absence must be
 *       NotFound, while an existing session must reject it as InvalidParameter
 *       without changing the visible node.
 */
TEST(GraphDocumentReloadErrors,
     MissingSessionPrecedesEmptyPathAndExistingSessionRejectsIt) {
  ScopedGraphDocumentDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);

  const VoidResult missing =
      host->reload_graph(GraphSessionId{"missing_session"}, "");
  ASSERT_FALSE(missing.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing.status), GraphErrc::NotFound);

  const GraphSessionId session = load_single_node_graph(
      *host, directory.root(), "reload_empty", "original_node");
  const VoidResult empty = host->reload_graph(session, "");
  ASSERT_FALSE(empty.status.ok);
  EXPECT_EQ(checked_graph_error_code(empty.status),
            GraphErrc::InvalidParameter);
  expect_single_node(*host, session, "original_node");
}

/**
 * @brief Exercises the recoverable reload matrix on one published graph.
 *
 * @throws Nothing when every exact category preserves the original node.
 * @note Sequential failures cover IO, syntax, semantic schema, missing
 *       dependency, cycle, and unexpected internal normalization without
 *       replacing or closing the session between cases.
 */
TEST(GraphDocumentReloadErrors,
     RecoverableMatrixPreservesPublishedGraphAndExactCategories) {
  ScopedGraphDocumentDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);
  const GraphSessionId session = load_single_node_graph(
      *host, directory.root(), "reload_matrix", "original_node");
  const std::filesystem::path source_root = directory.root() / "source";

  expect_reload_failure_preserves(*host, session,
                                  source_root / "missing_reload.yaml",
                                  GraphErrc::Io, "original_node");

  const std::filesystem::path syntax = source_root / "reload_syntax.yaml";
  write_document(syntax, "[unterminated\n");
  expect_reload_failure_preserves(*host, session, syntax,
                                  GraphErrc::InvalidYaml, "original_node");

  const std::filesystem::path schema = source_root / "reload_schema.yaml";
  write_document(schema,
                 "- id: 2\n"
                 "  name: invalid_edge\n"
                 "  type: fixture\n"
                 "  subtype: source\n"
                 "  parameter_inputs:\n"
                 "    - from_node_id: 1\n"
                 "      from_output_name: ''\n"
                 "      to_parameter_name: ''\n");
  expect_reload_failure_preserves(*host, session, schema,
                                  GraphErrc::InvalidYaml, "original_node");

  const std::filesystem::path dependency =
      source_root / "reload_dependency.yaml";
  write_document(dependency,
                 "- id: 2\n"
                 "  name: missing_dependency\n"
                 "  type: fixture\n"
                 "  subtype: sink\n"
                 "  image_inputs:\n"
                 "    - from_node_id: 99\n"
                 "      from_output_name: image\n");
  expect_reload_failure_preserves(*host, session, dependency,
                                  GraphErrc::MissingDependency,
                                  "original_node");

  const std::filesystem::path cycle = source_root / "reload_cycle.yaml";
  write_document(cycle,
                 "- id: 1\n"
                 "  name: first\n"
                 "  type: fixture\n"
                 "  subtype: transform\n"
                 "  image_inputs:\n"
                 "    - from_node_id: 2\n"
                 "- id: 2\n"
                 "  name: second\n"
                 "  type: fixture\n"
                 "  subtype: transform\n"
                 "  image_inputs:\n"
                 "    - from_node_id: 1\n");
  expect_reload_failure_preserves(*host, session, cycle, GraphErrc::Cycle,
                                  "original_node");

  const std::filesystem::path unexpected =
      source_root / "reload_unexpected.yaml";
  write_document(unexpected,
                 "- !photospider-test-load-unknown\n"
                 "  id: 2\n"
                 "  name: unexpected\n"
                 "  type: fixture\n"
                 "  subtype: source\n");
  expect_reload_failure_preserves(*host, session, unexpected,
                                  GraphErrc::Unknown, "original_node");
}

/**
 * @brief Proves reload bad_alloc preserves state and a valid retry commits.
 *
 * @throws Nothing when resource exhaustion propagates and retry succeeds.
 * @note The same session remains inspectable after the exception and is then
 *       replaced by a normal document, proving failure did not poison it.
 */
TEST(GraphDocumentReloadErrors,
     ResourceExhaustionPreservesPublishedGraphAndValidRetryCommits) {
  ScopedGraphDocumentDirectory directory;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  configure_document_host(*host);
  const GraphSessionId session = load_single_node_graph(
      *host, directory.root(), "reload_resource", "original_node");
  const std::filesystem::path replacement =
      directory.root() / "source" / "reload_resource.yaml";
  write_document(replacement,
                 "- !photospider-test-reload-bad-alloc\n"
                 "  id: 2\n"
                 "  name: exhausted_replacement\n"
                 "  type: fixture\n"
                 "  subtype: source\n");

  EXPECT_THROW((void)host->reload_graph(session, replacement.string()),
               std::bad_alloc);
  expect_single_node(*host, session, "original_node");

  write_document(replacement,
                 "- id: 2\n"
                 "  name: committed_replacement\n"
                 "  type: fixture\n"
                 "  subtype: source\n");
  const VoidResult retried = host->reload_graph(session, replacement.string());
  ASSERT_TRUE(retried.status.ok) << retried.status.message;
  const Result<GraphInspectionView> graph = host->inspect_graph(session);
  ASSERT_TRUE(graph.status.ok) << graph.status.message;
  ASSERT_EQ(graph.value.nodes.size(), 1U);
  EXPECT_EQ(graph.value.nodes.front().id.value, 2);
  EXPECT_EQ(graph.value.nodes.front().name, "committed_replacement");
}

/**
 * @brief Proves GraphModel replacement commits all topology/runtime state once.
 *
 * @throws Nothing when failed load is invisible and valid load resets state.
 * @note This direct model test observes topology generation and runtime fields
 *       that public Host intentionally does not expose.
 */
TEST(GraphDocumentModelTransaction,
     FailedReplacementPreservesGenerationTopologyAndRuntimeState) {
  ScopedGraphDocumentDirectory directory;
  const std::filesystem::path valid =
      directory.root() / "source" / "model_valid.yaml";
  const std::filesystem::path invalid =
      directory.root() / "source" / "model_invalid.yaml";
  const std::filesystem::path replacement =
      directory.root() / "source" / "model_replacement.yaml";
  write_document(valid,
                 "- id: 1\n"
                 "  name: source\n"
                 "  type: fixture\n"
                 "  subtype: source\n"
                 "- id: 2\n"
                 "  name: child\n"
                 "  type: fixture\n"
                 "  subtype: sink\n"
                 "  image_inputs:\n"
                 "    - from_node_id: 1\n");
  write_document(invalid,
                 "- id: 3\n"
                 "  name: broken\n"
                 "  type: fixture\n"
                 "  subtype: sink\n"
                 "  image_inputs:\n"
                 "    - from_node_id: 99\n");
  write_document(replacement,
                 "- id: 4\n"
                 "  name: replacement\n"
                 "  type: fixture\n"
                 "  subtype: source\n");

  GraphModel graph(std::filesystem::path{});
  GraphIOService io;
  io.load(graph, valid);
  const std::uint64_t generation = graph.topology_generation();
  ASSERT_EQ(graph.upstream_edges(2).size(), 1U);
  graph.timing_results.total_ms = 42.0;
  graph.set_skip_save_cache(true);

  try {
    io.load(graph, invalid);
    FAIL() << "missing dependency replacement unexpectedly committed";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::MissingDependency);
  }
  EXPECT_EQ(graph.topology_generation(), generation);
  ASSERT_TRUE(graph.has_node(1));
  ASSERT_TRUE(graph.has_node(2));
  EXPECT_EQ(graph.node(1).name, "source");
  ASSERT_EQ(graph.upstream_edges(2).size(), 1U);
  EXPECT_EQ(graph.upstream_edges(2).front().from_node_id, 1);
  EXPECT_DOUBLE_EQ(graph.timing_results.total_ms, 42.0);
  EXPECT_TRUE(graph.skip_save_cache());

  io.load(graph, replacement);
  EXPECT_EQ(graph.topology_generation(), generation + 1U);
  EXPECT_FALSE(graph.has_node(1));
  EXPECT_FALSE(graph.has_node(2));
  ASSERT_TRUE(graph.has_node(4));
  EXPECT_DOUBLE_EQ(graph.timing_results.total_ms, 0.0);
  EXPECT_FALSE(graph.skip_save_cache());
}

}  // namespace
}  // namespace ps
