// Photospider kernel: GraphRuntime implementation (Objective-C++)
#include "runtime/graph_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "compute/realtime_proxy_graph.hpp"

#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

namespace ps {
namespace {

/**
 * @brief Increments an unsigned trace counter without wrapping.
 * @param value Counter to update.
 * @return Nothing.
 * @throws Nothing.
 */
void saturating_increment(uint64_t& value) noexcept {
  if (value != kObservationSequenceExhausted) {
    ++value;
  }
}

/**
 * @brief Adds two unsigned trace gap counts without wrapping.
 * @param lhs First count.
 * @param rhs Second count.
 * @return Saturating sum.
 * @throws Nothing.
 */
uint64_t saturating_add(uint64_t lhs, uint64_t rhs) noexcept {
  if (kObservationSequenceExhausted - lhs < rhs) {
    return kObservationSequenceExhausted;
  }
  return lhs + rhs;
}

/**
 * @brief Advances a valid trace sequence to its successor or sentinel.
 * @param sequence Valid publication sequence.
 * @return Next sequence or `kObservationSequenceExhausted`.
 * @throws Nothing.
 */
uint64_t sequence_successor(uint64_t sequence) noexcept {
  if (sequence >= kObservationSequenceExhausted - 1) {
    return kObservationSequenceExhausted;
  }
  return sequence + 1;
}

/**
 * @brief Counts unavailable valid sequences between a cursor and boundary.
 * @param after_sequence Exclusive caller cursor.
 * @param first_available First retained/next sequence after missing history.
 * @return Exact gap, or zero when the boundary immediately follows the cursor.
 * @throws Nothing.
 */
uint64_t sequence_gap(uint64_t after_sequence,
                      uint64_t first_available) noexcept {
  if (after_sequence == kObservationSequenceExhausted ||
      first_available <= after_sequence) {
    return 0;
  }
  const uint64_t distance = first_available - after_sequence;
  return distance > 1 ? distance - 1 : 0;
}

/**
 * @brief Runs explicit scheduler shutdown and detach as a best-effort sweep.
 *
 * Shutdown is attempted before detach. Each stage has an independent exception
 * fence so a hostile shutdown cannot prevent detach.
 *
 * @param scheduler Scheduler whose lifecycle ownership is being rolled back or
 * released; null is accepted as an already-clean owner.
 * @return The exact first lifecycle exception, or an empty pointer on success.
 * @throws Nothing; lifecycle failures are returned to the caller.
 * @note The helper neither destroys nor publishes the scheduler owner.
 */
std::exception_ptr cleanup_scheduler_lifecycle(IScheduler* scheduler) noexcept {
  if (!scheduler) {
    return nullptr;
  }

  std::exception_ptr first_error;
  try {
    scheduler->shutdown();
  } catch (...) {
    first_error = std::current_exception();
  }
  try {
    scheduler->detach();
  } catch (...) {
    if (!first_error) {
      first_error = std::current_exception();
    }
  }
  return first_error;
}

}  // namespace

/** @copydoc GraphRuntime::tls_worker_id_ */
thread_local int GraphRuntime::tls_worker_id_ = -1;
/** @copydoc GraphRuntime::tls_scheduler_log_worker_id_ */
thread_local int GraphRuntime::tls_scheduler_log_worker_id_ = -1;
/** @copydoc GraphRuntime::tls_scheduler_log_epoch_ */
thread_local uint64_t GraphRuntime::tls_scheduler_log_epoch_ = 0;

/**
 * @brief Owns private platform GPU objects for one graph runtime.
 * @note The type is complete only in this implementation and never crosses the
 *       scheduler SDK host-context boundary.
 */
struct GraphRuntime::GpuContext {
#ifdef __APPLE__
  /** @brief Runtime-owned default Metal device, or nil when unavailable. */
  id<MTLDevice> device;
  /** @brief Runtime-owned Metal command queue, or nil without a device. */
  id<MTLCommandQueue> commandQueue;
#endif
};

/** @copydoc GraphRuntime::GraphRuntime */
GraphRuntime::GraphRuntime(const Info& info)
    : info_(info),
      model_(info.cache_root.empty() ? info.root / "cache" : info.cache_root),
      graph_state_(model_),
      event_service_(info.compute_event_capacity,
                     info.compute_event_initial_sequence,
                     info.compute_event_initial_dropped_count),
      realtime_proxy_graph_(std::make_unique<compute::RealtimeProxyGraph>()),
      scheduler_trace_slots_(info.scheduler_trace_capacity),
      scheduler_trace_next_sequence_(info.scheduler_trace_initial_sequence),
      scheduler_trace_unsequenced_drops_(
          info.scheduler_trace_initial_dropped_count) {
  if (info.scheduler_trace_capacity == 0) {
    throw std::invalid_argument(
        "scheduler-trace ring capacity must be nonzero");
  }
  if (info.scheduler_trace_initial_sequence == 0) {
    throw std::invalid_argument(
        "scheduler-trace initial sequence must be nonzero");
  }
  static_assert(std::is_nothrow_move_constructible_v<SchedulerEvent>);
  static_assert(std::is_nothrow_move_assignable_v<SchedulerEvent>);

  std::filesystem::create_directories(info_.root);
  if (!model_.cache_root.empty()) {
    std::filesystem::create_directories(model_.cache_root);
  }

#ifdef __APPLE__
  gpu_context_ = std::make_unique<GpuContext>();
  gpu_context_->device = MTLCreateSystemDefaultDevice();
  if (gpu_context_->device) {
    gpu_context_->commandQueue = [gpu_context_->device newCommandQueue];
  } else {
    fprintf(stderr, "Warning: Could not create default Metal device.\n");
  }
#else
  // On non-Apple platforms, gpu_context_ remains null
#endif
}

/** @copydoc GraphRuntime::~GraphRuntime */
GraphRuntime::~GraphRuntime() noexcept {
  try {
    graph_state_.close_and_drain();
  } catch (...) {
    std::terminate();
  }
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  running_.store(false, std::memory_order_release);
  for (auto& [intent, binding] : schedulers_) {
    (void)intent;
    (void)cleanup_scheduler_lifecycle(binding.scheduler.get());
  }
  schedulers_.clear();
}

/** @copydoc GraphRuntime::this_worker_id */
int GraphRuntime::this_worker_id() noexcept {
  if (tls_worker_id_ >= 0) {
    return tls_worker_id_;
  }
  return tls_scheduler_log_worker_id_;
}

/** @copydoc GraphRuntime::get_metal_device */
id GraphRuntime::get_metal_device() noexcept {
#ifdef __APPLE__
  return gpu_context_ ? gpu_context_->device : nil;
#else
  return nullptr;
#endif
}

/** @copydoc GraphRuntime::get_metal_command_queue */
id GraphRuntime::get_metal_command_queue() noexcept {
#ifdef __APPLE__
  return gpu_context_ ? gpu_context_->commandQueue : nil;
#else
  return nullptr;
#endif
}

/** @copydoc GraphRuntime::realtime_proxy_graph */
compute::RealtimeProxyGraph& GraphRuntime::realtime_proxy_graph() {
  return *realtime_proxy_graph_;
}

/** @copydoc GraphRuntime::start */
void GraphRuntime::start() {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  if (running_.load(std::memory_order_acquire)) {
    return;
  }

  std::vector<IScheduler*> started_schedulers;
  started_schedulers.reserve(schedulers_.size());
  try {
    for (auto& [intent, binding] : schedulers_) {
      (void)intent;
      auto& scheduler = binding.scheduler;
      if (scheduler && !scheduler->is_running()) {
        started_schedulers.push_back(scheduler.get());
        scheduler->start();
      }
    }
  } catch (...) {
    const std::exception_ptr start_error = std::current_exception();
    for (auto it = started_schedulers.rbegin(); it != started_schedulers.rend();
         ++it) {
      try {
        if (*it) {
          (*it)->shutdown();
        }
      } catch (...) {
      }
    }
    running_.store(false, std::memory_order_release);
    std::rethrow_exception(start_error);
  }
  running_.store(true, std::memory_order_release);
}

/** @copydoc GraphRuntime::stop */
void GraphRuntime::stop() {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  running_.store(false, std::memory_order_release);
  std::exception_ptr first_stop_error;
  for (auto& [intent, binding] : schedulers_) {
    (void)intent;
    auto& scheduler = binding.scheduler;
    if (!scheduler) {
      continue;
    }

    bool shutdown_required = true;
    try {
      shutdown_required = scheduler->is_running();
    } catch (...) {
      if (!first_stop_error) {
        first_stop_error = std::current_exception();
      }
    }

    if (!shutdown_required) {
      continue;
    }

    try {
      scheduler->shutdown();
    } catch (...) {
      if (!first_stop_error) {
        first_stop_error = std::current_exception();
      }
    }
  }
  if (first_stop_error) {
    std::rethrow_exception(first_stop_error);
  }
}

/** @copydoc GraphRuntime::this_task_epoch */
uint64_t GraphRuntime::this_task_epoch() noexcept {
  return tls_scheduler_log_epoch_;
}

/** @copydoc GraphRuntime::set_scheduler_log_context */
void GraphRuntime::set_scheduler_log_context(int worker_id,
                                             uint64_t epoch) noexcept {
  tls_scheduler_log_worker_id_ = worker_id;
  tls_scheduler_log_epoch_ = epoch;
}

/** @copydoc GraphRuntime::clear_scheduler_log_context */
void GraphRuntime::clear_scheduler_log_context() noexcept {
  tls_scheduler_log_worker_id_ = -1;
  tls_scheduler_log_epoch_ = 0;
}

/** @copydoc GraphRuntime::is_device_available */
bool GraphRuntime::is_device_available(Device device) const noexcept {
  switch (device) {
    case Device::CPU:
      return true;
    case Device::GPU_METAL:
#ifdef __APPLE__
      return gpu_context_ != nullptr && gpu_context_->device != nil;
#else
      return false;
#endif
    case Device::GPU_CUDA:
    case Device::ASIC_NPU:
      return false;
  }
  return false;
}

/** @copydoc GraphRuntime::set_task_context */
void GraphRuntime::set_task_context(int worker_id, uint64_t epoch) noexcept {
  set_scheduler_log_context(worker_id, epoch);
}

/** @copydoc GraphRuntime::clear_task_context */
void GraphRuntime::clear_task_context() noexcept {
  clear_scheduler_log_context();
}

/** @copydoc GraphRuntime::log_event(SchedulerTraceAction,int,int,uint64_t) */
void GraphRuntime::log_event(SchedulerTraceAction action, int node_id,
                             int worker_id, uint64_t epoch) noexcept {
  SchedulerEvent::Action runtime_action = SchedulerEvent::EXECUTE;
  switch (action) {
    case SchedulerTraceAction::AssignInitial:
      runtime_action = SchedulerEvent::ASSIGN_INITIAL;
      break;
    case SchedulerTraceAction::Execute:
      runtime_action = SchedulerEvent::EXECUTE;
      break;
    case SchedulerTraceAction::ExecuteTile:
      runtime_action = SchedulerEvent::EXECUTE_TILE;
      break;
    case SchedulerTraceAction::ExecuteDirtySource:
      runtime_action = SchedulerEvent::EXECUTE_DIRTY_SOURCE;
      break;
    case SchedulerTraceAction::ExecuteDirtyDownstreamNode:
      runtime_action = SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_NODE;
      break;
    case SchedulerTraceAction::ExecuteDirtyDownstreamTile:
      runtime_action = SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE;
      break;
    case SchedulerTraceAction::SkipStaleGeneration:
      runtime_action = SchedulerEvent::SKIP_STALE_GENERATION;
      break;
    case SchedulerTraceAction::RethrowException:
      runtime_action = SchedulerEvent::RETHROW_EXCEPTION;
      break;
  }
  try {
    log_event(runtime_action, node_id, worker_id, epoch);
  } catch (...) {
    // Scheduler observations are best effort and cannot replace task failures.
  }
}

/** @copydoc GraphRuntime::log_event(SchedulerEvent::Action,int) */
void GraphRuntime::log_event(SchedulerEvent::Action action, int node_id) {
  uint64_t epoch = this_task_epoch();
  int worker_id = this_worker_id();
  if (worker_id < 0) {
    worker_id = tls_scheduler_log_worker_id_;
  }
  log_event(action, node_id, worker_id, epoch);
}

/** @copydoc GraphRuntime::log_event(SchedulerEvent::Action,int,int,uint64_t)
 */
void GraphRuntime::log_event(SchedulerEvent::Action action, int node_id,
                             int worker_id, uint64_t epoch) {
  std::lock_guard<std::mutex> lock(log_mutex_);
  if (scheduler_trace_next_sequence_ == kObservationSequenceExhausted) {
    saturating_increment(scheduler_trace_unsequenced_drops_);
    return;
  }

  const uint64_t sequence = scheduler_trace_next_sequence_;
  SchedulerEvent event{sequence, epoch,
                       node_id,  worker_id,
                       action,   std::chrono::high_resolution_clock::now()};
  scheduler_trace_next_sequence_ = sequence_successor(sequence);

  if (scheduler_trace_size_ == scheduler_trace_slots_.size()) {
    scheduler_trace_slots_[scheduler_trace_head_] = std::move(event);
    scheduler_trace_head_ =
        (scheduler_trace_head_ + 1) % scheduler_trace_slots_.size();
    return;
  }

  const std::size_t insertion =
      (scheduler_trace_head_ + scheduler_trace_size_) %
      scheduler_trace_slots_.size();
  scheduler_trace_slots_[insertion].emplace(std::move(event));
  ++scheduler_trace_size_;
}

/** @copydoc GraphRuntime::scheduler_trace_page */
GraphRuntime::SchedulerEventPage GraphRuntime::scheduler_trace_page(
    uint64_t after_sequence,
    std::size_t limit) const {  // NOLINT(whitespace/indent_namespace)
  if (limit < kSchedulerTraceMinLimit || limit > kSchedulerTraceMaxLimit) {
    throw std::invalid_argument("scheduler-trace limit is out of range");
  }

  std::lock_guard<std::mutex> lock(log_mutex_);
  SchedulerEventPage page;
  if (after_sequence == kObservationSequenceExhausted) {
    if (scheduler_trace_next_sequence_ != kObservationSequenceExhausted) {
      throw std::invalid_argument(
          "scheduler-trace exhausted cursor precedes exhaustion");
    }
    page.next_sequence = kObservationSequenceExhausted;
    return page;
  }

  const uint64_t last_published =
      scheduler_trace_next_sequence_ == kObservationSequenceExhausted
          ? kObservationSequenceExhausted - 1
          : scheduler_trace_next_sequence_ - 1;
  if (after_sequence > last_published) {
    throw std::invalid_argument("scheduler-trace cursor is in the future");
  }

  page.events.reserve(std::min(limit, scheduler_trace_size_));
  std::size_t matching_count = 0;
  uint64_t first_later_sequence = scheduler_trace_next_sequence_;
  for (std::size_t offset = 0; offset < scheduler_trace_size_; ++offset) {
    const std::size_t index =
        (scheduler_trace_head_ + offset) % scheduler_trace_slots_.size();
    const SchedulerEvent& event = *scheduler_trace_slots_[index];
    if (event.sequence <= after_sequence) {
      continue;
    }
    if (matching_count == 0) {
      first_later_sequence = event.sequence;
    }
    if (page.events.size() < limit) {
      page.events.push_back(event);
    }
    ++matching_count;
  }

  page.has_more = matching_count > page.events.size();
  page.dropped_count =
      saturating_add(sequence_gap(after_sequence, first_later_sequence),
                     scheduler_trace_unsequenced_drops_);

  if (!page.events.empty()) {
    const uint64_t last_returned = page.events.back().sequence;
    const bool terminal_page =
        scheduler_trace_next_sequence_ == kObservationSequenceExhausted &&
        !page.has_more && last_returned == kObservationSequenceExhausted - 1;
    page.next_sequence =
        terminal_page ? kObservationSequenceExhausted : last_returned;
  } else if (scheduler_trace_next_sequence_ == kObservationSequenceExhausted) {
    page.next_sequence = kObservationSequenceExhausted;
    page.has_more = false;
  } else {
    page.next_sequence = after_sequence;
  }
  return page;
}

/** @copydoc GraphRuntime::clear_scheduler_log */
void GraphRuntime::clear_scheduler_log() {
  std::lock_guard<std::mutex> lock(log_mutex_);
  for (auto& slot : scheduler_trace_slots_) {
    slot.reset();
  }
  scheduler_trace_head_ = 0;
  scheduler_trace_size_ = 0;
}

/** @copydoc GraphRuntime::set_scheduler */
void GraphRuntime::set_scheduler(ComputeIntent intent,
                                 std::unique_ptr<IScheduler> scheduler) {
  replace_scheduler(intent, std::move(scheduler));
}

/** @copydoc GraphRuntime::set_scheduler */
void GraphRuntime::set_scheduler(ComputeIntent intent,
                                 std::unique_ptr<IScheduler> scheduler,
                                 SchedulerExecutionRoute execution_route) {
  replace_scheduler(intent, std::move(scheduler), execution_route);
}

/** @copydoc GraphRuntime::get_scheduler(ComputeIntent) */
IScheduler* GraphRuntime::get_scheduler(ComputeIntent intent) {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  auto it = schedulers_.find(intent);
  return (it != schedulers_.end()) ? it->second.scheduler.get() : nullptr;
}

/** @copydoc GraphRuntime::get_scheduler(ComputeIntent) const */
const IScheduler* GraphRuntime::get_scheduler(ComputeIntent intent) const {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  auto it = schedulers_.find(intent);
  return (it != schedulers_.end()) ? it->second.scheduler.get() : nullptr;
}

/** @copydoc GraphRuntime::get_scheduler_execution_route */
GraphRuntime::SchedulerExecutionRoute
GraphRuntime::get_scheduler_execution_route(ComputeIntent intent) const {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  const auto it = schedulers_.find(intent);
  return it != schedulers_.end() ? it->second.execution_route
                                 : SchedulerExecutionRoute{};
}

/** @copydoc GraphRuntime::replace_scheduler */
void GraphRuntime::replace_scheduler(ComputeIntent intent,
                                     std::unique_ptr<IScheduler> scheduler) {
  replace_scheduler(intent, std::move(scheduler), SchedulerExecutionRoute{});
}

/** @copydoc GraphRuntime::replace_scheduler */
void GraphRuntime::replace_scheduler(ComputeIntent intent,
                                     std::unique_ptr<IScheduler> scheduler,
                                     SchedulerExecutionRoute execution_route) {
  if (execution_route.domain ==
      SchedulerExecutionRoute::Domain::ProcessCpuService) {
    if (scheduler != nullptr) {
      throw std::invalid_argument(
          "Process CPU scheduler route cannot carry a Graph-owned scheduler.");
    }
  }

  std::lock_guard<std::mutex> lock(schedulers_mutex_);

  // Reserve any new map node before candidate lifecycle calls. Once this
  // succeeds, publication into the existing unique_ptr slot cannot allocate.
  auto [slot, inserted] = schedulers_.try_emplace(intent);
  try {
    if (scheduler) {
      scheduler->attach(*this);
      if (running_.load(std::memory_order_acquire)) {
        scheduler->start();
      }
    }
  } catch (...) {
    const std::exception_ptr candidate_error = std::current_exception();
    (void)cleanup_scheduler_lifecycle(scheduler.get());
    if (inserted) {
      schedulers_.erase(slot);
    }
    std::rethrow_exception(candidate_error);
  }

  // Candidate preparation is complete. Swap publishes it without allocation
  // or ownership destruction; scheduler now owns the previous map value.
  slot->second.scheduler.swap(scheduler);
  slot->second.execution_route = execution_route;

  // Publication cannot be rolled back truthfully. Cleanup remains a complete
  // best-effort sweep, but a displaced-owner lifecycle error is post-commit
  // diagnostic state rather than a replacement failure.
  (void)cleanup_scheduler_lifecycle(scheduler.get());
  scheduler.reset();
}

/** @copydoc GraphRuntime::has_scheduler */
bool GraphRuntime::has_scheduler(ComputeIntent intent) const {
  std::lock_guard<std::mutex> lock(schedulers_mutex_);
  auto it = schedulers_.find(intent);
  return it != schedulers_.end() &&
         (it->second.scheduler != nullptr ||
          it->second.execution_route.domain ==
              SchedulerExecutionRoute::Domain::ProcessCpuService);
}

}  // namespace ps
