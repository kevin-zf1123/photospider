#include <algorithm>
#include <cstddef>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/ps_types.hpp"                // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"            // NOLINT(build/include_subdir)
#include "runtime/graph_event_service.hpp"  // 为了事件结构
#include "runtime/interaction.hpp"
#include "runtime/kernel.hpp"
#include "support/kernel_test_access.hpp"

/** @brief Micro-tile fallback edge length used by the scriptable test tool. */
const int TILE_SIZE_MICRO = 64;

/** @brief Macro-tile fallback edge length used by the scriptable test tool. */
const int TILE_SIZE_MACRO = 256;

/**
 * @brief Maximum compute-event pages consumed by one manual-tool pass.
 *
 * @note The ceiling formula covers one complete fixed production ring in the
 *       absence of concurrent publication and prevents a producer from making
 *       this scriptable tool chase pages forever.
 */
constexpr std::size_t kComputeEventDrainPageBudget =
    (ps::kComputeEventRingCapacity + ps::kComputeEventDrainMaxLimit - 1) /
    ps::kComputeEventDrainMaxLimit;

static_assert(kComputeEventDrainPageBudget == 8);

/**
 * @brief Drains one fixed-budget pass of compute-event pages.
 *
 * @tparam ConsumeBatch Callable accepting each available batch by const
 *         reference.
 * @param svc Interaction facade owning the bounded drain operation.
 * @param graph_name Loaded graph whose event ring is drained.
 * @param consume_batch Callback invoked once for every successful page.
 * @return true when every attempted backend call returns a batch, false when
 *         the graph or event service is unavailable.
 * @throws Any exception from bounded result construction or the callback,
 *         including `std::bad_alloc`.
 * @note The helper stops early when `has_more` is false and otherwise performs
 *       at most `kComputeEventDrainPageBudget` calls. It never restores an
 *       unbounded event-drain API.
 */
template <typename ConsumeBatch>
bool drain_compute_event_pages(ps::InteractionService& svc,
                               const std::string& graph_name,
                               ConsumeBatch&& consume_batch) {
  for (std::size_t page_index = 0; page_index < kComputeEventDrainPageBudget;
       ++page_index) {
    auto batch = svc.cmd_drain_compute_events(graph_name,
                                              ps::kComputeEventDrainMaxLimit);
    if (!batch) {
      return false;
    }
    const bool has_more = batch->has_more;
    consume_batch(*batch);
    if (!has_more) {
      return true;
    }
  }
  return true;
}

/**
 * @brief Resolves the tile edge length implied by a node operation metadata.
 *
 * @param node Graph node whose type/subtype metadata should be inspected.
 * @return Tile edge length in pixels; defaults to TILE_SIZE_MICRO when the op
 * metadata is missing or monolithic.
 * @throws Nothing directly; registry lookup returns optional metadata.
 * @note This helper is only used by the propagation test REPL output and does
 * not mutate graph state.
 */
int get_tile_size(const ps::Node& node) {
  auto meta_opt =
      ps::OpRegistry::instance().get_metadata(node.type, node.subtype);
  if (meta_opt) {
    if (meta_opt->tile_preference == ps::TileSizePreference::MICRO)
      return TILE_SIZE_MICRO;
    if (meta_opt->tile_preference == ps::TileSizePreference::MACRO)
      return TILE_SIZE_MACRO;
  }
  // 对于 monolithic 或未定义的，我们假设一个基础粒度用于报告
  return TILE_SIZE_MICRO;
}

/**
 * @brief Prints computed tile counts for one node or all nodes in a graph.
 *
 * @param kernel Kernel owning the graph; used only through internal test
 * access to inspect cached output dimensions.
 * @param svc Interaction facade used for graph lifecycle and compute commands.
 * @param graph_name Loaded graph session name.
 * @param target_node_str Either "all" or a decimal node id.
 * @throws std::runtime_error if the graph is unexpectedly missing from Kernel.
 * @note The function computes graph endings before inspection so cached output
 * metadata exists for tile reporting.
 */
