#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "photospider/core/graph_error.hpp"
#include "photospider/host/host.hpp"

namespace ps {
namespace {

/**
 * @brief Owns one isolated temporary root for execution configuration tests.
 *
 * @throws std::filesystem::filesystem_error if the root cannot be prepared.
 * @note Destruction uses the non-throwing error-code overload so cleanup never
 *       replaces a test assertion or an exception under test.
 */
class ScopedExecutionTempDir final {
 public:
  /**
   * @brief Creates an empty uniquely named directory.
   * @param label Stable test label used as the directory prefix.
   * @throws std::filesystem::filesystem_error if setup fails.
   */
  explicit ScopedExecutionTempDir(const std::string& label)
      : root_(std::filesystem::temp_directory_path() /
              (label + "_" +
               std::to_string(std::chrono::high_resolution_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  /** @brief Removes the owned directory without throwing. */
  ~ScopedExecutionTempDir() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  ScopedExecutionTempDir(const ScopedExecutionTempDir&) = delete;
  ScopedExecutionTempDir& operator=(const ScopedExecutionTempDir&) = delete;

  /**
   * @brief Returns the owned root.
   * @return Borrowed path valid for this helper's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Unique temporary directory removed at destruction. */
  std::filesystem::path root_;
};

/**
 * @brief Creates one empty-Graph load request below an isolated root.
 * @param root Temporary root that outlives the Host call.
 * @param session Unique session label.
 * @return Fully owned public request with no source document.
 * @throws std::bad_alloc if copied path or label storage cannot allocate.
 * @note An empty Graph is sufficient to observe future-session route binding
 *       without introducing operation plugin or cache behavior.
 */
GraphLoadRequest make_empty_load_request(const std::filesystem::path& root,
                                         const std::string& session) {
  GraphLoadRequest request;
  request.session = GraphSessionId{session};
  request.root_dir = (root / "sessions").string();
  request.cache_root_dir = (root / "cache").string();
  return request;
}

/**
 * @brief Configures symmetric HP/RT routes with one process worker request.
 * @param host Public Host whose future-session defaults are updated.
 * @param type Exact private execution route.
 * @param worker_count Zero for automatic selection or an exact count in range.
 * @return Host result without interpretation.
 * @throws std::bad_alloc if copied request or status storage cannot allocate.
 * @note The helper changes no policy binding and exposes no physical owner.
 */
VoidResult configure_execution(Host& host, const std::string& type,
                               unsigned int worker_count) {
  HostExecutionConfig config;
  config.hp_type = type;
  config.rt_type = type;
  config.worker_count = worker_count;
  return host.configure_execution_defaults(config);
}

/**
 * @brief Verifies the private execution vocabulary is exact and closed.
 * @return Nothing; GoogleTest records contract mismatches.
 * @throws std::bad_alloc from public snapshot construction.
 * @note Removed scheduler names and the heterogeneous alias are rejected
 *       rather than translated through compatibility logic.
 */
TEST(EmbeddedHostExecutionConfiguration, ExposesOnlyPrivateExecutionRoutes) {
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const Result<std::vector<std::string>> types =
      host->execution_available_types();
  ASSERT_TRUE(types.status.ok) << types.status.message;
  EXPECT_EQ(types.value,
            (std::vector<std::string>{"cpu", "gpu_pipeline", "serial_debug"}));

  for (const std::string& type : types.value) {
    const Result<std::string> description = host->execution_description(type);
    ASSERT_TRUE(description.status.ok) << description.status.message;
    EXPECT_FALSE(description.value.empty());
  }
  for (const std::string& removed :
       {"cpu_work_stealing", "heterogeneous", "scheduler"}) {
    const Result<std::string> description =
        host->execution_description(removed);
    EXPECT_FALSE(description.status.ok);
    EXPECT_EQ(description.status.domain, OperationErrorDomain::Graph);
    EXPECT_EQ(checked_graph_error_code(description.status),
              GraphErrc::NotFound);
  }
}

/**
 * @brief Proves an out-of-range worker request preserves accepted defaults.
 * @return Nothing; GoogleTest records status and route mismatches.
 * @throws Filesystem or allocation failures from ordinary fixture setup.
 * @note Validation occurs before starting or resizing the private process pool.
 */
TEST(EmbeddedHostExecutionConfiguration,
     RejectsWorkerOverflowWithoutChangingFutureRoutes) {
  ScopedExecutionTempDir temp("photospider_execution_worker_limit");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const VoidResult accepted = configure_execution(*host, "serial_debug", 0U);
  ASSERT_TRUE(accepted.status.ok) << accepted.status.message;

  HostExecutionConfig rejected;
  rejected.hp_type = "cpu";
  rejected.rt_type = "cpu";
  rejected.worker_count = kExecutionWorkerRequestMax + 1U;
  const VoidResult rejected_result =
      host->configure_execution_defaults(rejected);
  ASSERT_FALSE(rejected_result.status.ok);
  EXPECT_EQ(rejected_result.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(rejected_result.status),
            GraphErrc::InvalidParameter);

  const GraphSessionId session{"preserved_serial_execution_defaults"};
  const Result<GraphSessionId> loaded =
      host->load_graph(make_empty_load_request(temp.root(), session.value));
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  for (const ComputeIntent intent :
       {ComputeIntent::GlobalHighPrecision, ComputeIntent::RealTimeUpdate}) {
    const Result<ExecutionInfoSnapshot> info =
        host->execution_info(session, intent);
    ASSERT_TRUE(info.status.ok) << info.status.message;
    EXPECT_EQ(info.value.intent, intent);
    EXPECT_EQ(info.value.execution_type, "serial_debug");
  }
  EXPECT_TRUE(host->close_graph(session).status.ok);
}

/**
 * @brief Proves one Host pool is fixed while independent Hosts remain isolated.
 * @return Nothing; GoogleTest records configuration mismatches.
 * @throws std::bad_alloc or std::system_error if a fixed pool cannot start.
 * @note A zero/equal request reuses the pool. A different positive request is
 *       rejected without affecting another Host composition.
 */
TEST(EmbeddedHostExecutionConfiguration,
     FixesOnePoolPerHostAndKeepsHostCompositionsIndependent) {
  std::unique_ptr<Host> first = create_embedded_host();
  std::unique_ptr<Host> second = create_embedded_host();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  const VoidResult first_configured = configure_execution(*first, "cpu", 1U);
  ASSERT_TRUE(first_configured.status.ok) << first_configured.status.message;
  const VoidResult second_configured = configure_execution(*second, "cpu", 2U);
  ASSERT_TRUE(second_configured.status.ok) << second_configured.status.message;

  const VoidResult conflicting = configure_execution(*first, "cpu", 2U);
  ASSERT_FALSE(conflicting.status.ok);
  EXPECT_EQ(checked_graph_error_code(conflicting.status),
            GraphErrc::InvalidParameter);
  EXPECT_TRUE(configure_execution(*first, "cpu", 0U).status.ok);
  EXPECT_TRUE(configure_execution(*first, "cpu", 1U).status.ok);
  EXPECT_TRUE(configure_execution(*second, "cpu", 2U).status.ok);
}

/**
 * @brief Verifies ownerless session route replacement is validation-first.
 * @return Nothing; GoogleTest records replacement and snapshot mismatches.
 * @throws Filesystem or allocation failures from ordinary setup and snapshots.
 * @note Unknown route failure preserves the prior generation's copied route;
 *       no plugin, worker grant, or per-Graph executor owner participates.
 */
TEST(EmbeddedHostExecutionConfiguration,
     ReplacesSessionRouteAndPreservesItAfterInvalidCandidate) {
  ScopedExecutionTempDir temp("photospider_execution_route_replacement");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  ASSERT_TRUE(configure_execution(*host, "serial_debug", 0U).status.ok);

  const GraphSessionId session{"execution_route_replacement"};
  const Result<GraphSessionId> loaded =
      host->load_graph(make_empty_load_request(temp.root(), session.value));
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const VoidResult replaced = host->replace_execution(
      session, ComputeIntent::GlobalHighPrecision, "cpu");
  ASSERT_TRUE(replaced.status.ok) << replaced.status.message;
  const Result<ExecutionInfoSnapshot> cpu_info =
      host->execution_info(session, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(cpu_info.status.ok) << cpu_info.status.message;
  EXPECT_EQ(cpu_info.value.execution_type, "cpu");

  const VoidResult invalid = host->replace_execution(
      session, ComputeIntent::GlobalHighPrecision, "cpu_work_stealing");
  ASSERT_FALSE(invalid.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid.status),
            GraphErrc::InvalidParameter);
  const Result<ExecutionInfoSnapshot> retained =
      host->execution_info(session, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(retained.status.ok) << retained.status.message;
  EXPECT_EQ(retained.value.execution_type, "cpu");

  const GraphSessionId missing{"missing_execution_session"};
  const VoidResult missing_result = host->replace_execution(
      missing, ComputeIntent::GlobalHighPrecision, "serial_debug");
  EXPECT_FALSE(missing_result.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_result.status),
            GraphErrc::NotFound);
  EXPECT_TRUE(host->close_graph(session).status.ok);
}

}  // namespace
}  // namespace ps
