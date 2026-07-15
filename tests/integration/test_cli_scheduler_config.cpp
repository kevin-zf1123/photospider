#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "graph_cli/cli_config.hpp"
#include "graph_cli/command/commands.hpp"
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
 * @brief Temporarily changes the process working directory for one CLI test.
 *
 * @throws std::filesystem::filesystem_error If reading or changing the current
 * directory fails.
 * @note Tests using this guard must not run other working-directory-dependent
 * code concurrently. Destruction restores the exact previous directory.
 */
class ScopedCurrentWorkingDirectory final {
 public:
  /**
   * @brief Saves the current directory and enters the supplied test root.
   * @param path Existing directory that becomes current for this scope.
   * @throws std::filesystem::filesystem_error If either filesystem operation
   * fails.
   * @note The guard owns process-global current-directory restoration and must
   * remain local to a non-concurrent test scope.
   */
  explicit ScopedCurrentWorkingDirectory(const std::filesystem::path& path)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(path);
  }

  /**
   * @brief Restores the exact directory captured by construction.
   * @throws Nothing; restoration failure terminates because later tests could
   * otherwise mutate an unknown filesystem location.
   * @note Destruction is the only restoration path and runs exactly once.
   */
  ~ScopedCurrentWorkingDirectory() noexcept {
    std::filesystem::current_path(previous_);
  }

  /**
   * @brief Prevents duplicate restoration ownership.
   * @param other Guard whose process-global restoration ownership must not be
   * copied.
   * @throws Nothing because construction is unavailable.
   * @note Copying would permit two destructors to restore the same snapshot.
   */
  ScopedCurrentWorkingDirectory(const ScopedCurrentWorkingDirectory& other) =
      delete;

  /**
   * @brief Prevents replacement of restoration ownership.
   * @param other Guard whose restoration ownership must not replace this one.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   * @note Assignment would orphan one captured current-directory snapshot.
   */
  ScopedCurrentWorkingDirectory& operator=(
      const ScopedCurrentWorkingDirectory& other) = delete;

 private:
  /** @brief Original process directory restored at scope exit. */
  std::filesystem::path previous_;
};

/**
 * @brief Writes one focused CLI configuration fixture.
 *
 * @param path Destination YAML path.
 * @param worker_count Exact scalar emitted for scheduler worker configuration.
 * @param cache_root Sentinel cache root used to prove transactional parsing.
 * @param switch_after_load Whether interactive load selects the new session.
 * @return Nothing.
 * @throws std::runtime_error If the fixture cannot be opened or fully written.
 * @note The YAML intentionally contains only fields relevant to this boundary.
 */
void write_scheduler_config(const std::filesystem::path& path, int worker_count,
                            const std::string& cache_root,
                            bool switch_after_load = true) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open CLI scheduler config fixture");
  }
  output << "cache_root_dir: " << cache_root << '\n'
         << "scheduler_hp_type: cpu_work_stealing\n"
         << "scheduler_rt_type: serial_debug\n"
         << "scheduler_worker_count: " << worker_count << '\n'
         << "switch_after_load: " << (switch_after_load ? "true" : "false")
         << '\n';
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

/**
 * @brief Keeps later option actions bound to the graph loaded in this run.
 * @return Nothing; GoogleTest assertions report action-targeting failures.
 * @throws Nothing when output receives the Host-returned session id even while
 * interactive switch-after-load policy is disabled.
 * @note The disabled policy applies only to interactive session switching.
 */
