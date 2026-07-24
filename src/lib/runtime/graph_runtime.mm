// Photospider kernel: GraphRuntime implementation (Objective-C++)
#include "runtime/graph_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "compute/realtime_proxy_graph.hpp"
#include "compute/run_lifecycle_registry.hpp"
#include "photospider/core/graph_error.hpp"

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
 * @brief Tests the fixed private execution-route vocabulary.
 * @param type Candidate route id.
 * @return True only for a current Host-owned route.
 * @throws Nothing.
 */
bool valid_execution_type(const std::string& type) noexcept {
  return type == "cpu" || type == "gpu_pipeline" || type == "serial_debug";
}

/**
 * @brief Tests whether an intent owns one physical execution route.
 * @param intent Candidate compute intent.
 * @return True for GlobalHighPrecision or RealTimeUpdate.
 * @throws Nothing.
 */
bool valid_execution_intent(ComputeIntent intent) noexcept {
  return intent == ComputeIntent::GlobalHighPrecision ||
         intent == ComputeIntent::RealTimeUpdate;
}

/**
 * @brief Builds the complete initial execution-route binding map.
 * @param info Borrowed construction values for the HP and RT route ids.
 * @return Owned bindings for both supported compute intents at generation one.
 * @throws std::bad_alloc If route-string copies or map nodes cannot allocate.
 * @note Route vocabulary validation remains in `GraphRuntime` construction so
 * the complete owner is staged before the runtime becomes observable.
 */
std::map<ComputeIntent, GraphRuntime::ExecutionRouteBinding>
make_initial_execution_routes(const GraphRuntime::Info& info) {
  return {
      {ComputeIntent::GlobalHighPrecision,
       GraphRuntime::ExecutionRouteBinding{info.hp_execution_type, 1U}},
      {ComputeIntent::RealTimeUpdate,
       GraphRuntime::ExecutionRouteBinding{info.rt_execution_type, 1U}},
  };
}

}  // namespace

/** @copydoc GraphRuntime::tls_worker_id_ */
thread_local int GraphRuntime::tls_worker_id_ = -1;
/** @copydoc GraphRuntime::tls_execution_context_worker_id_ */
thread_local int GraphRuntime::tls_execution_context_worker_id_ = -1;
/** @copydoc GraphRuntime::tls_execution_context_epoch_ */
thread_local uint64_t GraphRuntime::tls_execution_context_epoch_ = 0;

/**
 * @brief Owns private platform GPU objects for one graph runtime.
 * @note The type is complete only in this implementation and never crosses the
 *       private execution host-context boundary.
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
      lifetime_anchor_(
          std::make_shared<compute::GraphLifetimeAnchor>(model_.instance_id())),
      graph_state_(model_),
      compute_requests_(model_, GraphStateExecutor::kDefaultQueueCapacity,
                        GraphStateExecutor::CapacityMode::TotalAdmission),
      compute_request_coordinator_(graph_state_, compute_requests_,
                                   info.supersession_first_generation),
      event_service_(info.compute_event_capacity,
                     info.compute_event_initial_sequence,
                     info.compute_event_initial_dropped_count),
      realtime_proxy_graph_(std::make_unique<compute::RealtimeProxyGraph>()),
      execution_routes_(make_initial_execution_routes(info)),
      execution_trace_slots_(info.execution_trace_capacity),
      execution_trace_next_sequence_(info.execution_trace_initial_sequence),
      execution_trace_unsequenced_drops_(
          info.execution_trace_initial_dropped_count) {
  if (!valid_execution_type(info.hp_execution_type) ||
      !valid_execution_type(info.rt_execution_type)) {
    throw std::invalid_argument("GraphRuntime received an invalid route id");
  }
  if (info.execution_trace_capacity == 0) {
    throw std::invalid_argument(
        "execution-trace ring capacity must be nonzero");
  }
  if (info.execution_trace_initial_sequence == 0) {
    throw std::invalid_argument(
        "execution-trace initial sequence must be nonzero");
  }
  static_assert(std::is_nothrow_move_constructible_v<ExecutionEvent>);
  static_assert(std::is_nothrow_move_assignable_v<ExecutionEvent>);

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
    compute_request_coordinator_.stop_admission();
    compute_requests_.close_and_drain();
    graph_state_.close_and_drain();
  } catch (...) {
    std::terminate();
  }
  running_.store(false, std::memory_order_release);
  lifetime_anchor_->mark_retired();
}

/** @copydoc GraphRuntime::clear_last_error */
void GraphRuntime::clear_last_error() {
  std::lock_guard<std::mutex> lock(last_error_mutex_);
  last_error_.reset();
}

