#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "benchmark/benchmark_types.hpp"
#include "kernel/graph_runtime.hpp"
#include "kernel/interaction.hpp"
#include "kernel/kernel.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "ps_types.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct TraceStore {
  std::mutex mutex;
  std::vector<json> events;
  std::string phase = "bootstrap";
  std::atomic<uint64_t> seq{0};

  void set_phase(const std::string& next) {
    std::lock_guard<std::mutex> lock(mutex);
    phase = next;
  }

  void record(json event) {
    std::lock_guard<std::mutex> lock(mutex);
    event["seq"] = seq.fetch_add(1);
    event["phase"] = phase;
    event["worker_id"] = ps::GraphRuntime::this_worker_id();
    event["task_epoch"] = ps::GraphRuntime::this_task_epoch();
    event["ts_us"] =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch())
            .count();
    events.push_back(std::move(event));
  }

  std::vector<json> snapshot() {
    std::lock_guard<std::mutex> lock(mutex);
    return events;
  }
};

TraceStore g_trace;

json rect_json(const cv::Rect& rect) {
  return {{"x", rect.x},
          {"y", rect.y},
          {"width", rect.width},
          {"height", rect.height}};
}

json size_json(const cv::Size& size) {
  return {{"width", size.width}, {"height", size.height}};
}

std::string data_type_name(ps::DataType type) {
  switch (type) {
    case ps::DataType::UINT8:
      return "UINT8";
    case ps::DataType::INT8:
      return "INT8";
    case ps::DataType::UINT16:
      return "UINT16";
    case ps::DataType::INT16:
      return "INT16";
    case ps::DataType::FLOAT32:
      return "FLOAT32";
    case ps::DataType::FLOAT64:
      return "FLOAT64";
  }
  return "UNKNOWN";
}

std::string yaml_string(const YAML::Node& node) {
  if (!node) {
    return "";
  }
  YAML::Emitter emitter;
  emitter << node;
  return emitter.c_str();
}

float yaml_float(const YAML::Node& node, const std::string& key,
                 float fallback) {
  if (node && node[key]) {
    try {
      return node[key].as<float>();
    } catch (...) {
    }
  }
  return fallback;
}

int yaml_int(const YAML::Node& node, const std::string& key, int fallback) {
  if (node && node[key]) {
    try {
      return node[key].as<int>();
    } catch (...) {
    }
  }
  return fallback;
}

json image_stats(const ps::NodeOutput* output) {
  if (!output) {
    return {{"present", false}};
  }
  const auto& buffer = output->image_buffer;
  json out = {{"present", true},
              {"width", buffer.width},
              {"height", buffer.height},
              {"channels", buffer.channels},
              {"type", data_type_name(buffer.type)},
              {"step", buffer.step},
              {"has_data", static_cast<bool>(buffer.data)}};
  if (buffer.width <= 0 || buffer.height <= 0 || buffer.channels <= 0 ||
      !buffer.data) {
    return out;
  }

  cv::Mat mat = ps::toCvMat(buffer);
  cv::Mat flat = mat.reshape(1);
  double min_val = 0.0;
  double max_val = 0.0;
  cv::minMaxLoc(flat, &min_val, &max_val);
  cv::Scalar mean = cv::mean(mat);
  cv::Scalar sum = cv::sum(mat);
  double checksum = 0.0;
  std::vector<double> means;
  std::vector<double> sums;
  for (int i = 0; i < std::max(1, buffer.channels); ++i) {
    means.push_back(mean[i]);
    sums.push_back(sum[i]);
    checksum += sum[i];
  }

  out["min"] = min_val;
  out["max"] = max_val;
  out["mean_channels"] = means;
  out["sum_channels"] = sums;
  out["checksum"] = checksum;
  out["debug"] = {{"worker", output->debug.computed_by_worker_id},
                  {"timestamp_us", output->debug.timestamp_us},
                  {"execution_time_ms", output->debug.execution_time_ms},
                  {"min_val", output->debug.min_val},
                  {"max_val", output->debug.max_val},
                  {"has_nan", output->debug.has_nan},
                  {"compute_device", output->debug.compute_device}};
  out["space"] = {{"absolute_roi", rect_json(output->space.absolute_roi)},
                  {"global_scale_x", output->space.global_scale_x},
                  {"global_scale_y", output->space.global_scale_y}};
  return out;
}

void write_text(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << text;
}

void write_json(const fs::path& path, const json& value) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << std::setw(2) << value << "\n";
}

void write_jsonl(const fs::path& path, const std::vector<json>& rows) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  for (const auto& row : rows) {
    out << row.dump() << "\n";
  }
}

std::string command_line(int argc, char** argv) {
  std::ostringstream os;
  for (int i = 0; i < argc; ++i) {
    if (i)
      os << ' ';
    os << argv[i];
  }
  return os.str();
}

json scheduler_events_json(
    const std::vector<ps::GraphRuntime::SchedulerEvent>& events) {
  json out = json::array();
  for (const auto& event : events) {
    std::string action = "UNKNOWN";
    switch (event.action) {
      case ps::GraphRuntime::SchedulerEvent::ASSIGN_INITIAL:
        action = "ASSIGN_INITIAL";
        break;
      case ps::GraphRuntime::SchedulerEvent::EXECUTE:
        action = "EXECUTE";
        break;
      case ps::GraphRuntime::SchedulerEvent::EXECUTE_TILE:
        action = "EXECUTE_TILE";
        break;
    }
    out.push_back(
        {{"epoch", event.epoch},
         {"node_id", event.node_id},
         {"worker_id", event.worker_id},
         {"action", action},
         {"ts_us", std::chrono::duration_cast<std::chrono::microseconds>(
                       event.timestamp.time_since_epoch())
                       .count()}});
  }
  return out;
}

json compute_events_json(
    const std::optional<std::vector<ps::GraphEventService::ComputeEvent>>&
        events) {
  json out = json::array();
  if (!events) {
    return out;
  }
  for (const auto& event : *events) {
    out.push_back({{"node_id", event.id},
                   {"name", event.name},
                   {"source", event.source},
                   {"elapsed_ms", event.elapsed_ms}});
  }
  return out;
}

json benchmark_events_json(const std::vector<ps::BenchmarkEvent>& events) {
  json out = json::array();
  for (const auto& event : events) {
    out.push_back({{"node_id", event.node_id},
                   {"op_name", event.op_name},
                   {"source", event.source},
                   {"dependency_duration_ms", event.dependency_duration_ms},
                   {"execution_duration_ms", event.execution_duration_ms},
                   {"thread_id", event.thread_id}});
  }
  return out;
}

std::string dirty_domain_name(ps::compute::DirtyDomain domain) {
  switch (domain) {
    case ps::compute::DirtyDomain::HighPrecision:
      return "high_precision";
    case ps::compute::DirtyDomain::RealTime:
      return "real_time";
  }
  return "unknown";
}

std::string dirty_tile_level_name(ps::compute::DirtyTileLevel level) {
  switch (level) {
    case ps::compute::DirtyTileLevel::Micro:
      return "micro";
    case ps::compute::DirtyTileLevel::Macro:
      return "macro";
  }
  return "unknown";
}

std::string dirty_edge_direction_name(
    ps::compute::DirtyEdgeDirection direction) {
  switch (direction) {
    case ps::compute::DirtyEdgeDirection::ForwardAffected:
      return "forward_affected";
    case ps::compute::DirtyEdgeDirection::BackwardDemand:
      return "backward_demand";
  }
  return "unknown";
}

std::string compute_intent_name(ps::ComputeIntent intent) {
  switch (intent) {
    case ps::ComputeIntent::GlobalHighPrecision:
      return "global_high_precision";
    case ps::ComputeIntent::RealTimeUpdate:
      return "real_time_update";
  }
  return "unknown";
}

std::string planned_task_kind_name(ps::compute::PlannedTaskKind kind) {
  switch (kind) {
    case ps::compute::PlannedTaskKind::Node:
      return "node";
    case ps::compute::PlannedTaskKind::Tile:
      return "tile";
    case ps::compute::PlannedTaskKind::Monolithic:
      return "monolithic";
  }
  return "unknown";
}

std::string propagation_status_name(ps::PropagationContractStatus status) {
  switch (status) {
    case ps::PropagationContractStatus::Explicit:
      return "explicit";
    case ps::PropagationContractStatus::LegacyIdentityFallback:
      return "legacy_identity_fallback";
  }
  return "unknown";
}

