/**
 * @file kernel.cpp
 * @brief Implements Kernel Graph lifecycle and execution-default methods.
 *
 * The broader Kernel facade is split by responsibility across
 * kernel_compute.cpp, kernel_io_cache_facade.cpp,
 * kernel_inspection_facade.cpp, kernel_dirty_roi_facade.cpp, and
 * kernel_execution_facade.cpp. This file keeps ownership setup for Graph
 * runtimes, Graph listing/closing, Metal device access, and execution
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
#include "photospider/host/host.hpp"
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

/** @copydoc Kernel::policy_available_types */
std::vector<std::string> Kernel::policy_available_types() const {
  return execution_service_->policy_available_types();
}

/** @copydoc Kernel::policy_description */
std::string Kernel::policy_description(const std::string& type_name) const {
  return execution_service_->policy_description(type_name);
}

/** @copydoc Kernel::policy_scan */
std::size_t Kernel::policy_scan(const std::vector<std::string>& directories) {
  return execution_service_->policy_scan(directories);
}

/** @copydoc Kernel::policy_load */
void Kernel::policy_load(const std::string& path) {
  execution_service_->policy_load(path);
}

/** @copydoc Kernel::policy_loaded_plugins */
std::vector<std::string> Kernel::policy_loaded_plugins() const {
  return execution_service_->policy_loaded_plugins();
}

/** @copydoc Kernel::configure_policy_defaults */
void Kernel::configure_policy_defaults(const HostPolicyConfig& config) {
  execution_service_->configure_policy_defaults(config);
}

/** @copydoc Kernel::policy_info */
PolicyInfoSnapshot Kernel::policy_info(PolicyClass policy_class) const {
  return execution_service_->policy_info(policy_class);
}

/** @copydoc Kernel::replace_policy */
void Kernel::replace_policy(PolicyClass policy_class, const std::string& type) {
  execution_service_->replace_policy(policy_class, type);
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

  if (!execution_service_->is_configured()) {
    execution_service_->configure_worker_count(execution_config_.worker_count);
    execution_config_.worker_count = execution_service_->worker_count();
  }
  GraphRuntime::Info info;
  info.name = name;
  info.root = std::filesystem::path(root_dir) / name;
  info.yaml = effective_yaml_path;
  info.config = config_path;
  info.cache_root = effective_cache_root;
  info.hp_execution_type = execution_config_.hp_type;
  info.rt_execution_type = execution_config_.rt_type;
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
  it->second->stop_compute_request_admission();
  return true;
}

/** @copydoc Kernel::close_graph */
bool Kernel::close_graph(const std::string& name) {
  auto it = graphs_.find(name);
  if (it == graphs_.end()) {
    return false;
  }

  GraphRuntime& runtime = *it->second;
  runtime.close_compute_requests();
  runtime.graph_state().close_and_drain();
  try {
    runtime.stop();
  } catch (...) {
    const std::exception_ptr stop_failure = std::current_exception();
    runtime.graph_state().restart_after_close_failure();
    runtime.restart_compute_requests_after_close_failure();
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

/** @copydoc Kernel::set_execution_config */
void Kernel::set_execution_config(const ExecutionConfig& config) {
  ExecutionConfig candidate = config;
  if (!compute::ExecutionService::is_execution_type(candidate.hp_type) ||
      !compute::ExecutionService::is_execution_type(candidate.rt_type) ||
      candidate.worker_count > kExecutionWorkerRequestMax) {
    throw std::invalid_argument("Invalid execution defaults.");
  }
  execution_service_->configure_worker_count(candidate.worker_count);
  candidate.worker_count = execution_service_->worker_count();
  execution_config_ = std::move(candidate);
}

const Kernel::ExecutionConfig& Kernel::get_execution_config() const {
  return execution_config_;
}

/** @copydoc Kernel::execution_available_types */
std::vector<std::string> Kernel::execution_available_types() const {
  return compute::ExecutionService::available_execution_types();
}

/** @copydoc Kernel::execution_description */
std::string Kernel::execution_description(const std::string& type_name) const {
  return compute::ExecutionService::execution_description(type_name);
}

}  // namespace ps
