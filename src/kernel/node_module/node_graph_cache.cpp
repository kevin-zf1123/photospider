// NodeGraph cache and filesystem persistence
#include "node_graph.hpp"
#include "adapter/buffer_adapter_opencv.hpp" // 引入适配器
#include <yaml-cpp/yaml.h>
#include <fstream>

namespace ps {

fs::path NodeGraph::node_cache_dir(int node_id) const { return cache_root / std::to_string(node_id); }

/**
 * @brief 根据配置保存节点的缓存数据。
 *
 * 当满足以下条件时，此函数将节点的缓存数据保存为图像及相应的元数据：
 *   - cache_root 不为空；
 *   - 节点的缓存列表 caches 非空；
 *   - 节点的 cached_output 有有效的值。
 *
 * 对于每个缓存条目，如果缓存类型为 "image" 且缓存位置不为空，将进行以下操作：
 *   1. 根据节点的 id 生成缓存目录，并确保目录已创建。
 *   2. 拼接目录和缓存条目的 location 得到最终保存路径。
 *
 * 核心修复逻辑：
 *   - 在保存图像之前，首先检查输出图像缓冲区 image_buffer 的宽度和高度是否均大于 0，
 *     因为某些分析节点可能会包含 "image" 类型的缓存条目但不输出实际图像。
 *   - 如果 image_buffer 有效，则转换为 OpenCV 的 cv::Mat 对象。
 *   - 如果转换后的图像不为空，根据 cache_precision 参数的值进行图像数据转换：
 *       - 如果 cache_precision 为 "int16"，则将图像转换为 CV_16U 类型，并按适当比例缩放；
 *       - 否则，默认转换为 CV_8U 类型，并按另一适当比例缩放。
 *   - 将转换后的图像保存至最终文件路径。
 *
 * 除图像保存外，如果输出中包含非空的元数据（data 部分），
 * 则将该元数据保存到与图像文件同名但扩展名为 ".yml" 的文件中。
 *
 * @param node  节点对象，包含图像输出、缓存配置及相关元数据。
 * @param cache_precision 指定图像缓存精度，如 "int16" 表示 16 位整数精度，否则默认使用 8 位精度。
 */
void NodeGraph::save_cache_if_configured(const Node& node, const std::string& cache_precision) const {
    if (cache_root.empty() || node.caches.empty() || !node.cached_output.has_value()) {
        return;
    }

    const auto& output = *node.cached_output;
    for (const auto& cache_entry : node.caches) {
        if (cache_entry.cache_type == "image" && !cache_entry.location.empty()) {
            fs::path dir = node_cache_dir(node.id);
            fs::create_directories(dir);
            fs::path final_path = dir / cache_entry.location;
            auto start_io = std::chrono::high_resolution_clock::now();
            // --- 核心修复逻辑 ---
            // 在尝试保存图像之前，必须检查 image_buffer 是否有效。
            // 一个分析节点（如 get_dimensions）可能有一个 "image" 类型的缓存条目
            // （用于保存其分析结果的元数据），但它本身不输出图像。
            if (output.image_buffer.width > 0 && output.image_buffer.height > 0) {
                cv::Mat mat_to_save = toCvMat(output.image_buffer);

                // 这个检查现在变得有些冗余，但保留也无妨
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
            // --- 修复结束 ---

            // 元数据（data部分）的保存逻辑应该独立于图像保存
            if (!output.data.empty()) {
                fs::path meta_path = final_path;
                meta_path.replace_extension(".yml");
                YAML::Node meta_node;
                for(const auto& pair : output.data) {
                    meta_node[pair.first] = pair.second;
                }
                std::ofstream fout(meta_path);
                fout << meta_node;
            }
                auto end_io = std::chrono::high_resolution_clock::now();
                // Atomic addition for std::atomic<double> requires a compare-exchange loop.
                double duration_to_add = std::chrono::duration<double, std::milli>(end_io - start_io).count();
                auto& atomic_io_time = const_cast<NodeGraph*>(this)->total_io_time_ms;

                double current_io_time = atomic_io_time.load();
                while (!atomic_io_time.compare_exchange_weak(current_io_time, current_io_time + duration_to_add)) {
                    // 循环直到成功为止。如果失败，compare_exchange_weak 会自动将 current_io_time 更新为最新的值。
                }
        }
    }
}

bool NodeGraph::try_load_from_disk_cache(Node& node) {
    if (node.cached_output.has_value() || cache_root.empty() || node.caches.empty()) return node.cached_output.has_value();
    auto start_io = std::chrono::high_resolution_clock::now();
    bool loaded = false;
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
    auto end_io = std::chrono::high_resolution_clock::now();
    if (loaded) {
        // Atomic addition for std::atomic<double> requires a compare-exchange loop.
        double duration_to_add = std::chrono::duration<double, std::milli>(end_io - start_io).count();

        double current_io_time = total_io_time_ms.load();
        while (!total_io_time_ms.compare_exchange_weak(current_io_time, current_io_time + duration_to_add)) {
            // 循环直到成功为止。
        }
    }
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
