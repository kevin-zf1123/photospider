#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#if defined(__linux__)
#include <sched.h>
#endif
#include <type_traits>

#include "05-filter/frequency_fft_layout.hpp"
#include "05-filter/frequency_work.hpp"
#include "common/floating_environment.hpp"

namespace ps::plugin_internal::frequency {

// The pool owns scheduling state only. A borrowed job exists until run()
// joins every participant; no payload, cancellation token or Run ledger is
// retained between calls. Concurrent invocations use their caller when busy.
class TransformPool final {
 public:
  TransformPool() {
    unsigned count = std::max(1U, std::thread::hardware_concurrency());
#if defined(__linux__) && defined(CPU_COUNT)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0)
      count = std::max(
          1U, std::min(count, static_cast<unsigned>(CPU_COUNT(&allowed))));
#endif
    // Respect the calling thread's CPU allowance; P/E-core placement
    // remain with the OS. Small phases never construct this pool.
    if (const auto* value = std::getenv("PHOTOSPIDER_FRQ_THREADS")) {
      char* end = nullptr;
      const auto requested = std::strtoul(value, &end, 10);
      if (*value >= '0' && *value <= '9' && end != value && *end == '\0' &&
          requested > 0)
        count = static_cast<unsigned>(
            std::min<std::uint64_t>(requested, kTransformWorkers));
    }
    count = std::min(count, kTransformWorkers);
    for (unsigned id = 1; id < count; ++id) {
      try {
        threads_[id - 1] = std::thread([this, id] { worker(id); });
        ++count_;
      } catch (const std::system_error&) {
        break;
      }
    }
  }
  ~TransformPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      ++generation_;
    }
#if defined(__x86_64__)
    for (auto& signal : ready_)
      signal.notify_one();
#else
    ready_.notify_all();
#endif
    for (auto& thread : threads_)
      if (thread.joinable())
        thread.join();
  }
  TransformPool(const TransformPool&) = delete;
  TransformPool& operator=(const TransformPool&) = delete;
  unsigned count() const { return count_; }

  template <class Function>
  void run(std::uint64_t tasks, unsigned requested, Function&& function) {
    const auto participants = static_cast<unsigned>(
        std::min<std::uint64_t>({tasks, requested, count_}));
    std::unique_lock<std::mutex> lease(lease_, std::try_to_lock);
    if (participants <= 1 || !lease.owns_lock()) {
      for (std::uint64_t i = 0; i < tasks; ++i)
        function(i, 0);
      return;
    }
    Job job;
    job.context = const_cast<void*>(static_cast<const void*>(&function));
    job.invoke = [](void* context, std::uint64_t i, unsigned id) {
      (*static_cast<std::remove_reference_t<Function>*>(context))(i, id);
    };
    job.tasks = tasks;
    job.remaining.store(participants - 1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job_ = &job;
      participants_ = participants;
      ++generation_;
#if !defined(__x86_64__)
      claimed_ = 0;
      // Keep the notifications under the lock. A quick worker must not
      // finish, wait again, and consume a second notification for this job.
      if (participants < count_) {
        for (unsigned i = 1; i < participants; ++i)
          ready_.notify_one();
      }
#endif
    }
#if defined(__x86_64__)
    // Each participant has its own wait queue. Waking only selected workers
    // avoids scheduling inactive FFT workers during shorter memory phases.
    for (unsigned id = 1; id < participants; ++id)
      ready_[id - 1].notify_one();
#else
    if (participants == count_)
      ready_.notify_all();
#endif
    execute(job, 0);
    {
      std::unique_lock<std::mutex> lock(mutex_);
      done_.wait(lock, [&] {
        return job.remaining.load(std::memory_order_acquire) == 0;
      });
      job_ = nullptr;
    }
    if (job.failure)
      std::rethrow_exception(job.failure);
  }

 private:
  struct Job {
    void* context = nullptr;
    void (*invoke)(void*, std::uint64_t, unsigned) = nullptr;
    std::uint64_t tasks = 0;
    std::atomic<std::uint64_t> next{0};
    std::atomic<unsigned> remaining{0};
    std::atomic<bool> failed{false};
    std::mutex failure_mutex;
    std::exception_ptr failure;
  };
  static void execute(Job& job, unsigned id) noexcept {
    try {
      FloatingEnvironment environment;
      require(environment.active(), "transform floating environment");
      while (!job.failed.load(std::memory_order_relaxed)) {
        const auto i = job.next.fetch_add(1, std::memory_order_relaxed);
        if (i >= job.tasks)
          break;
        job.invoke(job.context, i, id);
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(job.failure_mutex);
      if (!job.failure)
        job.failure = std::current_exception();
      job.failed.store(true, std::memory_order_relaxed);
    }
  }
  void worker(unsigned id) {
    std::uint64_t observed = 0;
    for (;;) {
      Job* job = nullptr;
      unsigned slot = id;
      {
        std::unique_lock<std::mutex> lock(mutex_);
#if defined(__x86_64__)
        auto& ready = ready_[id - 1];
#else
        auto& ready = ready_;
#endif
        ready.wait(lock, [&] { return stopping_ || generation_ != observed; });
        if (stopping_)
          return;
        observed = generation_;
        if (!job_)
          continue;
#if defined(__x86_64__)
        if (id >= participants_)
          continue;
#else
        if (participants_ < count_) {
          if (claimed_ >= participants_ - 1)
            continue;
          slot = ++claimed_;
        }
#endif
        job = job_;
      }
      execute(*job, slot);
      // The last participant takes the same mutex as the waiter before its
      // notification, so completion cannot be lost between predicate and wait.
      if (job->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        done_.notify_one();
      }
    }
  }
  std::array<std::thread, kTransformWorkers - 1> threads_;
  unsigned count_ = 1, participants_ = 0;
#if !defined(__x86_64__)
  unsigned claimed_ = 0;
#endif
  std::uint64_t generation_ = 0;
  bool stopping_ = false;
  std::mutex lease_, mutex_;
#if defined(__x86_64__)
  std::array<std::condition_variable, kTransformWorkers - 1> ready_;
#else
  std::condition_variable ready_;
#endif
  std::condition_variable done_;
  Job* job_ = nullptr;
};

