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
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "scheduler/scheduler_factory.hpp"
#if defined(PHOTOSPIDER_INTERNAL_REQUIRED_TARGET_TESTING)
#include "runtime/kernel_required_target_test_access.hpp"
#endif

namespace ps {

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

/**
 * @brief Creates and transactionally loads one internal graph runtime.
 *
 * @param name Unique graph/session name.
 * @param root_dir Root directory that owns the session folder.
 * @param yaml_path Optional source YAML copied into the session before load.
 * @param config_path Optional config file copied into the session.
 * @param cache_root_dir Optional external cache-root directory.
 * @return Loaded graph name, or nullopt for duplicate names and recoverable
 *         graph-load failures.
 * @throws std::bad_alloc if path, runtime, scheduler, graph, or diagnostic
 *         allocation exhausts memory.
 * @throws std::exception for scheduler/runtime startup failures not classified
 *         as recoverable graph-load errors.
 * @note The return label is allocated before the runtime enters `graphs_`.
 * After insertion, returning it uses only noexcept moves, so a propagated
 * exception never leaves a newly published session.
 */
std::optional<std::string> Kernel::load_graph(
    const std::string& name, const std::string& root_dir,
    const std::string& yaml_path, const std::string& config_path,
    const std::string& cache_root_dir) {
  if (graphs_.count(name)) {
    return std::nullopt;
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

    if (!info.yaml.empty() && std::filesystem::exists(info.yaml) &&
        !yaml_path.empty()) {
      std::filesystem::copy_file(
          info.yaml, yaml_target,
          std::filesystem::copy_options::overwrite_existing);
    }

    if (!config_path.empty() && std::filesystem::exists(config_path)) {
      const auto config_target = info.root / "config.yaml";
      std::filesystem::copy_file(
          config_path, config_target,
          std::filesystem::copy_options::overwrite_existing);
    }
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
  }

  setup_schedulers_for_runtime(name, *runtime);
  runtime->start();

  const auto final_yaml_to_load = info.root / "content.yaml";
  if (std::filesystem::exists(final_yaml_to_load)) {
    try {
      runtime->graph_state()
          .submit([this, yaml = final_yaml_to_load](GraphModel& graph) {
            io_service_.load(graph, yaml);
            return 0;
          })
          .get();
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& e) {
      std::cerr << "Failed to load YAML for graph '" << name
                << "': " << e.what() << std::endl;
      return std::nullopt;
    }
  } else if (!yaml_path.empty()) {
    std::cerr << "Warning: source YAML file not found for graph '" << name
              << "': " << yaml_path << std::endl;
  }

  std::optional<std::string> loaded_name(std::in_place, name);
  const auto inserted = graphs_.emplace(name, std::move(runtime));
  if (!inserted.second) {
    return std::nullopt;
  }
  return loaded_name;
}

/** @copydoc Kernel::close_graph */
bool Kernel::close_graph(const std::string& name) {
  auto it = graphs_.find(name);
  if (it == graphs_.end()) {
    return false;
  }

  GraphRuntime& runtime = *it->second;
  runtime.graph_state()
      .submit([&runtime](GraphModel&) {
        runtime.stop();
        return 0;
      })
      .get();
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

void Kernel::set_scheduler_config(const SchedulerConfig& config) {
  scheduler_config_ = config;
}

const Kernel::SchedulerConfig& Kernel::get_scheduler_config() const {
  return scheduler_config_;
}

void Kernel::setup_schedulers_for_runtime(const std::string& name,
                                          GraphRuntime& runtime) {
  std::vector<std::string> failures;

  auto hp_scheduler = SchedulerFactory::create(scheduler_config_.hp_type,
                                               scheduler_config_.worker_count);
  if (hp_scheduler) {
    runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                          std::move(hp_scheduler));
  } else {
    failures.push_back("HP scheduler type '" + scheduler_config_.hp_type + "'");
  }

  auto rt_scheduler = SchedulerFactory::create(scheduler_config_.rt_type,
                                               scheduler_config_.worker_count);
  if (rt_scheduler) {
    runtime.set_scheduler(ComputeIntent::RealTimeUpdate,
                          std::move(rt_scheduler));
  } else {
    failures.push_back("RT scheduler type '" + scheduler_config_.rt_type + "'");
  }

  if (!failures.empty()) {
    std::ostringstream message;
    message << "Failed to create configured scheduler";
    if (failures.size() > 1) {
      message << "s";
    }
    message << ": ";
    for (size_t i = 0; i < failures.size(); ++i) {
      if (i > 0) {
        message << ", ";
      }
      message << failures[i];
    }
    message << ". Check scheduler_dirs startup scanning and scheduler plugin "
               "ABI requirements.";
    store_last_error(name,
                     LastError{GraphErrc::InvalidParameter, message.str()});
  } else {
    clear_last_error(name);
  }
}

}  // namespace ps