json dirty_snapshot_json(
    const std::optional<ps::compute::DirtyRegionSnapshot>& snapshot) {
  if (!snapshot) {
    return json(nullptr);
  }
  json out;
  out["graph_generation"] = snapshot->graph_generation;
  out["dirty_tiles"] = json::array();
  for (const auto& tile : snapshot->dirty_tiles) {
    out["dirty_tiles"].push_back({{"node_id", tile.node_id},
                                  {"domain", dirty_domain_name(tile.domain)},
                                  {"level", dirty_tile_level_name(tile.level)},
                                  {"tile_x", tile.tile_x},
                                  {"tile_y", tile.tile_y},
                                  {"tile_size", tile.tile_size},
                                  {"pixel_roi", rect_json(tile.pixel_roi)}});
  }
  out["dirty_monolithic_nodes"] = json::array();
  for (const auto& region : snapshot->dirty_monolithic_nodes) {
    out["dirty_monolithic_nodes"].push_back(
        {{"node_id", region.node_id},
         {"domain", dirty_domain_name(region.domain)},
         {"pixel_roi", rect_json(region.pixel_roi)},
         {"whole_output", region.whole_output}});
  }
  out["per_node_dirty_rois"] = json::object();
  std::vector<int> node_ids;
  node_ids.reserve(snapshot->per_node_dirty_rois.size());
  for (const auto& [node_id, _] : snapshot->per_node_dirty_rois) {
    node_ids.push_back(node_id);
  }
  std::sort(node_ids.begin(), node_ids.end());
  for (int node_id : node_ids) {
    json rois = json::array();
    for (const auto& roi : snapshot->per_node_dirty_rois.at(node_id)) {
      rois.push_back(rect_json(roi));
    }
    out["per_node_dirty_rois"][std::to_string(node_id)] = rois;
  }
  out["edge_mappings"] = json::array();
  for (const auto& edge : snapshot->edge_mappings) {
    out["edge_mappings"].push_back(
        {{"from_node_id", edge.from_node_id},
         {"to_node_id", edge.to_node_id},
         {"domain", dirty_domain_name(edge.domain)},
         {"from_roi", rect_json(edge.from_roi)},
         {"to_roi", rect_json(edge.to_roi)},
         {"direction", dirty_edge_direction_name(edge.direction)}});
  }
  return out;
}

json dirty_snapshot_history_json(
    const std::vector<ps::compute::DirtyRegionSnapshot>& snapshots) {
  json out = json::array();
  for (const auto& snapshot : snapshots) {
    out.push_back(dirty_snapshot_json(snapshot));
  }
  return out;
}

json compute_plan_json(const std::optional<ps::compute::ComputePlan>& plan) {
  if (!plan) {
    return json(nullptr);
  }
  json planned_work = json::array();
  for (const auto& work : plan->planned_work) {
    planned_work.push_back(
        {{"node_id", work.node_id},
         {"domain", dirty_domain_name(work.domain)},
         {"represented_hp_roi", rect_json(work.represented_hp_roi)},
         {"execution_roi", rect_json(work.execution_roi)},
         {"whole_output", work.whole_output},
         {"dirty_rois", json::array()},
         {"dependency_node_ids", work.dependency_node_ids},
         {"dependent_node_ids", work.dependent_node_ids},
         {"task_ids", work.task_ids}});
    for (const auto& roi : work.dirty_rois) {
      planned_work.back()["dirty_rois"].push_back(rect_json(roi));
    }
  }

  json dependencies = json::array();
  for (const auto& dependency : plan->task_graph.dependencies) {
    dependencies.push_back(
        {{"from_node_id", dependency.from_node_id},
         {"to_node_id", dependency.to_node_id},
         {"domain", dirty_domain_name(dependency.domain)},
         {"input_kind", dependency.input_kind},
         {"from_roi", rect_json(dependency.from_roi)},
         {"to_roi", rect_json(dependency.to_roi)},
         {"direction", dirty_edge_direction_name(dependency.direction)}});
  }

  json tasks = json::array();
  for (const auto& task : plan->task_graph.tasks) {
    tasks.push_back({{"task_id", task.task_id},
                     {"node_id", task.node_id},
                     {"kind", planned_task_kind_name(task.kind)},
                     {"domain", dirty_domain_name(task.domain)},
                     {"output_roi", rect_json(task.output_roi)},
                     {"tile_x", task.tile_x},
                     {"tile_y", task.tile_y},
                     {"tile_size", task.tile_size},
                     {"whole_output", task.whole_output},
                     {"dependency_task_ids", task.dependency_task_ids}});
  }

  return {{"intent", compute_intent_name(plan->intent)},
          {"target_node_id", plan->target_node_id},
          {"parallel", plan->parallel},
          {"execution_order", plan->execution_order},
          {"planned_nodes", plan->planned_nodes},
          {"planned_work", planned_work},
          {"task_graph",
           {{"dependencies", dependencies},
            {"tasks", tasks},
            {"initial_task_ids", plan->task_graph.initial_task_ids}}}};
}

json compute_plan_history_json(
    const std::vector<ps::compute::ComputePlan>& plans) {
  json out = json::array();
  for (const auto& plan : plans) {
    out.push_back(compute_plan_json(plan));
  }
  return out;
}

std::vector<std::string> trace_phase_set(const std::vector<json>& events) {
  std::vector<std::string> phases;
  for (const auto& event : events) {
    const std::string phase = event.value("phase", "");
    if (!phase.empty() &&
        std::find(phases.begin(), phases.end(), phase) == phases.end()) {
      phases.push_back(phase);
    }
  }
  std::sort(phases.begin(), phases.end());
  return phases;
}

bool json_int_array_equals(const json& actual,
                           const std::vector<int>& expected) {
  if (!actual.is_array() || actual.size() != expected.size()) {
    return false;
  }
  for (size_t i = 0; i < expected.size(); ++i) {
    if (!actual[i].is_number_integer() || actual[i].get<int>() != expected[i]) {
      return false;
    }
  }
  return true;
}

bool contains_compute_plan_graph(const json& plans, const std::string& intent,
                                 bool parallel,
                                 const std::vector<int>& planned_nodes,
                                 size_t min_dependencies, size_t min_tasks) {
  if (!plans.is_array()) {
    return false;
  }
  return std::any_of(plans.begin(), plans.end(), [&](const json& plan) {
    if (!plan.contains("planned_nodes") || plan.value("intent", "") != intent ||
        plan.value("parallel", !parallel) != parallel ||
        !json_int_array_equals(plan["planned_nodes"], planned_nodes)) {
      return false;
    }
    if (!plan.contains("planned_work") || !plan["planned_work"].is_array() ||
        plan["planned_work"].size() != planned_nodes.size()) {
      return false;
    }
    if (!plan.contains("task_graph") || !plan["task_graph"].is_object()) {
      return false;
    }
    const json& graph = plan["task_graph"];
    return graph.contains("dependencies") && graph["dependencies"].is_array() &&
           graph["dependencies"].size() >= min_dependencies &&
           graph.contains("tasks") && graph["tasks"].is_array() &&
           graph["tasks"].size() >= min_tasks &&
           graph.contains("initial_task_ids") &&
           graph["initial_task_ids"].is_array() &&
           !graph["initial_task_ids"].empty();
  });
}

bool compute_events_contain_source(const json& events,
                                   const std::string& source) {
  if (!events.is_array()) {
    return false;
  }
  return std::any_of(events.begin(), events.end(), [&](const json& event) {
    return event.value("source", "") == source;
  });
}

bool contains_phase(const std::vector<std::string>& phases,
                    const std::string& phase) {
  return std::find(phases.begin(), phases.end(), phase) != phases.end();
}

