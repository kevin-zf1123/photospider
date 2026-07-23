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
 * @param worker_count Exact scalar emitted for execution worker configuration.
 * @param cache_root Sentinel cache root used to prove transactional parsing.
 * @param switch_after_load Whether interactive load selects the new session.
 * @return Nothing.
 * @throws std::runtime_error If the fixture cannot be opened or fully written.
 * @note The YAML intentionally contains only fields relevant to this boundary.
 */
void write_policy_execution_config(const std::filesystem::path& path,
                                   int worker_count,
                                   const std::string& cache_root,
                                   bool switch_after_load = true) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error(
        "failed to open CLI policy/execution config fixture");
  }
  output << "cache_root_dir: " << cache_root << '\n'
         << "policy_interactive_type: interactive\n"
         << "policy_throughput_type: throughput\n"
         << "execution_hp_type: cpu\n"
         << "execution_rt_type: serial_debug\n"
         << "execution_worker_count: " << worker_count << '\n'
         << "switch_after_load: " << (switch_after_load ? "true" : "false")
         << '\n';
  if (!output) {
    throw std::runtime_error(
        "failed to write CLI policy/execution config fixture");
  }
}

/**
 * @brief Accepts both automatic and exact-ceiling YAML worker counts.
 * @throws Nothing when complete YAML parsing publishes each boundary value.
 */
TEST(CliPolicyExecutionConfigParsing, AcceptsAutomaticAndExactCeilingValues) {
  ScopedCliConfigTempDir directory("photospider_cli_execution_bounds");
  const std::array<int, 2> accepted = {
      0, static_cast<int>(ps::kExecutionWorkerRequestMax)};

  for (const int worker_count : accepted) {
    const auto path = directory.root() /
                      ("accepted_" + std::to_string(worker_count) + ".yaml");
    write_policy_execution_config(path, worker_count, "accepted-cache");
    CliConfig config;
    config.execution_worker_count = 3;

    load_or_create_config(path.string(), config);

    EXPECT_EQ(config.execution_worker_count, worker_count);
    EXPECT_EQ(config.cache_root_dir, "accepted-cache");
    EXPECT_EQ(config.policy_interactive_type, "interactive");
    EXPECT_EQ(config.policy_throughput_type, "throughput");
    EXPECT_EQ(config.execution_hp_type, "cpu");
    EXPECT_EQ(config.execution_rt_type, "serial_debug");
    EXPECT_EQ(config.loaded_config_path,
              std::filesystem::absolute(path).string());
  }
}

/**
 * @brief Rejects negative and limit-plus-one YAML without partial publication.
 * @throws Nothing when each invalid document retains the full prior snapshot.
 */
TEST(CliPolicyExecutionConfigParsing, RejectsOutOfRangeValuesTransactionally) {
  ScopedCliConfigTempDir directory("photospider_cli_execution_rejected");
  const std::array<int, 2> rejected = {
      -1, static_cast<int>(ps::kExecutionWorkerRequestMax) + 1};

  for (const int worker_count : rejected) {
    const auto path = directory.root() /
                      ("rejected_" + std::to_string(worker_count) + ".yaml");
    write_policy_execution_config(path, worker_count, "must-not-publish");
    CliConfig config;
    config.loaded_config_path = "prior-config-path";
    config.cache_root_dir = "prior-cache";
    config.policy_interactive_type = "prior_interactive";
    config.policy_throughput_type = "prior_throughput";
    config.execution_hp_type = "serial_debug";
    config.execution_rt_type = "gpu_pipeline";
    config.execution_worker_count = 3;

    load_or_create_config(path.string(), config);

    EXPECT_EQ(config.loaded_config_path, "prior-config-path");
    EXPECT_EQ(config.cache_root_dir, "prior-cache");
    EXPECT_EQ(config.policy_interactive_type, "prior_interactive");
    EXPECT_EQ(config.policy_throughput_type, "prior_throughput");
    EXPECT_EQ(config.execution_hp_type, "serial_debug");
    EXPECT_EQ(config.execution_rt_type, "gpu_pipeline");
    EXPECT_EQ(config.execution_worker_count, 3);
  }
}