TEST(CliOptionActions, LoadedSessionFeedsLaterOutputWhenSwitchPolicyIsOff) {
  ScopedCliConfigTempDir directory("photospider_cli_option_session_chain");
  const auto config_path = directory.root() / "config.yaml";
  write_scheduler_config(config_path, 1, "option-cache",
                         /*switch_after_load=*/false);

  ps::testing::IpcHostSpy host(ps::GraphSessionId{"loaded-option-session"});
  std::array<std::string, 7> arguments = {
      "graph_cli",  "--config", config_path.string(), "-r",
      "input.yaml", "-o",       "output.yaml"};
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }

  EXPECT_EQ(run_graph_cli(static_cast<int>(argv.size()), argv.data(), host), 0);
  EXPECT_EQ(host.call_count("graph.save"), 1U);
  const auto invocations = host.invocations();
  const auto saved = std::find_if(
      invocations.begin(), invocations.end(),
      [](const auto& invocation) { return invocation.method == "graph.save"; });
  ASSERT_NE(saved, invocations.end());
  EXPECT_EQ(saved->session.value, "loaded-option-session");
  EXPECT_EQ(saved->text, "output.yaml");
}

/**
 * @brief Treats short traversal as an argument-free option at end of argv.
 * @return Nothing; GoogleTest assertions report parsing/dispatch failures.
 * @throws Nothing when parsing reaches traversal and targets the loaded graph.
 * @note Terminal placement proves `getopt_long` does not consume a following
 * argument for `-t`.
 */
TEST(CliOptionActions, ShortTraversalNeedsNoFollowingArgument) {
  ScopedCliConfigTempDir directory("photospider_cli_short_traversal");
  const auto config_path = directory.root() / "config.yaml";
  write_scheduler_config(config_path, 1, "traversal-cache");

  ps::testing::IpcHostSpy host(ps::GraphSessionId{"traversal-session"});
  std::array<std::string, 6> arguments = {
      "graph_cli", "--config", config_path.string(), "-r", "input.yaml", "-t"};
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }

  EXPECT_EQ(run_graph_cli(static_cast<int>(argv.size()), argv.data(), host), 0);
  EXPECT_EQ(host.call_count("inspect.dependency_tree"), 1U);
  EXPECT_EQ(host.call_count("inspect.traversal_orders"), 1U);
  const auto invocations = host.invocations();
  const auto traversed = std::find_if(
      invocations.begin(), invocations.end(), [](const auto& invocation) {
        return invocation.method == "inspect.traversal_orders";
      });
  ASSERT_NE(traversed, invocations.end());
  EXPECT_EQ(traversed->session.value, "traversal-session");
}

/**
 * @brief Aborts session copy when serializing the source graph fails.
 * @return Nothing; GoogleTest assertions report precondition side effects.
 * @throws std::filesystem::filesystem_error or std::bad_alloc if fixture setup
 * cannot complete.
 * @note A stale source file is deliberately present; copying it would prove
 * the handler ignored the authoritative Host save failure.
 */
TEST(CliSessionSwitch, CopyRequiresSuccessfulSourceSave) {
  ScopedCliConfigTempDir directory("photospider_cli_switch_save_gate");
  ScopedCurrentWorkingDirectory current_directory(directory.root());
  const auto source_directory =
      std::filesystem::path("sessions") / "source-session";
  std::filesystem::create_directories(source_directory);
  std::ofstream stale_yaml(source_directory / "content.yaml");
  ASSERT_TRUE(stale_yaml.is_open());
  stale_yaml << "- id: 1\n  type: stale\n";
  stale_yaml.close();
  ASSERT_TRUE(stale_yaml);

  ps::testing::IpcHostSpy host;
  host.set_status(
      "graph.save",
      ps::OperationStatus{false, ps::OperationErrorDomain::Graph,
                          static_cast<std::int32_t>(ps::GraphErrc::Io), "io",
                          "source serialization failed"});
  std::string current_graph = "source-session";
  bool modified = true;
  CliConfig config;
  config.session_warning = false;
  config.loaded_config_path.clear();
  std::istringstream command("target-session c");

  EXPECT_TRUE(handle_switch(command, host, current_graph, modified, config));
  EXPECT_EQ(host.call_count("graph.save"), 1U);
  EXPECT_EQ(host.call_count("graph.load"), 0U);
  EXPECT_EQ(host.call_count("graph.reload"), 0U);
  EXPECT_EQ(current_graph, "source-session");
  EXPECT_TRUE(modified);
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path("sessions") /
                                       "target-session"));
}

}  // namespace