/** @copydoc GraphRuntime::store_last_error */
void GraphRuntime::store_last_error(LastError error) {
  std::lock_guard<std::mutex> lock(last_error_mutex_);
  last_error_ = std::move(error);
}

/** @copydoc GraphRuntime::last_error */
std::optional<GraphRuntime::LastError> GraphRuntime::last_error() const {
  std::lock_guard<std::mutex> lock(last_error_mutex_);
  return last_error_;
}

/** @copydoc GraphRuntime::this_worker_id */
int GraphRuntime::this_worker_id() noexcept {
  if (tls_worker_id_ >= 0) {
    return tls_worker_id_;
  }
  return tls_execution_context_worker_id_;
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
  running_.store(true, std::memory_order_release);
}

/** @copydoc GraphRuntime::stop */
void GraphRuntime::stop() {
  running_.store(false, std::memory_order_release);
}

/** @copydoc GraphRuntime::this_task_epoch */
uint64_t GraphRuntime::this_task_epoch() noexcept {
  return tls_execution_context_epoch_;
}

/** @copydoc GraphRuntime::set_execution_trace_context */
void GraphRuntime::set_execution_trace_context(int worker_id,
                                               uint64_t epoch) noexcept {
  tls_execution_context_worker_id_ = worker_id;
  tls_execution_context_epoch_ = epoch;
}

/** @copydoc GraphRuntime::clear_execution_trace_context */
void GraphRuntime::clear_execution_trace_context() noexcept {
  tls_execution_context_worker_id_ = -1;
  tls_execution_context_epoch_ = 0;
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
  set_execution_trace_context(worker_id, epoch);
}

/** @copydoc GraphRuntime::clear_task_context */
void GraphRuntime::clear_task_context() noexcept {
  clear_execution_trace_context();
}

/** @copydoc GraphRuntime::log_event(ExecutionTraceAction,int,int,uint64_t) */
void GraphRuntime::log_event(ExecutionTraceAction action, int node_id,
                             int worker_id, uint64_t epoch) noexcept {
  ExecutionEvent::Action runtime_action = ExecutionEvent::EXECUTE;
  switch (action) {
    case ExecutionTraceAction::AssignInitial:
      runtime_action = ExecutionEvent::ASSIGN_INITIAL;
      break;
    case ExecutionTraceAction::Execute:
      runtime_action = ExecutionEvent::EXECUTE;
      break;
    case ExecutionTraceAction::ExecuteTile:
      runtime_action = ExecutionEvent::EXECUTE_TILE;
      break;
    case ExecutionTraceAction::ExecuteDirtySource:
      runtime_action = ExecutionEvent::EXECUTE_DIRTY_SOURCE;
      break;
    case ExecutionTraceAction::ExecuteDirtyDownstreamNode:
      runtime_action = ExecutionEvent::EXECUTE_DIRTY_DOWNSTREAM_NODE;
      break;
    case ExecutionTraceAction::ExecuteDirtyDownstreamTile:
      runtime_action = ExecutionEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE;
      break;
    case ExecutionTraceAction::SkipStaleGeneration:
      runtime_action = ExecutionEvent::SKIP_STALE_GENERATION;
      break;
    case ExecutionTraceAction::RethrowException:
      runtime_action = ExecutionEvent::RETHROW_EXCEPTION;
      break;
  }
  try {
    log_event(runtime_action, node_id, worker_id, epoch);
  } catch (...) {
    // Execution observations are best effort and cannot replace task failures.
  }
}

/** @copydoc GraphRuntime::log_event(ExecutionEvent::Action,int) */
void GraphRuntime::log_event(ExecutionEvent::Action action, int node_id) {
  uint64_t epoch = this_task_epoch();
  int worker_id = this_worker_id();
  if (worker_id < 0) {
    worker_id = tls_execution_context_worker_id_;
  }
  log_event(action, node_id, worker_id, epoch);
}

/** @copydoc GraphRuntime::log_event(ExecutionEvent::Action,int,int,uint64_t)
 */
void GraphRuntime::log_event(ExecutionEvent::Action action, int node_id,
                             int worker_id, uint64_t epoch) {
  std::lock_guard<std::mutex> lock(log_mutex_);
  if (execution_trace_next_sequence_ == kObservationSequenceExhausted) {
    saturating_increment(execution_trace_unsequenced_drops_);
    return;
  }

  const uint64_t sequence = execution_trace_next_sequence_;
  ExecutionEvent event{sequence, epoch,
                       node_id,  worker_id,
                       action,   std::chrono::high_resolution_clock::now()};
  execution_trace_next_sequence_ = sequence_successor(sequence);

  if (execution_trace_size_ == execution_trace_slots_.size()) {
    execution_trace_slots_[execution_trace_head_] = std::move(event);
    execution_trace_head_ =
        (execution_trace_head_ + 1) % execution_trace_slots_.size();
    return;
  }

  const std::size_t insertion =
      (execution_trace_head_ + execution_trace_size_) %
      execution_trace_slots_.size();
  execution_trace_slots_[insertion].emplace(std::move(event));
  ++execution_trace_size_;
}

