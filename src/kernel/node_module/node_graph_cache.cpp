// NodeGraph cache and filesystem persistence
#include "node_graph.hpp"
#include "adapter/buffer_adapter_opencv.hpp" // 引入适配器
#include <yaml-cpp/yaml.h>
#include <fstream>

namespace ps {

fs::path NodeGraph::node_cache_dir(int node_id) const { return cache_root / std::to_string(node_id); }

void NodeGraph::save_cache_if_configured(const Node& node, const std::string& cache_precision) const {
    if (cache_root.empty() || node.caches.empty() || !node.cached_output.has_value()) return;
    const auto& output = *node.cached_output;
    for (const auto& cache_entry : node.caches) {
        if (cache_entry.cache_type == "image" && !cache_entry.location.empty()) {
            fs::path dir = node_cache_dir(node.id); fs::create_directories(dir);
            fs::path final_path = dir / cache_entry.location;

            // 使用适配器将 ImageBuffer 转为 Mat
            cv::Mat mat_to_save = toCvMat(output.image_buffer);

            if (!mat_to_save.empty()) {
                cv::Mat out_mat;
                if (cache_precision == "int16") mat_to_save.convertTo(out_mat, CV_16U, 65535.0);
                else mat_to_save.convertTo(out_mat, CV_8U, 255.0);
                cv::imwrite(final_path.string(), out_mat);
            }
            if (!output.data.empty()) {
                fs::path meta_path = final_path; meta_path.replace_extension(".yml");
                YAML::Node meta_node; for(const auto& pair : output.data) meta_node[pair.first] = pair.second;
                std::ofstream fout(meta_path); fout << meta_node;
            }
        }
    }
}

bool NodeGraph::try_load_from_disk_cache(Node& node) {
    if (node.cached_output.has_value() || cache_root.empty() || node.caches.empty()) return node.cached_output.has_value();
    for (const auto& cache_entry : node.caches) {
        if (cache_entry.cache_type == "image" && !cache_entry.location.empty()) {
            fs::path cache_file = node_cache_dir(node.id) / cache_entry.location;
            fs::path metadata_file = cache_file; metadata_file.replace_extension(".yml");
            if (fs::exists(cache_file) || fs::exists(metadata_file)) {
                NodeOutput loaded_output;
                if (fs::exists(cache_file)) {
                    cv::Mat loaded_mat = cv::imread(cache_file.string(), cv::IMREAD_UNCHANGED);
                    if (!loaded_mat.empty()) {
                        cv::Mat float_mat;
                        double scale = (loaded_mat.depth() == CV_8U) ? 1.0/255.0 : (loaded_mat.depth()==CV_16U ? 1.0/65535.0 : 1.0);
                        loaded_mat.convertTo(float_mat, CV_32F, scale);
                        // 从 Mat 创建 ImageBuffer
                        loaded_output.image_buffer = fromCvMat(float_mat);
                    }
                }
                if (fs::exists(metadata_file)) {
                    YAML::Node meta = YAML::LoadFile(metadata_file.string());
                    for(auto it = meta.begin(); it != meta.end(); ++it) loaded_output.data[it->first.as<std::string>()] = it->second;
                }
                node.cached_output = std::move(loaded_output);
                return true;
            }
        }
    }
    return false;
}

NodeGraph::DriveClearResult NodeGraph::clear_drive_cache() {
    DriveClearResult r; if (!cache_root.empty() && fs::exists(cache_root)) { r.removed_entries = fs::remove_all(cache_root); fs::create_directories(cache_root); }
    return r;
}

NodeGraph::MemoryClearResult NodeGraph::clear_memory_cache() {
    MemoryClearResult r; for (auto& pair : nodes) { if (pair.second.cached_output.has_value()) { pair.second.cached_output.reset(); r.cleared_nodes++; } }
    return r;
}

void NodeGraph::clear_cache() { (void)clear_drive_cache(); (void)clear_memory_cache(); }

NodeGraph::CacheSaveResult NodeGraph::cache_all_nodes(const std::string& cache_precision) {
    CacheSaveResult r; for (const auto& pair : nodes) { if (pair.second.cached_output.has_value()) { save_cache_if_configured(pair.second, cache_precision); r.saved_nodes++; } }
    return r;
}

NodeGraph::MemoryClearResult NodeGraph::free_transient_memory() {
    MemoryClearResult r; auto ends = this->ending_nodes(); std::unordered_set<int> endset(ends.begin(), ends.end());
    for (auto& pair : nodes) { if (pair.second.cached_output.has_value() && !endset.count(pair.first)) { pair.second.cached_output.reset(); r.cleared_nodes++; } }
    return r;
}

NodeGraph::DiskSyncResult NodeGraph::synchronize_disk_cache(const std::string& cache_precision) {
    DiskSyncResult r; r.saved_nodes = this->cache_all_nodes(cache_precision).saved_nodes;
    for (const auto& pair : nodes) {
        const Node& node = pair.second;
        if (!node.cached_output.has_value() && !node.caches.empty()) {
            fs::path dir_path = node_cache_dir(node.id); if (!fs::exists(dir_path)) continue;
            for (const auto& cache_entry : node.caches) {
                if (!cache_entry.location.empty()) {
                    fs::path cache_file = node_cache_dir(node.id) / cache_entry.location;
                    fs::path meta_file = cache_file; meta_file.replace_extension(".yml");
                    if (fs::exists(cache_file)) { fs::remove(cache_file); r.removed_files++; }
                    if (fs::exists(meta_file)) { fs::remove(meta_file); r.removed_files++; }
                }
            }
            if (fs::is_empty(dir_path)) { fs::remove(dir_path); r.removed_dirs++; }
        }
    }
    return r;
}

} // namespace ps