/**
 * @brief Strictly accepts editor boundary text and rejects malformed values.
 * @throws Nothing when parsing never coerces or partially consumes input.
 */
TEST(CliPolicyExecutionConfigParsing, StrictEditorParserEnforcesCompleteRange) {
  EXPECT_EQ(parse_cli_execution_worker_count("0"), 0);
  EXPECT_EQ(parse_cli_execution_worker_count("8"),
            static_cast<int>(ps::kExecutionWorkerRequestMax));
  for (const std::string invalid : {"-1", "9", "8workers", "", " 8"}) {
    EXPECT_THROW(parse_cli_execution_worker_count(invalid),
                 std::invalid_argument)
        << invalid;
  }
}

/**
 * @brief Rejects removed scheduler-prefixed YAML keys without translation.
 * @return Nothing; GoogleTest records any partial-publication mismatch.
 * @throws Filesystem or allocation failures from ordinary fixture setup.
 * @note The old key remains in this negative fixture only to prove the breaking
 *       generation has no compatibility parser.
 */
TEST(CliPolicyExecutionConfigParsing,
     RejectsRemovedSchedulerKeysWithoutChangingSnapshot) {
  ScopedCliConfigTempDir directory("photospider_cli_removed_config_key");
  const auto path = directory.root() / "removed.yaml";
  std::ofstream output(path);
  ASSERT_TRUE(output.is_open());
  output << "scheduler_worker_count: 1\n";
  output.close();
  ASSERT_TRUE(output);

  CliConfig config;
  config.loaded_config_path = "prior-config";
  config.cache_root_dir = "prior-cache";
  config.execution_worker_count = 3;
  load_or_create_config(path.string(), config);

  EXPECT_EQ(config.loaded_config_path, "prior-config");
  EXPECT_EQ(config.cache_root_dir, "prior-cache");
  EXPECT_EQ(config.execution_worker_count, 3);
}

/**
 * @brief Prevents invalid in-memory CLI values from reaching the Host.
 * @throws Nothing when validation raises `std::invalid_argument` first.
 */
TEST(CliPolicyExecutionConfigApply, RejectsInvalidValuesBeforeHostAccess) {
  ps::testing::IpcHostSpy host;
  CliConfig config;

  for (const int worker_count :
       {-1, static_cast<int>(ps::kExecutionWorkerRequestMax) + 1}) {
    config.execution_worker_count = worker_count;
    EXPECT_THROW(apply_cli_policy_execution_defaults(host, config),
                 std::invalid_argument);
  }
  EXPECT_EQ(host.call_count("policy.configure_defaults"), 0U);
  EXPECT_EQ(host.call_count("execution.configure_defaults"), 0U);
}

/**
 * @brief Routes automatic and exact-ceiling values without coercion.
 * @throws Nothing when each accepted snapshot reaches the Host exactly once.
 */
TEST(CliPolicyExecutionConfigApply, PreservesAcceptedValuesAtHostBoundary) {
  ps::testing::IpcHostSpy host;
  CliConfig config;
  config.policy_interactive_type = "interactive";
  config.policy_throughput_type = "throughput";
  config.execution_hp_type = "cpu";
  config.execution_rt_type = "serial_debug";

  config.execution_worker_count = 0;
  EXPECT_NO_THROW(apply_cli_policy_execution_defaults(host, config));
  config.execution_worker_count =
      static_cast<int>(ps::kExecutionWorkerRequestMax);
  EXPECT_NO_THROW(apply_cli_policy_execution_defaults(host, config));

  const auto invocations = host.invocations();
  ASSERT_EQ(invocations.size(), 4U);
  EXPECT_EQ(invocations[0].method, "policy.configure_defaults");
  EXPECT_EQ(invocations[0].text, "interactive\nthroughput");
  EXPECT_EQ(invocations[1].method, "execution.configure_defaults");
  EXPECT_EQ(invocations[1].text, "cpu\nserial_debug");
  EXPECT_EQ(invocations[1].worker_count, 0U);
  EXPECT_EQ(invocations[2].method, "policy.configure_defaults");
  EXPECT_EQ(invocations[2].text, "interactive\nthroughput");
  EXPECT_EQ(invocations[3].method, "execution.configure_defaults");
  EXPECT_EQ(invocations[3].text, "cpu\nserial_debug");
  EXPECT_EQ(invocations[3].worker_count, ps::kExecutionWorkerRequestMax);
}