json graph_snapshot(ps::Kernel& kernel, const std::string& graph_name) {
  auto& runtime = kernel.runtime(graph_name);
  return runtime
      .post([](ps::GraphModel& graph) {
        json out;
        out["cache_root"] = graph.cache_root.string();
        out["quiet"] = graph.is_quiet();
        out["skip_save_cache"] = graph.skip_save_cache();
        out["last_dirty_region_snapshot_debug"] =
            graph.last_dirty_region_snapshot_debug.value_or("");
        out["last_dirty_region_snapshot"] =
            dirty_snapshot_json(graph.last_dirty_region_snapshot);
        out["recent_dirty_region_snapshots"] =
            dirty_snapshot_history_json(graph.recent_dirty_region_snapshots);
        out["last_compute_plan"] = compute_plan_json(graph.last_compute_plan);
        out["recent_compute_plans"] =
            compute_plan_history_json(graph.recent_compute_plans);
        std::vector<int> ids;
        ids.reserve(graph.nodes.size());
        for (const auto& [id, _] : graph.nodes) {
          ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        out["node_order"] = ids;
        out["nodes"] = json::object();
        for (int id : ids) {
          const ps::Node& node = graph.nodes.at(id);
          json node_json;
          node_json["id"] = node.id;
          node_json["name"] = node.name;
          node_json["type"] = node.type;
          node_json["subtype"] = node.subtype;
          node_json["hp_version"] = node.hp_version;
          node_json["rt_version"] = node.rt_version;
          node_json["hp_roi"] =
              node.hp_roi ? rect_json(*node.hp_roi) : json(nullptr);
          node_json["rt_roi"] =
              node.rt_roi ? rect_json(*node.rt_roi) : json(nullptr);
          node_json["last_input_size_hp"] =
              node.last_input_size_hp ? size_json(*node.last_input_size_hp)
                                      : json(nullptr);
          node_json["parameters_yaml"] = yaml_string(node.parameters);
          node_json["runtime_parameters_yaml"] =
              yaml_string(node.runtime_parameters);
          if (node.runtime_parameters && node.runtime_parameters["gain"]) {
            node_json["runtime_gain"] =
                yaml_float(node.runtime_parameters, "gain", -999.0f);
          }
          node_json["image_inputs"] = json::array();
          for (const auto& input : node.image_inputs) {
            node_json["image_inputs"].push_back(
                {{"from_node_id", input.from_node_id},
                 {"from_output_name", input.from_output_name}});
          }
          node_json["parameter_inputs"] = json::array();
          for (const auto& input : node.parameter_inputs) {
            node_json["parameter_inputs"].push_back(
                {{"from_node_id", input.from_node_id},
                 {"from_output_name", input.from_output_name},
                 {"to_parameter_name", input.to_parameter_name}});
          }
          node_json["cache"] = {
              {"hp", image_stats(node.cached_output_high_precision
                                     ? &*node.cached_output_high_precision
                                     : nullptr)},
              {"rt", image_stats(node.cached_output_real_time
                                     ? &*node.cached_output_real_time
                                     : nullptr)}};
          out["nodes"][std::to_string(id)] = node_json;
        }
        return out;
      })
      .get();
}

ps::NodeOutput make_output(int width, int height, int channels, float value) {
  ps::NodeOutput output;
  output.image_buffer = ps::make_aligned_cpu_image_buffer(
      width, height, channels, ps::DataType::FLOAT32);
  ps::toCvMat(output.image_buffer).setTo(cv::Scalar::all(value));
  output.space.absolute_roi = cv::Rect(0, 0, width, height);
  output.space.global_scale_x = 1.0;
  output.space.global_scale_y = 1.0;
  return output;
}

json tile_inputs_json(const std::vector<ps::Tile>& input_tiles) {
  json inputs = json::array();
  for (size_t i = 0; i < input_tiles.size(); ++i) {
    const auto& tile = input_tiles[i];
    json item = {{"index", i}, {"roi", rect_json(tile.roi)}};
    if (tile.buffer) {
      item["buffer"] = {{"width", tile.buffer->width},
                        {"height", tile.buffer->height},
                        {"channels", tile.buffer->channels},
                        {"type", data_type_name(tile.buffer->type)}};
    }
    inputs.push_back(item);
  }
  return inputs;
}

void register_trace_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto& registry = ps::OpRegistry::instance();

    registry.register_op_hp_monolithic(
        "trace_source", "image",
        ps::MonolithicOpFunc(
            [](const ps::Node& node,
               const std::vector<const ps::NodeOutput*>& inputs) {
              const int width = yaml_int(node.runtime_parameters, "width", 64);
              const int height =
                  yaml_int(node.runtime_parameters, "height", 48);
              const int channels =
                  yaml_int(node.runtime_parameters, "channels", 1);
              const float value =
                  yaml_float(node.runtime_parameters, "value", 0.0f);
              const float gain =
                  yaml_float(node.runtime_parameters, "gain", 0.0f);
              ps::NodeOutput output =
                  make_output(width, height, channels, value);
              output.data["gain"] = YAML::Node(gain);
              g_trace.record({{"kind", "operator_call"},
                              {"op", "trace_source:image"},
                              {"implementation", "hp_monolithic"},
                              {"node_id", node.id},
                              {"node_name", node.name},
                              {"input_count", inputs.size()},
                              {"output", image_stats(&output)},
                              {"data_outputs", {{"gain", gain}}}});
              return output;
            }));

    registry.register_op_hp_monolithic(
        "trace_process", "apply_gain",
        ps::MonolithicOpFunc(
            [](const ps::Node& node,
               const std::vector<const ps::NodeOutput*>& inputs) {
              if (inputs.empty()) {
                throw ps::GraphError(ps::GraphErrc::MissingDependency,
                                     "apply_gain requires an image input");
              }
              const float gain =
                  yaml_float(node.runtime_parameters, "gain", 1.0f);
              const auto& input = inputs.front()->image_buffer;
              ps::NodeOutput output =
                  make_output(input.width, input.height, input.channels, 0.0f);
              cv::Mat src = ps::toCvMat(input);
              cv::Mat dst = ps::toCvMat(output.image_buffer);
              cv::add(src, cv::Scalar::all(gain), dst);
              output.data["applied_gain"] = YAML::Node(gain);
              g_trace.record({{"kind", "operator_call"},
                              {"op", "trace_process:apply_gain"},
                              {"implementation", "hp_monolithic"},
                              {"node_id", node.id},
                              {"runtime_gain", gain},
                              {"input", image_stats(inputs.front())},
                              {"output", image_stats(&output)}});
              return output;
            }));

    ps::OpMetadata gain_rt_meta;
    gain_rt_meta.tile_preference = ps::TileSizePreference::MICRO;
    registry.register_op_rt_tiled(
        "trace_process", "apply_gain",
        ps::TileOpFunc([](const ps::Node& node, const ps::Tile& output_tile,
                          const std::vector<ps::Tile>& input_tiles) {
          if (input_tiles.empty()) {
            throw ps::GraphError(ps::GraphErrc::MissingDependency,
                                 "apply_gain RT requires input tiles");
          }
          const float gain = yaml_float(node.runtime_parameters, "gain", 1.0f);
          cv::Scalar mean = cv::mean(ps::toCvMat(input_tiles.front()));
          cv::Mat dst = ps::toCvMat(output_tile);
          dst.setTo(mean + cv::Scalar::all(gain));
          g_trace.record({{"kind", "operator_call"},
                          {"op", "trace_process:apply_gain"},
                          {"implementation", "rt_tiled"},
                          {"node_id", node.id},
                          {"runtime_gain", gain},
                          {"output_roi", rect_json(output_tile.roi)},
                          {"input_tiles", tile_inputs_json(input_tiles)}});
        }),
        gain_rt_meta);

    registry.register_op_hp_tiled(
        "image_mixing", "trace_mix",
        ps::TileOpFunc([](const ps::Node& node, const ps::Tile& output_tile,
                          const std::vector<ps::Tile>& input_tiles) {
          if (input_tiles.size() < 2) {
            throw ps::GraphError(ps::GraphErrc::MissingDependency,
                                 "trace_mix requires two input tiles");
          }
          cv::Mat a = ps::toCvMat(input_tiles[0]);
          cv::Mat b = ps::toCvMat(input_tiles[1]);
          cv::Mat dst = ps::toCvMat(output_tile);
          cv::addWeighted(a, 0.5, b, 0.5, 0.0, dst);
          g_trace.record(
              {{"kind", "operator_call"},
               {"op", "image_mixing:trace_mix"},
               {"implementation", "hp_tiled"},
               {"node_id", node.id},
               {"merge_strategy", yaml_string(node.runtime_parameters)},
               {"output_roi", rect_json(output_tile.roi)},
               {"input_tiles", tile_inputs_json(input_tiles)}});
        }),
        ps::OpMetadata{});

    ps::OpMetadata random_meta;
    random_meta.tile_preference = ps::TileSizePreference::MICRO;
    random_meta.access_pattern =
        ps::OpMetadata::InputAccessPattern::RandomAccess;
    registry.register_op_hp_tiled(
        "trace_process", "random_roi",
        ps::TileOpFunc([](const ps::Node& node, const ps::Tile& output_tile,
                          const std::vector<ps::Tile>& input_tiles) {
          if (input_tiles.empty()) {
            throw ps::GraphError(ps::GraphErrc::MissingDependency,
                                 "random_roi requires input tiles");
          }
          const float offset =
              yaml_float(node.runtime_parameters, "offset", 1.0f);
          cv::Scalar mean = cv::mean(ps::toCvMat(input_tiles.front()));
          cv::Mat dst = ps::toCvMat(output_tile);
          dst.setTo(mean + cv::Scalar::all(offset));
          g_trace.record({{"kind", "operator_call"},
                          {"op", "trace_process:random_roi"},
                          {"implementation", "hp_tiled"},
                          {"node_id", node.id},
                          {"output_roi", rect_json(output_tile.roi)},
                          {"input_tiles", tile_inputs_json(input_tiles)}});
        }),
        random_meta);
    registry.register_op_rt_tiled(
        "trace_process", "random_roi",
        ps::TileOpFunc([](const ps::Node& node, const ps::Tile& output_tile,
                          const std::vector<ps::Tile>& input_tiles) {
          if (input_tiles.empty()) {
            throw ps::GraphError(ps::GraphErrc::MissingDependency,
                                 "random_roi RT requires input tiles");
          }
          const float offset =
              yaml_float(node.runtime_parameters, "offset", 1.0f);
          cv::Scalar mean = cv::mean(ps::toCvMat(input_tiles.front()));
          cv::Mat dst = ps::toCvMat(output_tile);
          dst.setTo(mean + cv::Scalar::all(offset));
          g_trace.record({{"kind", "operator_call"},
                          {"op", "trace_process:random_roi"},
                          {"implementation", "rt_tiled"},
                          {"node_id", node.id},
                          {"output_roi", rect_json(output_tile.roi)},
                          {"input_tiles", tile_inputs_json(input_tiles)}});
        }),
        random_meta);

    registry.register_op_hp_monolithic(
        "trace_process", "legacy_identity",
        ps::MonolithicOpFunc(
            [](const ps::Node& node,
               const std::vector<const ps::NodeOutput*>& inputs) {
              if (inputs.empty()) {
                throw ps::GraphError(ps::GraphErrc::MissingDependency,
                                     "legacy_identity requires input");
              }
              ps::NodeOutput output = *inputs.front();
              g_trace.record({{"kind", "operator_call"},
                              {"op", "trace_process:legacy_identity"},
                              {"implementation", "hp_monolithic"},
                              {"node_id", node.id},
                              {"input", image_stats(inputs.front())},
                              {"output", image_stats(&output)}});
              return output;
            }));

    registry.register_op_hp_monolithic(
        "trace_process", "explode",
        ps::MonolithicOpFunc(
            [](const ps::Node& node,
               const std::vector<const ps::NodeOutput*>& inputs) {
              g_trace.record({{"kind", "operator_call"},
                              {"op", "trace_process:explode"},
                              {"implementation", "hp_monolithic"},
                              {"node_id", node.id},
                              {"input_count", inputs.size()},
                              {"will_throw", true}});
              throw std::runtime_error("trace explode failure");
              return ps::NodeOutput{};
            }));

    auto apply_dirty = ps::DirtyRoiPropFunc(
        [](const ps::Node& node, const cv::Rect& roi, const ps::GraphModel&) {
          const int radius = yaml_int(node.runtime_parameters, "radius", 2);
          cv::Rect out = ps::compute::expand_rect(roi, std::max(0, radius));
          g_trace.record({{"kind", "dirty_propagator"},
                          {"op", "trace_process:apply_gain"},
                          {"node_id", node.id},
                          {"input_roi", rect_json(roi)},
                          {"output_roi", rect_json(out)}});
          return out;
        });
    auto apply_forward = ps::ForwardRoiPropFunc(
        [](const ps::Node& node, const cv::Rect& roi, const ps::GraphModel&,
           const cv::Size& parent_size, const cv::Size& child_size) {
          cv::Rect out =
              roi & cv::Rect(0, 0, child_size.width, child_size.height);
          g_trace.record({{"kind", "forward_propagator"},
                          {"op", "trace_process:apply_gain"},
                          {"node_id", node.id},
                          {"parent_size", size_json(parent_size)},
                          {"child_size", size_json(child_size)},
                          {"input_roi", rect_json(roi)},
                          {"output_roi", rect_json(out)}});
          return out;
        });
    registry.register_dirty_propagator("trace_process", "apply_gain",
                                       apply_dirty);
    registry.register_forward_propagator("trace_process", "apply_gain",
                                         apply_forward);

    registry.register_dirty_propagator(
        "image_mixing", "trace_mix",
        ps::DirtyRoiPropFunc([](const ps::Node& node, const cv::Rect& roi,
                                const ps::GraphModel&) {
          g_trace.record({{"kind", "dirty_propagator"},
                          {"op", "image_mixing:trace_mix"},
                          {"node_id", node.id},
                          {"input_roi", rect_json(roi)},
                          {"output_roi", rect_json(roi)}});
          return roi;
        }));
    registry.register_forward_propagator(
        "image_mixing", "trace_mix",
        ps::ForwardRoiPropFunc(
            [](const ps::Node& node, const cv::Rect& roi, const ps::GraphModel&,
               const cv::Size& parent_size, const cv::Size& child_size) {
              cv::Rect out =
                  roi & cv::Rect(0, 0, child_size.width, child_size.height);
              g_trace.record({{"kind", "forward_propagator"},
                              {"op", "image_mixing:trace_mix"},
                              {"node_id", node.id},
                              {"parent_size", size_json(parent_size)},
                              {"child_size", size_json(child_size)},
                              {"input_roi", rect_json(roi)},
                              {"output_roi", rect_json(out)}});
              return out;
            }));

    registry.register_dirty_propagator(
        "trace_process", "random_roi",
        ps::DirtyRoiPropFunc([](const ps::Node& node, const cv::Rect& roi,
                                const ps::GraphModel&) {
          const int radius = yaml_int(node.runtime_parameters, "radius", 3);
          cv::Rect out = ps::compute::expand_rect(roi, std::max(0, radius));
          g_trace.record({{"kind", "dirty_propagator"},
                          {"op", "trace_process:random_roi"},
                          {"node_id", node.id},
                          {"input_roi", rect_json(roi)},
                          {"output_roi", rect_json(out)}});
          return out;
        }));
    registry.register_forward_propagator(
        "trace_process", "random_roi",
        ps::ForwardRoiPropFunc(
            [](const ps::Node& node, const cv::Rect& roi, const ps::GraphModel&,
               const cv::Size& parent_size, const cv::Size& child_size) {
              cv::Rect out =
                  roi & cv::Rect(0, 0, child_size.width, child_size.height);
              g_trace.record({{"kind", "forward_propagator"},
                              {"op", "trace_process:random_roi"},
                              {"node_id", node.id},
                              {"parent_size", size_json(parent_size)},
                              {"child_size", size_json(child_size)},
                              {"input_roi", rect_json(roi)},
                              {"output_roi", rect_json(out)}});
              return out;
            }));
  });
}

