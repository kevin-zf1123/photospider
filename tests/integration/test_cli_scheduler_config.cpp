#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "graph_cli/cli_config.hpp"
#include "photospider/core/result_types.hpp"
#include "photospider/scheduler/scheduler.hpp"
#include "support/ipc_host_spy.hpp"

namespace {

/**
 * @brief Owns one unique temporary directory for CLI configuration tests.
 *
 * @throws std::filesystem::filesystem_error If creation cannot complete.
 * @note Destruction performs best-effort cleanup and never masks an assertion.
 */
class ScopedCliConfigTempDir final {
 public:
  /**
   * @brief Creates an empty temporary directory with the requested stem.
   *
   * @param stem Human-readable prefix used for the unique directory name.
   * @throws std::filesystem::filesystem_error If cleanup or creation fails.
   */
  explicit ScopedCliConfigTempDir(const std::string& stem)
      : root_(std::filesystem::temp_directory_path() /
              (stem + "_" +
               std::to_string(std::chrono::high_resolution_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  ScopedCliConfigTempDir(const ScopedCliConfigTempDir&) = delete;
  ScopedCliConfigTempDir& operator=(const ScopedCliConfigTempDir&) = delete;

  /**
   * @brief Removes the owned directory without throwing.
   * @throws Nothing.
   */
  ~ScopedCliConfigTempDir() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /**
   * @brief Returns the owned directory root.
   * @return Borrowed path valid for this helper's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Unique directory removed by the destructor. */
  std::filesystem::path root_;
};

/**
 * @brief Writes one focused CLI configuration fixture.
 *
 * @param path Destination YAML path.
 * @param worker_count Exact scalar emitted for scheduler worker configuration.
 * @param cache_root Sentinel cache root used to prove transactional parsing.
 * @return Nothing.
 * @throws std::runtime_error If the fixture cannot be opened or fully written.
 * @note The YAML intentionally contains only fields relevant to this boundary.
 */
void write_scheduler_config(const std::filesystem::path& path, int worker_count,
                            const std::string& cache_root) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open CLI scheduler config fixture");
  }
  output << "cache_root_dir: " << cache_root << '\n'
         << "scheduler_hp_type: cpu_work_stealing\n"
         << "scheduler_rt_type: serial_debug\n"
         << "scheduler_worker_count: " << worker_count << '\n';
  if (!output) {
    throw std::runtime_error("failed to write CLI scheduler config fixture");
  }
}

/**
 * @brief Accepts both automatic and exact-ceiling YAML worker counts.
 * @throws Nothing when complete YAML parsing publishes each boundary value.
 */
TEST(CliSchedulerConfigParsing, AcceptsAutomaticAndExactCeilingValues) {
  ScopedCliConfigTempDir directory("photospider_cli_scheduler_bounds");
  const std::array<int, 2> accepted = {
      0, static_cast<int>(ps::kSchedulerWorkerRequestMax)};

  for (const int worker_count : accepted) {
    const auto path = directory.root() /
                      ("accepted_" + std::to_string(worker_count) + ".yaml");
    write_scheduler_config(path, worker_count, "accepted-cache");
    CliConfig config;
    config.scheduler_worker_count = 3;

    load_or_create_config(path.string(), config);

    EXPECT_EQ(config.scheduler_worker_count, worker_count);
    EXPECT_EQ(config.cache_root_dir, "accepted-cache");
    EXPECT_EQ(config.scheduler_rt_type, "serial_debug");
    EXPECT_EQ(config.loaded_config_path,
              std::filesystem::absolute(path).string());
  }
}

/**
 * @brief Rejects negative and limit-plus-one YAML without partial publication.
 * @throws Nothing when each invalid document retains the full prior snapshot.
 */
TEST(CliSchedulerConfigParsing, RejectsOutOfRangeValuesTransactionally) {
  ScopedCliConfigTempDir directory("photospider_cli_scheduler_rejected");
  const std::array<int, 2> rejected = {
      -1, static_cast<int>(ps::kSchedulerWorkerRequestMax) + 1};

  for (const int worker_count : rejected) {
    const auto path = directory.root() /
                      ("rejected_" + std::to_string(worker_count) + ".yaml");
    write_scheduler_config(path, worker_count, "must-not-publish");
    CliConfig config;
    config.loaded_config_path = "prior-config-path";
    config.cache_root_dir = "prior-cache";
    config.scheduler_hp_type = "serial_debug";
    config.scheduler_rt_type = "cpu_work_stealing";
    config.scheduler_worker_count = 3;

    load_or_create_config(path.string(), config);

    EXPECT_EQ(config.loaded_config_path, "prior-config-path");
    EXPECT_EQ(config.cache_root_dir, "prior-cache");
    EXPECT_EQ(config.scheduler_hp_type, "serial_debug");
    EXPECT_EQ(config.scheduler_rt_type, "cpu_work_stealing");
    EXPECT_EQ(config.scheduler_worker_count, 3);
  }
}

/**
 * @brief Strictly accepts editor boundary text and rejects malformed values.
 * @throws Nothing when parsing never coerces or partially consumes input.
 */
TEST(CliSchedulerConfigParsing, StrictEditorParserEnforcesCompleteRange) {
  EXPECT_EQ(parse_cli_scheduler_worker_count("0"), 0);
  EXPECT_EQ(parse_cli_scheduler_worker_count("8"),
            static_cast<int>(ps::kSchedulerWorkerRequestMax));
  for (const std::string invalid : {"-1", "9", "8workers", "", " 8"}) {
    EXPECT_THROW(parse_cli_scheduler_worker_count(invalid),
                 std::invalid_argument)
        << invalid;
  }
}

/**
 * @brief Prevents invalid in-memory CLI values from reaching the Host.
 * @throws Nothing when validation raises `std::invalid_argument` first.
 */
TEST(CliSchedulerConfigApply, RejectsInvalidValuesBeforeHostAccess) {
  ps::testing::IpcHostSpy host;
  CliConfig config;

  for (const int worker_count :
       {-1, static_cast<int>(ps::kSchedulerWorkerRequestMax) + 1}) {
    config.scheduler_worker_count = worker_count;
    EXPECT_THROW(apply_cli_scheduler_defaults(host, config),
                 std::invalid_argument);
  }
  EXPECT_EQ(host.call_count("scheduler.configure_defaults"), 0U);
}

/**
 * @brief Routes automatic and exact-ceiling values without coercion.
 * @throws Nothing when each accepted snapshot reaches the Host exactly once.
 */
TEST(CliSchedulerConfigApply, PreservesAcceptedValuesAtHostBoundary) {
  ps::testing::IpcHostSpy host;
  CliConfig config;
  config.scheduler_hp_type = "cpu_work_stealing";
  config.scheduler_rt_type = "serial_debug";

  config.scheduler_worker_count = 0;
  EXPECT_NO_THROW(apply_cli_scheduler_defaults(host, config));
  config.scheduler_worker_count =
      static_cast<int>(ps::kSchedulerWorkerRequestMax);
  EXPECT_NO_THROW(apply_cli_scheduler_defaults(host, config));

  const auto invocations = host.invocations();
  ASSERT_EQ(invocations.size(), 2U);
  EXPECT_EQ(invocations[0].method, "scheduler.configure_defaults");
  EXPECT_EQ(invocations[0].text, "cpu_work_stealing\nserial_debug");
  EXPECT_EQ(invocations[0].worker_count, 0U);
  EXPECT_EQ(invocations[1].method, "scheduler.configure_defaults");
  EXPECT_EQ(invocations[1].text, "cpu_work_stealing\nserial_debug");
  EXPECT_EQ(invocations[1].worker_count, ps::kSchedulerWorkerRequestMax);
}

/**
 * @brief Surfaces a failed Host scheduler-default status to the CLI boundary.
 * @throws Nothing when the shared apply helper raises after exactly one call.
 */
TEST(CliSchedulerConfigApply, PropagatesHostRejection) {
  ps::testing::IpcHostSpy host;
  host.set_status(
      "scheduler.configure_defaults",
      ps::OperationStatus{
          false, ps::OperationErrorDomain::Graph,
          static_cast<std::int32_t>(ps::GraphErrc::InvalidParameter),
          "invalid_parameter", "scheduler defaults rejected by test Host"});
  CliConfig config;
  config.scheduler_worker_count = 1;

  EXPECT_THROW(apply_cli_scheduler_defaults(host, config), std::runtime_error);
  EXPECT_EQ(host.call_count("scheduler.configure_defaults"), 1U);
}

/**
 * @brief Stops CLI startup before graph work when Host defaults are rejected.
 * @throws Nothing when the reusable run boundary reports exit code two.
 */
TEST(CliSchedulerConfigApply, HostRejectionStopsRunGraphCliStartup) {
  ScopedCliConfigTempDir directory("photospider_cli_scheduler_startup");
  const auto config_path = directory.root() / "config.yaml";
  write_scheduler_config(config_path, 1, "startup-cache");

  ps::testing::IpcHostSpy host;
  host.set_status(
      "scheduler.configure_defaults",
      ps::OperationStatus{
          false, ps::OperationErrorDomain::Graph,
          static_cast<std::int32_t>(ps::GraphErrc::InvalidParameter),
          "invalid_parameter", "startup scheduler defaults rejected"});
  std::array<std::string, 5> arguments = {
      "graph_cli", "--config", config_path.string(), "--read", "ignored.yaml"};
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }

  EXPECT_EQ(run_graph_cli(static_cast<int>(argv.size()), argv.data(), host), 2);
  EXPECT_EQ(host.call_count("scheduler.configure_defaults"), 1U);
  EXPECT_EQ(host.call_count("graph.load"), 0U);
}

}  // namespace
