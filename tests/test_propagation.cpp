#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "graph_model.hpp"
#include "kernel/interaction.hpp"
#include "kernel/kernel.hpp"
#include "kernel/services/graph_event_service.hpp"  // 为了事件结构
#include "ps_types.hpp"

// Tiling 常量
const int TILE_SIZE_MICRO = 64;
const int TILE_SIZE_MACRO = 256;

// 获取节点的瓦片大小 (这个辅助函数仍然有用)
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

void handle_tiles(ps::InteractionService& svc, const std::string& graph_name,
                  const std::string& target_node_str) {
  std::vector<int> node_ids_to_query;
  ps::GraphModel& model = svc.kernel().runtime(graph_name).model();

  if (target_node_str == "all") {
    node_ids_to_query = model.node_ids();
  } else {
    try {
      int id = std::stoi(target_node_str);
      if (model.has_node(id))
        node_ids_to_query.push_back(id);
      else {
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
      auto fut_opt = svc.cmd_compute_async(graph_name, end_node_id, "int8",
                                           false, false, true);
      if (fut_opt)
        fut_opt->get();
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

void handle_dirty(ps::InteractionService& svc, const std::string& graph_name,
                  int start_node_id, const std::string& tile_coords_str) {
  ps::GraphModel& model = svc.kernel().runtime(graph_name).model();
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
  cv::Rect initial_roi(tile_x * tile_size, tile_y * tile_size, tile_size,
                       tile_size);
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
  cv::Rect target_roi = *projected_roi_opt;
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

  std::function<void(int, const cv::Rect&, int)> trace_backward;
  std::set<int> visited_in_trace;

  trace_backward = [&](int current_id, const cv::Rect& current_roi, int depth) {
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

  // 清空事件队列以捕获本次计算的事件
  svc.cmd_drain_compute_events(graph_name);

  std::cout << "--- Triggering Actual Kernel Compute ---" << std::endl;
  auto future_opt = svc.kernel().compute_async(
      graph_name, target_end_node,
      "int8",                             // cache_precision
      false,                              // force_recache
      false,                              // enable_timing
      true,                               // parallel
      true,                               // quiet
      true,                               // disable_disk_cache
      false,                              // nosave
      nullptr,                            // benchmark_events
      ps::ComputeIntent::RealTimeUpdate,  // **关键意图**
      target_roi                          // **关键脏区（已投射到目标坐标）**
  );

  if (!future_opt) {
    std::cerr << "Failed to start compute task." << std::endl;
    return;
  }
  bool success = future_opt->get();  // 等待计算完成
  if (!success) {
    std::cerr << "Compute task failed for node " << target_end_node << ".";
    if (auto err = svc.cmd_last_error(graph_name)) {
      std::cerr << " Reason: [" << static_cast<int>(err->code) << "] "
                << err->message;
    }
    std::cerr << std::endl;
    return;
  }

  // **分析结果：从事件服务中获取真正被重新计算的节点**
  auto events = svc.cmd_drain_compute_events(graph_name);
  if (!events || events->empty()) {
    std::cout << "(No nodes were re-computed)" << std::endl;
    return;
  }

  std::map<int, std::vector<ps::GraphEventService::ComputeEvent>>
      events_by_node;
  for (const auto& event : *events) {
    if (event.source == "rt_update" || event.source == "hp_update" ||
        event.source == "computed") {
      events_by_node[event.id].push_back(event);
    }
  }

  // 由于事件不包含像素区域，我们无法直接打印。
  // 我们将打印被触发的节点，这是一个更真实的集成测试结果。
  for (const auto& pair : events_by_node) {
    std::cout << "Node " << pair.first << " (" << pair.second[0].name
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

    if (command == "exit")
      break;
    else if (command == "dirty") {
      int node_id;
      std::string tile_str;
      if (!(iss >> node_id >> tile_str))
        std::cerr << "Usage: dirty <id> <x,y>" << std::endl;
      else
        handle_dirty(svc, graph_name, node_id, tile_str);
    } else if (command == "tiles") {
      std::string target;
      if (!(iss >> target))
        std::cerr << "Usage: tiles all|<id>" << std::endl;
      else
        handle_tiles(svc, graph_name, target);
    } else if (!command.empty())
      std::cerr << "Unknown command." << std::endl;

    std::cout << "> " << std::flush;
  }
  return 0;
}