/**
 * @brief Surfaces a failed Host execution-default status to the CLI boundary.
 * @throws Nothing when policy succeeds and execution raises exactly once.
 */
TEST(CliPolicyExecutionConfigApply, PropagatesHostRejection) {
  ps::testing::IpcHostSpy host;
  host.set_status(
      "execution.configure_defaults",
      ps::OperationStatus{
          false, ps::OperationErrorDomain::Graph,
          static_cast<std::int32_t>(ps::GraphErrc::InvalidParameter),
          "invalid_parameter", "execution defaults rejected by test Host"});
  CliConfig config;
  config.execution_worker_count = 1;

  EXPECT_THROW(apply_cli_policy_execution_defaults(host, config),
               std::runtime_error);
  EXPECT_EQ(host.call_count("policy.configure_defaults"), 1U);
  EXPECT_EQ(host.call_count("execution.configure_defaults"), 1U);
}

/**
 * @brief Stops CLI startup before graph work when Host defaults are rejected.
 * @throws Nothing when the reusable run boundary reports exit code two.
 */
TEST(CliPolicyExecutionConfigApply, HostRejectionStopsRunGraphCliStartup) {
  ScopedCliConfigTempDir directory("photospider_cli_execution_startup");
  const auto config_path = directory.root() / "config.yaml";
  write_policy_execution_config(config_path, 1, "startup-cache");

  ps::testing::IpcHostSpy host;
  host.set_status(
      "execution.configure_defaults",
      ps::OperationStatus{
          false, ps::OperationErrorDomain::Graph,
          static_cast<std::int32_t>(ps::GraphErrc::InvalidParameter),
          "invalid_parameter", "startup execution defaults rejected"});
  std::array<std::string, 5> arguments = {
      "graph_cli", "--config", config_path.string(), "--read", "ignored.yaml"};
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }

  EXPECT_EQ(run_graph_cli(static_cast<int>(argv.size()), argv.data(), host), 2);
  EXPECT_EQ(host.call_count("policy.configure_defaults"), 1U);
  EXPECT_EQ(host.call_count("execution.configure_defaults"), 1U);
  EXPECT_EQ(host.call_count("graph.load"), 0U);
}

/**
 * @brief Returns a recoverable failure without entering REPL after load fails.
 * @return Nothing; GoogleTest assertions report exit and output violations.
 * @throws std::filesystem::filesystem_error, std::runtime_error, or
 * std::bad_alloc if fixture or captured-output storage cannot be prepared.
 * @note Absence of the REPL banner proves the failed action wins over the
 * no-successful-action interactive fallback.
 */