void handle_tiles(ps::Kernel& kernel, ps::InteractionService& svc,
                  const std::string& graph_name,
                  const std::string& target_node_str) {
  std::vector<int> node_ids_to_query;
  ps::GraphModel& model =
      ps::testing::KernelTestAccess::model(kernel, graph_name);

  if (target_node_str == "all") {
    node_ids_to_query = model.node_ids();
  } else {
    try {
      int id = std::stoi(target_node_str);
      if (model.has_node(id)) {
        node_ids_to_query.push_back(id);
      } else {
        std::cerr << "Error: Node " << id << " not found." << std::endl;
        return;
      }
    } catch (...) {
      std::cerr << "Error: Invalid node id." << std::endl;
      return;
    }
  }

  // 在查询前确保图是计算过的
  auto ending_nodes = svc.cmd_ending_nodes(graph_name);
  if (ending_nodes && !ending_nodes->empty()) {
    for (int end_node_id : *ending_nodes) {
      ps::Kernel::ComputeRequest request;
      request.name = graph_name;
      request.node_id = end_node_id;
      request.cache.precision = "int8";
      request.execution.parallel = true;
      auto fut_opt = svc.cmd_compute_async(request);
      if (fut_opt) {
        fut_opt->get();
      }
    }
  }

  for (int id : node_ids_to_query) {
    const auto& node = model.node(id);
    std::string pref_str = "UNDEFINED";
    int width = 0, height = 0;

    const auto* out = node.cached_output_high_precision.has_value()
                          ? &*node.cached_output_high_precision
                          : nullptr;
    if (out && out->image_buffer.width > 0) {
      width = out->image_buffer.width;
      height = out->image_buffer.height;
    } else {
      std::cout << id << " UNKNOWN (not computed or no image output) - - - -"
                << std::endl;
      continue;
    }

    auto meta_opt =
        ps::OpRegistry::instance().get_metadata(node.type, node.subtype);
    if (meta_opt) {
      if (meta_opt->tile_preference == ps::TileSizePreference::MICRO)
        pref_str = "MICRO";
      if (meta_opt->tile_preference == ps::TileSizePreference::MACRO)
        pref_str = "MACRO";
    }

    int tile_size = get_tile_size(node);
    int h_tiles = (width + tile_size - 1) / tile_size;
    int v_tiles = (height + tile_size - 1) / tile_size;

    std::cout << id << " " << pref_str << " " << h_tiles << " " << v_tiles
              << " " << width << " " << height << std::endl;
  }
}

/**
 * @brief Simulates a dirty tile update and prints propagated recompute events.
 *
 * @param kernel Kernel owning the graph; used only through internal test
 * access to inspect topology during scriptable propagation diagnostics.
 * @param svc Interaction facade used for ROI projection, compute, and events.
 * @param graph_name Loaded graph session name.
 * @param start_node_id Dirty source node id.
 * @param tile_coords_str Tile coordinate text in "x,y" form.
 * @throws std::runtime_error if the graph is unexpectedly missing from Kernel.
 * @note This scriptable helper validates the propagation path through public
 * InteractionService commands while limiting direct model access to internal
 * test setup and diagnostic traversal. Async failure output comes from the
 * work-item-owned AsyncComputeResult rather than mutable Kernel LastError.
 */