std::string full_graph_yaml() {
  return R"yaml(
- id: 1
  name: trace_source_main
  type: trace_source
  subtype: image
  parameters:
    width: 64
    height: 48
    channels: 3
    value: 10
    gain: 5
  caches:
    - cache_type: image
      location: source_main.png
- id: 2
  name: trace_apply_gain
  type: trace_process
  subtype: apply_gain
  image_inputs:
    - from_node_id: 1
  parameter_inputs:
    - from_node_id: 1
      from_output_name: gain
      to_parameter_name: gain
  parameters:
    gain: 1
    radius: 2
  caches:
    - cache_type: image
      location: apply_gain.png
- id: 4
  name: trace_secondary_smaller
  type: trace_source
  subtype: image
  parameters:
    width: 32
    height: 24
    channels: 1
    value: 2
    gain: 0
  caches:
    - cache_type: image
      location: secondary.png
- id: 30
  name: trace_mixing_normalization
  type: image_mixing
  subtype: trace_mix
  image_inputs:
    - from_node_id: 2
    - from_node_id: 4
  parameters:
    merge_strategy: resize
  caches:
    - cache_type: image
      location: mix.png
- id: 100
  name: trace_random_final
  type: trace_process
  subtype: random_roi
  image_inputs:
    - from_node_id: 30
  parameters:
    offset: 1
    radius: 3
  caches:
    - cache_type: image
      location: final.png
)yaml";
}

std::string dirty_graph_yaml() {
  return R"yaml(
- id: 1
  name: dirty_source
  type: trace_source
  subtype: image
  parameters:
    width: 128
    height: 96
    channels: 1
    value: 10
    gain: 5
  caches:
    - cache_type: image
      location: dirty_source.png
- id: 2
  name: dirty_apply_gain_monolithic
  type: trace_process
  subtype: apply_gain
  image_inputs:
    - from_node_id: 1
  parameter_inputs:
    - from_node_id: 1
      from_output_name: gain
      to_parameter_name: gain
  parameters:
    gain: 1
    radius: 2
  caches:
    - cache_type: image
      location: dirty_gain.png
- id: 100
  name: dirty_random_final
  type: trace_process
  subtype: random_roi
  image_inputs:
    - from_node_id: 2
  parameters:
    offset: 1
    radius: 3
  caches:
    - cache_type: image
      location: dirty_final.png
)yaml";
}

std::string legacy_graph_yaml() {
  return R"yaml(
- id: 1
  name: legacy_source
  type: trace_source
  subtype: image
  parameters:
    width: 40
    height: 30
    channels: 1
    value: 1
- id: 20
  name: legacy_identity_no_propagator
  type: trace_process
  subtype: legacy_identity
  image_inputs:
    - from_node_id: 1
)yaml";
}

std::string error_graph_yaml() {
  return R"yaml(
- id: 1
  name: error_source
  type: trace_source
  subtype: image
  parameters:
    width: 16
    height: 12
    channels: 1
    value: 3
- id: 100
  name: error_explode
  type: trace_process
  subtype: explode
  image_inputs:
    - from_node_id: 1
)yaml";
}

struct CheckSet {
  std::vector<json> checks;

  void add(const std::string& name, const json& expected, const json& actual,
           bool ok, const std::string& why) {
    checks.push_back({{"name", name},
                      {"expected", expected},
                      {"actual", actual},
                      {"ok", ok},
                      {"why", why}});
  }

  bool ok() const {
    return std::all_of(checks.begin(), checks.end(), [](const json& check) {
      return check["ok"].get<bool>();
    });
  }

  std::string log() const {
    std::ostringstream os;
    for (const auto& check : checks) {
      os << (check["ok"].get<bool>() ? "OK" : "FAIL") << ": "
         << check["name"].get<std::string>() << "\n"
         << "  expected: " << check["expected"].dump() << "\n"
         << "  actual:   " << check["actual"].dump() << "\n"
         << "  why:      " << check["why"].get<std::string>() << "\n";
    }
    return os.str();
  }
};

