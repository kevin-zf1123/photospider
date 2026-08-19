#include "compute/execution/execution_service.hpp"

#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "compute/execution/execution_service_internal.hpp"

namespace ps::compute {

using namespace execution_service_detail;  // NOLINT(build/namespaces)
using execution::make_default_device_executor_registry;

/** @copydoc operator==(const ReadyTaskResourceDemand&, const
 * ReadyTaskResourceDemand&) */
bool operator==(const ReadyTaskResourceDemand& lhs,
                const ReadyTaskResourceDemand& rhs) noexcept {
  return lhs.retained_memory_bytes == rhs.retained_memory_bytes &&
         lhs.scratch_bytes == rhs.scratch_bytes &&
         lhs.ready_bytes == rhs.ready_bytes && lhs.work_units == rhs.work_units;
}

/** @copydoc operator!=(const ReadyTaskResourceDemand&, const
 * ReadyTaskResourceDemand&) */
bool operator!=(const ReadyTaskResourceDemand& lhs,
                const ReadyTaskResourceDemand& rhs) noexcept {
  return !(lhs == rhs);
}

/** @copydoc owned_callback_resource_demand */
ReadyTaskResourceDemand owned_callback_resource_demand(
    std::uint64_t capture_bytes) {
  const std::uint64_t owned_bytes =
      owned_callable_retained_memory_bytes(capture_bytes);
  return ReadyTaskResourceDemand{owned_bytes, 0U, owned_bytes, 1U};
}

/** @copydoc ReadyTaskMetadata::ReadyTaskMetadata */
ReadyTaskMetadata::ReadyTaskMetadata(const ComputeRunDescriptor& descriptor,
                                     int trace_node_id, bool is_initial_ready,
                                     DeviceBackend device)
    : run_id_(descriptor.id()),
      graph_identity_(descriptor.graph_identity()),
      graph_instance_id_(descriptor.graph_instance_id()),
      revision_(descriptor.revision()),
      target_node_id_(descriptor.target_node_id()),
      intent_(descriptor.intent()),
      quality_(descriptor.quality()),
      qos_(descriptor.qos()),
      supersession_(descriptor.supersession()),
      trace_node_id_(trace_node_id),
      device_(device),
      is_initial_ready_(is_initial_ready) {
}  // NOLINT(whitespace/indent_namespace)

/** @copydoc ReadyTaskSubmission::ReadyTaskSubmission */
ReadyTaskSubmission::ReadyTaskSubmission(
    ComputeRunLease lease, ComputeRunTaskIdentity identity, int trace_node_id,
    bool is_initial_ready, Executable executable,
    ExecutionTaskPriority priority, ReadyTaskResourceDemand resource_demand,
    DeviceBackend device, OperationExecutionConstraints operation_constraints)
    : metadata_(lease.descriptor(), trace_node_id, is_initial_ready, device),
      identity_(identity),
      lease_(std::move(lease)),
      executable_(std::move(executable)),
      priority_(priority),
      resource_demand_(resource_demand),
      operation_constraints_(std::move(operation_constraints)) {
  if (identity_.run_id() != metadata_.run_id()) {
    throw std::invalid_argument(
        "ReadyTaskSubmission identity does not match its Run lease.");
  }
  if (!executable_) {
    throw std::invalid_argument(
        "ReadyTaskSubmission requires an owned executable.");
  }
  if ((!operation_constraints_.reentrant ||
       operation_constraints_.maximum_parallelism != 0U) &&
      operation_constraints_.implementation_identity == 0U) {
    throw std::invalid_argument(
        "Operation execution caps require a nonzero implementation identity.");
  }
  if (operation_constraints_.exclusive_key.size() >
      OperationExecutionConstraints::kExclusiveKeyMaxBytes) {
    throw std::invalid_argument(
        "Operation execution exclusive key exceeds 128 bytes.");
  }
  if (operation_constraints_.exclusive_key.find('\0') != std::string::npos) {
    throw std::invalid_argument(
        "Operation execution exclusive key contains an embedded NUL.");
  }
}

/** @copydoc ReadyTaskSubmission::default_resource_demand */
ReadyTaskResourceDemand
ReadyTaskSubmission::default_resource_demand() noexcept {
  return {};
}

/** @copydoc ReadyTaskSubmission::execute */
void ReadyTaskSubmission::execute(ExecutionTaskRuntime& task_runtime) {
  try {
    if (lease_.observe_cancellation().has_value()) {
      return;
    }
    executable_(lease_, identity_, task_runtime);
    (void)lease_.observe_cancellation();
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    try {
      (void)lease_.publish_task_failure(identity_, failure);
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
}

/** @copydoc ExecutionService::default_resource_limits */
ExecutionResourceLimits ExecutionService::default_resource_limits() {
  constexpr std::uint64_t kOneMebibyte = 1024U * 1024U;
  return ExecutionResourceLimits{
      32U,
      1024U * kOneMebibyte,
      512U * kOneMebibyte,
      65536U,
      256U * kOneMebibyte,
      64U,
      256U * kOneMebibyte,
      ResourceVector{1U, 64U * kOneMebibyte, 32U * kOneMebibyte, 1024U,
                     16U * kOneMebibyte},
      std::vector<DeviceResourceLimit>{DeviceResourceLimit{
          DeviceId(DeviceBackend::Metal),
          DeviceResourceVector{512U * kOneMebibyte, 256U * kOneMebibyte}}},
  };
}

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService()
    : ExecutionService(default_resource_limits()) {}

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService(ExecutionResourceLimits limits)
    : ExecutionService(limits, make_default_device_executor_registry()) {}

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService(
    ExecutionResourceLimits resource_limits,
    execution::DeviceExecutorRegistry device_executors)
    : lifecycle_telemetry_(std::make_unique<ExecutionLifecycleTelemetry>()),
      lifecycle_registry_(
          std::make_unique<RunLifecycleRegistry>(*lifecycle_telemetry_)) {
  retain_executable_device_limits(resource_limits, device_executors);
  pool_ = std::make_unique<PoolState>(
      std::move(resource_limits), std::move(device_executors),
      *lifecycle_telemetry_, policy_lifecycle_observer());
  const ExecutionLifecycleCounters counters = lifecycle_registry_->counters();
  lifecycle_telemetry_->publish(ExecutionLifecycleEventKind::ServiceStarted,
                                ExecutionLifecycleCategory::None, 0U, 0U, 0U,
                                0U, counters);
}

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService(unsigned int worker_count)
    : ExecutionService(worker_count, default_resource_limits()) {}

/** @copydoc ExecutionService::ExecutionService */
ExecutionService::ExecutionService(unsigned int worker_count,
                                   ExecutionResourceLimits resource_limits)
    : ExecutionService(resource_limits) {
  configure_worker_count(worker_count);
}

/** @copydoc ExecutionService::~ExecutionService */
ExecutionService::~ExecutionService() noexcept {
  if (!pool_) {
    return;
  }
  try {
    shutdown();
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionService::prepare_supersession_lineage */
void ExecutionService::prepare_supersession_lineage(
    GraphInstanceId graph_instance_id, const SupersessionIdentity& identity) {
  pool_->device_executors.track_lineage(graph_instance_id.value(),
                                        identity.key.target_node_id(),
                                        identity.key.request_intent());
}

/** @copydoc ExecutionService::observe_current_supersession */
void ExecutionService::observe_current_supersession(
    GraphInstanceId graph_instance_id,
    const SupersessionIdentity& identity) noexcept {
  pool_->device_executors.publish_current_generation(
      graph_instance_id.value(), identity.key.target_node_id(),
      identity.key.request_intent(), identity.generation.value());
}

/** @copydoc ExecutionService::policy_lifecycle_observer */
policy::PolicyLifecycleObserver
ExecutionService::policy_lifecycle_observer() noexcept {
  return policy::PolicyLifecycleObserver{
      this, &ExecutionService::observe_policy_invocation_entered,
      &ExecutionService::observe_policy_invocation_returned,
      &ExecutionService::observe_policy_binding_published,
      &ExecutionService::observe_policy_binding_retired};
}

/** @copydoc ExecutionService::observe_policy_invocation_entered */
void ExecutionService::observe_policy_invocation_entered(
    void* context) noexcept {
  if (context == nullptr) {
    std::terminate();
  }
  auto* service = static_cast<ExecutionService*>(context);
  service->lifecycle_telemetry_->increment_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyInvocation);
}

/** @copydoc ExecutionService::observe_policy_invocation_returned */
void ExecutionService::observe_policy_invocation_returned(
    void* context) noexcept {
  if (context == nullptr) {
    std::terminate();
  }
  auto* service = static_cast<ExecutionService*>(context);
  service->lifecycle_telemetry_->decrement_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyInvocation);
}

/** @copydoc ExecutionService::observe_policy_binding_published */
void ExecutionService::observe_policy_binding_published(
    void* context) noexcept {
  if (context == nullptr) {
    std::terminate();
  }
  auto* service = static_cast<ExecutionService*>(context);
  service->lifecycle_telemetry_->increment_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyBinding);
}

/** @copydoc ExecutionService::observe_policy_binding_retired */
void ExecutionService::observe_policy_binding_retired(
    void* context, std::uint64_t generation, bool destroy_failed) noexcept {
  if (context == nullptr) {
    std::terminate();
  }
  auto* service = static_cast<ExecutionService*>(context);
  service->lifecycle_telemetry_->decrement_physical_counter(
      ExecutionLifecyclePhysicalCounter::LivePolicyBinding);
  try {
    service->lifecycle_registry_->publish_physical_retirement(
        ExecutionLifecycleEventKind::BindingRetired,
        destroy_failed ? ExecutionLifecycleCategory::FailureOther
                       : ExecutionLifecycleCategory::None,
        generation);
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionService::configure_worker_count */
void ExecutionService::configure_worker_count(unsigned int worker_count) {
  if (worker_count > kExecutionWorkerRequestMax) {
    throw std::invalid_argument(
        "ExecutionService CPU worker count must be in [0,8].");
  }

  std::unique_lock<std::mutex> lock(pool_->mutex);
  if (pool_->configured_workers != 0U) {
    if (worker_count == 0U || worker_count == pool_->configured_workers) {
      return;
    }
    throw std::invalid_argument(
        "ExecutionService CPU worker count is already fixed.");
  }
  if (pool_->stopping) {
    throw std::logic_error("ExecutionService is stopping.");
  }

  const unsigned int resolved_workers = resolve_execution_worker_count(
      worker_count, std::thread::hardware_concurrency());
  const ResourceLedger::Snapshot resources = pool_->ledger.snapshot();
  if (static_cast<std::uint64_t>(resolved_workers) >
      resources.limits.cpu_slots) {
    throw std::invalid_argument(
        "ExecutionService worker count exceeds configured CPU capacity.");
  }

  std::vector<std::thread> staged_workers;
  staged_workers.reserve(static_cast<std::size_t>(resolved_workers) + 1U);
  pool_->configured_workers = resolved_workers;
  try {
    for (unsigned int index = 0; index < resolved_workers; ++index) {
      staged_workers.emplace_back(&ExecutionService::worker_loop, this,
                                  static_cast<int>(index),
                                  execution::PhysicalExecutionLane::Cpu);
    }
    staged_workers.emplace_back(&ExecutionService::worker_loop, this,
                                static_cast<int>(resolved_workers),
                                execution::PhysicalExecutionLane::Gpu);
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    pool_->stopping = true;
    pool_->advance_worker_notification_epoch();
    lock.unlock();
    pool_->ready_cv.notify_all();
    for (std::thread& worker : staged_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    lock.lock();
    pool_->stopping = false;
    pool_->configured_workers = 0U;
    lock.unlock();
    std::rethrow_exception(failure);
  }

  pool_->workers.swap(staged_workers);
}

/** @copydoc ExecutionService::worker_count */
unsigned int ExecutionService::worker_count() const {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  return pool_->configured_workers;
}

/** @copydoc ExecutionService::is_configured */
bool ExecutionService::is_configured() const {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  return pool_->configured_workers != 0U && !pool_->stopping &&
         pool_->workers.size() ==
             static_cast<std::size_t>(pool_->configured_workers) + 1U;
}

/** @copydoc ExecutionService::get_stats */
std::string ExecutionService::get_stats() const {
  const execution::ComputeIoExecutorSnapshot compute_io =
      pool_->compute_io_executor.snapshot();
  std::lock_guard<std::mutex> lock(pool_->mutex);
  const ResourceLedger::Snapshot resources = pool_->ledger.snapshot();
  std::ostringstream stream;
  stream << "Workers: " << pool_->configured_workers << ", GPU lanes: 1"
         << ", Active runs: " << pool_->ready_store.run_count()
         << ", Ready tasks: " << pool_->ready_store.entry_count()
         << ", Ready bytes: " << pool_->ready_store.byte_count()
         << ", Reserved CPU: " << resources.reserved.cpu_slots
         << ", Compute I/O tasks: " << compute_io.active_tasks
         << ", Compute I/O bytes: " << compute_io.active_planned_bytes;
  return stream.str();
}

/** @copydoc ExecutionService::policy_available_types */
std::vector<std::string> ExecutionService::policy_available_types() const {
  return pool_->registry.available_types();
}

/** @copydoc ExecutionService::policy_description */
std::string ExecutionService::policy_description(
    const std::string& type_name) const {
  return pool_->registry.description(type_name);
}

/** @copydoc ExecutionService::policy_scan */
std::size_t ExecutionService::policy_scan(
    const std::vector<std::string>& directories) {
  return pool_->registry.scan(directories);
}

/** @copydoc ExecutionService::policy_load */
void ExecutionService::policy_load(const std::string& path) {
  pool_->registry.load(path);
}

/** @copydoc ExecutionService::policy_loaded_plugins */
std::vector<std::string> ExecutionService::policy_loaded_plugins() const {
  return pool_->registry.loaded_plugins();
}

/** @copydoc ExecutionService::configure_policy_defaults */
void ExecutionService::configure_policy_defaults(
    const HostPolicyConfig& config) {
  policy::PolicyRegistry::assert_mutation_allowed("configure_policy_defaults");
  std::lock_guard<std::mutex> mutation_lock(pool_->policy_mutation_mutex);
  std::uint64_t interactive_generation = 0U;
  std::uint64_t throughput_generation = 0U;
  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    interactive_generation = pool_->interactive_binding->generation();
    throughput_generation = pool_->throughput_binding->generation();
  }
  if (interactive_generation == std::numeric_limits<std::uint64_t>::max() ||
      throughput_generation == std::numeric_limits<std::uint64_t>::max()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService policy generation exhausted.");
  }

  std::shared_ptr<policy::PolicyBinding> interactive =
      pool_->registry.create_binding(
          config.interactive_type, PolicyClass::Interactive,
          interactive_generation + 1U, policy_lifecycle_observer());
  std::shared_ptr<policy::PolicyBinding> throughput =
      pool_->registry.create_binding(
          config.throughput_type, PolicyClass::Throughput,
          throughput_generation + 1U, policy_lifecycle_observer());
  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    if (pool_->interactive_binding->generation() != interactive_generation ||
        pool_->throughput_binding->generation() != throughput_generation) {
      throw GraphError(GraphErrc::ComputeError,
                       "ExecutionService policy replacement raced state.");
    }
    pool_->interactive_binding.swap(interactive);
    pool_->throughput_binding.swap(throughput);
    pool_->interactive_binding->mark_service_published();
    pool_->throughput_binding->mark_service_published();
    pool_->advance_worker_notification_epoch();
  }
  pool_->ready_cv.notify_all();
}

/** @copydoc ExecutionService::policy_info */
PolicyInfoSnapshot ExecutionService::policy_info(
    PolicyClass policy_class) const {
  std::shared_ptr<policy::PolicyBinding> binding;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    switch (policy_class) {
      case PolicyClass::Interactive:
        binding = pool_->interactive_binding;
        break;
      case PolicyClass::Throughput:
        binding = pool_->throughput_binding;
        break;
      default:
        throw GraphError(GraphErrc::InvalidParameter, "Unknown policy class.");
    }
  }
  return PolicyInfoSnapshot{policy_class, binding->type_name(),
                            binding->generation(), binding->fault()};
}

/** @copydoc ExecutionService::replace_policy */
void ExecutionService::replace_policy(PolicyClass policy_class,
                                      const std::string& type) {
  policy::PolicyRegistry::assert_mutation_allowed("replace_policy");
  std::lock_guard<std::mutex> mutation_lock(pool_->policy_mutation_mutex);
  std::shared_ptr<policy::PolicyBinding> current;
  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    switch (policy_class) {
      case PolicyClass::Interactive:
        current = pool_->interactive_binding;
        break;
      case PolicyClass::Throughput:
        current = pool_->throughput_binding;
        break;
      default:
        throw GraphError(GraphErrc::InvalidParameter, "Unknown policy class.");
    }
  }
  if (current->generation() == std::numeric_limits<std::uint64_t>::max()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ExecutionService policy generation exhausted.");
  }
  std::shared_ptr<policy::PolicyBinding> candidate =
      pool_->registry.create_binding(type, policy_class,
                                     current->generation() + 1U,
                                     policy_lifecycle_observer());
  {
    std::lock_guard<std::mutex> pool_lock(pool_->mutex);
    std::shared_ptr<policy::PolicyBinding>& published =
        policy_class == PolicyClass::Interactive ? pool_->interactive_binding
                                                 : pool_->throughput_binding;
    if (published.get() != current.get()) {
      throw GraphError(GraphErrc::ComputeError,
                       "ExecutionService policy replacement raced state.");
    }
    published.swap(candidate);
    published->mark_service_published();
    pool_->advance_worker_notification_epoch();
  }
  pool_->ready_cv.notify_all();
}

/** @copydoc ExecutionService::available_execution_types */
std::vector<std::string> ExecutionService::available_execution_types() {
  return execution::PhysicalExecutionRoutes::available_types();
}

/** @copydoc ExecutionService::execution_description */
std::string ExecutionService::execution_description(
    const std::string& type_name) {
  return execution::PhysicalExecutionRoutes::description(type_name);
}

/** @copydoc ExecutionService::is_execution_type */
bool ExecutionService::is_execution_type(
    const std::string& type_name) noexcept {
  return execution::PhysicalExecutionRoutes::is_supported(type_name);
}

/** @copydoc ExecutionService::resource_snapshot */
ResourceLedger::Snapshot ExecutionService::resource_snapshot() const {
  return pool_->ledger.snapshot();
}

/** @copydoc ExecutionService::throughput_reservation_snapshot */
ExecutionThroughputReservationSnapshot
ExecutionService::throughput_reservation_snapshot() const {
  return pool_->throughput_reservations->snapshot();
}

/** @copydoc ExecutionService::ready_class_snapshot */
ExecutionReadyClassSnapshot ExecutionService::ready_class_snapshot() const {
  std::lock_guard<std::mutex> lock(pool_->mutex);
  return pool_->ready_store.class_snapshot();
}

/** @copydoc ExecutionService::compute_io_executor */
execution::ComputeIoExecutor& ExecutionService::compute_io_executor() noexcept {
  return pool_->compute_io_executor;
}

/** @copydoc ExecutionService::compute_io_executor */
const execution::ComputeIoExecutor& ExecutionService::compute_io_executor()
    const noexcept {
  return pool_->compute_io_executor;
}

/** @copydoc ExecutionService::device_resource_snapshot */
std::optional<ResourceLedger::DeviceSnapshot>
ExecutionService::device_resource_snapshot(DeviceId device) const {
  return pool_->ledger.device_snapshot(device);
}

/** @copydoc ExecutionService::device_resource_snapshots */
std::vector<ResourceLedger::DeviceSnapshot>
ExecutionService::device_resource_snapshots() const {
  return pool_->ledger.device_snapshots();
}

}  // namespace ps::compute
