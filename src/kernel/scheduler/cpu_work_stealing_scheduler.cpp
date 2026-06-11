// Photospider kernel: CpuWorkStealingScheduler implementation
// M3.3: 将现有的 run_loop 和队列逻辑迁移至可插拔调度器

#include "kernel/scheduler/cpu_work_stealing_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

#include "kernel/graph_runtime.hpp"

namespace ps {

// Thread-local storage for worker ID and epoch tracking
thread_local int CpuWorkStealingScheduler::tls_worker_id_ = -1;
thread_local uint64_t CpuWorkStealingScheduler::tls_active_epoch_ = 0;

CpuWorkStealingScheduler::CpuWorkStealingScheduler(unsigned int num_workers)
    : configured_workers_(num_workers) {
  if (configured_workers_ == 0) {
    configured_workers_ = std::max(1u, std::thread::hardware_concurrency());
  }
}

CpuWorkStealingScheduler::~CpuWorkStealingScheduler() {
  shutdown();
}

void CpuWorkStealingScheduler::attach(GraphRuntime* runtime) {
  runtime_ = runtime;
}

void CpuWorkStealingScheduler::detach() {
  runtime_ = nullptr;
}

void CpuWorkStealingScheduler::start() {
  if (running_.load(std::memory_order_acquire)) {
    return;
  }

  // Reset counters
  has_exception_.store(false, std::memory_order_relaxed);
  first_exception_ = nullptr;
  ready_task_count_.store(0, std::memory_order_relaxed);
  sleeping_thread_count_.store(0, std::memory_order_relaxed);
  tasks_to_complete_.store(0, std::memory_order_relaxed);
  high_enqueued_.store(0, std::memory_order_relaxed);
  normal_enqueued_.store(0, std::memory_order_relaxed);
  high_executed_.store(0, std::memory_order_relaxed);
  normal_executed_.store(0, std::memory_order_relaxed);

  running_.store(true, std::memory_order_release);

  num_workers_ = configured_workers_;
  local_task_queues_.resize(num_workers_);
  local_queue_mutexes_.reserve(num_workers_);
  for (unsigned int i = 0; i < num_workers_; ++i) {
    local_queue_mutexes_.push_back(std::make_unique<std::mutex>());
  }

  workers_.reserve(num_workers_);
  for (unsigned int i = 0; i < num_workers_; ++i) {
    workers_.emplace_back(&CpuWorkStealingScheduler::run_loop, this,
                          static_cast<int>(i));
  }
}

void CpuWorkStealingScheduler::shutdown() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  running_.store(false, std::memory_order_release);
  cv_task_available_.notify_all();
  cv_completion_.notify_all();

  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  local_task_queues_.clear();
  local_queue_mutexes_.clear();

  // Drain pending tasks
  {
    std::lock_guard<std::mutex> lock(global_queues_mutex_);
    while (!high_priority_queue_.empty()) {
      high_priority_queue_.pop();
    }
    while (!normal_priority_queue_.empty()) {
      normal_priority_queue_.pop();
    }
  }
}

std::string CpuWorkStealingScheduler::name() const {
  return "CpuWorkStealingScheduler";
}

std::string CpuWorkStealingScheduler::get_stats() const {
  std::ostringstream oss;
  oss << "Workers: " << num_workers_
      << ", Ready tasks: " << ready_task_count_.load(std::memory_order_relaxed)
      << ", Sleeping: "
      << sleeping_thread_count_.load(std::memory_order_relaxed)
      << ", High enqueued/executed: "
      << high_enqueued_.load(std::memory_order_relaxed) << "/"
      << high_executed_.load(std::memory_order_relaxed)
      << ", Normal enqueued/executed: "
      << normal_enqueued_.load(std::memory_order_relaxed) << "/"
      << normal_executed_.load(std::memory_order_relaxed)
      << ", Total scheduled: "
      << total_tasks_scheduled_.load(std::memory_order_relaxed);
  return oss.str();
}

bool CpuWorkStealingScheduler::is_running() const {
  return running_.load(std::memory_order_acquire);
}

bool CpuWorkStealingScheduler::task_runtime_running() const {
  return is_running();
}

void CpuWorkStealingScheduler::log_event(SchedulerTraceAction action,
                                         int node_id) {
  if (!runtime_) {
    return;
  }

  GraphRuntime::SchedulerEvent::Action runtime_action =
      GraphRuntime::SchedulerEvent::EXECUTE;
  switch (action) {
    case SchedulerTraceAction::AssignInitial:
      runtime_action = GraphRuntime::SchedulerEvent::ASSIGN_INITIAL;
      break;
    case SchedulerTraceAction::Execute:
      runtime_action = GraphRuntime::SchedulerEvent::EXECUTE;
      break;
    case SchedulerTraceAction::ExecuteTile:
      runtime_action = GraphRuntime::SchedulerEvent::EXECUTE_TILE;
      break;
  }
  runtime_->log_event(runtime_action, node_id, this_worker_id(),
                      this_task_epoch());
}

