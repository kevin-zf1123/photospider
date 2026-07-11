// FILE: apps/graph_cli/src/command/command_compute.cpp
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "graph_cli/command/commands.hpp"
#include "graph_cli/command/help_utils.hpp"

namespace {

/**
 * @brief CLI-local request for scheduling compute and polling status.
 *
 * The struct keeps CLI polling/logging switches separate from the public Host
 * compute request so execute_and_wait receives one stable object instead of a
 * sequence of booleans. The CLI never constructs an internal
 * `Kernel::ComputeRequest`.
 *
 * @note compute.telemetry.enable_timing is derived from timer_console or
 * timer_log before the request is passed to this helper.
 */
struct ComputeWaitRequest {
  /** @brief Host compute request built from parsed CLI arguments. */
  ps::HostComputeRequest compute;

  /** @brief Whether timing should be printed to the console after compute. */
  bool timer_console = false;

  /** @brief Whether timing should be appended to timer_log_path. */
  bool timer_log = false;

  /** @brief Whether progress events and startup text should be suppressed. */
  bool mute = false;

  /** @brief File path used when timer_log is enabled. */
  std::string timer_log_path;
};

/**
 * @brief Schedules one async compute request and waits for completion.
 *
 * @param svc Public Host seam used by the CLI command.
 * @param request Host compute request plus CLI polling/logging controls.
 * @return true when the future completes successfully, false on schedule or
 *         compute failure.
 * @throws std::bad_alloc if Host scheduling, copied event/diagnostic values, or
 *         async status mapping cannot allocate. Recoverable backend failures
 *         are converted by the embedded Host adapter into OperationStatus and
 *         last_error values.
 * @note Progress events are drained before and during the wait loop to keep the
 * CLI output behavior unchanged.
 */
bool execute_and_wait(ps::Host& svc, const ComputeWaitRequest& request) {
  (void)svc.drain_compute_events(request.compute.session);

  auto future_result = svc.compute_async(request.compute);

  if (!future_result.status.ok) {
    std::cout << "Error: failed to schedule compute task for node "
              << request.compute.node.value << ".\n";
    return false;
  }

  auto future = std::move(future_result.value);
  if (!request.mute) {
    std::cout << "Computing node " << request.compute.node.value << "..."
              << std::endl;
  }

  while (future.wait_for(std::chrono::milliseconds(50)) ==
         std::future_status::timeout) {
    if (!request.mute) {
      auto events = svc.drain_compute_events(request.compute.session);
      if (events.status.ok) {
        for (const auto& event : events.value) {
          std::cout << "  - Node " << event.node.value << " (" << event.name
                    << ") completed [" << event.source << "]" << std::endl;
        }
      }
    }
  }

  if (!request.mute) {
    auto tail_events = svc.drain_compute_events(request.compute.session);
    if (tail_events.status.ok) {
      for (const auto& event : tail_events.value) {
        std::cout << "  - Node " << event.node.value << " (" << event.name
                  << ") completed [" << event.source << "]" << std::endl;
      }
    }
  }

  auto status = future.get();
  if (!status.ok) {
    std::cout << "Error: Compute task failed for node "
              << request.compute.node.value << ".\n";
    auto err = svc.last_error(request.compute.session);
    if (!err.ok && !err.message.empty()) {
      std::cout << "  Reason: " << err.message << std::endl;
    } else if (!status.message.empty()) {
      std::cout << "  Reason: " << status.message << std::endl;
    }
  }
  return status.ok;
}

}  // namespace

