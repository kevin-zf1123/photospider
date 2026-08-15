#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "compute/execution/execution_service.hpp"
#include "execution/device/device_executor_registry.hpp"
#include "runtime/kernel.hpp"
#include "runtime/kernel_close_test_access.hpp"
#include "runtime/kernel_compute_test_access.hpp"
#include "support/kernel_test_access.hpp"
#include "support/kernel_test_dependencies.hpp"

namespace ps {
namespace {

/**
 * @brief Owns and removes one unique temporary Kernel session root.
 * @throws std::filesystem::filesystem_error when initial cleanup fails.
 * @note Destruction contains cleanup errors so test exceptions retain identity.
 */
class ScopedLineageCloseTempRoot final {
 public:
  /**
   * @brief Selects an isolated path and removes any impossible stale collision.
   * @throws std::filesystem::filesystem_error when cleanup fails.
   */
  ScopedLineageCloseTempRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("photospider-lineage-close-" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::remove_all(path_);
  }

  /** @brief Removes the complete isolated test tree. */
  ~ScopedLineageCloseTempRoot() noexcept {
    try {
      std::filesystem::remove_all(path_);
    } catch (...) {
    }
  }

  /** @brief Prevents duplicate filesystem cleanup ownership. */
  ScopedLineageCloseTempRoot(const ScopedLineageCloseTempRoot&) = delete;
  /** @brief Prevents replacement of filesystem cleanup ownership. */
  ScopedLineageCloseTempRoot& operator=(const ScopedLineageCloseTempRoot&) =
      delete;

  /**
   * @brief Returns the selected session root.
   * @return Borrowed immutable path valid for this owner lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Exact isolated root removed at scope exit. */
  std::filesystem::path path_;
};

/**
 * @brief One reusable condition-variable checkpoint for a deterministic race.
 * @throws Standard synchronization errors from test coordination.
 * @note The callback may enter once; release is idempotent and never resets.
 */
class LineageCloseCheckpoint final {
 public:
  /**
   * @brief Publishes entry and waits for explicit test release.
   * @return Nothing after release.
   * @throws std::system_error when synchronization fails.
   */
  void enter_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this]() { return released_; });
  }

  /**
   * @brief Waits for callback entry.
   * @param timeout Maximum bounded wait.
   * @return True when the callback entered.
   * @throws std::system_error when synchronization fails.
   */
  bool wait_until_entered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this]() { return entered_; });
  }

  /**
   * @brief Opens the checkpoint permanently.
   * @return Nothing.
   * @throws Nothing; synchronization failure terminates deterministic cleanup.
   */
  void release() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
      }
      changed_.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /** @brief Serializes checkpoint state. */
  std::mutex mutex_;
  /** @brief Wakes the test and blocked product caller. */
  std::condition_variable changed_;
  /** @brief True after the observed callback enters. */
  bool entered_ = false;
  /** @brief True after the test permits callback return. */
  bool released_ = false;
};

/**
 * @brief Owns both product hooks and every asynchronous race participant.
 *
 * The compute caller pauses after coordinator preparation but before residency
 * pretracking. The close owner pauses after Run lifecycle removal but before
 * request-lane drainage. Cleanup always releases both callbacks, settles both
 * callers, and only then clears borrowed process-global hooks.
 *
 * @throws std::bad_alloc or std::system_error from asynchronous setup.
 * @note The borrowed Kernel outlives this owner. One instance runs at a time.
 */
class ScopedPreparedCandidateCloseRace final {
 public:
  /**
   * @brief Installs candidate-admission and close-lifecycle checkpoints.
   * @param kernel Kernel whose exact loaded Graph will race close.
   * @throws Nothing.
   */
  explicit ScopedPreparedCandidateCloseRace(Kernel& kernel) noexcept
      : kernel_(kernel),
        admission_hook_{this,
                        &ScopedPreparedCandidateCloseRace::notify_admission},
        close_hook_{this, &ScopedPreparedCandidateCloseRace::notify_close} {
    testing::set_kernel_compute_admission_test_hook(&admission_hook_);
    testing::set_kernel_close_test_hook(&close_hook_);
  }

