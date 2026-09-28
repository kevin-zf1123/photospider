#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "data/input_validation.hpp"
#include "execution/cpu_range_context.hpp"
#include "execution/waiting_admission.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "photospider/plugin/cpu_parallel_api.h"

namespace ps::execution_internal {

/** @brief Intrusive synchronous range jobs serviced by the existing CPU pool.
 * All queue/claim fields use the pool mutex. No helper closure retains a job
 * after its active claim retires. Whole callers participate; staged callers
 * coordinate outside the pool so occupied workers cannot wait on themselves.
 */
class CpuRangeQueue final {
 public:
  struct Job {
    Job* next = nullptr;
    std::uint64_t count = 0, grain = 0, cursor = 0, slots = 1;
    std::uint32_t workers = 1, active = 0, helpers = 0;
    std::uint32_t peak = 0;
    bool caller_participates = true;
    ps_cpu_range_callback_v1 callback = nullptr;
    void* user = nullptr;
    const CancellationToken* cancellation = nullptr;
    const ResourceBudget* resources = nullptr;
    const std::function<bool()>* current = nullptr;
    std::condition_variable* completion = nullptr;
    Status status;
  };
  struct Claim {
    Job* job = nullptr;
    std::uint64_t begin = 0, end = 0;
    std::uint32_t slot = 0;
  };
  CpuRangeQueue(std::mutex& mutex, std::condition_variable& changed,
                std::uint32_t workers, WaitingAdmission& admission)
      : mutex_(mutex),
        changed_(changed),
        workers_(std::min(workers, 64U)),
        admission_(admission) {}
  std::uint32_t workers() const noexcept { return workers_; }
  bool ready_locked() const noexcept {
    for (auto* job = head_; job; job = job->next)
      if (eligible(*job))
        return true;
    return false;
  }
  Claim take_locked() {
    Job* previous = nullptr;
    for (auto* job = head_; job; previous = job, job = job->next) {
      if (!eligible(*job))
        continue;
      std::uint32_t slot = job->caller_participates ? 1 : 0;
      while (job->slots & (std::uint64_t{1} << slot))
        ++slot;
      auto claim = claim_locked(*job, slot);
      // Rotate this job behind other invocations for bounded helper fairness.
      if (job->next) {
        if (previous)
          previous->next = job->next;
        else
          head_ = job->next;
        tail_->next = job;
        tail_ = job;
        job->next = nullptr;
      }
      return claim;
    }
    return {};
  }
  void execute(Claim claim, bool queued = true) noexcept {
    auto& job = *claim.job;
    Status status;
    try {
      ErrorCode metadata_failure = ErrorCode::Ok;
      std::optional<ResourceAllocationScope> resources;
      if (job.resources)
        resources.emplace(*job.resources, &metadata_failure);
      input_internal::Float32Environment environment;
      if (job.cancellation->cancelled()) {
        status.code = ErrorCode::Cancelled;
      } else if (job.current && *job.current && !(*job.current)()) {
        status.code = ErrorCode::Stale;
      } else if (!environment.active()) {
        status.code = ErrorCode::OperationFailed;
      } else {
        in_cpu_range = true;
        try {
          const int code =
              job.callback(job.user, claim.begin, claim.end, claim.slot);
          status.code = code == 0   ? ErrorCode::Ok
                        : code == 2 ? ErrorCode::Cancelled
                        : code == 3 && !job.caller_participates
                            ? ErrorCode::BackendUnavailable
                        : code == 4 ? ErrorCode::ResourceExhausted
                        : code == 5 && !job.caller_participates
                            ? ErrorCode::TypeMismatch
                        : code == 6 ? ErrorCode::InvalidArgument
                                    : ErrorCode::OperationFailed;
        } catch (...) {
          in_cpu_range = false;
          throw;
        }
        in_cpu_range = false;
        if (metadata_failure != ErrorCode::Ok)
          status.code = metadata_failure;
      }
    } catch (const std::bad_alloc&) {
      status.code = ErrorCode::ResourceExhausted;
    } catch (...) {
      status.code = ErrorCode::OperationFailed;
    }
    bool finished = false, helper_available = false;
    {
      std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
      if (queued)
        lock.lock();
      if (job.status.ok() && !status.ok())
        job.status = std::move(status);
      job.slots &= ~(std::uint64_t{1} << claim.slot);
      if (claim.slot == 0 && job.caller_participates)
        job.slots |= 1;
      --job.active;
      if (claim.slot || !job.caller_participates)
        --job.helpers;
      const bool done =
          job.active == 0 && (job.cursor == job.count || !job.status.ok());
      finished = done && !job.completion;
      // Notify while holding the mutex: after retirement the coordinator may
      // unlink the stack job and destroy its completion variable immediately.
      if (done && job.completion)
        job.completion->notify_one();
      helper_available =
          (claim.slot || !job.caller_participates) && eligible(job);
    }
    // A parent waits only after all blocks are claimed or after failure.
    // Intermediate completions do not need to wake every sleeping worker.
    if (queued && finished)
      changed_.notify_all();
    else if (queued && helper_available)
      changed_.notify_one();
  }
  Status run(std::uint64_t count, std::uint64_t grain, std::uint32_t workers,
             ps_cpu_range_callback_v1 callback, void* user,
             const CancellationToken& cancellation,
             const ResourceBudget* resources,
             const std::function<bool()>& current) {
    if (!grain || !callback || workers > workers_ || in_cpu_range)
      return Status{ErrorCode::InvalidArgument, "invalid or nested CPU range"};
    if (cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (!count)
      return Status::success();
    Job job;
    job.count = count;
    job.grain = grain;
    job.workers = workers ? workers : workers_;
    job.callback = callback;
    job.user = user;
    job.cancellation = &cancellation;
    job.resources = resources;
    job.current = &current;
    ResourceLease lease;
    if (resources) {
      auto capacity = ResourceCapacity::host(sizeof(Job), sizeof(Job));
      capacity[ResourceKind::Entries] = 1;
      auto admitted = resources->reserve(capacity);
      if (!admitted.ok())
        return admitted.status();
      lease = admitted.take_value();
      auto charged = resources->consume(
          {count, 0, 0, count / grain + (count % grain != 0)});
      if (!charged.ok())
        return charged;
    }
    // No helper can participate in a one-worker/one-block range. Keep that
    // common case off the shared queue while preserving all service checks.
    if (job.workers == 1 || count <= grain) {
      while (job.cursor < count && job.status.ok())
        execute(claim_locked(job, 0), false);
      if (cancellation.cancelled())
        job.status.code = ErrorCode::Cancelled;
      return job.status;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (tail_)
      tail_->next = &job;
    else
      head_ = &job;
    tail_ = &job;
    changed_.notify_all();
    while (job.cursor < count && job.status.ok()) {
      if (cancellation.cancelled()) {
        job.status.code = ErrorCode::Cancelled;
        break;
      }
      auto claim = claim_locked(job, 0);
      lock.unlock();
      execute(claim);
      lock.lock();
    }
    changed_.wait(lock, [&] { return job.active == 0; });
    Job* previous = nullptr;
    for (auto* current = head_; current; current = current->next) {
      if (current == &job) {
        if (previous)
          previous->next = job.next;
        else
          head_ = job.next;
        if (tail_ == &job)
          tail_ = previous;
        break;
      }
      previous = current;
    }
    if (cancellation.cancelled())
      job.status.code = ErrorCode::Cancelled;
    return job.status;
  }

  /** @brief One pool callback per tile; coordinator participates only in
   * control. Holds shared waiting admission until unlink, bounding outstanding
   * external stages even when managed resources are disabled. All captures stay
   * borrowed through retirement. Pool threads reject this synchronous
   * coordinator path.
   */
  Status run_external(std::uint64_t count, std::uint32_t workers,
                      ps_cpu_range_callback_v1 callback, void* user,
                      const CancellationToken& cancellation,
                      const ResourceBudget* resources,
                      const std::function<bool()>& current,
                      std::uint32_t* peak = nullptr) {
    if (!callback || !workers || workers > workers_ || in_kernel_worker ||
        in_cpu_range)
      return Status{ErrorCode::InvalidArgument,
                    "invalid CPU stage coordinator"};
    if (cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (current && !current())
      return Status{ErrorCode::Stale, {}};
    if (!count)
      return Status::success();
    auto admission = admission_.try_acquire();
    if (!admission)
      return Status{ErrorCode::ResourceExhausted, "CPU stage queue is full"};
    std::condition_variable completion;
    Job job;
    job.count = count;
    job.grain = 1;
    job.slots = 0;
    job.workers = workers;
    job.caller_participates = false;
    job.callback = callback;
    job.user = user;
    job.cancellation = &cancellation;
    job.resources = resources;
    job.current = &current;
    job.completion = &completion;
    ResourceLease lease;
    if (resources) {
      const auto bytes = sizeof(Job) + sizeof(completion);
      auto capacity = ResourceCapacity::host(bytes, bytes);
      capacity[ResourceKind::Entries] = 1;
      capacity[ResourceKind::Queue] = 1;
      auto admitted = resources->reserve(capacity);
      if (!admitted.ok())
        return admitted.status();
      lease = admitted.take_value();
      auto charged = resources->consume({count, 0, 0, count});
      if (!charged.ok())
        return charged;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (tail_)
      tail_->next = &job;
    else
      head_ = &job;
    tail_ = &job;
    changed_.notify_all();
    const auto done = [&] {
      return job.active == 0 && (job.cursor == count || !job.status.ok());
    };
    try {
      while (!done()) {
        // Cancellation can remove the last eligible claim without a worker
        // notification. Poll it even when every worker is occupied elsewhere.
        lock.unlock();
        ErrorCode stopped = ErrorCode::Ok;
        try {
          if (cancellation.cancelled())
            stopped = ErrorCode::Cancelled;
          else if (current && !current())
            stopped = ErrorCode::Stale;
        } catch (const std::bad_alloc&) {
          stopped = ErrorCode::ResourceExhausted;
        } catch (...) {
          stopped = ErrorCode::OperationFailed;
        }
        lock.lock();
        if (stopped != ErrorCode::Ok && job.status.ok())
          job.status.code = stopped;
        if (!done())
          completion.wait_for(lock, std::chrono::milliseconds(1));
      }
    } catch (...) {
      if (!lock.owns_lock())
        lock.lock();
      job.status.code = ErrorCode::OperationFailed;
      // Cleanup keeps the intrusive record alive until every borrower retires.
      completion.wait(lock, [&] { return job.active == 0; });
    }
    Job* previous = nullptr;
    for (auto* item = head_; item; item = item->next) {
      if (item == &job) {
        if (previous)
          previous->next = job.next;
        else
          head_ = job.next;
        if (tail_ == &job)
          tail_ = previous;
        break;
      }
      previous = item;
    }
    lock.unlock();
    if (peak)
      *peak = job.peak;
    if (cancellation.cancelled())
      job.status.code = ErrorCode::Cancelled;
    else if (current && !current())
      job.status.code = ErrorCode::Stale;
    return job.status;
  }

 private:
  static bool eligible(const Job& job) noexcept {
    return job.status.ok() && job.cursor < job.count &&
           job.helpers < job.workers - (job.caller_participates ? 1U : 0U) &&
           !job.cancellation->cancelled();
  }
  static Claim claim_locked(Job& job, std::uint32_t slot) {
    const auto begin = job.cursor;
    const auto end = begin + std::min(job.grain, job.count - begin);
    job.cursor = end;
    job.slots |= std::uint64_t{1} << slot;
    ++job.active;
    job.peak = std::max(job.peak, job.active);
    if (slot || !job.caller_participates)
      ++job.helpers;
    return {&job, begin, end, slot};
  }
  std::mutex& mutex_;
  std::condition_variable& changed_;
  const std::uint32_t workers_;
  WaitingAdmission& admission_;
  Job* head_ = nullptr;
  Job* tail_ = nullptr;
};

/** @brief Callback-thread service with sticky errors and synchronous lifetime.
 */
class CpuRangeScope final {
 public:
  CpuRangeScope(CpuRangeQueue& queue, const CancellationToken& cancellation,
                const ResourceBudget* resources,
                std::function<bool()> current = {})
      : queue_(queue),
        cancellation_(cancellation),
        resources_(resources),
        current_(std::move(current)),
        owner_(std::this_thread::get_id()),
        service_{sizeof(service_), PS_CPU_PARALLEL_ABI_VERSION_1,
                 queue.workers(), this, invoke} {}
  const ps_cpu_parallel_service_v1* service() const noexcept {
    return &service_;
  }
  Status status() const {
    return violation_.load() ? Status{ErrorCode::InvalidArgument,
                                      "CPU range thread violation"}
                             : failure_;
  }

 private:
  static int invoke(void* context, std::uint64_t count, std::uint64_t grain,
                    std::uint32_t workers, ps_cpu_range_callback_v1 callback,
                    void* user) noexcept {
    auto& self = *static_cast<CpuRangeScope*>(context);
    // A worker's invalid nested call cannot mutate caller-owned sticky state.
    if (std::this_thread::get_id() != self.owner_ || in_cpu_range) {
      self.violation_.store(true);
      return 6;
    }
    try {
      if (self.failure_.ok())
        self.failure_ =
            self.queue_.run(count, grain, workers, callback, user,
                            self.cancellation_, self.resources_, self.current_);
    } catch (const std::bad_alloc&) {
      self.failure_.code = ErrorCode::ResourceExhausted;
    } catch (...) {
      self.failure_.code = ErrorCode::OperationFailed;
    }
    return self.failure_.ok()                                   ? 0
           : self.failure_.code == ErrorCode::Cancelled         ? 2
           : self.failure_.code == ErrorCode::ResourceExhausted ? 4
                                                                : 1;
  }
  CpuRangeQueue& queue_;
  CancellationToken cancellation_;
  const ResourceBudget* resources_;
  std::function<bool()> current_;
  const std::thread::id owner_;
  ps_cpu_parallel_service_v1 service_;
  Status failure_;
  std::atomic<bool> violation_{false};
};
}  // namespace ps::execution_internal
