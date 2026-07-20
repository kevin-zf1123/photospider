/**
 * @file kernel.cpp
 * @brief Implements Kernel graph lifecycle and scheduler bootstrap methods.
 *
 * The broader Kernel facade is split by responsibility across
 * kernel_compute.cpp, kernel_io_cache_facade.cpp,
 * kernel_inspection_facade.cpp, kernel_dirty_roi_facade.cpp, and
 * kernel_scheduler_facade.cpp. This file keeps ownership setup for graph
 * runtimes, graph listing/closing, Metal device access, and scheduler
 * configuration so the internal adapter-to-Kernel contract remains unchanged
 * while the implementation no longer concentrates every backend wrapper in one
 * translation unit. Public frontend calls enter through `ps::Host` and the
 * embedded Host adapter.
 */
#include "runtime/kernel.hpp"

#include <atomic>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "compute/execution_service.hpp"
#include "photospider/core/graph_error.hpp"
#include "scheduler/scheduler_factory.hpp"
#include "scheduler/scheduler_worker_budget.hpp"
#if defined(PHOTOSPIDER_INTERNAL_REQUIRED_TARGET_TESTING)
#include "runtime/kernel_required_target_test_access.hpp"
#endif

namespace ps {

namespace {

/**
 * @brief Checks one graph-lifecycle path without leaking filesystem errors.
 *
 * @param path Filesystem object whose existence is queried.
 * @param role Human-readable path role used in diagnostics.
 * @return True when the path exists, otherwise false.
 * @throws std::bad_alloc If diagnostic or filesystem storage exhausts memory.
 * @throws GraphError with `GraphErrc::Io` If the filesystem cannot inspect the
 *         path.
 * @note The function performs no creation, copying, or publication.
 */
bool graph_lifecycle_path_exists(const std::filesystem::path& path,
                                 const std::string& role) {
  try {
    return std::filesystem::exists(path);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::filesystem::filesystem_error& error) {
    throw GraphError(GraphErrc::Io, "Failed to inspect " + role + " '" +
                                        path.string() + "': " + error.what());
  }
}

}  // namespace

#if defined(PHOTOSPIDER_INTERNAL_REQUIRED_TARGET_TESTING)
namespace testing {
namespace {

/**
 * @brief Borrowed required-target hook published only in test-enabled builds.
 * @throws Nothing for atomic initialization and pointer publication.
 * @note Tests serialize replacement and join every callback before destroying
 *       the borrowed hook or its context.
 */
std::atomic<const RequiredTargetTestHook*> g_required_target_test_hook{nullptr};

}  // namespace

/** @copydoc ps::testing::set_required_target_test_hook */
void set_required_target_test_hook(
    const RequiredTargetTestHook* hook) noexcept {
  g_required_target_test_hook.store(hook, std::memory_order_release);
}

/** @copydoc ps::testing::notify_required_target_test_hook */
void notify_required_target_test_hook(RequiredTargetTestEvent event) noexcept {
  const RequiredTargetTestHook* hook =
      g_required_target_test_hook.load(std::memory_order_acquire);
  if (hook != nullptr && hook->wait != nullptr) {
    hook->wait(hook->context, event);
  }
}

}  // namespace testing
#endif

/** @copydoc Kernel::Kernel */
Kernel::Kernel(std::shared_ptr<const ImageArtifactCodec> image_codec,
               std::shared_ptr<const CacheMetadataCodec> metadata_codec,
               std::shared_ptr<const GraphDocumentReader> document_reader,
               std::shared_ptr<const GraphDocumentWriter> document_writer,
               std::shared_ptr<compute::ExecutionService> execution_service)
    : cache_service_(std::move(image_codec), std::move(metadata_codec)),
      io_service_(std::move(document_reader), std::move(document_writer)),
      execution_service_(std::move(execution_service)) {
  if (!execution_service_) {
    throw std::invalid_argument(
        "Kernel requires an injected ExecutionService owner.");
  }
}  // NOLINT

/** @copydoc Kernel::~Kernel */
Kernel::~Kernel() noexcept {
  graphs_.clear();
}

/**
 * @copydoc Kernel::clear_last_error
 */
void Kernel::clear_last_error(const std::string& name) {
  std::lock_guard<std::mutex> lock(last_error_mutex_);
  last_error_.erase(name);
}

/**
 * @copydoc Kernel::store_last_error
 */
void Kernel::store_last_error(const std::string& name, LastError error) {
  std::lock_guard<std::mutex> lock(last_error_mutex_);
  last_error_.insert_or_assign(name, std::move(error));
}

/**
 * @copydoc Kernel::copy_last_error
 */
std::optional<Kernel::LastError> Kernel::copy_last_error(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(last_error_mutex_);
  auto it = last_error_.find(name);
  if (it == last_error_.end()) {
    return std::nullopt;
  }
  return it->second;
}

id Kernel::get_metal_device(const std::string& name) {
  auto it = graphs_.find(name);
  if (it == graphs_.end()) {
    return nullptr;
  }
  return it->second->get_metal_device();
}

/** @copydoc Kernel::load_graph */
std::optional<std::string> Kernel::load_graph(
    const std::string& name, const std::string& root_dir,
    const std::string& yaml_path, const std::string& config_path,
    const std::string& cache_root_dir) {
  if (graphs_.count(name)) {
    return std::nullopt;
  }

  if (!yaml_path.empty() &&
      !graph_lifecycle_path_exists(yaml_path, "explicit graph YAML source")) {
    throw GraphError(GraphErrc::Io,
                     "explicit graph YAML source does not exist: " + yaml_path);
  }

  std::string effective_yaml_path = yaml_path;
  if (effective_yaml_path.empty()) {
    effective_yaml_path =
        (std::filesystem::path(root_dir) / name / "content.yaml").string();
  }

  std::filesystem::path effective_cache_root;
  if (!cache_root_dir.empty()) {
    effective_cache_root = std::filesystem::path(cache_root_dir) / name;
  }

  GraphRuntime::Info info{name, std::filesystem::path(root_dir) / name,
                          effective_yaml_path, config_path,
                          effective_cache_root};
  auto runtime = std::make_unique<GraphRuntime>(info);
  try {
    std::filesystem::create_directories(info.root);
    const auto yaml_target = info.root / "content.yaml";

    if (!yaml_path.empty()) {
      const bool same_file =
          graph_lifecycle_path_exists(yaml_target,
                                      "session graph YAML target") &&
          std::filesystem::equivalent(info.yaml, yaml_target);
      if (!same_file) {
        std::filesystem::copy_file(
            info.yaml, yaml_target,
            std::filesystem::copy_options::overwrite_existing);
      }
    }

    if (!config_path.empty() &&
        graph_lifecycle_path_exists(config_path,
                                    "graph configuration source")) {
      const auto config_target = info.root / "config.yaml";
      std::filesystem::copy_file(
          config_path, config_target,
          std::filesystem::copy_options::overwrite_existing);
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError&) {
    throw;
  } catch (const std::filesystem::filesystem_error& error) {
    throw GraphError(GraphErrc::Io,
                     "Failed to prepare graph session files for '" + name +
                         "': " + error.what());
  }

  setup_schedulers_for_runtime(name, *runtime);
  runtime->start();

  const auto final_yaml_to_load = info.root / "content.yaml";
  if (graph_lifecycle_path_exists(final_yaml_to_load, "session graph YAML")) {
    runtime->graph_state()
        .submit([this, yaml = final_yaml_to_load](GraphModel& graph) {
          io_service_.load(graph, yaml);
          return 0;
        })
        .get();
  }

  std::optional<std::string> loaded_name(std::in_place, name);
  const auto inserted = graphs_.emplace(name, std::move(runtime));
  if (!inserted.second) {
    return std::nullopt;
  }
  return loaded_name;
}

/** @copydoc Kernel::stop_graph_admission */
bool Kernel::stop_graph_admission(const std::string& name) {
  auto it = graphs_.find(name);
  if (it == graphs_.end()) {
    return false;
  }
  it->second->graph_state().stop_admission();
  return true;
}

/** @copydoc Kernel::close_graph */
bool Kernel::close_graph(const std::string& name) {
  auto it = graphs_.find(name);
  if (it == graphs_.end()) {
    return false;
  }

  GraphRuntime& runtime = *it->second;
  runtime.graph_state().close_and_drain();
  try {
    runtime.stop();
  } catch (...) {
    const std::exception_ptr stop_failure = std::current_exception();
    runtime.graph_state().restart_after_close_failure();
    std::rethrow_exception(stop_failure);
  }
  graphs_.erase(it);
  clear_last_error(name);
  return true;
}

std::vector<std::string> Kernel::list_graphs() const {
  std::vector<std::string> names;
  names.reserve(graphs_.size());
  for (const auto& [graph_name, _] : graphs_) {
    names.push_back(graph_name);
  }
  return names;
}

/** @copydoc Kernel::set_scheduler_config */
void Kernel::set_scheduler_config(const SchedulerConfig& config) {
  SchedulerConfig candidate = config;
  const std::optional<SchedulerPlan> hp_plan =
      SchedulerFactory::plan(candidate.hp_type, candidate.worker_count);
  const std::optional<SchedulerPlan> rt_plan =
      SchedulerFactory::plan(candidate.rt_type, candidate.worker_count);
  const bool selects_process_cpu =
      (hp_plan.has_value() && hp_plan->is_builtin_cpu()) ||
      (rt_plan.has_value() && rt_plan->is_builtin_cpu());
  if (execution_service_->is_configured() || selects_process_cpu) {
    execution_service_->configure_worker_count(candidate.worker_count);
    candidate.worker_count = execution_service_->worker_count();
  }
  scheduler_config_ = std::move(candidate);
}

const Kernel::SchedulerConfig& Kernel::get_scheduler_config() const {
  return scheduler_config_;
}

/** @copydoc Kernel::setup_schedulers_for_runtime */
void Kernel::setup_schedulers_for_runtime(const std::string& name,
                                          GraphRuntime& runtime) {
  std::optional<SchedulerPlan> hp_plan = SchedulerFactory::plan(
      scheduler_config_.hp_type, scheduler_config_.worker_count);
  std::optional<SchedulerPlan> rt_plan = SchedulerFactory::plan(
      scheduler_config_.rt_type, scheduler_config_.worker_count);
  if (!hp_plan.has_value()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "unsupported HP scheduler type '" + scheduler_config_.hp_type + "'");
  }
  if (!rt_plan.has_value()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "unsupported RT scheduler type '" + scheduler_config_.rt_type + "'");
  }
  if (hp_plan->is_builtin_cpu() || rt_plan->is_builtin_cpu()) {
    execution_service_->configure_worker_count(scheduler_config_.worker_count);
    scheduler_config_.worker_count = execution_service_->worker_count();
    hp_plan = SchedulerFactory::plan(scheduler_config_.hp_type,
                                     scheduler_config_.worker_count);
    rt_plan = SchedulerFactory::plan(scheduler_config_.rt_type,
                                     scheduler_config_.worker_count);
    if (!hp_plan.has_value() || !rt_plan.has_value()) {
      throw GraphError(
          GraphErrc::InvalidParameter,
          "scheduler type became unavailable during fixed CPU planning");
    }
  }