int CpuWorkStealingScheduler::this_worker_id() {
  return tls_worker_id_;
}

uint64_t CpuWorkStealingScheduler::this_task_epoch() {
  return tls_active_epoch_;
}

uint64_t CpuWorkStealingScheduler::active_epoch() const {
  return active_epoch_.load(std::memory_order_acquire);
}

uint64_t CpuWorkStealingScheduler::begin_new_epoch() {
  uint64_t next = epoch_counter_.fetch_add(1, std::memory_order_acq_rel) + 1;
  active_epoch_.store(next, std::memory_order_release);
  cancel_stale_enqueued_tasks(next);
  return next;
}

bool CpuWorkStealingScheduler::should_cancel_epoch(uint64_t epoch) const {
  if (epoch == 0) {
    return false;
  }
  return epoch < active_epoch();
}

void CpuWorkStealingScheduler::cancel_stale_enqueued_tasks(uint64_t min_epoch) {
  if (min_epoch == 0) {
    return;
  }

  size_t removed = 0;
  {
    std::lock_guard<std::mutex> lock(global_queues_mutex_);
    auto purge_queue = [&](auto& q) {
      std::queue<ScheduledTask> kept;
      while (!q.empty()) {
        auto task = std::move(q.front());
        q.pop();
        if (task.epoch != 0 && task.epoch < min_epoch) {
          ++removed;
          continue;
        }
        kept.push(std::move(task));
      }
      q.swap(kept);
    };
    purge_queue(high_priority_queue_);
    purge_queue(normal_priority_queue_);
  }

  for (size_t i = 0; i < local_task_queues_.size(); ++i) {
    if (i >= local_queue_mutexes_.size() || !local_queue_mutexes_[i]) {
      continue;
    }
    std::lock_guard<std::mutex> lock(*local_queue_mutexes_[i]);
    auto& dq = local_task_queues_[i];
    auto it = dq.begin();
    while (it != dq.end()) {
      if (it->epoch != 0 && it->epoch < min_epoch) {
        it = dq.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
  }

  if (removed > 0) {
    ready_task_count_.fetch_sub(static_cast<int>(removed),
                                std::memory_order_acq_rel);
  }
}

std::optional<CpuWorkStealingScheduler::ScheduledTask>
CpuWorkStealingScheduler::steal_task(int stealer_id) {
  int n = static_cast<int>(num_workers_);
  if (n <= 1) {
    return std::nullopt;
  }

  static thread_local std::mt19937 rng(std::random_device{}() + stealer_id);
  int start = std::uniform_int_distribution<int>(0, n - 2)(rng);

  for (int i = 0; i < n - 1; ++i) {
    int victim_id = (start + i) % (n - 1);
    if (victim_id >= stealer_id) {
      victim_id++;
    }

    if (victim_id < 0 ||
        victim_id >= static_cast<int>(local_queue_mutexes_.size()) ||
        victim_id >= static_cast<int>(local_task_queues_.size())) {
      continue;
    }

    std::lock_guard<std::mutex> lock(*local_queue_mutexes_[victim_id]);
    if (!local_task_queues_[victim_id].empty()) {
      ScheduledTask stolen_task =
          std::move(local_task_queues_[victim_id].front());
      local_task_queues_[victim_id].pop_front();
      return stolen_task;
    }
  }
  return std::nullopt;
}

void CpuWorkStealingScheduler::run_loop(int thread_id) {
  tls_worker_id_ = thread_id;

  while (running_.load(std::memory_order_acquire)) {
    // If an exception was raised, park until cleared
    if (has_exception_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(global_queues_mutex_);
      cv_task_available_.wait(lock, [&] {
        return !has_exception_.load(std::memory_order_acquire) ||
               !running_.load(std::memory_order_acquire);
      });
      continue;
    }

    ScheduledTask scheduled;
    bool found_task = false;

    // 1) Try high priority global queue first (preemptive)
    {
      std::lock_guard<std::mutex> lock(global_queues_mutex_);
      if (!high_priority_queue_.empty()) {
        scheduled = std::move(high_priority_queue_.front());
        high_priority_queue_.pop();
        found_task = true;
        high_executed_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // 2) Then try local normal queue (LIFO)
    if (!found_task) {
      if (thread_id >= 0 &&
          thread_id < static_cast<int>(local_queue_mutexes_.size())) {
        std::lock_guard<std::mutex> lock(*local_queue_mutexes_[thread_id]);
        if (thread_id < static_cast<int>(local_task_queues_.size()) &&
            !local_task_queues_[thread_id].empty()) {
          scheduled = std::move(local_task_queues_[thread_id].back());
          local_task_queues_[thread_id].pop_back();
          found_task = true;
          normal_executed_.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }

    // 3) Then try global normal queue (FIFO)
    if (!found_task) {
      std::lock_guard<std::mutex> lock(global_queues_mutex_);
      if (!normal_priority_queue_.empty()) {
        scheduled = std::move(normal_priority_queue_.front());
        normal_priority_queue_.pop();
        found_task = true;
        normal_executed_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // 4) Finally, attempt to steal from other workers
    if (!found_task) {
      auto stolen_task = steal_task(thread_id);
      if (stolen_task) {
        scheduled = std::move(*stolen_task);
        found_task = true;
        normal_executed_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    if (found_task) {
      ready_task_count_.fetch_sub(1, std::memory_order_release);
      if (should_cancel_epoch(scheduled.epoch)) {
        continue;
      }
      try {
        if (scheduled) {
          struct EpochScope {
            uint64_t* slot;
            uint64_t prev;
            EpochScope(uint64_t* s, uint64_t value) : slot(s), prev(*s) {
              *slot = value;
            }
            ~EpochScope() { *slot = prev; }
          } epoch_scope(&tls_active_epoch_, scheduled.epoch);
          struct RuntimeLogScope {
            RuntimeLogScope(int worker_id, uint64_t epoch) {
              GraphRuntime::set_scheduler_log_context(worker_id, epoch);
            }
            ~RuntimeLogScope() { GraphRuntime::clear_scheduler_log_context(); }
          } runtime_log_scope(thread_id, scheduled.epoch);
          scheduled.task();
        } else {
          set_exception(std::make_exception_ptr(std::runtime_error(
              "CpuWorkStealingScheduler: empty task invoked")));
        }
      } catch (...) {
        set_exception(std::current_exception());
      }
    } else {
      sleeping_thread_count_.fetch_add(1, std::memory_order_release);

      std::unique_lock<std::mutex> lock(global_queues_mutex_);

      if (ready_task_count_.load(std::memory_order_acquire) > 0) {
        sleeping_thread_count_.fetch_sub(1, std::memory_order_relaxed);
        continue;
      }

      cv_task_available_.wait(lock, [&] {
        return ready_task_count_.load(std::memory_order_acquire) > 0 ||
               !running_.load(std::memory_order_acquire) ||
               has_exception_.load(std::memory_order_acquire);
      });

      sleeping_thread_count_.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

void CpuWorkStealingScheduler::submit_initial_tasks(std::vector<Task>&& tasks,
                                                    int total_task_count,
                                                    TaskPriority priority) {
  has_exception_.store(false, std::memory_order_relaxed);
  first_exception_ = nullptr;

  uint64_t epoch = begin_new_epoch();
  tasks_to_complete_.store(total_task_count, std::memory_order_relaxed);

  if (tasks_to_complete_.load() == 0) {
    std::lock_guard<std::mutex> lk(completion_mutex_);
    cv_completion_.notify_one();
    return;
  }

  int num_threads = static_cast<int>(num_workers_);
  if (num_threads == 0 || tasks.empty()) {
    if (tasks_to_complete_.load(std::memory_order_relaxed) != 0) {
      tasks_to_complete_.store(0, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lk(completion_mutex_);
      cv_completion_.notify_one();
    }
    return;
  }

  auto wrap_task = [&](Task&& task) -> ScheduledTask {
    return ScheduledTask(epoch, std::move(task));
  };

  size_t task_count = tasks.size();
  if (priority == TaskPriority::High) {
    std::lock_guard<std::mutex> lock(global_queues_mutex_);
    for (auto& t : tasks) {
      high_priority_queue_.push(wrap_task(std::move(t)));
      high_enqueued_.fetch_add(1, std::memory_order_relaxed);
    }
    ready_task_count_.fetch_add(static_cast<int>(task_count),
                                std::memory_order_release);
    cv_task_available_.notify_all();
  } else {
    static thread_local std::mt19937 rng(std::random_device{}());
    for (size_t i = 0; i < tasks.size(); ++i) {
      int target_thread =
          std::uniform_int_distribution<int>(0, num_threads - 1)(rng);
      std::lock_guard<std::mutex> lock(*local_queue_mutexes_[target_thread]);
      local_task_queues_[target_thread].push_back(
          wrap_task(std::move(tasks[i])));
      normal_enqueued_.fetch_add(1, std::memory_order_relaxed);
    }
    ready_task_count_.fetch_add(static_cast<int>(task_count),
                                std::memory_order_release);
    cv_task_available_.notify_all();
  }
}

void CpuWorkStealingScheduler::submit_ready_task_any_thread(
    Task&& task, TaskPriority priority, std::optional<uint64_t> epoch) {
  if (!task) {
    return;
  }
  uint64_t resolved_epoch = epoch.has_value() ? *epoch : active_epoch();
  if (should_cancel_epoch(resolved_epoch)) {
    return;
  }
  ScheduledTask scheduled(resolved_epoch, std::move(task));
  {
    std::lock_guard<std::mutex> lock(global_queues_mutex_);
    if (priority == TaskPriority::High) {
      high_priority_queue_.push(std::move(scheduled));
      high_enqueued_.fetch_add(1, std::memory_order_relaxed);
    } else {
      normal_priority_queue_.push(std::move(scheduled));
      normal_enqueued_.fetch_add(1, std::memory_order_relaxed);
    }
    ready_task_count_.fetch_add(1, std::memory_order_relaxed);
  }
  cv_task_available_.notify_one();
}

void CpuWorkStealingScheduler::submit_ready_task_from_worker(
    Task&& task, TaskPriority priority) {
  int worker_id = this_worker_id();
  if (worker_id < 0 ||
      worker_id >= static_cast<int>(local_task_queues_.size())) {
    submit_ready_task_any_thread(std::move(task), priority, std::nullopt);
    return;
  }
  if (!task) {
    return;
  }
  uint64_t epoch = tls_active_epoch_;
  if (epoch == 0) {
    epoch = active_epoch();
  }
  if (should_cancel_epoch(epoch)) {
    return;
  }
  ScheduledTask scheduled(epoch, std::move(task));
  if (priority == TaskPriority::High) {
    submit_ready_task_any_thread(std::move(scheduled.task), TaskPriority::High,
                                 epoch);
  } else {
    {
      std::lock_guard<std::mutex> lock(*local_queue_mutexes_[worker_id]);
      local_task_queues_[worker_id].push_back(std::move(scheduled));
      normal_enqueued_.fetch_add(1, std::memory_order_relaxed);
      ready_task_count_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_task_available_.notify_one();
  }
}

void CpuWorkStealingScheduler::dec_tasks_to_complete() {
  uint64_t epoch = tls_active_epoch_;
  if (epoch != 0 && should_cancel_epoch(epoch)) {
    return;
  }
  if (tasks_to_complete_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::lock_guard<std::mutex> lk(completion_mutex_);
    cv_completion_.notify_one();
  }
}

void CpuWorkStealingScheduler::inc_tasks_to_complete(int delta) {
  if (delta <= 0) {
    return;
  }
  uint64_t epoch = tls_active_epoch_;
  if (epoch != 0 && should_cancel_epoch(epoch)) {
    return;
  }
  tasks_to_complete_.fetch_add(delta, std::memory_order_relaxed);
}

void CpuWorkStealingScheduler::wait_for_completion() {
  {
    std::unique_lock<std::mutex> lock(completion_mutex_);
    cv_completion_.wait(lock, [&] {
      return tasks_to_complete_.load(std::memory_order_acquire) == 0 ||
             has_exception_.load(std::memory_order_acquire) ||
             !running_.load(std::memory_order_acquire);
    });
  }

  if (has_exception_.load(std::memory_order_relaxed)) {
    std::exception_ptr e;
    {
      std::lock_guard<std::mutex> lock(exception_mutex_);
      e = first_exception_;
      first_exception_ = nullptr;
      has_exception_.store(false, std::memory_order_release);
    }
    cv_task_available_.notify_all();
    std::rethrow_exception(e);
  }
}

void CpuWorkStealingScheduler::set_exception(std::exception_ptr e) {
  if (!has_exception_.exchange(true, std::memory_order_acq_rel)) {
    std::lock_guard<std::mutex> lock(exception_mutex_);
    first_exception_ = e;
    {
      std::lock_guard<std::mutex> ql(global_queues_mutex_);
      while (!high_priority_queue_.empty()) {
        high_priority_queue_.pop();
      }
      while (!normal_priority_queue_.empty()) {
        normal_priority_queue_.pop();
      }
      ready_task_count_.store(0, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < local_task_queues_.size(); ++i) {
      if (i < local_queue_mutexes_.size() && local_queue_mutexes_[i]) {
        std::lock_guard<std::mutex> lk(*local_queue_mutexes_[i]);
        local_task_queues_[i].clear();
      }
    }
    cv_task_available_.notify_all();
    {
      std::lock_guard<std::mutex> lk_comp(completion_mutex_);
      cv_completion_.notify_all();
    }
  }
}

}  // namespace ps