TEST(CliOptionActions, FailedReadReturnsTwoWithoutEnteringRepl) {
  ScopedCliConfigTempDir directory("photospider_cli_failed_read");
  const auto config_path = directory.root() / "config.yaml";
  write_policy_execution_config(config_path, 1, "failed-read-cache");

  ps::testing::IpcHostSpy host;
  host.set_status(
      "graph.load",
      ps::OperationStatus{false, ps::OperationErrorDomain::Graph,
                          static_cast<std::int32_t>(ps::GraphErrc::Io), "io",
                          "input document is unavailable"});
  std::array<std::string, 5> arguments = {
      "graph_cli", "--config", config_path.string(), "-r", "missing.yaml"};
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }

  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const int exit_code =
      run_graph_cli(static_cast<int>(argv.size()), argv.data(), host);
  const std::string standard_error = testing::internal::GetCapturedStderr();
  const std::string standard_output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, 2);
  EXPECT_EQ(host.call_count("graph.load"), 1U);
  EXPECT_NE(standard_error.find("Failed to load graph"), std::string::npos);
  EXPECT_EQ(standard_output.find("Photospider dynamic graph shell"),
            std::string::npos);
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
  write_policy_execution_config(config_path, 1, "option-cache",
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
 * @brief Reports failure when a later action fails after a successful load.
 * @return Nothing; GoogleTest assertions report chain-result violations.
 * @throws std::filesystem::filesystem_error, std::runtime_error, or
 * std::bad_alloc if fixture or captured-output storage cannot be prepared.
 * @note The successful load remains observable, but neither a success footer
 * nor REPL entry may hide the later save failure.
 */
TEST(CliOptionActions, FailedOutputAfterSuccessfulReadReturnsTwo) {
  ScopedCliConfigTempDir directory("photospider_cli_failed_output");
  const auto config_path = directory.root() / "config.yaml";
  write_policy_execution_config(config_path, 1, "failed-output-cache");

  ps::testing::IpcHostSpy host(ps::GraphSessionId{"loaded-before-failure"});
  host.set_status(
      "graph.save",
      ps::OperationStatus{false, ps::OperationErrorDomain::Graph,
                          static_cast<std::int32_t>(ps::GraphErrc::Io), "io",
                          "output document is unavailable"});
  std::array<std::string, 7> arguments = {
      "graph_cli",  "--config", config_path.string(), "-r",
      "input.yaml", "-o",       "output.yaml"};
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }

  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const int exit_code =
      run_graph_cli(static_cast<int>(argv.size()), argv.data(), host);
  const std::string standard_error = testing::internal::GetCapturedStderr();
  const std::string standard_output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, 2);
  EXPECT_EQ(host.call_count("graph.load"), 1U);
  EXPECT_EQ(host.call_count("graph.save"), 1U);
  EXPECT_NE(standard_output.find("Loaded graph from input.yaml"),
            std::string::npos);
  EXPECT_NE(standard_error.find("Failed to save graph"), std::string::npos);
  EXPECT_EQ(standard_output.find("Photospider dynamic graph shell"),
            std::string::npos);
  EXPECT_EQ(standard_output.find("Command-line actions complete"),
            std::string::npos);
}

/**
 * @brief Preserves dependency-tree failure after a successful graph load.
 * @return Nothing; GoogleTest assertions report action-result violations.
 * @throws std::filesystem::filesystem_error, std::runtime_error, or
 * std::bad_alloc if fixture or captured-output storage cannot be prepared.
 * @note A prior successful action must not convert a later print failure into
 * successful process completion.
 */
TEST(CliOptionActions, FailedPrintAfterSuccessfulReadReturnsTwo) {
  ScopedCliConfigTempDir directory("photospider_cli_failed_print");
  const auto config_path = directory.root() / "config.yaml";
  write_policy_execution_config(config_path, 1, "failed-print-cache");

  ps::testing::IpcHostSpy host(ps::GraphSessionId{"loaded-before-print"});
  host.set_status(
      "inspect.dependency_tree",
      ps::OperationStatus{false, ps::OperationErrorDomain::Graph,
                          static_cast<std::int32_t>(ps::GraphErrc::NotFound),
                          "not_found", "dependency tree unavailable"});
  std::array<std::string, 6> arguments = {
      "graph_cli", "--config", config_path.string(), "-r", "input.yaml", "-p"};
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }

  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const int exit_code =
      run_graph_cli(static_cast<int>(argv.size()), argv.data(), host);
  const std::string standard_error = testing::internal::GetCapturedStderr();
  const std::string standard_output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, 2);
  EXPECT_EQ(host.call_count("graph.load"), 1U);
  EXPECT_EQ(host.call_count("inspect.dependency_tree"), 1U);
  EXPECT_NE(standard_error.find("Failed to print tree"), std::string::npos);
  EXPECT_EQ(standard_output.find("Command-line actions complete"),
            std::string::npos);
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
  write_policy_execution_config(config_path, 1, "traversal-cache");

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
 * @brief Preserves traversal-order failure after tree formatting succeeds.
 * @return Nothing; GoogleTest assertions report action-result violations.
 * @throws std::filesystem::filesystem_error, std::runtime_error, or
 * std::bad_alloc if fixture or captured-output storage cannot be prepared.
 * @note Partial output from the compound traversal action cannot make the
 * invocation successful when its traversal-order Host call fails.
 */