  std::optional<SchedulerWorkerBudget::ReservationPair> reservations =
      SchedulerWorkerBudget::process().try_reserve_pair(
          hp_plan->is_builtin_cpu() ? 0U : hp_plan->reservation_slots(),
          rt_plan->is_builtin_cpu() ? 0U : rt_plan->reservation_slots());
  if (!reservations.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "process scheduler worker budget cannot admit the "
                     "configured HP and RT scheduler pair");
  }

  std::unique_ptr<IScheduler> hp_scheduler;
  if (!hp_plan->is_builtin_cpu()) {
    hp_scheduler = SchedulerFactory::create(
        *hp_plan, std::move(reservations->high_precision));
    if (hp_scheduler == nullptr) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "HP scheduler type '" + scheduler_config_.hp_type +
                           "' became unavailable or returned no scheduler "
                           "instance during Graph load");
    }
  }

  std::unique_ptr<IScheduler> rt_scheduler;
  if (!rt_plan->is_builtin_cpu()) {
    rt_scheduler =
        SchedulerFactory::create(*rt_plan, std::move(reservations->real_time));
    if (rt_scheduler == nullptr) {
      throw GraphError(GraphErrc::InvalidParameter,
                       "RT scheduler type '" + scheduler_config_.rt_type +
                           "' became unavailable or returned no scheduler "
                           "instance during Graph load");
    }
  }

  GraphRuntime::SchedulerExecutionRoute hp_execution_route;
  if (hp_plan->is_builtin_cpu()) {
    hp_execution_route.domain =
        GraphRuntime::SchedulerExecutionRoute::Domain::ProcessCpuService;
  }
  GraphRuntime::SchedulerExecutionRoute rt_execution_route;
  if (rt_plan->is_builtin_cpu()) {
    rt_execution_route.domain =
        GraphRuntime::SchedulerExecutionRoute::Domain::ProcessCpuService;
  }
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::move(hp_scheduler), hp_execution_route);
  runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler),
                        rt_execution_route);
  clear_last_error(name);
}

}  // namespace ps
