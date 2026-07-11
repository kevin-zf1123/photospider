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
#include "kernel/kernel.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "kernel/scheduler/scheduler_factory.hpp"

namespace ps {

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
 * @note The runtime enters graphs_ only after YAML loading succeeds, so public
 * Host callers cannot observe a partially loaded session.
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

  graphs_[name] = std::move(runtime);
  return name;
}

bool Kernel::close_graph(const std::string& name) {
  auto it = graphs_.find(name);
  if (it == graphs_.end()) {
    return false;
  }
  it->second->stop();
  graphs_.erase(it);
  last_error_.erase(name);
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
    last_error_[name] = {GraphErrc::InvalidParameter, message.str()};
  } else {
    last_error_.erase(name);
  }
}

}  // namespace ps
