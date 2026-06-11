// Photospider kernel: SerialDebugScheduler implementation
// M3.4: 串行调试调度器，用于调试和单线程执行场景

#include "kernel/scheduler/serial_debug_scheduler.hpp"

#include <sstream>

#include "kernel/graph_runtime.hpp"

namespace ps {

SerialDebugScheduler::~SerialDebugScheduler() {
  shutdown();
}

void SerialDebugScheduler::attach(GraphRuntime* runtime) {
  runtime_ = runtime;
}

void SerialDebugScheduler::detach() {
  runtime_ = nullptr;
}

void SerialDebugScheduler::start() {
  running_.store(true, std::memory_order_release);
}

void SerialDebugScheduler::shutdown() {
  running_.store(false, std::memory_order_release);
}

std::string SerialDebugScheduler::name() const {
  return "serial_debug";
}

std::string SerialDebugScheduler::get_stats() const {
  std::ostringstream oss;
  oss << "SerialDebugScheduler Stats:\n"
      << "  Running: " << (running_.load() ? "yes" : "no") << "\n"
      << "  Tasks executed: " << tasks_executed_.load();
  return oss.str();
}

bool SerialDebugScheduler::is_running() const {
  return running_.load(std::memory_order_acquire);
}

bool SerialDebugScheduler::task_runtime_running() const {
  return is_running();
}

void SerialDebugScheduler::submit_initial_tasks(std::vector<Task>&& tasks,
                                                int total_task_count,
                                                TaskPriority priority) {
  (void)priority;
  inline_exception_ = nullptr;
  inline_tasks_to_complete_ = total_task_count;
  for (auto& task : tasks) {
    if (!task || inline_exception_) {
      continue;
    }
    try {
      task();
    } catch (...) {
      set_exception(std::current_exception());
    }
  }
}

void SerialDebugScheduler::submit_ready_task_from_worker(
    Task&& task, TaskPriority priority) {
  submit_ready_task_any_thread(std::move(task), priority, std::nullopt);
}

void SerialDebugScheduler::submit_ready_task_any_thread(
    Task&& task, TaskPriority priority, std::optional<uint64_t> epoch) {
  (void)priority;
  (void)epoch;
  if (!task || inline_exception_) {
    return;
  }
  try {
    task();
  } catch (...) {
    set_exception(std::current_exception());
  }
}

void SerialDebugScheduler::wait_for_completion() {
  if (inline_exception_) {
    std::rethrow_exception(inline_exception_);
  }
}

void SerialDebugScheduler::set_exception(std::exception_ptr e) {
  if (!inline_exception_) {
    inline_exception_ = e;
  }
}

void SerialDebugScheduler::inc_tasks_to_complete(int delta) {
  if (delta > 0) {
    inline_tasks_to_complete_ += delta;
  }
}

void SerialDebugScheduler::dec_tasks_to_complete() {
  if (inline_tasks_to_complete_ > 0) {
    --inline_tasks_to_complete_;
  }
}

void SerialDebugScheduler::log_event(SchedulerTraceAction action, int node_id) {
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
  runtime_->log_event(runtime_action, node_id);
}

}  // namespace ps
