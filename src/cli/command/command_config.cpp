// FILE: src/cli/command/command_config.cpp
#include <sstream>
#include <string>

#include "cli/command/commands.hpp"
#include "cli/command/help_utils.hpp"
#include "cli/config_editor.hpp"

/**
 * @brief Maps accepted CLI scheduler fields onto Host scheduler defaults.
 * @param svc Borrowed Host receiving the default configuration.
 * @param config Accepted CLI configuration snapshot.
 * @return Nothing.
 * @throws std::bad_alloc if strings or Host result storage cannot allocate.
 * @note The Host status is intentionally ignored by the legacy editor flow;
 * the Host is borrowed and no scheduler worker or plugin handle is retained.
 */
static void apply_scheduler_config(ps::Host& svc, const CliConfig& config) {
  ps::HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = config.scheduler_hp_type;
  scheduler_config.rt_type = config.scheduler_rt_type;
  scheduler_config.worker_count =
      config.scheduler_worker_count > 0
          ? static_cast<unsigned int>(config.scheduler_worker_count)
          : 0;
  (void)svc.configure_scheduler_defaults(scheduler_config);
}

/** @copydoc handle_config */
bool handle_config(std::istringstream& /*iss*/, ps::Host& svc,
                   std::string& /*current_graph*/, bool& /*modified*/,
                   CliConfig& config) {
  if (run_config_editor(config)) {
    apply_scheduler_config(svc, config);
  }
  return true;
}

/** @copydoc print_help_config */
void print_help_config(const CliConfig& /*config*/) {
  print_help_from_file("help_config.txt");
}