inline TransformPool& transform_pool() {
  static TransformPool pool;
  return pool;
}

// The pool has quiesced before either success or an exception reaches this
// boundary. Preserve the Run ledger's sticky failure even when a worker fails.
template <class Function>
void transform_phase(const Work& work, Function&& function) {
  try {
    function();
  } catch (...) {
    check(work(0));
    throw;
  }
  check(work(0));
}

// A deterministic phase is charged exactly once on the invocation's Work.
// Workers have independent counters and a finite exact share of that phase.
// All work has already been admitted; the caller probes the host ledger again
// after joining. Cancellation remains the original thread-safe Run query.
class TransformAdmission final {
 public:
  explicit TransformAdmission(const Work& origin) : origin_(origin) {}
  Work local(std::uint64_t amount) const {
    return Work{origin_.invocation, amount};
  }

 private:
  const Work& origin_;
};
// Independent rows also cover input packing, real-spectrum recombination and
// output materialization. No invocation or mutable counter is shared by
// workers.
template <class Function>
void transform_rows(std::uint64_t rows, std::uint64_t cost_per_row,
                    const Work& work, Function&& function) {
  const auto cost = rows * cost_per_row;
  check(work(cost));
  // Keep short phases serial; large row streams need enough workers to cover
  // first-touch faults and memory traffic even when their arithmetic is light.
  constexpr std::uint64_t minimum = UINT64_C(1) << 20;
  constexpr std::uint64_t serial_limit = UINT64_C(1) << 23;
  auto workers = cost < serial_limit
                     ? 1U
                     : static_cast<unsigned>(std::min<std::uint64_t>(
                           {rows, transform_pool().count(), cost / minimum}));
#if defined(__x86_64__)
  // Large packing/recombination/output streams saturate memory bandwidth
  // before the arithmetic pool. Avoid excess first-touch and store traffic.
  if (cost >= (UINT64_C(1) << 28))
    workers = std::min(workers, 8U);
#endif
  const TransformAdmission admission(work);
  transform_phase(work, [&] {
    if (workers <= 1) {
      auto local = admission.local(cost);
      function(0, rows, local);
      return;
    }
    const auto grain = std::max<std::uint64_t>(1, rows / (workers * 4));
    transform_pool().run((rows + grain - 1) / grain, workers,
                         [&](std::uint64_t task, unsigned) {
                           const auto begin = task * grain,
                                      count = std::min(grain, rows - begin);
                           auto local = admission.local(count * cost_per_row);
                           function(begin, count, local);
                         });
  });
}

#if defined(__x86_64__)
// Long contiguous inputs expose sample chunks even when there is only one
// logical row. Keep each participant's share exact and bound memory workers.
template <class Function>
void transform_linear(std::uint64_t samples, std::uint64_t cost_per_sample,
                      const Work& work, Function&& function) {
  const auto cost = samples * cost_per_sample;
  check(work(cost));
  const auto workers = static_cast<unsigned>(
      std::min<std::uint64_t>({transform_pool().count(), 8,
                               std::max<std::uint64_t>(1, samples / 8192)}));
  const TransformAdmission admission(work);
  constexpr std::uint64_t grain = 2048;
  transform_phase(work, [&] {
    if (workers <= 1) {
      auto local = admission.local(cost);
      function(0, samples, local);
      return;
    }
    transform_pool().run((samples + grain - 1) / grain, workers,
                         [&](std::uint64_t task, unsigned) {
                           const auto begin = task * grain,
                                      count = std::min(grain, samples - begin);
                           auto local =
                               admission.local(count * cost_per_sample);
                           function(begin, count, local);
                         });
  });
}

#endif
}  // namespace ps::plugin_internal::frequency
