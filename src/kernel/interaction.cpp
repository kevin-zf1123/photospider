// InteractionService implementation
// Non-trivial methods that require additional includes

#include "kernel/interaction.hpp"

#include <set>

#include "kernel/scheduler/scheduler_factory.hpp"
#include "kernel/scheduler/scheduler_plugin_loader.hpp"

namespace ps {

std::vector<std::string> InteractionService::cmd_scheduler_available_types() const {
  std::set<std::string> types_set;
  
  // Built-in types from SchedulerFactory
  for (const auto& t : SchedulerFactory::supported_types()) {
    types_set.insert(t);
  }
  
  // Plugin-provided types from SchedulerPluginLoader
  auto& loader = SchedulerPluginLoader::instance();
  for (const auto& t : loader.get_registered_types()) {
    types_set.insert(t);
  }
  
  return std::vector<std::string>(types_set.begin(), types_set.end());
}

std::string InteractionService::cmd_scheduler_description(const std::string& type_name) const {
  // First check built-in
  if (SchedulerFactory::is_supported(type_name)) {
    return SchedulerFactory::description(type_name);
  }
  
  // Then check plugin loader
  auto& loader = SchedulerPluginLoader::instance();
  return loader.get_description(type_name);
}

size_t InteractionService::cmd_scheduler_scan(const std::vector<std::string>& dirs) {
  auto& loader = SchedulerPluginLoader::instance();
  return loader.scan_and_load(dirs);
}

bool InteractionService::cmd_scheduler_load(const std::string& path) {
  auto& loader = SchedulerPluginLoader::instance();
  return loader.load_plugin(path);
}

std::vector<std::string> InteractionService::cmd_scheduler_loaded_plugins() const {
  auto& loader = SchedulerPluginLoader::instance();
  return loader.list_loaded_plugins();
}

}  // namespace ps