/** @copydoc GraphRuntime::execution_trace_page */
GraphRuntime::ExecutionEventPage GraphRuntime::execution_trace_page(
    uint64_t after_sequence,
    std::size_t limit) const {  // NOLINT(whitespace/indent_namespace)
  if (limit < kExecutionTraceMinLimit || limit > kExecutionTraceMaxLimit) {
    throw std::invalid_argument("execution-trace limit is out of range");
  }

  std::lock_guard<std::mutex> lock(log_mutex_);
  ExecutionEventPage page;
  if (after_sequence == kObservationSequenceExhausted) {
    if (execution_trace_next_sequence_ != kObservationSequenceExhausted) {
      throw std::invalid_argument(
          "execution-trace exhausted cursor precedes exhaustion");
    }
    page.next_sequence = kObservationSequenceExhausted;
    return page;
  }

  const uint64_t last_published =
      execution_trace_next_sequence_ == kObservationSequenceExhausted
          ? kObservationSequenceExhausted - 1
          : execution_trace_next_sequence_ - 1;
  if (after_sequence > last_published) {
    throw std::invalid_argument("execution-trace cursor is in the future");
  }

  page.events.reserve(std::min(limit, execution_trace_size_));
  std::size_t matching_count = 0;
  uint64_t first_later_sequence = execution_trace_next_sequence_;
  for (std::size_t offset = 0; offset < execution_trace_size_; ++offset) {
    const std::size_t index =
        (execution_trace_head_ + offset) % execution_trace_slots_.size();
    const ExecutionEvent& event = *execution_trace_slots_[index];
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
                     execution_trace_unsequenced_drops_);

  if (!page.events.empty()) {
    const uint64_t last_returned = page.events.back().sequence;
    const bool terminal_page =
        execution_trace_next_sequence_ == kObservationSequenceExhausted &&
        !page.has_more && last_returned == kObservationSequenceExhausted - 1;
    page.next_sequence =
        terminal_page ? kObservationSequenceExhausted : last_returned;
  } else if (execution_trace_next_sequence_ == kObservationSequenceExhausted) {
    page.next_sequence = kObservationSequenceExhausted;
    page.has_more = false;
  } else {
    page.next_sequence = after_sequence;
  }
  return page;
}

/** @copydoc GraphRuntime::clear_execution_trace */
void GraphRuntime::clear_execution_trace() {
  std::lock_guard<std::mutex> lock(log_mutex_);
  for (auto& slot : execution_trace_slots_) {
    slot.reset();
  }
  execution_trace_head_ = 0;
  execution_trace_size_ = 0;
}

/** @copydoc GraphRuntime::execution_route */
GraphRuntime::ExecutionRouteBinding GraphRuntime::execution_route(
    ComputeIntent intent) const {
  if (!valid_execution_intent(intent)) {
    throw std::invalid_argument("Unsupported execution-route intent.");
  }
  std::lock_guard<std::mutex> lock(execution_routes_mutex_);
  const auto found = execution_routes_.find(intent);
  if (found == execution_routes_.end()) {
    throw std::logic_error("GraphRuntime has no execution-route binding.");
  }
  return found->second;
}

/** @copydoc GraphRuntime::replace_execution_route */
void GraphRuntime::replace_execution_route(ComputeIntent intent,
                                           const std::string& execution_type) {
  if (!valid_execution_intent(intent) ||
      !valid_execution_type(execution_type)) {
    throw std::invalid_argument("Invalid execution route replacement.");
  }
  std::string staged_type(execution_type);
  std::lock_guard<std::mutex> lock(execution_routes_mutex_);
  const auto found = execution_routes_.find(intent);
  if (found == execution_routes_.end()) {
    throw std::logic_error("GraphRuntime has no execution-route binding.");
  }
  if (found->second.generation == std::numeric_limits<std::uint64_t>::max()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Execution route generation exhausted.");
  }
  found->second.execution_type.swap(staged_type);
  ++found->second.generation;
}

/** @copydoc GraphRuntime::has_execution_route */
bool GraphRuntime::has_execution_route(ComputeIntent intent) const noexcept {
  if (!valid_execution_intent(intent)) {
    return false;
  }
  try {
    std::lock_guard<std::mutex> lock(execution_routes_mutex_);
    return execution_routes_.find(intent) != execution_routes_.end();
  } catch (...) {
    return false;
  }
}

}  // namespace ps