  /**
   * @brief Releases, joins, and clears every borrowed test observer.
   * @throws Nothing.
   */
  ~ScopedPreparedCandidateCloseRace() noexcept {
    admission_checkpoint_.release();
    close_checkpoint_.release();
    settle_noexcept();
    testing::set_kernel_close_test_hook(nullptr);
    testing::set_kernel_compute_admission_test_hook(nullptr);
  }

  /** @brief Prevents duplicate hook and future ownership. */
  ScopedPreparedCandidateCloseRace(const ScopedPreparedCandidateCloseRace&) =
      delete;
  /** @brief Prevents replacement of hook and future ownership. */
  ScopedPreparedCandidateCloseRace& operator=(
      const ScopedPreparedCandidateCloseRace&) = delete;

  /**
   * @brief Starts one async-facade caller that blocks before lineage tracking.
   * @param request Exact Graph request copied into the caller.
   * @return Nothing after the caller thread starts.
   * @throws std::system_error or std::bad_alloc from asynchronous launch.
   */
  void start_candidate(Kernel::ComputeRequest request) {
    candidate_submission_ = std::async(
        std::launch::async, [this, request = std::move(request)]() mutable {
          return kernel_.compute_async(std::move(request));
        });
  }

  /**
   * @brief Waits until candidate preparation owns request-lane admission.
   * @param timeout Maximum bounded wait.
   * @return True when the product checkpoint entered.
   * @throws std::system_error when synchronization fails.
   */
  bool wait_until_candidate_prepared(std::chrono::milliseconds timeout) {
    return admission_checkpoint_.wait_until_entered(timeout);
  }

  /**
   * @brief Starts exact-Graph close on a separate caller.
   * @param graph_name Published Graph name to close.
   * @return Nothing after the close caller starts.
   * @throws std::system_error or std::bad_alloc from asynchronous launch.
   */
  void start_close(std::string graph_name) {
    close_result_ = std::async(std::launch::async,
                               [this, graph_name = std::move(graph_name)]() {
                                 return kernel_.close_graph(graph_name);
                               });
  }

  /**
   * @brief Waits until Run lifecycle drained ahead of request-lane drainage.
   * @param timeout Maximum bounded wait.
   * @return True when the close checkpoint entered.
   * @throws std::system_error when synchronization fails.
   */
  bool wait_until_lifecycle_drained(std::chrono::milliseconds timeout) {
    return close_checkpoint_.wait_until_entered(timeout);
  }

  /**
   * @brief Releases the candidate and consumes its close-rejection diagnostic.
   * @param timeout Maximum wait for submission and exact result settlement.
   * @return Runtime-error text published by rejected coordinator publication.
   * @throws std::runtime_error when a bounded wait expires, no candidate was
   * admitted, or the candidate unexpectedly succeeds.
   * @throws Any unexpected caller or future exception unchanged.
   * @note Close remains paused before request-lane drain while this returns.
   */
  std::string release_candidate_and_get_rejection(
      std::chrono::milliseconds timeout) {
    admission_checkpoint_.release();
    if (candidate_submission_.wait_for(timeout) != std::future_status::ready) {
      throw std::runtime_error(
          "prepared candidate caller missed its bounded deadline");
    }
    std::optional<std::future<Kernel::AsyncComputeResult>> admitted =
        candidate_submission_.get();
    if (!admitted.has_value()) {
      throw std::runtime_error(
          "prepared candidate lost its retained runtime admission");
    }
    candidate_result_ = std::move(*admitted);
    if (candidate_result_.wait_for(timeout) != std::future_status::ready) {
      throw std::runtime_error(
          "prepared candidate result missed its bounded deadline");
    }
    try {
      const Kernel::AsyncComputeResult unexpected = candidate_result_.get();
      (void)unexpected;
    } catch (const std::runtime_error& error) {
      return error.what();
    }
    throw std::runtime_error(
        "prepared candidate unexpectedly completed without close rejection");
  }