TEST(CliOptionActions, FailedTraversalAfterSuccessfulReadReturnsTwo) {
  ScopedCliConfigTempDir directory("photospider_cli_failed_traversal");
  const auto config_path = directory.root() / "config.yaml";
  write_policy_execution_config(config_path, 1, "failed-traversal-cache");

  ps::testing::IpcHostSpy host(ps::GraphSessionId{"loaded-before-traversal"});
  host.set_status(
      "inspect.traversal_orders",
      ps::OperationStatus{false, ps::OperationErrorDomain::Graph,
                          static_cast<std::int32_t>(ps::GraphErrc::NotFound),
                          "not_found", "traversal orders unavailable"});
  std::array<std::string, 6> arguments = {
      "graph_cli", "--config", config_path.string(), "-r", "input.yaml", "-t"};
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }

  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const int exit_code =
      run_graph_cli(static_cast<int>(argv.size()), argv.data(), host);
  const std::string standard_error = testing::internal::GetCapturedStderr();
  const std::string standard_output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, 2);
  EXPECT_EQ(host.call_count("inspect.dependency_tree"), 1U);
  EXPECT_EQ(host.call_count("inspect.traversal_orders"), 1U);
  EXPECT_NE(standard_error.find("Failed to compute traversal"),
            std::string::npos);
  EXPECT_EQ(standard_output.find("Command-line actions complete"),
            std::string::npos);
}

/**
 * @brief Preserves all-cache-clear failure after a successful graph load.
 * @return Nothing; GoogleTest assertions report action-result violations.
 * @throws std::filesystem::filesystem_error, std::runtime_error, or
 * std::bad_alloc if fixture or captured-output storage cannot be prepared.
 * @note A successful traversal invocation first leaves platform option-parser
 * state in a different terminal shape. The cache invocation must fully reset
 * that state before both parser passes; its status-only Host action then
 * participates in the same invocation-wide failure contract as value-returning
 * and document actions.
 */
TEST(CliOptionActions, FailedCacheClearAfterSuccessfulReadReturnsTwo) {
  ScopedCliConfigTempDir directory("photospider_cli_failed_cache_clear");
  const auto config_path = directory.root() / "config.yaml";
  write_policy_execution_config(config_path, 1, "failed-cache-clear-root");

  ps::testing::IpcHostSpy traversal_host(
      ps::GraphSessionId{"parser-reset-traversal"});
  std::array<std::string, 6> traversal_arguments = {
      "graph_cli", "--config", config_path.string(), "-r", "input.yaml", "-t"};
  std::vector<char*> traversal_argv;
  traversal_argv.reserve(traversal_arguments.size());
  for (std::string& argument : traversal_arguments) {
    traversal_argv.push_back(argument.data());
  }
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const int traversal_exit_code =
      run_graph_cli(static_cast<int>(traversal_argv.size()),
                    traversal_argv.data(), traversal_host);
  (void)testing::internal::GetCapturedStderr();
  (void)testing::internal::GetCapturedStdout();
  ASSERT_EQ(traversal_exit_code, 0);
  ASSERT_EQ(traversal_host.call_count("inspect.traversal_orders"), 1U);

  ps::testing::IpcHostSpy host(ps::GraphSessionId{"loaded-before-cache-clear"});
  host.set_status(
      "cache.clear_all",
      ps::OperationStatus{false, ps::OperationErrorDomain::Graph,
                          static_cast<std::int32_t>(ps::GraphErrc::NotFound),
                          "not_found", "cache owner unavailable"});
  std::array<std::string, 6> arguments = {
      "graph_cli", "--config",   config_path.string(),
      "-r",        "input.yaml", "--clear-cache"};
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }

  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const int exit_code =
      run_graph_cli(static_cast<int>(argv.size()), argv.data(), host);
  const std::string standard_error = testing::internal::GetCapturedStderr();
  const std::string standard_output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, 2);
  EXPECT_EQ(host.call_count("cache.clear_all"), 1U);
  EXPECT_NE(standard_error.find("Failed to clear cache"), std::string::npos);
  EXPECT_EQ(standard_output.find("Command-line actions complete"),
            std::string::npos);
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