double node_checksum(const json& snapshot, int node_id,
                     const std::string& cache_name) {
  const auto& node = snapshot["nodes"].at(std::to_string(node_id));
  const auto& cache = node["cache"].at(cache_name);
  if (!cache.value("present", false)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return cache.value("checksum", std::numeric_limits<double>::quiet_NaN());
}

int count_scheduler_action(const json& events, const std::string& action) {
  int count = 0;
  for (const auto& event : events) {
    if (event.value("action", "") == action) {
      ++count;
    }
  }
  return count;
}

bool has_operator_event(const std::vector<json>& events,
                        const std::string& phase, const std::string& op,
                        const std::string& implementation) {
  return std::any_of(events.begin(), events.end(), [&](const json& event) {
    return event.value("phase", "") == phase && event.value("op", "") == op &&
           event.value("implementation", "") == implementation;
  });
}

bool has_normalized_mix_event(const std::vector<json>& events) {
  for (const auto& event : events) {
    if (event.value("op", "") != "image_mixing:trace_mix" ||
        event.value("implementation", "") != "hp_tiled") {
      continue;
    }
    if (!event.contains("input_tiles") || event["input_tiles"].size() < 2) {
      continue;
    }
    const auto& buffer = event["input_tiles"][1]["buffer"];
    if (buffer.value("width", 0) == 64 && buffer.value("height", 0) == 48 &&
        buffer.value("channels", 0) == 3) {
      return true;
    }
  }
  return false;
}

bool has_expanded_random_access_event(const std::vector<json>& events) {
  for (const auto& event : events) {
    if (event.value("op", "") != "trace_process:random_roi" ||
        event.value("implementation", "") != "hp_tiled") {
      continue;
    }
    if (!event.contains("input_tiles") || event["input_tiles"].empty() ||
        !event.contains("output_roi")) {
      continue;
    }
    auto output = event["output_roi"];
    auto input = event["input_tiles"][0]["roi"];
    if (input.value("width", 0) > output.value("width", 0) ||
        input.value("height", 0) > output.value("height", 0) ||
        input.value("x", 0) < output.value("x", 0) ||
        input.value("y", 0) < output.value("y", 0)) {
      return true;
    }
  }
  return false;
}

void write_task_bundle(const fs::path& root, const std::string& task_dir,
                       const std::string& title, const std::string& spec,
                       const std::string& command,
                       const std::string& input_summary, const json& expected,
                       const json& actual, const CheckSet& checks,
                       const std::vector<json>& trace_rows) {
  fs::path dir = root / task_dir;
  fs::create_directories(dir);
  write_json(dir / "expected.json", expected);
  write_json(dir / "actual.json", actual);
  write_jsonl(dir / "trace.jsonl", trace_rows);
  write_text(dir / "compare.log", checks.log());

  std::ostringstream readme;
  readme << "# " << title << "\n\n"
         << "Task: " << task_dir << "\n\n"
         << "Spec summary: " << spec << "\n\n"
         << "Command: `" << command << "`\n\n"
         << "Input summary: " << input_summary << "\n\n"
         << "Artifacts:\n"
         << "- `expected.json`: expected runtime conditions.\n"
         << "- `actual.json`: observed kernel state, events, and outputs.\n"
         << "- `trace.jsonl`: structured execution events.\n"
         << "- `compare.log`: expected/actual comparison.\n\n"
         << "Conclusion: " << (checks.ok() ? "accepted" : "rejected")
         << ". The conclusion is based on runtime state and trace data, not on "
            "test-framework pass/fail status.\n";
  write_text(dir / "README.md", readme.str());
}

void mutate_dirty_region(ps::Kernel& kernel, const std::string& graph_name,
                         const cv::Rect& dirty_roi) {
  kernel.runtime(graph_name)
      .post([dirty_roi](ps::GraphModel& graph) {
        auto& source = graph.nodes.at(1);
        if (source.cached_output_high_precision) {
          cv::Mat mat =
              ps::toCvMat(source.cached_output_high_precision->image_buffer);
          mat(dirty_roi).setTo(cv::Scalar::all(33.0f));
        }
        for (int id : {2, 100}) {
          graph.nodes.at(id).cached_output_high_precision.reset();
          graph.nodes.at(id).cached_output_real_time.reset();
          graph.nodes.at(id).hp_roi.reset();
          graph.nodes.at(id).rt_roi.reset();
        }
        return 0;
      })
      .get();
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path root = argc > 1
                            ? fs::path(argv[1])
                            : fs::path("tests/results/split-compute-service");
  const std::string command = command_line(argc, argv);
  fs::create_directories(root / "full-kernel-run" / "input");
  fs::create_directories(root / "full-kernel-run" / "actual");
  fs::create_directories(root / "full-kernel-run" / "expected");

  std::ofstream run_log(root / "full-kernel-run" / "run.log");
  auto log = [&](const std::string& line) {
    std::cout << line << std::endl;
    run_log << line << "\n";
  };

  try {
    log("split-compute-service runtime trace started");
    log("command: " + command);
    register_trace_ops();

    const fs::path input_dir = root / "full-kernel-run" / "input";
    const fs::path session_root = root / "full-kernel-run" / "sessions";
    write_text(input_dir / "kernel_trace_graph.yaml", full_graph_yaml());
    write_text(input_dir / "dirty_region_graph.yaml", dirty_graph_yaml());
    write_text(input_dir / "legacy_identity_graph.yaml", legacy_graph_yaml());
    write_text(input_dir / "error_graph.yaml", error_graph_yaml());

    ps::Kernel kernel;
    ps::InteractionService svc(kernel);
    svc.cmd_seed_builtin_ops();

    const std::string full_graph = "split_trace_full";
    const std::string dirty_graph = "split_trace_dirty";
    const std::string legacy_graph = "split_trace_legacy";
    const std::string error_graph = "split_trace_error";

    auto loaded_full =
        svc.cmd_load_graph(full_graph, session_root.string(),
                           (input_dir / "kernel_trace_graph.yaml").string());
    auto loaded_dirty =
        svc.cmd_load_graph(dirty_graph, session_root.string(),
                           (input_dir / "dirty_region_graph.yaml").string());
    auto loaded_legacy =
        svc.cmd_load_graph(legacy_graph, session_root.string(),
                           (input_dir / "legacy_identity_graph.yaml").string());
    auto loaded_error =
        svc.cmd_load_graph(error_graph, session_root.string(),
                           (input_dir / "error_graph.yaml").string());
    log("loaded graphs: full=" + std::to_string(loaded_full.has_value()) +
        " dirty=" + std::to_string(loaded_dirty.has_value()) +
        " legacy=" + std::to_string(loaded_legacy.has_value()) +
        " error=" + std::to_string(loaded_error.has_value()));

    json before = {
        {"graphs", svc.cmd_list_graphs()},
        {"full_node_ids",
         svc.cmd_list_node_ids(full_graph).value_or(std::vector<int>{})},
        {"dirty_node_ids",
         svc.cmd_list_node_ids(dirty_graph).value_or(std::vector<int>{})},
        {"full_endings",
         svc.cmd_ending_nodes(full_graph).value_or(std::vector<int>{})},
        {"full_traversal_orders",
         svc.cmd_traversal_orders(full_graph)
             .value_or(std::map<int, std::vector<int>>{})},
        {"full_graph_inspect", svc.cmd_inspect_graph(full_graph).value_or("")},
        {"full_tree",
         svc.cmd_dump_tree(full_graph, 100, true, true).value_or("")}};
    write_json(root / "full-kernel-run" / "actual" / "before_compute.json",
               before);

    std::vector<ps::BenchmarkEvent> sequential_benchmark;
    g_trace.set_phase("sequential_hp");
    kernel.runtime(full_graph).clear_scheduler_log();
    const bool sequential_ok =
        svc.cmd_compute(full_graph, 100, "int8", true, true, false, false, true,
                        true, &sequential_benchmark);
    log("sequential compute ok=" + std::to_string(sequential_ok));
    json sequential_snapshot = graph_snapshot(kernel, full_graph);
    json sequential_actual = {
        {"ok", sequential_ok},
        {"graph_snapshot", sequential_snapshot},
        {"compute_events",
         compute_events_json(svc.cmd_drain_compute_events(full_graph))},
        {"scheduler_events",
         scheduler_events_json(kernel.runtime(full_graph).get_scheduler_log())},
        {"benchmark_events", benchmark_events_json(sequential_benchmark)}};
    write_json(root / "full-kernel-run" / "actual" / "sequential_hp.json",
               sequential_actual);

    std::vector<ps::BenchmarkEvent> parallel_benchmark;
    g_trace.set_phase("parallel_hp");
    kernel.runtime(full_graph).clear_scheduler_log();
    const bool parallel_ok =
        svc.cmd_compute(full_graph, 100, "int8", true, true, true, false, true,
                        true, &parallel_benchmark);
    log("parallel compute ok=" + std::to_string(parallel_ok));
    json parallel_scheduler =
        scheduler_events_json(kernel.runtime(full_graph).get_scheduler_log());
    json parallel_snapshot = graph_snapshot(kernel, full_graph);
    json parallel_actual = {
        {"ok", parallel_ok},
        {"graph_snapshot", parallel_snapshot},
        {"compute_events",
         compute_events_json(svc.cmd_drain_compute_events(full_graph))},
        {"scheduler_events", parallel_scheduler},
        {"benchmark_events", benchmark_events_json(parallel_benchmark)}};
    write_json(root / "full-kernel-run" / "actual" / "parallel_hp.json",
               parallel_actual);

    g_trace.set_phase("legacy_identity_fallback");
    std::vector<ps::BenchmarkEvent> legacy_benchmark;
    const bool legacy_ok =
        svc.cmd_compute(legacy_graph, 20, "int8", true, true, false, false,
                        true, true, &legacy_benchmark);
    auto& registry = ps::OpRegistry::instance();
    auto legacy_forward =
        svc.cmd_project_roi(legacy_graph, 1, cv::Rect(3, 4, 5, 6), 20);
    json legacy_actual = {
        {"ok", legacy_ok},
        {"dirty_propagation_contract_status",
         propagation_status_name(registry.dirty_propagation_contract_status(
             "trace_process", "legacy_identity"))},
        {"forward_propagation_contract_status",
         propagation_status_name(registry.forward_propagation_contract_status(
             "trace_process", "legacy_identity"))},
        {"forward_projected_roi",
         legacy_forward ? rect_json(*legacy_forward) : json(nullptr)},
        {"graph_snapshot", graph_snapshot(kernel, legacy_graph)},
        {"benchmark_events", benchmark_events_json(legacy_benchmark)}};
    write_json(root / "full-kernel-run" / "actual" / "legacy_identity.json",
               legacy_actual);

    std::vector<ps::BenchmarkEvent> dirty_bootstrap_benchmark;
    g_trace.set_phase("dirty_bootstrap_hp");
    const bool dirty_bootstrap_ok =
        svc.cmd_compute(dirty_graph, 100, "int8", true, true, true, false, true,
                        true, &dirty_bootstrap_benchmark);
    log("dirty bootstrap hp ok=" + std::to_string(dirty_bootstrap_ok));
    const cv::Rect dirty_roi(16, 20, 18, 12);
    mutate_dirty_region(kernel, dirty_graph, dirty_roi);

    std::vector<ps::BenchmarkEvent> dirty_benchmark;
    g_trace.set_phase("dirty_rt_update");
    kernel.runtime(dirty_graph).clear_scheduler_log();
    auto dirty_future = kernel.compute_async(
        dirty_graph, 100, "int8", true, true, true, false, true, true,
        &dirty_benchmark, ps::ComputeIntent::RealTimeUpdate, dirty_roi);
    const bool dirty_ok = dirty_future && dirty_future->get();
    log("dirty rt update ok=" + std::to_string(dirty_ok));
    auto dirty_snapshot_text =
        svc.cmd_dirty_region_snapshot_debug(dirty_graph).value_or("");
    auto dirty_snapshot_struct = svc.cmd_dirty_region_snapshot(dirty_graph);
    auto dirty_forward = svc.cmd_project_roi(dirty_graph, 1, dirty_roi, 100);
    auto dirty_backward =
        svc.cmd_project_roi_backward(dirty_graph, 100, dirty_roi, 1);
    json dirty_scheduler =
        scheduler_events_json(kernel.runtime(dirty_graph).get_scheduler_log());
    json dirty_actual = {
        {"bootstrap_ok", dirty_bootstrap_ok},
        {"requested_parallel", true},
        {"dirty_update_ok", dirty_ok},
        {"dirty_roi", rect_json(dirty_roi)},
        {"interaction_dirty_snapshot_debug", dirty_snapshot_text},
        {"interaction_dirty_snapshot",
         dirty_snapshot_json(dirty_snapshot_struct)},
        {"project_forward",
         dirty_forward ? rect_json(*dirty_forward) : json(nullptr)},
        {"project_backward",
         dirty_backward ? rect_json(*dirty_backward) : json(nullptr)},
        {"graph_snapshot", graph_snapshot(kernel, dirty_graph)},
        {"scheduler_events", dirty_scheduler},
        {"compute_events",
         compute_events_json(svc.cmd_drain_compute_events(dirty_graph))},
        {"benchmark_events", benchmark_events_json(dirty_benchmark)}};
    write_json(root / "full-kernel-run" / "actual" / "dirty_rt_update.json",
               dirty_actual);

    const cv::Rect dirty_single_thread_roi(24, 12, 16, 16);
    mutate_dirty_region(kernel, dirty_graph, dirty_single_thread_roi);

    std::vector<ps::BenchmarkEvent> dirty_single_thread_benchmark;
    g_trace.set_phase("dirty_rt_update_single_thread");
    kernel.runtime(dirty_graph).clear_scheduler_log();
    auto dirty_single_thread_future = kernel.compute_async(
        dirty_graph, 100, "int8", true, true, false, false, true, true,
        &dirty_single_thread_benchmark, ps::ComputeIntent::RealTimeUpdate,
        dirty_single_thread_roi);
    const bool dirty_single_thread_ok =
        dirty_single_thread_future && dirty_single_thread_future->get();
    log("dirty rt update single-thread ok=" +
        std::to_string(dirty_single_thread_ok));
    auto dirty_single_thread_snapshot_text =
        svc.cmd_dirty_region_snapshot_debug(dirty_graph).value_or("");
    auto dirty_single_thread_snapshot_struct =
        svc.cmd_dirty_region_snapshot(dirty_graph);
    json dirty_single_thread_actual = {
        {"requested_parallel", false},
        {"dirty_update_ok", dirty_single_thread_ok},
        {"dirty_roi", rect_json(dirty_single_thread_roi)},
        {"interaction_dirty_snapshot_debug", dirty_single_thread_snapshot_text},
        {"interaction_dirty_snapshot",
         dirty_snapshot_json(dirty_single_thread_snapshot_struct)},
        {"graph_snapshot", graph_snapshot(kernel, dirty_graph)},
        {"scheduler_events",
         scheduler_events_json(
             kernel.runtime(dirty_graph).get_scheduler_log())},
        {"compute_events",
         compute_events_json(svc.cmd_drain_compute_events(dirty_graph))},
        {"benchmark_events",
         benchmark_events_json(dirty_single_thread_benchmark)}};
    write_json(root / "full-kernel-run" / "actual" /
                   "dirty_rt_update_single_thread.json",
               dirty_single_thread_actual);

    g_trace.set_phase("parallel_error_path");
    std::vector<ps::BenchmarkEvent> error_benchmark;
    kernel.runtime(error_graph).clear_scheduler_log();
    const bool error_ok =
        svc.cmd_compute(error_graph, 100, "int8", true, true, true, false, true,
                        true, &error_benchmark);
    auto last_error = svc.cmd_last_error(error_graph);
    json error_actual = {
        {"compute_returned_ok", error_ok},
        {"last_error", last_error
                           ? json{{"code", static_cast<int>(last_error->code)},
                                  {"message", last_error->message}}
                           : json(nullptr)},
        {"scheduler_events",
         scheduler_events_json(
             kernel.runtime(error_graph).get_scheduler_log())},
        {"benchmark_events", benchmark_events_json(error_benchmark)}};
    write_json(root / "full-kernel-run" / "actual" / "error_path.json",
               error_actual);

    std::vector<json> operator_trace = g_trace.snapshot();
    const std::vector<std::string> observed_phases =
        trace_phase_set(operator_trace);
    write_jsonl(root / "full-kernel-run" / "trace.jsonl", operator_trace);
    write_json(root / "full-kernel-run" / "actual" / "operator_trace.json",
               operator_trace);

    const double expected_final_checksum = 64.0 * 48.0 * 3.0 * 9.5;
    json expected_common = {
        {"full_graph_final",
         {{"width", 64},
          {"height", 48},
          {"channels", 3},
          {"mean", 9.5},
          {"checksum", expected_final_checksum}}},
        {"dirty_snapshot_contains", {"generation=", "tiles=", "edges="}},
        {"legacy_identity_forward_roi", rect_json(cv::Rect(3, 4, 5, 6))}};
    write_json(root / "full-kernel-run" / "expected" / "runtime_expected.json",
               expected_common);

    CheckSet task2;
    const bool full_graph_input_exists =
        fs::exists(input_dir / "kernel_trace_graph.yaml");
    const bool sequential_state_exists =
        fs::exists(root / "full-kernel-run" / "actual" / "sequential_hp.json");
    task2.add("kernel trace testbench produced full graph input", true,
              full_graph_input_exists, full_graph_input_exists,
              "input graph was generated and loaded through Kernel");
    task2.add("operator trace captures runtime calls", "non_empty",
              operator_trace.size(), !operator_trace.empty(),
              "trace.jsonl records op calls and propagator calls");
    task2.add("full run writes actual state dumps", true,
              sequential_state_exists, sequential_state_exists,
              "kernel state dumps exist for review");
    json task2_actual = {
        {"coverage_matrix",
         {{"task_3", "geometry/cache/metadata observed in task-03"},
          {"task_4", "input resolver/node executor observed in task-04"},
          {"task_5",
           "dirty planner/interaction/task planner observed in task-05"},
          {"task_6", "parallel executor/scheduler/error observed in task-06"},
          {"task_7", "facade/build integration observed in task-07"}}},
        {"trace_event_count", operator_trace.size()}};
    write_task_bundle(root, "task-02", "Task 2 runtime validation plan",
                      "Focused tests must be identified and must emit runtime "
                      "process evidence instead of only pass/fail.",
                      command, "Generated kernel graphs and trace testbench.",
                      {{"expected_trace", "non_empty"}}, task2_actual, task2,
                      operator_trace);

    CheckSet task3;
    json geometry_actual = {
        {"clip", rect_json(ps::compute::clip_rect(cv::Rect(-5, 2, 12, 10),
                                                  cv::Size(10, 8)))},
        {"align",
         rect_json(ps::compute::align_rect(cv::Rect(5, 6, 10, 11), 8))},
        {"merge", rect_json(ps::compute::merge_rect(cv::Rect(2, 3, 4, 5),
                                                    cv::Rect(10, 1, 2, 4)))},
        {"scale_down_size",
         size_json(ps::compute::scale_down_size(cv::Size(65, 33), 4))},
        {"halo", rect_json(ps::compute::calculate_halo(cv::Rect(4, 4, 8, 8), 3,
                                                       cv::Size(14, 20)))}};
    task3.add("geometry clip helper", rect_json(cv::Rect(0, 2, 7, 6)),
              geometry_actual["clip"],
              geometry_actual["clip"] == rect_json(cv::Rect(0, 2, 7, 6)),
              "helper output is recorded alongside expected ROI");
    task3.add("HP cache present after compute", true,
              sequential_snapshot["nodes"]["100"]["cache"]["hp"]["present"],
              sequential_snapshot["nodes"]["100"]["cache"]["hp"]["present"]
                  .get<bool>(),
              "formal HP cache is populated by kernel compute");
    task3.add(
        "RT cache coexists without replacing HP", true,
        dirty_actual["graph_snapshot"]["nodes"]["100"]["cache"]["hp"]
                    ["present"],
        dirty_actual["graph_snapshot"]["nodes"]["100"]["cache"]["hp"]["present"]
                .get<bool>() &&
            dirty_actual["graph_snapshot"]["nodes"]["100"]["cache"]["rt"]
                        ["present"]
                            .get<bool>(),
        "dirty RT update leaves HP cache present and records RT separately");
    json task3_actual = {{"geometry", geometry_actual},
                         {"sequential_snapshot", sequential_snapshot},
                         {"dirty_snapshot", dirty_actual["graph_snapshot"]}};
    json task3_expected = {{"geometry_clip", rect_json(cv::Rect(0, 2, 7, 6))},
                           {"hp_cache_present", true},
                           {"rt_cache_coexists_with_hp", true}};
    write_task_bundle(
        root, "task-03", "Task 3 utility/cache runtime evidence",
        "Geometry, metrics, and cache policy helpers must preserve "
        "HP authority and RT non-authority.",
        command, "Full and dirty kernel graphs.", task3_expected, task3_actual,
        task3, operator_trace);

    CheckSet task4;
    task4.add(
        "parameter input transferred to runtime gain", 5.0,
        sequential_snapshot["nodes"]["2"].value("runtime_gain", -1.0),
        std::abs(sequential_snapshot["nodes"]["2"].value("runtime_gain", -1.0) -
                 5.0) < 0.001,
        "NodeInputResolver copied source data output into runtime parameters");
    task4.add("last_input_size_hp recorded", size_json(cv::Size(64, 48)),
              sequential_snapshot["nodes"]["2"]["last_input_size_hp"],
              sequential_snapshot["nodes"]["2"]["last_input_size_hp"] ==
                  size_json(cv::Size(64, 48)),
              "resolver captured HP input extent during real compute");
    task4.add("image_mixing input normalized", true,
              has_normalized_mix_event(operator_trace),
              has_normalized_mix_event(operator_trace),
              "trace_mix saw resized/channel-expanded second input at 64x48x3");
    task4.add("random-access ROI expanded", true,
              has_expanded_random_access_event(operator_trace),
              has_expanded_random_access_event(operator_trace),
              "random access tile input ROI is larger than output ROI");
    task4.add(
        "node exception wrapped through kernel", false, error_ok,
        !error_ok && last_error.has_value() &&
            last_error->message.find("trace explode failure") !=
                std::string::npos,
        "parallel error graph returns false and exposes wrapped last_error");
    json task4_actual = {{"sequential_snapshot", sequential_snapshot},
                         {"error_path", error_actual},
                         {"operator_trace_count", operator_trace.size()}};
    json task4_expected = {{"runtime_gain", 5.0},
                           {"last_input_size_hp", size_json(cv::Size(64, 48))},
                           {"image_mixing_input_normalized", true},
                           {"random_access_roi_expanded", true},
                           {"parallel_error_returns_ok", false}};
    write_task_bundle(
        root, "task-04", "Task 4 input/execution runtime evidence",
        "Shared input resolution and node execution must be visible "
        "in kernel execution, including normalization and errors.",
        command, "Full graph plus error graph.", task4_expected, task4_actual,
        task4, operator_trace);

    CheckSet task5;
    const std::string dirty_text =
        dirty_actual.value("interaction_dirty_snapshot_debug", "");
    const json& dirty_struct = dirty_actual["interaction_dirty_snapshot"];
    const bool snapshot_has_shape =
        dirty_text.find("generation=") != std::string::npos &&
        dirty_text.find("tiles=") != std::string::npos &&
        dirty_text.find("edges=") != std::string::npos;
    const bool dirty_detail_ok =
        dirty_struct.is_object() && dirty_struct.contains("dirty_tiles") &&
        dirty_struct.contains("dirty_monolithic_nodes") &&
        dirty_struct.contains("per_node_dirty_rois") &&
        dirty_struct.contains("edge_mappings") &&
        dirty_struct.value("graph_generation", 0) >= 1 &&
        dirty_struct["dirty_tiles"].size() == 1 &&
        dirty_struct["dirty_monolithic_nodes"].size() == 2 &&
        dirty_struct["per_node_dirty_rois"].size() == 3 &&
        dirty_struct["edge_mappings"].size() == 2;
    task5.add("InteractionService exposes dirty snapshot", true,
              snapshot_has_shape, snapshot_has_shape,
              "frontend facade read graph-scoped dirty snapshot summary");
    task5.add("InteractionService exposes dirty snapshot details", true,
              dirty_detail_ok, dirty_detail_ok,
              "structured snapshot exposes stable node ids, tile keys, ROIs, "
              "monolithic escalation, and edge mappings");
    task5.add(
        "dirty RT update succeeded", true, dirty_actual["dirty_update_ok"],
        dirty_actual["dirty_update_ok"].get<bool>(),
        "Kernel intent async path executed RealTimeUpdate with dirty ROI");
    task5.add(
        "explicit propagators ran", true,
        has_operator_event(operator_trace, "dirty_rt_update",
                           "trace_process:random_roi", "") ||
            std::any_of(operator_trace.begin(), operator_trace.end(),
                        [](const json& event) {
                          return event.value("phase", "") ==
                                     "dirty_rt_update" &&
                                 event.value("kind", "") == "dirty_propagator";
                        }),
        std::any_of(operator_trace.begin(), operator_trace.end(),
                    [](const json& event) {
                      return event.value("phase", "") == "dirty_rt_update" &&
                             event.value("kind", "") == "dirty_propagator";
                    }),
        "dirty/forward propagator events were emitted during planning");
    task5.add("legacy identity fallback observable",
              rect_json(cv::Rect(3, 4, 5, 6)),
              legacy_actual["forward_projected_roi"],
              legacy_actual["forward_projected_roi"] ==
                  rect_json(cv::Rect(3, 4, 5, 6)),
              "node without explicit propagator still projects identity and is "
              "documented as legacy");
    task5.add(
        "missing propagation contract is explicitly flagged as legacy",
        "legacy_identity_fallback",
        legacy_actual["forward_propagation_contract_status"],
        legacy_actual.value("dirty_propagation_contract_status", "") ==
                "legacy_identity_fallback" &&
            legacy_actual.value("forward_propagation_contract_status", "") ==
                "legacy_identity_fallback",
        "missing propagators are diagnosable and are not reported as explicit");
    task5.add(
        "RT cache present after dirty update", true,
        dirty_actual["graph_snapshot"]["nodes"]["100"]["cache"]["rt"]
                    ["present"],
        dirty_actual["graph_snapshot"]["nodes"]["100"]["cache"]["rt"]["present"]
            .get<bool>(),
        "dirty update produced frontend RT state without making "
        "InteractionService the authority");
    task5.add("dirty execution consumed planner output", true,
              dirty_actual["graph_snapshot"]["recent_compute_plans"],
              contains_compute_plan_graph(
                  dirty_actual["graph_snapshot"]["recent_compute_plans"],
                  "global_high_precision", false, {1, 2, 100}, 2, 3) &&
                  contains_compute_plan_graph(
                      dirty_actual["graph_snapshot"]["recent_compute_plans"],
                      "real_time_update", false, {1, 2, 100}, 2, 3),
              "HP and RT dirty update plans expose regions, dependencies, and "
              "planned task graph semantics consumed by execution");
    task5.add(
        "intent coordinator drove concurrent dual submit", true,
        dirty_actual["compute_events"],
        compute_events_contain_source(
            dirty_actual["compute_events"],
            "intent_coordinator_decision_concurrent") &&
            compute_events_contain_source(dirty_actual["compute_events"],
                                          "intent_coordinator_submit_hp") &&
            compute_events_contain_source(dirty_actual["compute_events"],
                                          "intent_coordinator_submit_rt") &&
            compute_events_contain_source(dirty_actual["compute_events"],
                                          "intent_coordinator_complete"),
        "RealTimeUpdate HP/RT dual submit was orchestrated inside "
        "IntentUpdateCoordinator");
    task5.add(
        "non-parallel realtime intent still ran HP and RT", true,
        dirty_single_thread_actual["compute_events"],
        dirty_single_thread_actual["dirty_update_ok"].get<bool>() &&
            dirty_single_thread_actual["requested_parallel"] == false &&
            compute_events_contain_source(
                dirty_single_thread_actual["compute_events"],
                "intent_coordinator_decision_inline") &&
            compute_events_contain_source(
                dirty_single_thread_actual["compute_events"],
                "intent_coordinator_inline_hp") &&
            compute_events_contain_source(
                dirty_single_thread_actual["compute_events"],
                "intent_coordinator_inline_rt") &&
            contains_compute_plan_graph(
                dirty_single_thread_actual["graph_snapshot"]
                                          ["recent_compute_plans"],
                "global_high_precision", false, {1, 2, 100}, 2, 3) &&
            contains_compute_plan_graph(
                dirty_single_thread_actual["graph_snapshot"]
                                          ["recent_compute_plans"],
                "real_time_update", false, {1, 2, 100}, 2, 3),
        "parallel=false selected inline execution, but RealTimeUpdate still "
        "coordinated both HP and RT plans");
    json task5_actual = {
        {"dirty_update", dirty_actual},
        {"dirty_update_single_thread", dirty_single_thread_actual},
        {"legacy_identity", legacy_actual}};
    json task5_expected = {
        {"dirty_snapshot_debug_contains", {"generation=", "tiles=", "edges="}},
        {"dirty_snapshot_detail",
         {{"dirty_tiles", 1},
          {"dirty_monolithic_nodes", 2},
          {"per_node_dirty_rois", 3},
          {"edge_mappings", 2}}},
        {"dirty_planned_nodes", {1, 2, 100}},
        {"dirty_plan_dependencies", ">=2"},
        {"dirty_plan_tasks", ">=3"},
        {"intent_coordinator_sources",
         {"intent_coordinator_decision_concurrent",
          "intent_coordinator_submit_hp", "intent_coordinator_submit_rt",
          "intent_coordinator_complete"}},
        {"intent_coordinator_inline_sources",
         {"intent_coordinator_decision_inline", "intent_coordinator_inline_hp",
          "intent_coordinator_inline_rt"}},
        {"non_parallel_realtime_dual_path", true},
        {"legacy_identity_forward_roi", rect_json(cv::Rect(3, 4, 5, 6))},
        {"legacy_contract_status", "legacy_identity_fallback"},
        {"rt_cache_present", true}};
    write_task_bundle(
        root, "task-05", "Task 5 dirty planning runtime evidence",
        "Dirty planning, snapshots, InteractionService exposure, "
        "task planning, and intent coordination must be observed.",
        command, "Dirty graph, legacy fallback graph.", task5_expected,
        task5_actual, task5, operator_trace);

    CheckSet task6;
    const int execute_count =
        count_scheduler_action(parallel_scheduler, "EXECUTE");
    const int tile_count =
        count_scheduler_action(parallel_scheduler, "EXECUTE_TILE");
    const double seq_checksum = node_checksum(sequential_snapshot, 100, "hp");
    const double par_checksum = node_checksum(parallel_snapshot, 100, "hp");
    task6.add("parallel compute returned ok", true, parallel_ok, parallel_ok,
              "parallel facade returned success");
    task6.add("scheduler node events recorded", ">=5", execute_count,
              execute_count >= 5,
              "parallel executor emitted scheduler dispatch events");
    task6.add("scheduler tile events recorded", ">0", tile_count,
              tile_count > 0,
              "tiled nodes emitted micro-task completion events");
    task6.add("parallel checksum equals sequential", seq_checksum, par_checksum,
              std::abs(seq_checksum - par_checksum) < 0.01,
              "temp result commit produced the same final HP output");
    task6.add(
        "parallel execution consumed planner output",
        json::array({1, 2, 4, 30, 100}), parallel_snapshot["last_compute_plan"],
        json_int_array_equals(
            parallel_snapshot["last_compute_plan"]["planned_nodes"],
            {1, 2, 4, 30, 100}) &&
            contains_compute_plan_graph(
                json::array({parallel_snapshot["last_compute_plan"]}),
                "global_high_precision", true, {1, 2, 4, 30, 100}, 4, 5),
        "ParallelGraphExecutor builds dependency counters and initial tasks "
        "from ComputeTaskGraph semantics");
    task6.add(
        "sparse node id target committed", true,
        parallel_snapshot["nodes"]["100"]["cache"]["hp"]["present"],
        parallel_snapshot["nodes"]["100"]["cache"]["hp"]["present"].get<bool>(),
        "target node id 100 proves sparse mapping is handled");
    task6.add(
        "parallel exception propagated", false, error_ok,
        !error_ok && last_error.has_value(),
        "error graph propagates executor exception to Kernel::last_error");
    json task6_actual = {{"parallel", parallel_actual},
                         {"error_path", error_actual},
                         {"sequential_checksum", seq_checksum},
                         {"parallel_checksum", par_checksum}};
    json task6_expected = {{"parallel_ok", true},
                           {"scheduler_execute_count", ">=5"},
                           {"scheduler_tile_count", ">0"},
                           {"final_checksum", expected_final_checksum},
                           {"planned_nodes", {1, 2, 4, 30, 100}},
                           {"plan_dependencies", ">=4"},
                           {"plan_tasks", ">=5"},
                           {"parallel_error_returns_ok", false}};
    write_task_bundle(root, "task-06",
                      "Task 6 parallel executor runtime evidence",
                      "Parallel executor must preserve dependency scheduling, "
                      "tile completion, commit, cache, events, and errors.",
                      command, "Full graph and error graph.", task6_expected,
                      task6_actual, task6, operator_trace);

    CheckSet task7;
    task7.add(
        "public facade used for sequential compute", true, sequential_ok,
        sequential_ok,
        "InteractionService::cmd_compute drove Kernel and ComputeService");
    task7.add("public facade used for parallel compute", true, parallel_ok,
              parallel_ok,
              "parallel compute stayed behind ComputeService facade");
    task7.add("public Kernel intent API used for dirty update", true, dirty_ok,
              dirty_ok,
              "Kernel::compute_async intent path drove HP/RT coordination");
    task7.add("public Kernel intent API respects execution mode", true,
              dirty_single_thread_ok,
              dirty_single_thread_ok &&
                  dirty_single_thread_actual["requested_parallel"] == false,
              "Kernel::compute_async intent path used single-threaded "
              "execution without disabling HP/RT coordination");
    task7.add(
        "operator trace includes facade-visible phases", 5, observed_phases,
        contains_phase(observed_phases, "sequential_hp") &&
            contains_phase(observed_phases, "parallel_hp") &&
            contains_phase(observed_phases, "dirty_rt_update") &&
            contains_phase(observed_phases, "dirty_rt_update_single_thread") &&
            contains_phase(observed_phases, "parallel_error_path"),
        "runtime evidence covers facade, scheduler, dirty, and error paths");
    json task7_actual = {{"sequential", sequential_actual},
                         {"parallel", parallel_actual},
                         {"dirty", dirty_actual},
                         {"dirty_single_thread", dirty_single_thread_actual},
                         {"error", error_actual},
                         {"observed_phases", observed_phases}};
    json task7_expected = {
        {"public_facade_sequential_ok", true},
        {"public_facade_parallel_ok", true},
        {"public_kernel_dirty_intent_ok", true},
        {"public_kernel_dirty_intent_single_thread_ok", true},
        {"required_phases",
         {"sequential_hp", "parallel_hp", "dirty_rt_update",
          "dirty_rt_update_single_thread", "parallel_error_path"}}};
    write_task_bundle(root, "task-07",
                      "Task 7 facade/build integration evidence",
                      "ComputeService must remain a facade while new internal "
                      "modules integrate with build and runtime paths.",
                      command, "All generated graphs.", task7_expected,
                      task7_actual, task7, operator_trace);

    CheckSet all;
    all.checks.insert(all.checks.end(), task2.checks.begin(),
                      task2.checks.end());
    all.checks.insert(all.checks.end(), task3.checks.begin(),
                      task3.checks.end());
    all.checks.insert(all.checks.end(), task4.checks.begin(),
                      task4.checks.end());
    all.checks.insert(all.checks.end(), task5.checks.begin(),
                      task5.checks.end());
    all.checks.insert(all.checks.end(), task6.checks.begin(),
                      task6.checks.end());
    all.checks.insert(all.checks.end(), task7.checks.begin(),
                      task7.checks.end());
    write_text(root / "full-kernel-run" / "compare.log", all.log());

    json summary = {{"ok", all.ok()},
                    {"task2_ok", task2.ok()},
                    {"task3_ok", task3.ok()},
                    {"task4_ok", task4.ok()},
                    {"task5_ok", task5.ok()},
                    {"task6_ok", task6.ok()},
                    {"task7_ok", task7.ok()},
                    {"operator_trace_events", operator_trace.size()},
                    {"sequential_final_checksum", seq_checksum},
                    {"parallel_final_checksum", par_checksum},
                    {"dirty_snapshot", dirty_text}};
    write_json(root / "full-kernel-run" / "summary.json", summary);

    log(std::string("runtime trace finished ok=") +
        (all.ok() ? "true" : "false"));
    return all.ok() ? 0 : 2;
  } catch (const std::exception& e) {
    log(std::string("fatal error: ") + e.what());
    write_text(root / "full-kernel-run" / "fatal.log", e.what());
    return 1;
  }
}
