// FILE: src/kernel/node_module/node_graph_parallel.cpp (重构后的完整文件)

#include "node_graph.hpp"
#include "kernel/graph_runtime.hpp"
#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/param_utils.hpp"
#include <algorithm>
#include <vector>
#include <map>
#include <atomic>
#include <exception>
#include <random>
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include <functional>
#include <optional>

namespace {

constexpr int kRtDownscaleFactor = 4;
constexpr int kRtTileSize = 16;
constexpr int kHpAlignment = kRtDownscaleFactor * kRtTileSize; // 64px alignment on full-res

inline bool is_rect_empty(const cv::Rect& rect) {
    return rect.width <= 0 || rect.height <= 0;
}

inline cv::Rect clip_rect(const cv::Rect& rect, const cv::Size& bounds) {
    if (bounds.width <= 0 || bounds.height <= 0) return cv::Rect();
    int x0 = std::clamp(rect.x, 0, bounds.width);
    int y0 = std::clamp(rect.y, 0, bounds.height);
    int x1 = std::clamp(rect.x + rect.width, 0, bounds.width);
    int y1 = std::clamp(rect.y + rect.height, 0, bounds.height);
    if (x1 <= x0 || y1 <= y0) return cv::Rect();
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

inline cv::Rect expand_rect(const cv::Rect& rect, int padding) {
    if (padding <= 0 || is_rect_empty(rect)) return rect;
    int x0 = rect.x - padding;
    int y0 = rect.y - padding;
    int x1 = rect.x + rect.width + padding;
    int y1 = rect.y + rect.height + padding;
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

inline cv::Rect align_rect(const cv::Rect& rect, int alignment) {
    if (is_rect_empty(rect) || alignment <= 1) return rect;
    int x0 = (rect.x / alignment) * alignment;
    int y0 = (rect.y / alignment) * alignment;
    int x1 = ((rect.x + rect.width + alignment - 1) / alignment) * alignment;
    int y1 = ((rect.y + rect.height + alignment - 1) / alignment) * alignment;
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

inline cv::Rect merge_rect(const cv::Rect& a, const cv::Rect& b) {
    if (is_rect_empty(a)) return b;
    if (is_rect_empty(b)) return a;
    int x0 = std::min(a.x, b.x);
    int y0 = std::min(a.y, b.y);
    int x1 = std::max(a.x + a.width, b.x + b.width);
    int y1 = std::max(a.y + a.height, b.y + b.height);
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

inline cv::Size scale_down_size(const cv::Size& size, int factor) {
    if (size.width <= 0 || size.height <= 0) return cv::Size();
    return cv::Size(
        (size.width + factor - 1) / factor,
        (size.height + factor - 1) / factor
    );
}

inline cv::Rect scale_down_rect(const cv::Rect& rect, int factor) {
    if (is_rect_empty(rect)) return cv::Rect();
    int x0 = rect.x / factor;
    int y0 = rect.y / factor;
    int x1 = (rect.x + rect.width + factor - 1) / factor;
    int y1 = (rect.y + rect.height + factor - 1) / factor;
    return cv::Rect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

inline cv::Rect scale_up_rect(const cv::Rect& rect, int factor) {
    if (is_rect_empty(rect)) return cv::Rect();
    int x0 = rect.x * factor;
    int y0 = rect.y * factor;
    int x1 = (rect.x + rect.width) * factor;
    int y1 = (rect.y + rect.height) * factor;
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

struct RtPlanEntry {
    cv::Rect roi_hp;
    cv::Rect roi_rt;
    cv::Size hp_size;
    cv::Size rt_size;
    int halo_hp = 0;
    int halo_rt = 0;
};

} // anonymous namespace

namespace ps {

// Forward declarations from compute module (same namespace)
void execute_tile_task(const ps::TileTask& task, const TileOpFunc& tiled_op);
cv::Rect calculate_halo(const cv::Rect& roi, int halo_size, const cv::Size& bounds);

// New: non-recursive compute used by the parallel scheduler. Assumes dependencies are ready.
NodeOutput& NodeGraph::compute_node_no_recurse(int node_id,
                                               const std::string& cache_precision,
                                               bool enable_timing,
                                               bool allow_disk_cache,
                                               std::vector<BenchmarkEvent>* benchmark_events) {
    auto& target_node = nodes.at(node_id);
    // Fast path: already computed
    if (target_node.cached_output.has_value()) return *target_node.cached_output;

    // Optionally load from disk cache for this node itself
    if (allow_disk_cache) {
        (void)try_load_from_disk_cache(target_node);
        if (target_node.cached_output.has_value()) return *target_node.cached_output;
    }

    // Ensure visibility of upstream writes before reading their cached outputs
    std::atomic_thread_fence(std::memory_order_acquire);

    // Build runtime parameters starting from static parameters.
    // Use deep clone to avoid yaml-cpp memory_holder merges across threads.
    target_node.runtime_parameters =
        target_node.parameters ? YAML::Clone(target_node.parameters) : YAML::Node(YAML::NodeType::Map);

    // Read parameter inputs from already computed parents
    for (const auto& p_input : target_node.parameter_inputs) {
        if (p_input.from_node_id < 0) continue;
        const auto itn = nodes.find(p_input.from_node_id);
        if (itn == nodes.end() || !itn->second.cached_output.has_value()) {
            throw GraphError(GraphErrc::MissingDependency,
                             "Parallel scheduler bug: parameter input not ready for node " + std::to_string(node_id));
        }
        const auto& up_out = *itn->second.cached_output;
        auto it = up_out.data.find(p_input.from_output_name);
        if (it == up_out.data.end()) {
            throw GraphError(GraphErrc::MissingDependency,
                "Node " + std::to_string(p_input.from_node_id) +
                " did not produce output '" + p_input.from_output_name + "'");
        }
        target_node.runtime_parameters[p_input.to_parameter_name] = it->second;
    }

    // Gather image inputs from parents (must be ready)
    std::vector<const NodeOutput*> inputs_ready;
    inputs_ready.reserve(target_node.image_inputs.size());
    for (const auto& i_input : target_node.image_inputs) {
        if (i_input.from_node_id < 0) continue;
        const auto itn = nodes.find(i_input.from_node_id);
        if (itn == nodes.end() || !itn->second.cached_output.has_value()) {
            throw GraphError(GraphErrc::MissingDependency,
                             "Parallel scheduler bug: image input not ready for node " + std::to_string(node_id));
        }
        inputs_ready.push_back(&*itn->second.cached_output);
    }

    auto op_opt = OpRegistry::instance().resolve_for_intent(target_node.type, target_node.subtype, ComputeIntent::GlobalHighPrecision);
    if (!op_opt) {
        throw GraphError(GraphErrc::NoOperation,
                         "No op for " + target_node.type + ":" + target_node.subtype);
    }

    // Timing event for benchmarking
    BenchmarkEvent current_event;
    current_event.node_id = node_id;
    current_event.op_name = make_key(target_node.type, target_node.subtype);
    current_event.dependency_start_time = std::chrono::high_resolution_clock::now();
    current_event.execution_start_time = current_event.dependency_start_time;

    try {
        std::visit([&](auto&& op_func) {
            using T = std::decay_t<decltype(op_func)>;
            if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                target_node.cached_output = op_func(target_node, inputs_ready);
                target_node.cached_output_high_precision = *target_node.cached_output;
                target_node.hp_version++;
            } else if constexpr (std::is_same_v<T, TileOpFunc>) {
                // Prepare normalized inputs similar to compute_internal
                std::vector<NodeOutput> normalized_storage;
                std::vector<const NodeOutput*> inputs_for_tiling = inputs_ready;

                bool is_mixing = (target_node.type == "image_mixing");
                if (is_mixing && inputs_ready.size() >= 2) {
                    const auto& base_buffer = inputs_ready[0]->image_buffer;
                    if (base_buffer.width == 0 || base_buffer.height == 0) {
                        throw GraphError(GraphErrc::InvalidParameter, "Base image for image_mixing is empty.");
                    }
                    const int base_w = base_buffer.width;
                    const int base_h = base_buffer.height;
                    const int base_c = base_buffer.channels;
                    const std::string strategy = as_str(target_node.runtime_parameters, "merge_strategy", "resize");
                    normalized_storage.reserve(inputs_ready.size() - 1);
                    for (size_t i = 1; i < inputs_ready.size(); ++i) {
                        const auto& current_buffer = inputs_ready[i]->image_buffer;
                        if (current_buffer.width == 0 || current_buffer.height == 0) {
                            throw GraphError(GraphErrc::InvalidParameter, "Secondary image for image_mixing is empty.");
                        }
                        cv::Mat current_mat = toCvMat(current_buffer);
                        if (current_mat.cols != base_w || current_mat.rows != base_h) {
                            if (strategy == "resize") {
                                cv::resize(current_mat, current_mat, cv::Size(base_w, base_h), 0, 0, cv::INTER_LINEAR);
                            } else if (strategy == "crop") {
                                cv::Rect crop_roi(0, 0, std::min(current_mat.cols, base_w), std::min(current_mat.rows, base_h));
                                cv::Mat cropped = cv::Mat::zeros(base_h, base_w, current_mat.type());
                                current_mat(crop_roi).copyTo(cropped(crop_roi));
                                current_mat = cropped;
                            } else {
                                throw GraphError(GraphErrc::InvalidParameter, "Unsupported merge_strategy for tiled mixing.");
                            }
                        }
                        if (current_mat.channels() != base_c) {
                            if (current_mat.channels() == 1 && (base_c == 3 || base_c == 4)) {
                                std::vector<cv::Mat> planes(base_c, current_mat);
                                cv::merge(planes, current_mat);
                            } else if ((current_mat.channels() == 3 || current_mat.channels() == 4) && base_c == 1) {
                                cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2GRAY);
                            } else if (current_mat.channels() == 4 && base_c == 3) {
                                cv::cvtColor(current_mat, current_mat, cv::COLOR_BGRA2BGR);
                            } else if (current_mat.channels() == 3 && base_c == 4) {
                                cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2BGRA);
                            } else {
                                throw GraphError(GraphErrc::InvalidParameter, "Unsupported channel conversion in tiled mixing.");
                            }
                        }
                        NodeOutput tmp; tmp.image_buffer = fromCvMat(current_mat);
                        normalized_storage.push_back(std::move(tmp));
                        inputs_for_tiling[i] = &normalized_storage.back();
                    }
                }

                // Infer output shape
                int out_w = inputs_for_tiling.empty() ? as_int_flexible(target_node.runtime_parameters, "width", 256)
                                                      : inputs_for_tiling[0]->image_buffer.width;
                int out_h = inputs_for_tiling.empty() ? as_int_flexible(target_node.runtime_parameters, "height", 256)
                                                      : inputs_for_tiling[0]->image_buffer.height;
                int out_c = inputs_for_tiling.empty() ? 1 : inputs_for_tiling[0]->image_buffer.channels;
                auto out_t = inputs_for_tiling.empty() ? ps::DataType::FLOAT32 : inputs_for_tiling[0]->image_buffer.type;

                target_node.cached_output = NodeOutput();
                auto& ob = target_node.cached_output->image_buffer;
                ob.width = out_w; ob.height = out_h; ob.channels = out_c; ob.type = out_t;
                size_t pix_sz = sizeof(float);
                ob.step = out_w * out_c * pix_sz;
                ob.data.reset(new char[ob.step * ob.height], std::default_delete<char[]>());

                const int TILE_SIZE = 256, HALO_SIZE = 16;
                for (int y = 0; y < ob.height; y += TILE_SIZE) {
                    for (int x = 0; x < ob.width; x += TILE_SIZE) {
                        ps::TileTask task;
                        task.node = &target_node;
                        task.output_tile.buffer = &ob;
                        task.output_tile.roi = cv::Rect(x, y, std::min(TILE_SIZE, ob.width - x), std::min(TILE_SIZE, ob.height - y));
                        const bool needs_halo = (target_node.type == "image_process" && target_node.subtype == "gaussian_blur");
                        for (auto const* in_out : inputs_for_tiling) {
                            ps::Tile in_tile;
                            in_tile.buffer = const_cast<ps::ImageBuffer*>(&in_out->image_buffer);
                            if (needs_halo) {
                                in_tile.roi = calculate_halo(task.output_tile.roi, HALO_SIZE, {in_out->image_buffer.width, in_out->image_buffer.height});
                            } else {
                                in_tile.roi = task.output_tile.roi;
                            }
                            task.input_tiles.push_back(in_tile);
                        }
                        execute_tile_task(task, op_func);
                    }
                }
                // Mirror to HP cache and bump version after tiling finishes
                target_node.cached_output_high_precision = *target_node.cached_output;
                target_node.hp_version++;
            }
        }, *op_opt);
    } catch (const cv::Exception& e) {
        throw GraphError(GraphErrc::ComputeError,
                         "Node " + std::to_string(target_node.id) + " (" + target_node.name + ") failed: " + std::string(e.what()));
    } catch (const std::exception& e) {
        throw GraphError(GraphErrc::ComputeError,
                         "Node " + std::to_string(target_node.id) + " (" + target_node.name + ") failed: " + std::string(e.what()));
    }

    // Save disk cache if configured
    save_cache_if_configured(target_node, cache_precision);

    // Timing & events
    if (enable_timing) {
        current_event.execution_end_time = std::chrono::high_resolution_clock::now();
        current_event.source = "computed";
        current_event.execution_duration_ms = std::chrono::duration<double, std::milli>(current_event.execution_end_time - current_event.execution_start_time).count();
        if (benchmark_events) benchmark_events->push_back(current_event);
        {
            std::lock_guard lk(timing_mutex_);
            timing_results.node_timings.push_back({ target_node.id, target_node.name, current_event.execution_duration_ms, "computed" });
        }
        push_compute_event(target_node.id, target_node.name, "computed", current_event.execution_duration_ms);
    } else {
        push_compute_event(target_node.id, target_node.name, "computed", 0.0);
    }
    return *target_node.cached_output;
}

NodeOutput& NodeGraph::compute_real_time_update(GraphRuntime* runtime,
                                                int node_id,
                                                const std::string& cache_precision,
                                                bool force_recache,
                                                bool enable_timing,
                                                bool disable_disk_cache,
                                                std::vector<BenchmarkEvent>* benchmark_events,
                                                const cv::Rect& dirty_roi)
{
    (void)runtime;
    (void)cache_precision;
    (void)disable_disk_cache;
    (void)benchmark_events;
    (void)enable_timing;

    if (!has_node(node_id)) {
        throw GraphError(GraphErrc::NotFound, "Cannot compute RT update: node " + std::to_string(node_id) + " not found.");
    }

    if (dirty_roi.width <= 0 || dirty_roi.height <= 0) {
        throw GraphError(GraphErrc::InvalidParameter, "Cannot compute RT update: dirty ROI is empty.");
    }

    auto execution_order = topo_postorder_from(node_id);
    if (execution_order.empty()) {
        execution_order.push_back(node_id);
    }

    std::unordered_map<int, cv::Size> hp_size_cache;
    std::function<cv::Size(int)> infer_hp_size = [&](int nid) -> cv::Size {
        auto cached = hp_size_cache.find(nid);
        if (cached != hp_size_cache.end()) return cached->second;

        cv::Size size{0, 0};
        const Node& node = nodes.at(nid);

        auto take_from_output = [&](const std::optional<NodeOutput>& opt) -> bool {
            if (!opt.has_value()) return false;
            const auto& img = opt->image_buffer;
            if (img.width <= 0 || img.height <= 0) return false;
            size = cv::Size(img.width, img.height);
            return true;
        };

        if (take_from_output(node.cached_output_high_precision)) {
            hp_size_cache[nid] = size;
            return size;
        }
        if (take_from_output(node.cached_output)) {
            hp_size_cache[nid] = size;
            return size;
        }
        if (node.cached_output_real_time) {
            const auto& img = node.cached_output_real_time->image_buffer;
            if (img.width > 0 && img.height > 0) {
                size = cv::Size(img.width * kRtDownscaleFactor, img.height * kRtDownscaleFactor);
                hp_size_cache[nid] = size;
                return size;
            }
        }

        for (const auto& input : node.image_inputs) {
            if (input.from_node_id < 0) continue;
            cv::Size parent_size = infer_hp_size(input.from_node_id);
            if (parent_size.width > 0 && parent_size.height > 0) {
                size = parent_size;
                break;
            }
        }

        if (size.width <= 0 || size.height <= 0) {
            int width = as_int_flexible(node.parameters, "width", 0);
            int height = as_int_flexible(node.parameters, "height", 0);
            if (width > 0 && height > 0) {
                size = cv::Size(width, height);
            }
        }

        hp_size_cache[nid] = size;
        return size;
    };

    auto infer_halo_hp = [&](const Node& node) -> int {
        if (node.type == "image_process" && node.subtype == "gaussian_blur") {
            int k = as_int_flexible(node.parameters, "ksize", 3);
            if (k <= 0) k = 1;
            if (k % 2 == 0) k += 1;
            return std::max(0, k / 2);
        }
        return 0;
    };

    std::unordered_map<int, RtPlanEntry> plan;
    auto ensure_entry = [&](int nid) -> RtPlanEntry& {
        auto [it, inserted] = plan.emplace(nid, RtPlanEntry{});
        if (inserted) {
            it->second.hp_size = infer_hp_size(nid);
            it->second.rt_size = scale_down_size(it->second.hp_size, kRtDownscaleFactor);
            it->second.halo_hp = infer_halo_hp(nodes.at(nid));
            it->second.halo_rt = (it->second.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;
        } else {
            if (it->second.hp_size.width <= 0 || it->second.hp_size.height <= 0) {
                it->second.hp_size = infer_hp_size(nid);
                it->second.rt_size = scale_down_size(it->second.hp_size, kRtDownscaleFactor);
            }
            if (it->second.halo_hp == 0) {
                it->second.halo_hp = infer_halo_hp(nodes.at(nid));
                it->second.halo_rt = (it->second.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;
            }
        }
        return it->second;
    };

    RtPlanEntry& target_entry = ensure_entry(node_id);
    target_entry.roi_hp = clip_rect(align_rect(dirty_roi, kHpAlignment), target_entry.hp_size);
    if (is_rect_empty(target_entry.roi_hp)) {
        throw GraphError(GraphErrc::InvalidParameter, "Dirty ROI does not intersect node output.");
    }
    target_entry.roi_rt = clip_rect(
        align_rect(scale_down_rect(target_entry.roi_hp, kRtDownscaleFactor), kRtTileSize),
        target_entry.rt_size
    );
    if (is_rect_empty(target_entry.roi_rt)) {
        throw GraphError(GraphErrc::InvalidParameter, "Dirty ROI collapses after RT scaling.");
    }

    for (auto it = execution_order.rbegin(); it != execution_order.rend(); ++it) {
        int current_id = *it;
        auto plan_it = plan.find(current_id);
        if (plan_it == plan.end()) continue;

        RtPlanEntry& current_entry = plan_it->second;
        if (is_rect_empty(current_entry.roi_hp)) continue;

        const Node& current_node = nodes.at(current_id);
        current_entry.halo_hp = std::max(current_entry.halo_hp, infer_halo_hp(current_node));
        current_entry.halo_rt = (current_entry.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;

        cv::Rect upstream_roi_hp = current_entry.roi_hp;
        if (current_entry.halo_hp > 0) {
            upstream_roi_hp = expand_rect(upstream_roi_hp, current_entry.halo_hp);
        }

        for (const auto& img_input : current_node.image_inputs) {
            if (img_input.from_node_id < 0) continue;
            int parent_id = img_input.from_node_id;
            RtPlanEntry& parent_entry = ensure_entry(parent_id);
            cv::Rect parent_roi = clip_rect(align_rect(upstream_roi_hp, kHpAlignment), parent_entry.hp_size);
            if (is_rect_empty(parent_roi)) continue;
            parent_entry.roi_hp = is_rect_empty(parent_entry.roi_hp)
                                   ? parent_roi
                                   : clip_rect(merge_rect(parent_entry.roi_hp, parent_roi), parent_entry.hp_size);
        }
    }

    std::vector<int> erase_ids;
    erase_ids.reserve(plan.size());
    for (auto& kv : plan) {
        auto& entry = kv.second;
        if (entry.hp_size.width <= 0 || entry.hp_size.height <= 0) {
            erase_ids.push_back(kv.first);
            continue;
        }
        entry.roi_hp = clip_rect(align_rect(entry.roi_hp, kHpAlignment), entry.hp_size);
        if (is_rect_empty(entry.roi_hp)) {
            erase_ids.push_back(kv.first);
            continue;
        }
        entry.rt_size = scale_down_size(entry.hp_size, kRtDownscaleFactor);
        entry.roi_rt = clip_rect(
            align_rect(scale_down_rect(entry.roi_hp, kRtDownscaleFactor), kRtTileSize),
            entry.rt_size
        );
        if (is_rect_empty(entry.roi_rt)) {
            erase_ids.push_back(kv.first);
            continue;
        }
        if (entry.halo_hp == 0) {
            entry.halo_hp = infer_halo_hp(nodes.at(kv.first));
            entry.halo_rt = (entry.halo_hp + kRtDownscaleFactor - 1) / kRtDownscaleFactor;
        }
    }
    for (int nid : erase_ids) {
        plan.erase(nid);
    }

    if (plan.empty()) {
        throw GraphError(GraphErrc::InvalidParameter, "RT planner produced empty execution set.");
    }

    if (force_recache) {
        for (const auto& kv : plan) {
            Node& node = nodes.at(kv.first);
            node.cached_output_real_time.reset();
            node.rt_roi.reset();
            node.rt_version = 0;
        }
    }

    auto compute_node_rt = [&](int nid, RtPlanEntry& entry) {
        Node& node = nodes.at(nid);
        if (is_rect_empty(entry.roi_rt)) return;

        YAML::Node runtime_params = node.parameters ? YAML::Clone(node.parameters)
                                                    : YAML::Node(YAML::NodeType::Map);
        for (const auto& p_input : node.parameter_inputs) {
            if (p_input.from_node_id < 0) continue;
            const Node& parent_node = nodes.at(p_input.from_node_id);
            const NodeOutput* parent_out = nullptr;
            if (parent_node.cached_output_real_time) {
                parent_out = &*parent_node.cached_output_real_time;
            } else if (parent_node.cached_output_high_precision) {
                parent_out = &*parent_node.cached_output_high_precision;
            } else if (parent_node.cached_output) {
                parent_out = &*parent_node.cached_output;
            }
            if (!parent_out) {
                throw GraphError(GraphErrc::MissingDependency,
                                 "RT parameter input not ready for node " + std::to_string(nid));
            }
            auto it_val = parent_out->data.find(p_input.from_output_name);
            if (it_val == parent_out->data.end()) {
                throw GraphError(GraphErrc::MissingDependency,
                                 "RT parameter '" + p_input.from_output_name + "' missing for node " + std::to_string(nid));
            }
            runtime_params[p_input.to_parameter_name] = it_val->second;
        }
        node.runtime_parameters = runtime_params;

        std::vector<const NodeOutput*> image_inputs_ready;
        image_inputs_ready.reserve(node.image_inputs.size());
        for (const auto& img_input : node.image_inputs) {
            if (img_input.from_node_id < 0) continue;
            Node& parent_node = nodes.at(img_input.from_node_id);
            const NodeOutput* parent_out = nullptr;
            if (parent_node.cached_output_real_time) {
                parent_out = &*parent_node.cached_output_real_time;
            } else if (parent_node.cached_output_high_precision) {
                parent_out = &*parent_node.cached_output_high_precision;
            } else if (parent_node.cached_output) {
                parent_out = &*parent_node.cached_output;
            }
            if (!parent_out) {
                throw GraphError(GraphErrc::MissingDependency,
                                 "RT image input not ready for node " + std::to_string(nid));
            }
            image_inputs_ready.push_back(parent_out);
        }

        auto op_variant = OpRegistry::instance().resolve_for_intent(
            node.type, node.subtype, ComputeIntent::RealTimeUpdate);
        if (!op_variant) {
            op_variant = OpRegistry::instance().resolve_for_intent(
                node.type, node.subtype, ComputeIntent::GlobalHighPrecision);
        }
        if (!op_variant) {
            throw GraphError(GraphErrc::NoOperation,
                             "No operator registered for node " + node.type + ":" + node.subtype);
        }

        auto infer_output_spec = [&]() -> std::pair<int, DataType> {
            if (node.cached_output_real_time) {
                const auto& buf = node.cached_output_real_time->image_buffer;
                if (buf.width > 0 && buf.height > 0 && buf.channels > 0) {
                    return {buf.channels, buf.type};
                }
            }
            for (const auto* input_out : image_inputs_ready) {
                const auto& buf = input_out->image_buffer;
                if (buf.width > 0 && buf.height > 0 && buf.channels > 0) {
                    return {buf.channels, buf.type};
                }
            }
            if (node.cached_output_high_precision) {
                const auto& buf = node.cached_output_high_precision->image_buffer;
                if (buf.width > 0 && buf.height > 0 && buf.channels > 0) {
                    return {buf.channels, buf.type};
                }
            }
            return {1, DataType::FLOAT32};
        };

        auto [channels, dtype] = infer_output_spec();
        if (!node.cached_output_real_time) {
            node.cached_output_real_time = NodeOutput{};
        }
        ImageBuffer& rt_buffer = node.cached_output_real_time->image_buffer;
        bool needs_alloc = (rt_buffer.width != entry.rt_size.width) ||
                           (rt_buffer.height != entry.rt_size.height) ||
                           (rt_buffer.channels != channels) ||
                           (rt_buffer.type != dtype) ||
                           (!rt_buffer.data);
        if (needs_alloc) {
            rt_buffer.width = entry.rt_size.width;
            rt_buffer.height = entry.rt_size.height;
            rt_buffer.channels = channels;
            rt_buffer.type = dtype;
            rt_buffer.device = Device::CPU;
            size_t pixel_size = sizeof(float);
            size_t row_bytes = static_cast<size_t>(rt_buffer.width) * channels * pixel_size;
            rt_buffer.step = row_bytes;
            rt_buffer.data.reset(new char[row_bytes * rt_buffer.height], std::default_delete<char[]>());
            std::memset(rt_buffer.data.get(), 0, row_bytes * rt_buffer.height);
        }

        try {
            std::visit([&](auto&& fn) {
                using T = std::decay_t<decltype(fn)>;
                if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                    NodeOutput result = fn(node, image_inputs_ready);
                    if (result.image_buffer.width > 0 && result.image_buffer.height > 0) {
                        cv::Mat result_mat = toCvMat(result.image_buffer);
                        if (result_mat.cols != entry.rt_size.width || result_mat.rows != entry.rt_size.height) {
                            cv::resize(result_mat, result_mat,
                                       cv::Size(entry.rt_size.width, entry.rt_size.height),
                                       0, 0, cv::INTER_LINEAR);
                        }
                        cv::Mat dest = toCvMat(rt_buffer);
                        result_mat(entry.roi_rt).copyTo(dest(entry.roi_rt));
                    }
                    node.cached_output_real_time->data = result.data;
                } else if constexpr (std::is_same_v<T, TileOpFunc>) {
                    if (image_inputs_ready.empty()) {
                        throw GraphError(GraphErrc::MissingDependency,
                                         "RT tiled op requires image inputs for node " + std::to_string(nid));
                    }
                    TileTask task;
                    task.node = &node;
                    task.output_tile.buffer = &rt_buffer;
                    const cv::Size out_bounds(rt_buffer.width, rt_buffer.height);
                    const int halo_rt = entry.halo_rt;
                    for (int y = entry.roi_rt.y; y < entry.roi_rt.y + entry.roi_rt.height; y += kRtTileSize) {
                        for (int x = entry.roi_rt.x; x < entry.roi_rt.x + entry.roi_rt.width; x += kRtTileSize) {
                            int tile_w = std::min(kRtTileSize, entry.roi_rt.x + entry.roi_rt.width - x);
                            int tile_h = std::min(kRtTileSize, entry.roi_rt.y + entry.roi_rt.height - y);
                            cv::Rect tile_roi(x, y, tile_w, tile_h);
                            tile_roi = clip_rect(tile_roi, out_bounds);
                            if (is_rect_empty(tile_roi)) continue;
                            task.output_tile.roi = tile_roi;
                            task.input_tiles.clear();
                            for (const NodeOutput* input_out : image_inputs_ready) {
                                Tile input_tile;
                                input_tile.buffer = const_cast<ImageBuffer*>(&input_out->image_buffer);
                                cv::Rect input_roi = tile_roi;
                                if (halo_rt > 0) {
                                    input_roi = expand_rect(input_roi, halo_rt);
                                }
                                input_roi = clip_rect(input_roi,
                                                      cv::Size(input_out->image_buffer.width,
                                                               input_out->image_buffer.height));
                                if (is_rect_empty(input_roi)) {
                                    input_roi = clip_rect(tile_roi,
                                                          cv::Size(input_out->image_buffer.width,
                                                                   input_out->image_buffer.height));
                                }
                                input_tile.roi = input_roi;
                                task.input_tiles.push_back(input_tile);
                            }
                            execute_tile_task(task, fn);
                        }
                    }
                }
            }, *op_variant);
        } catch (const cv::Exception& e) {
            throw GraphError(GraphErrc::ComputeError,
                             "RT compute failed at node " + std::to_string(nid) + ": " + std::string(e.what()));
        } catch (const GraphError&) {
            throw;
        } catch (const std::exception& e) {
            throw GraphError(GraphErrc::ComputeError,
                             "RT compute failed at node " + std::to_string(nid) + ": " + std::string(e.what()));
        }

        if (node.rt_roi.has_value()) {
            node.rt_roi = clip_rect(merge_rect(*node.rt_roi, entry.roi_hp), entry.hp_size);
        } else {
            node.rt_roi = entry.roi_hp;
        }
        node.rt_version++;
        push_compute_event(node.id, node.name, "rt_update", 0.0);
    };

    for (int nid : execution_order) {
        auto it = plan.find(nid);
        if (it == plan.end()) continue;
        compute_node_rt(nid, it->second);
    }

    Node& target = nodes.at(node_id);
    if (!target.cached_output_real_time) {
        throw GraphError(GraphErrc::ComputeError, "RT compute finished without target output.");
    }
    return *target.cached_output_real_time;
}

NodeOutput& NodeGraph::compute_parallel(
    GraphRuntime& runtime,
    int node_id, 
    const std::string& cache_precision,
    bool force_recache, 
    bool enable_timing,
    bool disable_disk_cache,
    std::vector<BenchmarkEvent>* benchmark_events)
{
    // [健壮性修复] 将整个设置过程包裹在 try...catch 中
    try {
        if (!has_node(node_id)) {
            throw GraphError(GraphErrc::NotFound, "Cannot compute: node " + std::to_string(node_id) + " not found.");
        }

        if (enable_timing) {
            clear_timing_results();
            total_io_time_ms = 0.0;
        }

        auto execution_order = topo_postorder_from(node_id);
        std::unordered_set<int> execution_set(execution_order.begin(), execution_order.end());

        if (force_recache) {
            std::scoped_lock lock(graph_mutex_);
            for (int id : execution_order) {
                if (nodes.count(id)) {
                    nodes.at(id).cached_output.reset();
                }
            }
        }
        
        // --- 核心修复：建立稀疏 ID 到稠密索引的映射 ---
        std::unordered_map<int, int> id_to_idx;
        id_to_idx.reserve(execution_order.size());
        for (size_t i = 0; i < execution_order.size(); ++i) {
            id_to_idx[execution_order[i]] = i;
        }

        size_t num_nodes = execution_order.size();
        
        // --- 使用基于稠密索引的 std::vector 进行依赖管理 ---
        std::vector<std::atomic<int>> dependency_counters(num_nodes);
        std::vector<std::vector<int>> dependents_map(num_nodes);
        std::vector<Task> all_tasks;
        all_tasks.resize(num_nodes);
        // Scheme B: temporary results storage per node index
        std::vector<std::optional<NodeOutput>> temp_results(num_nodes);
        // Pre-resolve ops on main thread to avoid concurrent registry access
        std::vector<std::optional<OpRegistry::OpVariant>> resolved_ops(num_nodes);
        std::vector<std::optional<OpMetadata>> resolved_meta(num_nodes);
        for (size_t i = 0; i < num_nodes; ++i) {
            const auto& n = nodes.at(execution_order[i]);
            resolved_ops[i] = OpRegistry::instance().resolve_for_intent(n.type, n.subtype, ComputeIntent::GlobalHighPrecision);
            resolved_meta[i] = OpRegistry::instance().get_metadata(n.type, n.subtype);
        }
        
        for (size_t i = 0; i < num_nodes; ++i) {
            int current_node_id = execution_order[i];
            auto& node = nodes.at(current_node_id);
            int num_deps = 0;
            
            auto count_dep = [&](int dep_id) {
                if (dep_id != -1 && execution_set.count(dep_id)) {
                    // 使用稠密索引构建依赖关系
                    int dep_idx = id_to_idx.at(dep_id);
                    dependents_map[dep_idx].push_back(i); // i 是当前节点的稠密索引
                    num_deps++;
                }
            };
            
            for (const auto& input : node.image_inputs) count_dep(input.from_node_id);
            for (const auto& input : node.parameter_inputs) count_dep(input.from_node_id);

            dependency_counters[i] = num_deps;
        }
        
        // --- 为每个节点（按稠密索引）创建任务 ---
        for (size_t i = 0; i < num_nodes; ++i) {
            int current_node_id = execution_order[i];
            int current_node_idx = i;

            auto inner_task = [this, &runtime, &dependency_counters, &dependents_map, &execution_order, &all_tasks,
                               &id_to_idx, &temp_results, &resolved_ops, &resolved_meta, current_node_id, current_node_idx,
                               cache_precision, enable_timing, disable_disk_cache, benchmark_events, force_recache] () {
                // 1) Pure compute into temp_results without mutating NodeGraph
                try {
                    const Node& target_node = nodes.at(current_node_id);
                    bool allow_disk_cache = (!disable_disk_cache) && (!force_recache);

                    // Ensure upstream writes visible
                    std::atomic_thread_fence(std::memory_order_acquire);

                    auto get_upstream_output = [&](int up_id) -> const NodeOutput* {
                        if (up_id < 0) return nullptr;
                        auto itn = nodes.find(up_id);
                        if (itn == nodes.end()) return nullptr;
                        auto it_idx = id_to_idx.find(up_id);
                        if (it_idx != id_to_idx.end()) {
                            int up_idx = it_idx->second;
                            if (temp_results[up_idx].has_value()) return &*temp_results[up_idx];
                        }
                        if (itn->second.cached_output.has_value()) return &*itn->second.cached_output;
                        return nullptr;
                    };

                    // Memory or disk fast path
                    if (!target_node.cached_output.has_value() && allow_disk_cache && !temp_results[current_node_idx].has_value()) {
                        NodeOutput from_disk;
                        if (try_load_from_disk_cache_into(target_node, from_disk)) {
                            temp_results[current_node_idx] = std::move(from_disk);
                            if (enable_timing) {
                                // Log a zero-cost event indicating disk cache hit (IO time tracked separately)
                                BenchmarkEvent ev;
                                ev.node_id = current_node_id;
                                ev.op_name = make_key(target_node.type, target_node.subtype);
                                ev.dependency_start_time = std::chrono::high_resolution_clock::now();
                                ev.execution_start_time = ev.dependency_start_time;
                                ev.execution_end_time = ev.execution_start_time;
                                ev.execution_duration_ms = 0.0;
                                ev.source = "disk_cache";
                                if (benchmark_events) {
                                    std::lock_guard lk(timing_mutex_);
                                    benchmark_events->push_back(ev);
                                }
                                {
                                    std::lock_guard lk(timing_mutex_);
                                    timing_results.node_timings.push_back({ target_node.id, target_node.name, 0.0, "disk_cache" });
                                }
                                push_compute_event(target_node.id, target_node.name, "disk_cache", 0.0);
                            }
                        }
                    }

                    if (!target_node.cached_output.has_value() && !temp_results[current_node_idx].has_value()) {
                        YAML::Node runtime_params = target_node.parameters ? YAML::Clone(target_node.parameters)
                                                                           : YAML::Node(YAML::NodeType::Map);
                        for (const auto& p_input : target_node.parameter_inputs) {
                            if (p_input.from_node_id < 0) continue;
                            auto const* up_out = get_upstream_output(p_input.from_node_id);
                            if (!up_out) {
                                throw GraphError(GraphErrc::MissingDependency, "Parameter input not ready for node " + std::to_string(current_node_id));
                            }
                            auto it = up_out->data.find(p_input.from_output_name);
                            if (it == up_out->data.end()) {
                                throw GraphError(GraphErrc::MissingDependency,
                                    "Node " + std::to_string(p_input.from_node_id) + " missing output '" + p_input.from_output_name + "'");
                            }
                            runtime_params[p_input.to_parameter_name] = it->second;
                        }

                        std::vector<const NodeOutput*> inputs_ready;
                        inputs_ready.reserve(target_node.image_inputs.size());
                        for (const auto& i_input : target_node.image_inputs) {
                            if (i_input.from_node_id < 0) continue;
                            auto const* up_out = get_upstream_output(i_input.from_node_id);
                            if (!up_out) {
                                throw GraphError(GraphErrc::MissingDependency, "Image input not ready for node " + std::to_string(current_node_id));
                            }
                            inputs_ready.push_back(up_out);
                        }

                        const auto& op_opt = resolved_ops[current_node_idx];
                        if (!op_opt.has_value()) {
                            throw GraphError(GraphErrc::NoOperation, "No op for " + target_node.type + ":" + target_node.subtype);
                        }

                        BenchmarkEvent current_event;
                        current_event.node_id = current_node_id;
                        current_event.op_name = make_key(target_node.type, target_node.subtype);
                        current_event.dependency_start_time = std::chrono::high_resolution_clock::now();
                        current_event.execution_start_time = current_event.dependency_start_time;

                        // Build execution node with resolved runtime parameters
                        Node node_for_exec = target_node;
                        node_for_exec.runtime_parameters = runtime_params;

                        NodeOutput result;
                        bool tiled_dispatched = false;
                        try {
                            std::visit([&](auto&& op_func) {
                                using T = std::decay_t<decltype(op_func)>;
                                if constexpr (std::is_same_v<T, MonolithicOpFunc>) {
                                    result = op_func(node_for_exec, inputs_ready);
                                } else if constexpr (std::is_same_v<T, TileOpFunc>) {
                                    // Build normalized inputs that must outlive tile micro-tasks
                                    std::vector<NodeOutput> normalized_storage_local;
                                    std::vector<const NodeOutput*> inputs_for_tiling = inputs_ready;
                                    bool is_mixing = (target_node.type == "image_mixing");
                                    if (is_mixing && inputs_ready.size() >= 2) {
                                        const auto& base_buffer = inputs_ready[0]->image_buffer;
                                        if (base_buffer.width == 0 || base_buffer.height == 0) {
                                            throw GraphError(GraphErrc::InvalidParameter, "Base image for image_mixing is empty.");
                                        }
                                        const int base_w = base_buffer.width;
                                        const int base_h = base_buffer.height;
                                        const int base_c = base_buffer.channels;
                                        const std::string strategy = as_str(node_for_exec.runtime_parameters, "merge_strategy", "resize");
                                        normalized_storage_local.reserve(inputs_ready.size() - 1);
                                        for (size_t i = 1; i < inputs_ready.size(); ++i) {
                                            const auto& current_buffer = inputs_ready[i]->image_buffer;
                                            if (current_buffer.width == 0 || current_buffer.height == 0) {
                                                throw GraphError(GraphErrc::InvalidParameter, "Secondary image for image_mixing is empty.");
                                            }
                                            cv::Mat current_mat = toCvMat(current_buffer);
                                            if (current_mat.cols != base_w || current_mat.rows != base_h) {
                                                if (strategy == "resize") {
                                                    cv::resize(current_mat, current_mat, cv::Size(base_w, base_h), 0, 0, cv::INTER_LINEAR);
                                                } else if (strategy == "crop") {
                                                    cv::Rect crop_roi(0, 0, std::min(current_mat.cols, base_w), std::min(current_mat.rows, base_h));
                                                    cv::Mat cropped = cv::Mat::zeros(base_h, base_w, current_mat.type());
                                                    current_mat(crop_roi).copyTo(cropped(crop_roi));
                                                    current_mat = cropped;
                                                } else {
                                                    throw GraphError(GraphErrc::InvalidParameter, "Unsupported merge_strategy for tiled mixing.");
                                                }
                                            }
                                            if (current_mat.channels() != base_c) {
                                                if (current_mat.channels() == 1 && (base_c == 3 || base_c == 4)) {
                                                    std::vector<cv::Mat> planes(base_c, current_mat);
                                                    cv::merge(planes, current_mat);
                                                } else if ((current_mat.channels() == 3 || current_mat.channels() == 4) && base_c == 1) {
                                                    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2GRAY);
                                                } else if (current_mat.channels() == 4 && base_c == 3) {
                                                    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGRA2BGR);
                                                } else if (current_mat.channels() == 3 && base_c == 4) {
                                                    cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2BGRA);
                                                } else {
                                                    throw GraphError(GraphErrc::InvalidParameter, "Unsupported channel conversion in tiled mixing.");
                                                }
                                            }
                                            NodeOutput tmp; tmp.image_buffer = fromCvMat(current_mat);
                                            normalized_storage_local.push_back(std::move(tmp));
                                            inputs_for_tiling[i] = &normalized_storage_local.back();
                                        }
                                    }

                                    // Prepare shared store for normalized inputs to extend lifetime
                                    auto norm_store_sp = std::make_shared<std::vector<NodeOutput>>(std::move(normalized_storage_local));
                                    // Build input_ptrs that reference shared store for secondary images
                                    std::vector<const NodeOutput*> input_ptrs = inputs_ready;
                                    if (is_mixing && inputs_ready.size() >= 2) {
                                        for (size_t i = 1, k = 0; i < inputs_ready.size(); ++i, ++k) {
                                            if (k < norm_store_sp->size()) input_ptrs[i] = &(*norm_store_sp)[k];
                                        }
                                    }

                                    // Infer output shape
                                    int out_w = input_ptrs.empty() ? as_int_flexible(node_for_exec.runtime_parameters, "width", 256)
                                                                   : input_ptrs[0]->image_buffer.width;
                                    int out_h = input_ptrs.empty() ? as_int_flexible(node_for_exec.runtime_parameters, "height", 256)
                                                                   : input_ptrs[0]->image_buffer.height;
                                    int out_c = input_ptrs.empty() ? 1 : input_ptrs[0]->image_buffer.channels;
                                    auto out_t = input_ptrs.empty() ? ps::DataType::FLOAT32 : input_ptrs[0]->image_buffer.type;

                                    // Allocate output buffer in temp_results (visible for tiles)
                                    temp_results[current_node_idx] = NodeOutput{};
                                    auto& ob = temp_results[current_node_idx]->image_buffer;
                                    ob.width = out_w; ob.height = out_h; ob.channels = out_c; ob.type = out_t;
                                    ob.step = static_cast<size_t>(out_w) * out_c * sizeof(float);
                                    ob.data.reset(new char[ob.step * ob.height], std::default_delete<char[]>());

                                    // Tile size from metadata preference
                                    int tile_size = 128;
                                    if (resolved_meta[current_node_idx].has_value()) {
                                        auto pref = resolved_meta[current_node_idx]->tile_preference;
                                        if (pref == TileSizePreference::MICRO) tile_size = 16;
                                        else if (pref == TileSizePreference::MACRO) tile_size = 256;
                                    }
                                    const bool needs_halo = (node_for_exec.type == "image_process" && node_for_exec.subtype.find("gaussian_blur") != std::string::npos);
                                    const int HALO_SIZE = 16;

                                    // Plan tiles and spawn micro tasks
                                    int tiles_x = (out_w + tile_size - 1) / tile_size;
                                    int tiles_y = (out_h + tile_size - 1) / tile_size;
                                    int total_tiles = tiles_x * tiles_y;
                                    runtime.inc_graph_tasks_to_complete(total_tiles);
                                    auto remaining = std::make_shared<std::atomic<int>>(total_tiles);
                                    auto start_tp = std::make_shared<std::chrono::high_resolution_clock::time_point>(std::chrono::high_resolution_clock::now());

                                    for (int ty = 0; ty < tiles_y; ++ty) {
                                        for (int tx = 0; tx < tiles_x; ++tx) {
                                            int x = tx * tile_size;
                                            int y = ty * tile_size;
                                            int w = std::min(tile_size, out_w - x);
                                            int h = std::min(tile_size, out_h - y);
                                            Task tile_task = [this, &runtime, &dependents_map, &dependency_counters, &all_tasks, &temp_results,
                                                              x, y, w, h, needs_halo, HALO_SIZE, input_ptrs, norm_store_sp, remaining, start_tp,
                                                              current_node_idx, current_node_id, node_for_exec, op_func, benchmark_events, enable_timing]() {
                                                try {
                                                    TileTask tt;
                                                    tt.node = &node_for_exec;
                                                    tt.output_tile.buffer = &temp_results[current_node_idx]->image_buffer;
                                                    tt.output_tile.roi = cv::Rect(x, y, w, h);
                                                    for (auto const* in_out : input_ptrs) {
                                                        Tile in_tile;
                                                        in_tile.buffer = const_cast<ImageBuffer*>(&in_out->image_buffer);
                                                        in_tile.roi = needs_halo ? calculate_halo(tt.output_tile.roi, HALO_SIZE, {in_out->image_buffer.width, in_out->image_buffer.height})
                                                                                 : tt.output_tile.roi;
                                                        tt.input_tiles.push_back(std::move(in_tile));
                                                    }
                                                    execute_tile_task(tt, op_func);
                                                    if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                                        std::atomic_thread_fence(std::memory_order_release);
                                                        if (enable_timing) {
                                                            auto end_tp = std::chrono::high_resolution_clock::now();
                                                            double exec_ms = std::chrono::duration<double, std::milli>(end_tp - *start_tp).count();
                                                            BenchmarkEvent ev; ev.node_id = current_node_id; ev.op_name = make_key(node_for_exec.type, node_for_exec.subtype);
                                                            ev.execution_start_time = *start_tp; ev.dependency_start_time = *start_tp; ev.execution_end_time = end_tp; ev.execution_duration_ms = exec_ms; ev.source = "computed";
                                                            if (benchmark_events) {
                                                                std::lock_guard lk(timing_mutex_);
                                                                benchmark_events->push_back(ev);
                                                            }
                                                            {
                                                                std::lock_guard lk(timing_mutex_);
                                                                timing_results.node_timings.push_back({ current_node_id, nodes.at(current_node_id).name, exec_ms, std::string("computed") });
                                                            }
                                                            push_compute_event(current_node_id, nodes.at(current_node_id).name, "computed", exec_ms);
                                                        } else {
                                                            push_compute_event(current_node_id, nodes.at(current_node_id).name, "computed", 0.0);
                                                        }
                                                        for (int dependent_idx : dependents_map[current_node_idx]) {
                                                            if (--dependency_counters[dependent_idx] == 0) {
                                                                runtime.submit_ready_task_from_worker(std::move(all_tasks[dependent_idx]));
                                                            }
                                                        }
                                                    }
                                                } catch (const std::exception& e) {
                                                    runtime.set_exception(std::make_exception_ptr(
                                                        GraphError(GraphErrc::ComputeError, std::string("Tile stage at node ") + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: " + e.what())
                                                    ));
                                                } catch (...) {
                                                    runtime.set_exception(std::make_exception_ptr(
                                                        GraphError(GraphErrc::ComputeError, std::string("Tile stage at node ") + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: unknown exception")
                                                    ));
                                                }
                                                runtime.dec_graph_tasks_to_complete();
                                            };
                                            runtime.submit_ready_task_from_worker(std::move(tile_task));
                                        }
                                    }
                                    tiled_dispatched = true;
                                }
                            }, *op_opt);
                        } catch (const std::exception& e) {
                            throw;
                        }

                        if (tiled_dispatched) {
                            // Tiled micro-tasks handle timing and dependent scheduling; skip monolithic finalization.
                            return;
                        }
                        temp_results[current_node_idx] = std::move(result);

                        if (enable_timing) {
                            current_event.execution_end_time = std::chrono::high_resolution_clock::now();
                            double ms = std::chrono::duration<double, std::milli>(current_event.execution_end_time - current_event.execution_start_time).count();
                            current_event.source = "computed";
                            current_event.execution_duration_ms = ms;
                            if (benchmark_events) {
                                std::lock_guard lk(timing_mutex_);
                                benchmark_events->push_back(current_event);
                            }
                            {
                                std::lock_guard lk(timing_mutex_);
                                timing_results.node_timings.push_back({ target_node.id, target_node.name, ms, "computed" });
                            }
                            push_compute_event(target_node.id, target_node.name, "computed", ms);
                        } else {
                            push_compute_event(target_node.id, target_node.name, "computed", 0.0);
                        }
                    }
                } catch (const cv::Exception& e) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Compute stage at node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: " + std::string(e.what()))
                    ));
                    return;
                } catch (const std::exception& e) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Compute stage at node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: " + e.what())
                    ));
                    return;
                } catch (...) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Compute stage at node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: unknown exception")
                    ));
                    return;
                }

                // 2) 触发后续依赖任务（捕获并精确标注阶段）
                try {
                    // Ensure all writes to temp_results are visible before scheduling dependents
                    std::atomic_thread_fence(std::memory_order_release);
                    for (int dependent_idx : dependents_map[current_node_idx]) {
                        if (--dependency_counters[dependent_idx] == 0) {
                            runtime.submit_ready_task_from_worker(std::move(all_tasks[dependent_idx]));
                        }
                    }
                } catch (const std::out_of_range& e) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Scheduling stage after node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: out_of_range: " + std::string(e.what()))
                    ));
                } catch (const std::exception& e) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Scheduling stage after node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: " + e.what())
                    ));
                } catch (...) {
                    runtime.set_exception(std::make_exception_ptr(
                        GraphError(GraphErrc::ComputeError, "Scheduling stage after node " + std::to_string(current_node_id) + " (" + nodes.at(current_node_id).name + ") failed: unknown exception")
                    ));
                }
            };
            
            // --- 包装任务以处理异常和完成计数 ---
            all_tasks[i] = [inner_task = std::move(inner_task), &runtime]() {
                inner_task();
                runtime.dec_graph_tasks_to_complete();
            };
        }
        
        // --- 提交初始就绪任务 ---
        std::vector<Task> initial_tasks;
        for (size_t i = 0; i < num_nodes; ++i) {
            if (dependency_counters[i] == 0) {
                initial_tasks.push_back(std::move(all_tasks[i]));
            }
        }

        if (execution_order.empty() && has_node(node_id)) {
             // 处理图中只有一个节点的情况
            if(!nodes.at(node_id).cached_output.has_value()){
                 std::unordered_map<int, bool> visiting;
                 compute_internal(node_id, cache_precision, visiting, enable_timing, !disable_disk_cache, benchmark_events);
            }
        } else {
            runtime.submit_initial_tasks(std::move(initial_tasks), execution_order.size());
            runtime.wait_for_completion();
        }


        // --- 后续处理（计时、结果返回） ---
        if (enable_timing) {
            double total = 0.0;
            {
                std::lock_guard lk(timing_mutex_);
                for(const auto& timing : timing_results.node_timings) {
                    total += timing.elapsed_ms;
                }
                timing_results.total_ms = total;
            }
        }

        // Commit results: write back to nodes and save caches
        {
            std::scoped_lock lock(graph_mutex_);
            for (size_t i = 0; i < num_nodes; ++i) {
                if (temp_results[i].has_value()) {
                    int nid = execution_order[i];
                    nodes.at(nid).cached_output = std::move(*temp_results[i]);
                    try {
                        nodes.at(nid).cached_output_high_precision = *nodes.at(nid).cached_output;
                        nodes.at(nid).hp_version++;
                    } catch (...) {
                        // Non-fatal; maintain backward compatibility
                    }
                    save_cache_if_configured(nodes.at(nid), cache_precision);
                }
            }
        }

        if (!nodes.at(node_id).cached_output) {
            throw GraphError(GraphErrc::ComputeError, "Parallel computation finished but target node has no output. An upstream error likely occurred.");
        }
        return *nodes.at(node_id).cached_output;

    } catch (...) {
        // 捕获在本函数内（任务提交前）抛出的异常，并传递给运行时
        runtime.set_exception(std::current_exception());
        // 等待运行时处理异常并唤醒
        runtime.wait_for_completion();
        // wait_for_completion 内部会重新抛出异常，这里我们只需确保函数有返回值
        // 在实际情况下，由于 rethrow，代码不会执行到这里
        throw GraphError(GraphErrc::Unknown, "Caught pre-flight exception during parallel compute setup.");
    }
}

// Phase 1 overload: intent-based entry to parallel compute
NodeOutput& NodeGraph::compute_parallel(
    GraphRuntime& runtime,
    ComputeIntent intent,
    int node_id,
    const std::string& cache_precision,
    bool force_recache,
    bool enable_timing,
    bool disable_disk_cache,
    std::vector<BenchmarkEvent>* benchmark_events,
    std::optional<cv::Rect> dirty_roi)
{
    switch (intent) {
        case ComputeIntent::GlobalHighPrecision:
            // Use existing Global HP path (monolithic preferred; else MACRO tiled)
            return compute_parallel(runtime, node_id, cache_precision, force_recache, enable_timing, disable_disk_cache, benchmark_events);
        case ComputeIntent::RealTimeUpdate:
            if (!dirty_roi.has_value()) {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "RealTimeUpdate intent requires a dirty ROI region.");
            }
            return compute_real_time_update(&runtime, node_id, cache_precision,
                                            force_recache, enable_timing,
                                            disable_disk_cache, benchmark_events,
                                            *dirty_roi);
        default:
            return compute_parallel(runtime, node_id, cache_precision, force_recache, enable_timing, disable_disk_cache, benchmark_events);
    }
}

} // namespace ps