void handle_dirty(ps::Kernel& kernel, ps::InteractionService& svc,
                  const std::string& graph_name, int start_node_id,
                  const std::string& tile_coords_str) {
  ps::GraphModel& model =
      ps::testing::KernelTestAccess::model(kernel, graph_name);
  if (!model.has_node(start_node_id)) {
    std::cerr << "Error: Start node " << start_node_id << " not found."
              << std::endl;
    return;
  }

  int tile_x, tile_y;
  char comma;
  std::istringstream iss(tile_coords_str);
  iss >> tile_x >> comma >> tile_y;
  if (iss.fail() || comma != ',') {
    std::cerr << "Error: Invalid tile format. Use x,y (e.g., 3,1)."
              << std::endl;
    return;
  }

  const auto& start_node = model.node(start_node_id);
  int tile_size = get_tile_size(start_node);
  ps::PixelRect initial_roi{tile_x * tile_size, tile_y * tile_size, tile_size,
                            tile_size};
  std::cout << "Triggering dirty region at Node " << start_node_id << " tile ("
            << tile_x << "," << tile_y << ") -> pixel ROI " << initial_roi.x
            << "," << initial_roi.y << "," << initial_roi.width << ","
            << initial_roi.height << std::endl;

  auto ending_nodes = svc.cmd_ending_nodes(graph_name);
  if (!ending_nodes || ending_nodes->empty()) {
    std::cerr << "Error: No ending nodes in the graph to compute." << std::endl;
    return;
  }

  int target_end_node = (*ending_nodes)[0];

  auto projected_roi_opt = svc.cmd_project_roi(graph_name, start_node_id,
                                               initial_roi, target_end_node);
  if (!projected_roi_opt) {
    std::cout << "(Dirty ROI does not affect target node " << target_end_node
              << ")" << std::endl;
    return;
  }
  ps::PixelRect target_roi = *projected_roi_opt;
  std::cout << "Projected Forward to End Node " << target_end_node
            << " -> pixel ROI " << target_roi.x << "," << target_roi.y << ","
            << target_roi.width << "," << target_roi.height << std::endl;

  // =========================================================
  // --- 新增：可视化反向传播路径 (Recursive Trace) ---
  // =========================================================
  std::cout << "\n--- Visualizing Backward Propagation Path ---" << std::endl;
  std::cout << "(Walking backwards from End Node " << target_end_node
            << " to Source Node " << start_node_id << ")\n"
            << std::endl;

  std::function<void(int, const ps::PixelRect&, int)> trace_backward;
  std::set<int> visited_in_trace;

  trace_backward = [&](int current_id, const ps::PixelRect& current_roi,
                       int depth) {
    std::string indent(depth * 2, ' ');
    const auto& node = model.node(current_id);

    std::cout << indent << "<- Node " << current_id << " (" << node.name
              << " | " << node.subtype << ")";
    std::cout << " requires ROI: [" << current_roi.x << "," << current_roi.y
              << " " << current_roi.width << "x" << current_roi.height << "]"
              << std::endl;

    if (current_id == start_node_id) {
      std::cout << indent << "   (Hit Dirty Source!)" << std::endl;
      return;
    }

    if (visited_in_trace.count(current_id)) {
      std::cout << indent << "   (Visited)" << std::endl;
      return;
    }
    visited_in_trace.insert(current_id);

    for (const auto& edge : model.upstream_edges(current_id)) {
      if (edge.kind != ps::GraphTopologyEdgeKind::ImageInput)
        continue;
      int parent_id = edge.from_node_id;
      if (parent_id < 0)
        continue;

      auto parent_roi_opt = svc.cmd_project_roi_backward(
          graph_name, current_id, current_roi, parent_id);

      if (parent_roi_opt) {
        trace_backward(parent_id, *parent_roi_opt, depth + 1);
      } else {
        std::cout << indent << "   <- Node " << parent_id
                  << " (ROI blocked/empty)" << std::endl;
      }
    }
    visited_in_trace.erase(current_id);
  };

  trace_backward(target_end_node, target_roi, 0);
  std::cout << "---------------------------------------------\n" << std::endl;

  // 清空当前固定 ring，以捕获本次计算的事件。
  (void)drain_compute_event_pages(svc, graph_name,
                                  [](const ps::ComputeEventBatch&) {});

  std::cout << "--- Triggering Actual Kernel Compute ---" << std::endl;
  ps::Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = target_end_node;
  request.cache.precision = "int8";
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  request.execution.quiet = true;
  request.intent = ps::ComputeIntent::RealTimeUpdate;
  request.dirty_roi = target_roi;
  auto future_opt = svc.cmd_compute_async(request);

  if (!future_opt) {
    std::cerr << "Failed to start compute task." << std::endl;
    return;
  }
  const ps::Kernel::AsyncComputeResult outcome =
      future_opt->get();  // 等待计算完成
  if (!outcome.ok) {
    std::cerr << "Compute task failed for node " << target_end_node << ".";
    if (outcome.error) {
      std::cerr << " Reason: [" << static_cast<int>(outcome.error->code) << "] "
                << outcome.error->message;
    }
    std::cerr << std::endl;
    return;
  }

  // **分析结果：逐页处理事件服务中真正被重新计算的节点**
  std::map<int, std::string> recomputed_nodes;
  const bool events_available = drain_compute_event_pages(
      svc, graph_name, [&](const ps::ComputeEventBatch& batch) {
        for (const auto& event : batch.events) {
          if (event.source == "rt_update" || event.source == "hp_update" ||
              event.source == "computed") {
            recomputed_nodes.emplace(event.node.value, event.name);
          }
        }
      });
  if (!events_available || recomputed_nodes.empty()) {
    std::cout << "(No nodes were re-computed)" << std::endl;
    return;
  }

  // 由于事件不包含像素区域，我们无法直接打印。
  // 我们将打印被触发的节点，这是一个更真实的集成测试结果。
  for (const auto& pair : recomputed_nodes) {
    std::cout << "Node " << pair.first << " (" << pair.second
              << ") was recomputed." << std::endl;
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <path_to_graph.yaml>" << std::endl;
    return 1;
  }

  ps::Kernel kernel;
  ps::InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();
  svc.cmd_plugins_load({"build/plugins"});

  std::string graph_path = argv[1];
  std::string graph_name = "propagation_test";
  auto loaded_name = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  if (!loaded_name) {
    std::cerr << "Error: Failed to load graph." << std::endl;
    return 1;
  }
  std::cout << "Graph loaded from '" << graph_path << "'." << std::endl;

  // 进入REPL循环
  std::cout << "\nEnter commands ('dirty <id> <x,y>', 'tiles all', 'exit').\n> "
            << std::flush;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream iss(line);
    std::string command;
    iss >> command;

    if (command == "exit") {
      break;
    } else if (command == "dirty") {
      int node_id;
      std::string tile_str;
      if (!(iss >> node_id >> tile_str)) {
        std::cerr << "Usage: dirty <id> <x,y>" << std::endl;
      } else {
        handle_dirty(kernel, svc, graph_name, node_id, tile_str);
      }
    } else if (command == "tiles") {
      std::string target;
      if (!(iss >> target)) {
        std::cerr << "Usage: tiles all|<id>" << std::endl;
      } else {
        handle_tiles(kernel, svc, graph_name, target);
      }
    } else if (!command.empty()) {
      std::cerr << "Unknown command." << std::endl;
    }

    std::cout << "> " << std::flush;
  }
  return 0;
}
