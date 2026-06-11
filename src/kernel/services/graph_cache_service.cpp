#include "kernel/services/graph_cache_service.hpp"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <fstream>
#include <unordered_set>

#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/services/graph_traversal_service.hpp"

namespace ps {

namespace {

const NodeOutput* hp_cache_ptr(const Node& node) {
  if (node.cached_output_high_precision) {
    return &*node.cached_output_high_precision;
  }
  return nullptr;
}

bool has_memory_cache(const Node& node) {
  return node.cached_output_high_precision.has_value() ||
         node.cached_output_real_time.has_value();
}

void reset_memory_cache(Node& node) {
  node.cached_output_high_precision.reset();
  node.cached_output_real_time.reset();
}

}  // namespace

std::filesystem::path GraphCacheService::node_cache_dir(const GraphModel& graph,
                                                        int node_id) const {
  return graph.cache_root / std::to_string(node_id);
}

void GraphCacheService::save_cache_if_configured(
    GraphModel& graph, const Node& node,
    const std::string& cache_precision) const {
  if (graph.skip_save_cache_.load(std::memory_order_relaxed)) {
    return;
  }
  const NodeOutput* output = hp_cache_ptr(node);
  if (graph.cache_root.empty() || node.caches.empty() || !output) {
    return;
  }

  for (const auto& cache_entry : node.caches) {
    if (cache_entry.cache_type != "image" || cache_entry.location.empty()) {
      continue;
    }

    auto dir = node_cache_dir(graph, node.id);
    fs::create_directories(dir);
    auto final_path = dir / cache_entry.location;

    auto start_io = std::chrono::high_resolution_clock::now();

    if (output->image_buffer.width > 0 && output->image_buffer.height > 0) {
      cv::Mat mat_to_save = toCvMat(output->image_buffer);
      if (!mat_to_save.empty()) {
        cv::Mat out_mat;
        if (cache_precision == "int16") {
          mat_to_save.convertTo(out_mat, CV_16U, 65535.0);
        } else {
          mat_to_save.convertTo(out_mat, CV_8U, 255.0);
        }
        cv::imwrite(final_path.string(), out_mat);
      }
    }

    if (!output->data.empty()) {
      fs::path meta_path = final_path;
      meta_path.replace_extension(".yml");
      YAML::Node meta_node;
      for (const auto& pair : output->data) {
        meta_node[pair.first] = pair.second;
      }
      std::ofstream fout(meta_path);
      fout << meta_node;
    }

    auto end_io = std::chrono::high_resolution_clock::now();
    double duration_ms =
        std::chrono::duration<double, std::milli>(end_io - start_io).count();

    double expected = graph.total_io_time_ms.load();
    while (!graph.total_io_time_ms.compare_exchange_weak(
        expected, expected + duration_ms)) {
      // retry until success
    }
  }
}

bool GraphCacheService::try_load_from_disk_cache(GraphModel& graph,
                                                 Node& node) const {
  if (node.cached_output_high_precision.has_value() ||
      graph.cache_root.empty() || node.caches.empty()) {
    return node.cached_output_high_precision.has_value();
  }

  auto start_io = std::chrono::high_resolution_clock::now();
  bool loaded = false;

  try {
    for (const auto& cache_entry : node.caches) {
      if (cache_entry.cache_type != "image" || cache_entry.location.empty()) {
        continue;
      }

      auto cache_file = node_cache_dir(graph, node.id) / cache_entry.location;
      auto metadata_file = cache_file;
      metadata_file.replace_extension(".yml");

      if (!fs::exists(cache_file) && !fs::exists(metadata_file)) {
        continue;
      }

      NodeOutput loaded_output;
      if (fs::exists(cache_file)) {
        cv::Mat loaded_mat =
            cv::imread(cache_file.string(), cv::IMREAD_UNCHANGED);
        if (!loaded_mat.empty()) {
          cv::Mat float_mat;
          double scale =
              (loaded_mat.depth() == CV_8U)
                  ? 1.0 / 255.0
                  : (loaded_mat.depth() == CV_16U ? 1.0 / 65535.0 : 1.0);
          loaded_mat.convertTo(float_mat, CV_32F, scale);
          loaded_output.image_buffer = fromCvMat(float_mat);
        }
      }

      if (fs::exists(metadata_file)) {
        YAML::Node meta = YAML::LoadFile(metadata_file.string());
        for (auto it = meta.begin(); it != meta.end(); ++it) {
          loaded_output.data[it->first.as<std::string>()] = it->second;
        }
      }

      node.cached_output_high_precision = std::move(loaded_output);
      loaded = true;
      break;
    }
  } catch (const cv::Exception&) {
    loaded = false;
  } catch (const std::exception&) {
    loaded = false;
  }

  if (loaded) {
    auto end_io = std::chrono::high_resolution_clock::now();
    double duration_ms =
        std::chrono::duration<double, std::milli>(end_io - start_io).count();
    double expected = graph.total_io_time_ms.load();
    while (!graph.total_io_time_ms.compare_exchange_weak(
        expected, expected + duration_ms)) {
    }
  }

  return loaded;
}

bool GraphCacheService::try_load_from_disk_cache_into(GraphModel& graph,
                                                      const Node& node,
                                                      NodeOutput& out) const {
  if (graph.cache_root.empty() || node.caches.empty()) {
    return false;
  }

  auto start_io = std::chrono::high_resolution_clock::now();
  bool loaded = false;

  try {
    for (const auto& cache_entry : node.caches) {
      if (cache_entry.cache_type != "image" || cache_entry.location.empty()) {
        continue;
      }

      auto cache_file = node_cache_dir(graph, node.id) / cache_entry.location;
      auto metadata_file = cache_file;
      metadata_file.replace_extension(".yml");

      if (!fs::exists(cache_file) && !fs::exists(metadata_file)) {
        continue;
      }

      NodeOutput tmp;
      if (fs::exists(cache_file)) {
        cv::Mat loaded_mat =
            cv::imread(cache_file.string(), cv::IMREAD_UNCHANGED);
        if (!loaded_mat.empty()) {
          cv::Mat float_mat;
          double scale =
              (loaded_mat.depth() == CV_8U)
                  ? 1.0 / 255.0
                  : (loaded_mat.depth() == CV_16U ? 1.0 / 65535.0 : 1.0);
          loaded_mat.convertTo(float_mat, CV_32F, scale);
          tmp.image_buffer = fromCvMat(float_mat);
        }
      }
      if (fs::exists(metadata_file)) {
        YAML::Node meta = YAML::LoadFile(metadata_file.string());
        for (auto it = meta.begin(); it != meta.end(); ++it) {
          tmp.data[it->first.as<std::string>()] = it->second;
        }
      }
      out = std::move(tmp);
      loaded = true;
      break;
    }
  } catch (...) {
    loaded = false;
  }

  if (loaded) {
    auto end_io = std::chrono::high_resolution_clock::now();
    double duration_ms =
        std::chrono::duration<double, std::milli>(end_io - start_io).count();
    double expected = graph.total_io_time_ms.load();
    while (!graph.total_io_time_ms.compare_exchange_weak(
        expected, expected + duration_ms)) {
    }
  }

  return loaded;
}

GraphModel::DriveClearResult GraphCacheService::clear_drive_cache(
    GraphModel& graph) const {
  GraphModel::DriveClearResult result;
  if (!graph.cache_root.empty() && fs::exists(graph.cache_root)) {
    result.removed_entries = fs::remove_all(graph.cache_root);
    fs::create_directories(graph.cache_root);
  }
  return result;
}

GraphModel::MemoryClearResult GraphCacheService::clear_memory_cache(
    GraphModel& graph) const {
  GraphModel::MemoryClearResult result;
  for (int node_id : graph.node_ids()) {
    Node& node = graph.mutable_node(node_id);
    if (has_memory_cache(node)) {
      reset_memory_cache(node);
      result.cleared_nodes++;
    }
  }
  return result;
}

void GraphCacheService::clear_cache(GraphModel& graph) const {
  (void)clear_drive_cache(graph);
  (void)clear_memory_cache(graph);
}

GraphModel::CacheSaveResult GraphCacheService::cache_all_nodes(
    GraphModel& graph, const std::string& cache_precision) const {
  GraphModel::CacheSaveResult result;
  for (int node_id : graph.node_ids()) {
    const Node& node = graph.node(node_id);
    if (hp_cache_ptr(node)) {
      save_cache_if_configured(graph, node, cache_precision);
      result.saved_nodes++;
    }
  }
  return result;
}

GraphModel::MemoryClearResult GraphCacheService::free_transient_memory(
    GraphModel& graph) const {
  GraphTraversalService traversal;
  auto ends = traversal.ending_nodes(graph);
  std::unordered_set<int> endset(ends.begin(), ends.end());

  GraphModel::MemoryClearResult result;
  for (int node_id : graph.node_ids()) {
    Node& node = graph.mutable_node(node_id);
    if (has_memory_cache(node) && !endset.count(node_id)) {
      reset_memory_cache(node);
      result.cleared_nodes++;
    }
  }
  return result;
}

GraphModel::DiskSyncResult GraphCacheService::synchronize_disk_cache(
    GraphModel& graph, const std::string& cache_precision) const {
  GraphModel::DiskSyncResult result;
  result.saved_nodes = cache_all_nodes(graph, cache_precision).saved_nodes;

  for (int node_id : graph.node_ids()) {
    const Node& node = graph.node(node_id);
    if (node.cached_output_high_precision.has_value() || node.caches.empty()) {
      continue;
    }

    auto dir_path = node_cache_dir(graph, node.id);
    if (!fs::exists(dir_path)) {
      continue;
    }

    for (const auto& cache_entry : node.caches) {
      if (cache_entry.location.empty()) {
        continue;
      }
      auto cache_file = dir_path / cache_entry.location;
      auto meta_file = cache_file;
      meta_file.replace_extension(".yml");

      if (fs::exists(cache_file)) {
        fs::remove(cache_file);
        result.removed_files++;
      }
      if (fs::exists(meta_file)) {
        fs::remove(meta_file);
        result.removed_files++;
      }
    }

    if (fs::is_empty(dir_path)) {
      fs::remove(dir_path);
      result.removed_dirs++;
    }
  }

  return result;
}

}  // namespace ps