  /**
   * @brief Releases and consumes exact Graph close.
   * @param timeout Maximum bounded wait.
   * @return Kernel close result.
   * @throws std::runtime_error when close misses the deadline.
   * @throws Any close failure unchanged.
   */
  bool release_close_and_get(std::chrono::milliseconds timeout) {
    close_checkpoint_.release();
    if (close_result_.wait_for(timeout) != std::future_status::ready) {
      throw std::runtime_error("Graph close missed its bounded deadline");
    }
    return close_result_.get();
  }

 private:
  /**
   * @brief Blocks only the post-prepare/pre-lineage product checkpoint.
   * @param context Borrowed race owner.
   * @param event Exact compute-admission event.
   * @return Nothing after test release.
   * @throws Nothing; synchronization failure terminates.
   */
  static void notify_admission(
      void* context, testing::KernelComputeAdmissionTestEvent event) noexcept {
    if (event != testing::KernelComputeAdmissionTestEvent::
                     ProductCandidatePreparedBeforeLineageTracking) {
      return;
    }
    auto* race = static_cast<ScopedPreparedCandidateCloseRace*>(context);
    if (race == nullptr) {
      std::terminate();
    }
    try {
      race->admission_checkpoint_.enter_and_wait();
    } catch (...) {
      std::terminate();
    }
  }

  /**
   * @brief Blocks only the post-lifecycle/pre-request-drain close checkpoint.
   * @param context Borrowed race owner.
   * @param event Exact close event.
   * @return Nothing after test release.
   * @throws std::system_error when test synchronization fails.
   */
  static void notify_close(void* context, testing::KernelCloseTestEvent event) {
    if (event !=
        testing::KernelCloseTestEvent::LifecycleDrainedBeforeRequestLaneDrain) {
      return;
    }
    auto* race = static_cast<ScopedPreparedCandidateCloseRace*>(context);
    if (race == nullptr) {
      throw std::logic_error("prepared-candidate close race is missing");
    }
    race->close_checkpoint_.enter_and_wait();
  }

  /**
   * @brief Best-effort settles every still-valid caller during scope cleanup.
   * @return Nothing.
   * @throws Nothing; test failures retain their original assertion identity.
   */
  void settle_noexcept() noexcept {
    try {
      if (candidate_submission_.valid()) {
        std::optional<std::future<Kernel::AsyncComputeResult>> admitted =
            candidate_submission_.get();
        if (admitted.has_value()) {
          candidate_result_ = std::move(*admitted);
        }
      }
    } catch (...) {
    }
    try {
      if (candidate_result_.valid()) {
        (void)candidate_result_.get();
      }
    } catch (...) {
    }
    try {
      if (close_result_.valid()) {
        (void)close_result_.get();
      }
    } catch (...) {
    }
  }

  /** @brief Borrowed Kernel retained by the owning test. */
  Kernel& kernel_;
  /** @brief Candidate checkpoint ahead of residency pretracking. */
  LineageCloseCheckpoint admission_checkpoint_;
  /** @brief Close checkpoint ahead of request-lane drainage. */
  LineageCloseCheckpoint close_checkpoint_;
  /** @brief Borrowed candidate-admission hook record. */
  testing::KernelComputeAdmissionTestHook admission_hook_;
  /** @brief Borrowed close-lifecycle hook record. */
  testing::KernelCloseTestHook close_hook_;
  /** @brief Caller returning the admitted candidate result future. */
  std::future<std::optional<std::future<Kernel::AsyncComputeResult>>>
      candidate_submission_;
  /** @brief Exact coordinator-settled candidate result. */
  std::future<Kernel::AsyncComputeResult> candidate_result_;
  /** @brief Exact Graph close result. */
  std::future<bool> close_result_;
};

