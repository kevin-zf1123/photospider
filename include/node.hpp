#pragma once
#include "ps_types.hpp"

namespace ps {

/**
 * @class Node
 * @brief 表示图中的一个节点，包含节点的基本属性、输入输出端口、参数配置以及缓存数据。
 *
 * 该类主要包含以下成员：
 * - id：节点的唯一标识符，初始值为 -1。
 * - name：节点的名称，用于标识和描述此节点。
 * - type：节点的类型，可用以区分不同的功能或类别。
 * - subtype：节点的子类型，用于进一步细分节点类别。
 *
 * 输入部分：
 * - image_inputs：图像输入列表，用于接收来自其他节点的图像数据。
 * - parameter_inputs：参数输入列表，用于接收传递给节点的动态参数。
 *
 * 参数部分：
 * - parameters：从 YAML 文件中读取的静态参数。
 * - runtime_parameters：结合了静态参数与动态参数输入形成的运行时参数集合。
 *
 * 输出部分：
 * - outputs：存储节点输出数据的列表。
 *
 * 缓存部分：
 * - caches：缓存条目列表，用于存储中间结果或其他缓存数据。
 * - cached_output：节点输出的完整缓存，如果存在则可防止重复计算。
 *
 * 其他成员：
 * - preserved：标记该节点是否应被保留，防止被强制重新计算。
 *
 * 静态和成员函数：
 * - from_yaml：根据给定的 YAML 数据构造一个 Node 对象。
 * - to_yaml：将 Node 对象序列化为 YAML 格式的数据。
 */
class Node {
public:
    int id = -1;
    std::string name;
    std::string type;
    std::string subtype;

    // --- MODIFIED: Inputs are now split into two categories for clarity and function ---
    std::vector<ImageInput> image_inputs;
    std::vector<ParameterInput> parameter_inputs;
    
    // --- DEPRECATED: Old unified input model ---
    // std::vector<InputPort> inputs; 

    // Static parameters defined in the YAML file.
    YAML::Node parameters;
    
    // --- NEW: Parameters populated at runtime from upstream nodes ---
    // This is a combination of static `parameters` and dynamic `parameter_inputs`.
    YAML::Node runtime_parameters;

    std::vector<OutputPort> outputs;
    std::vector<CacheEntry> caches;
    // this is an indicator on whether the node should be prevented from force computing
    bool preserved = false;

    // --- MODIFIED: The in-memory cache now holds the entire NodeOutput ---
    // Legacy unified cache (kept for backward compatibility with existing code paths)
    std::optional<NodeOutput> cached_output;

    // Phase 1: Dual-cache state for RT/HP separation (not yet fully used by planner)
    std::optional<NodeOutput> cached_output_real_time;      // RT cache for interactive preview
    std::optional<NodeOutput> cached_output_high_precision; // HP cache for final quality
    int rt_version = 0;
    int hp_version = 0;
    std::optional<cv::Rect> rt_roi; // Most recent RT dirty/updated ROI
    std::optional<cv::Rect> hp_roi; // Most recent HP dirty/updated ROI

    // --- DEPRECATED: Old image-only cache ---
    // cv::Mat image_matrix;

    static Node from_yaml(const YAML::Node& n);
    YAML::Node to_yaml() const;
};

} // namespace ps
