#include "execution/result_stage_submission.hpp"

#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include "core/resource_observation.hpp"
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
#include "execution/execution_test_hooks.hpp"
#endif

namespace ps::execution_internal {
namespace {
struct ResultStageRetirement {
  std::shared_ptr<ResultStageCompletion> completion;
  std::atomic<bool> delivered{false};
  explicit ResultStageRetirement(std::shared_ptr<ResultStageCompletion> owner)
      : completion(std::move(owner)) {}
  ~ResultStageRetirement() { finish(true); }
  void finish(bool rejected = false) noexcept {
    if (delivered.exchange(true))
      return;
    completion->work = {};
    if (rejected) {
      completion->result = Result<int>(Status{ErrorCode::Cancelled, {}});
      if (completion->lease.valid()) {
        ResourceCapacity waiting;
        waiting[ResourceKind::Queue] = 1;
        (void)completion->lease.shrink(waiting);
      }
    }
    auto promise = std::move(completion->promise);
    try {
      promise.set_value(std::move(completion->result));
    } catch (...) {
      try {
        promise.set_exception(std::current_exception());
      } catch (...) {
      }
    }
  }
};

}  // namespace
Result<std::shared_ptr<ResultStageCompletion>> submit_result_stage(
    ThreadPool* pool, WaitingAdmission* admission,
    std::function<Result<int>()> work, const ResourceBudget* resources) try {
  using Completion = ResultStageCompletion;
  using Answer = Result<std::shared_ptr<Completion>>;
  ResourceLease lease;
  if (resources) {
    auto capacity =
        ResourceCapacity::host(sizeof(Completion), sizeof(Completion));
    capacity[ResourceKind::Queue] = 1;
    capacity[ResourceKind::Entries] = 1;
    auto admitted = resources->reserve(capacity);
    if (!admitted.ok()) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      execution_testing::notify_before_scheduler_failure(
          execution_testing::SchedulerFailurePoint::WaitingAdmissionRejected);
#endif
      return Answer(admitted.status());
    }
    lease = admitted.take_value();
  }
  auto completion = std::make_shared<Completion>();
  completion->lease = std::move(lease);
  completion->work = std::move(work);
  auto retirement =
      resources ? std::allocate_shared<ResultStageRetirement>(
                      ResourceAllocator<ResultStageRetirement>(*resources),
                      completion)
                : std::make_shared<ResultStageRetirement>(completion);
  auto slot = admission->try_acquire();
  if (!slot) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    execution_testing::notify_before_scheduler_failure(
        execution_testing::SchedulerFailurePoint::WaitingAdmissionRejected);
#endif
    return Answer(Status::failure(ErrorCode::ResourceExhausted,
                                  "dependency waiting queue exhausted"));
  }
  if (resources) {
    auto issued = resources->consume({0, 0, 0, 1});
    if (!issued.ok()) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      execution_testing::notify_before_scheduler_failure(
          execution_testing::SchedulerFailurePoint::WaitingAdmissionRejected);
#endif
      return Answer(issued);
    }
  }
  const auto payload_capture =
      resources ? core_internal::ResourcePayloadScope::capture(*resources)
                : core_internal::PayloadCapture{};
  QueuedCallback callback{
      [completion, retirement, resources, payload_capture] {
        // The callable's owners retire before the completion notification.
        auto work = std::move(completion->work);
        try {
          ErrorCode metadata_failure = ErrorCode::Ok;
          std::optional<ResourceAllocationScope> scope;
          std::optional<core_internal::ResourcePayloadScope> payload;
          if (resources) {
            scope.emplace(*resources, &metadata_failure);
            payload.emplace(*resources, payload_capture);
          }
          completion->result = work();
          if (metadata_failure != ErrorCode::Ok &&
              completion->result.status().detail.origin !=
                  FailureOrigin::Protocol &&
              completion->result.status().code !=
                  ErrorCode::ResourceExhausted &&
              completion->result.status().code != ErrorCode::Cancelled &&
              completion->result.status().code != ErrorCode::Stale)
            completion->result = Result<int>(
                Status{metadata_failure,
                       "dependency stage metadata allocation failed",
                       FailureReason::CapacityLimit,
                       {FailureOrigin::Resource, FailureScope::Unspecified}});
        } catch (const std::bad_alloc&) {
          completion->result = Result<int>(
              scheduler_exception_status(ErrorCode::ResourceExhausted));
        } catch (const std::exception& error) {
          completion->result = Result<int>(scheduler_exception_status(
              ErrorCode::OperationFailed, error.what()));
        } catch (...) {
          completion->result = Result<int>(
              scheduler_exception_status(ErrorCode::OperationFailed));
        }
      },
      std::move(*slot), [retirement] { retirement->finish(); },
      completion->lease};
  if (!pool->submit(std::move(callback))) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    execution_testing::notify_before_scheduler_failure(
        execution_testing::SchedulerFailurePoint::CallbackSubmitRejected);
#endif
    return Answer(Status::failure(ErrorCode::ResourceExhausted,
                                  "dependency callback queue stopped"));
  }
  return Answer(std::move(completion));
} catch (const std::bad_alloc&) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  execution_testing::notify_before_scheduler_failure(
      execution_testing::SchedulerFailurePoint::CallbackSubmitException);
#endif
  return Result<std::shared_ptr<ResultStageCompletion>>(
      scheduler_exception_status(ErrorCode::ResourceExhausted));
} catch (const std::exception& error) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  execution_testing::notify_before_scheduler_failure(
      execution_testing::SchedulerFailurePoint::CallbackSubmitException);
#endif
  return Result<std::shared_ptr<ResultStageCompletion>>(
      scheduler_exception_status(ErrorCode::Internal, error.what()));
} catch (...) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
  execution_testing::notify_before_scheduler_failure(
      execution_testing::SchedulerFailurePoint::CallbackSubmitException);
#endif
  return Result<std::shared_ptr<ResultStageCompletion>>(
      scheduler_exception_status(ErrorCode::Internal));
}
}  // namespace ps::execution_internal