/**
 * @brief Proves Graph close drains prepared candidates before lineage
 * retirement.
 * @return Nothing; GoogleTest assertions report ordering or isolation failure.
 * @throws Setup, synchronization, or Kernel failures unchanged.
 * @note Reversing the production retirement and request-lane drain steps leaves
 * one late zero-generation row for `closing_graph`, making the final zero-row
 * assertion fail. The second Graph proves retirement remains identity-scoped.
 */
TEST(ComputeContracts,
     CloseDrainsPreparedCandidateBeforeResidencyLineageRetirement) {
  const std::chrono::seconds timeout(2);
  const std::string closing_graph = "lineage_close_race";
  const std::string other_graph = "lineage_close_other";
  ScopedLineageCloseTempRoot root;

  execution::DeviceExecutorRegistry device_executors;
  const std::shared_ptr<execution::ResidencyManager> residency =
      device_executors.residency_manager();
  auto execution_service = std::make_shared<compute::ExecutionService>(
      compute::ExecutionService::default_resource_limits(),
      std::move(device_executors));
  auto document_adapter = testing::make_yaml_graph_document_adapter();
  Kernel kernel(providers::make_configured_image_artifact_codec(),
                testing::make_yaml_cache_metadata_codec(), document_adapter,
                document_adapter, execution_service);

  ASSERT_TRUE(
      kernel.load_graph(closing_graph, root.path().string(), std::string()));
  ASSERT_TRUE(
      kernel.load_graph(other_graph, root.path().string(), std::string()));
  const std::shared_ptr<GraphRuntime> closing_runtime =
      testing::KernelTestAccess::runtime_owner(kernel, closing_graph);
  const std::shared_ptr<GraphRuntime> other_runtime =
      testing::KernelTestAccess::runtime_owner(kernel, other_graph);
  const GraphInstanceId closing_instance =
      closing_runtime->model().instance_id();
  const GraphInstanceId other_instance = other_runtime->model().instance_id();
  ASSERT_NE(closing_instance, other_instance);

  const compute::SupersessionIdentity other_identity{
      compute::SupersessionKey(7, ComputeIntent::GlobalHighPrecision),
      compute::SupersessionGeneration(1U)};
  execution_service->prepare_supersession_lineage(other_instance,
                                                  other_identity);
  ASSERT_EQ(residency->lineage_count_for_graph(other_instance.value()), 1U);

  ScopedPreparedCandidateCloseRace race(kernel);
  Kernel::ComputeRequest request;
  request.name = closing_graph;
  request.node_id = 1;
  request.intent = ComputeIntent::GlobalHighPrecision;
  race.start_candidate(std::move(request));
  ASSERT_TRUE(race.wait_until_candidate_prepared(timeout));

  const compute::ComputeRequestCoordinator::Snapshot prepared =
      closing_runtime->compute_request_snapshot();
  EXPECT_EQ(prepared.provisional_adopters, 1U);
  EXPECT_EQ(prepared.reserved_tickets, 1U);
  EXPECT_EQ(prepared.lane_admitted_units, 1U);
  EXPECT_EQ(residency->lineage_count_for_graph(closing_instance.value()), 0U);

  race.start_close(closing_graph);
  ASSERT_TRUE(race.wait_until_lifecycle_drained(timeout));
  const std::string rejection =
      race.release_candidate_and_get_rejection(timeout);
  EXPECT_NE(rejection.find("rejected by Graph close"), std::string::npos)
      << rejection;
  ASSERT_EQ(residency->lineage_count_for_graph(closing_instance.value()), 1U);
  ASSERT_EQ(residency->lineage_count_for_graph(other_instance.value()), 1U);

  EXPECT_TRUE(race.release_close_and_get(timeout));
  EXPECT_EQ(residency->lineage_count_for_graph(closing_instance.value()), 0U);
  EXPECT_EQ(residency->lineage_count_for_graph(other_instance.value()), 1U);

  EXPECT_TRUE(kernel.close_graph(other_graph));
  EXPECT_EQ(residency->lineage_count_for_graph(other_instance.value()), 0U);
}

}  // namespace
}  // namespace ps