/** @copydoc handle_compute */
bool handle_compute(std::istringstream& iss, ps::Host& svc,
                    std::string& current_graph, bool& /*modified*/,
                    CliConfig& config) {
  if (current_graph.empty()) {
    std::cout << "No current graph. Use load/switch.\n";
    return true;
  }

  std::string target_str;
  iss >> target_str;
  if (target_str.empty()) {
    std::cout << "Usage: compute <id|all> [flags...]\n";
    return true;
  }

  // [核心修复] 参数解析与验证
  std::vector<int> nodes_to_compute;
  auto all_node_ids_result =
      svc.list_node_ids(ps::GraphSessionId{current_graph});
  if (!all_node_ids_result.status.ok) {
    std::cout << "Error: Could not retrieve node list for current graph.\n";
    return true;
  }
  auto& all_node_ids = all_node_ids_result.value;

  if (target_str == "all") {
    // [核心修复] 使用 Host ending_nodes 快照选择终端节点。
    auto ending_nodes_result =
        svc.ending_nodes(ps::GraphSessionId{current_graph});
    if (ending_nodes_result.status.ok) {
      for (const auto& node : ending_nodes_result.value) {
        nodes_to_compute.push_back(node.value);
      }
    }
    if (nodes_to_compute.empty()) {
      std::cout << "No ending nodes to compute in the graph.\n";
      return true;
    }
  } else {
    try {
      int node_id = std::stoi(target_str);
      bool exists = false;
      for (const auto& id : all_node_ids) {
        if (id.value == node_id) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        std::cout << "Error: Node with ID " << node_id
                  << " does not exist in the current graph.\n";
        return true;
      }
      nodes_to_compute.push_back(node_id);
    } catch (const std::invalid_argument&) {
      std::cout << "Error: Invalid target '" << target_str
                << "'. Must be an integer ID or 'all'.\n";
      return true;
    }
  }

  // 解析 flags
  std::vector<std::string> flags;
  std::string flag;
  while (iss >> flag)
    flags.push_back(flag);
  if (flags.empty() && !config.default_compute_args.empty()) {
    std::istringstream def_iss(config.default_compute_args);
    while (def_iss >> flag)
      flags.push_back(flag);
  }
  bool force = false, force_deep = false, parallel = false,
       timer_console = false, timer_log = false, mute = false, nosave = false;
  std::string timer_log_path = config.default_timer_log_path;
  // Known compute flags to avoid mis-parsing as a tl path
  static const std::unordered_set<std::string> kKnownFlags = {
      "force", "force-deep", "parallel", "t",    "-t",     "timer", "tl",
      "-tl",   "m",          "-m",       "mute", "nosave", "ns"};
  for (size_t i = 0; i < flags.size(); ++i) {
    const auto& f = flags[i];
    if (f == "force") {
      force = true;
    } else if (f == "force-deep") {
      force_deep = true;
    } else if (f == "parallel") {
      parallel = true;
    } else if (f == "t" || f == "-t" || f == "timer") {
      timer_console = true;
    } else if (f == "tl" || f == "-tl") {
      // Enable timer log; only consume next token as a path if it is NOT a
      // known flag.
      timer_log = true;
      if (i + 1 < flags.size()) {
        const std::string& maybe_path = flags[i + 1];
        if (kKnownFlags.find(maybe_path) == kKnownFlags.end()) {
          timer_log_path = maybe_path;
          ++i;  // consume the path token
        }
      }
    } else if (f == "m" || f == "-m" || f == "mute") {
      mute = true;
    } else if (f == "nosave" || f == "ns") {
      nosave = true;
    }
  }

  // [核心修复] 循环执行计算，并聚合每次 compute 的计时
  bool all_ok = true;
  auto overall_start_time = std::chrono::high_resolution_clock::now();
  std::vector<ps::NodeTimingSnapshot> aggregated_timings;
  aggregated_timings.reserve(128);

  for (int node_id : nodes_to_compute) {
    ps::HostComputeRequest compute_request;
    compute_request.session = ps::GraphSessionId{current_graph};
    compute_request.node = ps::NodeId{node_id};
    compute_request.cache.precision = config.cache_precision;
    compute_request.cache.force_recache = force || force_deep;
    compute_request.cache.disable_disk_cache = force_deep;
    compute_request.cache.nosave = nosave;
    compute_request.execution.parallel = parallel;
    compute_request.execution.quiet = mute;
    compute_request.telemetry.enable_timing = timer_console || timer_log;
    ComputeWaitRequest wait_request{std::move(compute_request), timer_console,
                                    timer_log, mute, timer_log_path};
    if (!execute_and_wait(svc, wait_request)) {
      all_ok = false;
      break;  // 一个失败就停止
    }
    // 在下一次 compute 重置计时之前，抓取本次的节点计时并聚合
    if (timer_console || timer_log) {
      auto timers = svc.timing(ps::GraphSessionId{current_graph});
      if (timers.status.ok) {
        for (const auto& nt : timers.value.node_timings)
          aggregated_timings.push_back(nt);
      }
    }
  }

  auto overall_end_time = std::chrono::high_resolution_clock::now();

  std::cout << (all_ok ? "Computation finished." : "Computation failed.")
            << std::endl;

  if (timer_console || timer_log) {
    // 构造聚合后的 timing snapshot
    ps::TimingSnapshot agg;
    agg.node_timings = std::move(aggregated_timings);
    // 使用节点耗时之和作为 "total"，避免包含 REPL 轮询等待等开销
    double total_node_ms = 0.0;
    for (const auto& nt : agg.node_timings)
      total_node_ms += nt.elapsed_ms;
    agg.total_ms = total_node_ms;

    const double wall_ms = std::chrono::duration<double, std::milli>(
                               overall_end_time - overall_start_time)
                               .count();

    std::stringstream log_buffer;
    log_buffer << "Timing Report (total " << agg.total_ms << " ms, wall "
               << wall_ms << " ms):" << std::endl;
    for (const auto& nt : agg.node_timings) {
      log_buffer << "  - Node " << nt.node.value << " (" << nt.name
                 << ") completed in " << nt.elapsed_ms << " ms [" << nt.source
                 << "]" << std::endl;
    }
    if (timer_console)
      std::cout << log_buffer.str();
    if (timer_log) {
      std::ofstream log_file(timer_log_path, std::ios::app);  // 追加模式
      if (log_file) {
        log_file << log_buffer.str() << "\n";
        std::cout << "Timing report appended to " << timer_log_path
                  << std::endl;
      } else {
        std::cout << "Error: Could not open log file " << timer_log_path
                  << std::endl;
      }
    }
  }

  return true;
}

/** @copydoc print_help_compute */
void print_help_compute(const CliConfig& /*config*/) {
  print_help_from_file("help_compute.txt");
}
