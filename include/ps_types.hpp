// in: include/ps_types.hpp (OVERWRITE)
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <optional>
#include <sstream>
#include <iostream>
#include <variant> // ADD: 用于支持多种函数签名
#include <opencv2/opencv.hpp>
#include <yaml-cpp/yaml.h>
#include "image_buffer.hpp"
#include <variant>

namespace ps {
namespace fs = std::filesystem;

using OutputValue = YAML::Node;

struct ImageInput {
    int from_node_id = -1;
    std::string from_output_name = "image";
};

struct ParameterInput {
    int from_node_id = -1;
    std::string from_output_name;
    std::string to_parameter_name;
};

struct OutputPort {
    int output_id = -1;
    std::string output_type;
    YAML::Node output_parameters;
};

struct CacheEntry {
    std::string cache_type;
    std::string location;
};

enum class ExecutionPolicy {
    MONOLITHIC,       // 必须作为一个整体、不可分割的任务来计算
    TILED_PARALLEL,   // 可以安全地分解为并行的分块任务
    // TILED_SEQUENTIAL // (未来可以添加)
};

struct NodeOutput {
    ps::ImageBuffer image_buffer;
    std::unordered_map<std::string, OutputValue> data;
};

enum class GraphErrc {
    Unknown = 1, NotFound, Cycle, Io, InvalidYaml,
    MissingDependency, NoOperation, InvalidParameter, ComputeError,
};

struct GraphError : public std::runtime_error {
    explicit GraphError(const std::string& what)
        : std::runtime_error(what), code_(GraphErrc::Unknown) {}
    GraphError(GraphErrc code, const std::string& what)
        : std::runtime_error(what), code_(code) {}
    GraphErrc code() const noexcept { return code_; }
private:
    GraphErrc code_;
};

class Node;
class NodeGraph;

// --- NEW: 定义两种操作函数签名 ---
// 用于需要一次性处理完整图像的节点 (例如：加载、全局分析)
using MonolithicOpFunc = std::function<NodeOutput(const Node&, const std::vector<const NodeOutput*>&)>;
// 用于可以分块计算的节点 (例如：滤镜、逐像素操作)
using TileOpFunc = std::function<void(const Node&, const Tile&, const std::vector<Tile>&)>;

class OpRegistry {
public:
    static OpRegistry& instance();
    
    // NEW: 使用 std::variant 存储不同类型的函数
    using OpVariant = std::variant<MonolithicOpFunc, TileOpFunc>;

    void register_op(const std::string& type, const std::string& subtype, MonolithicOpFunc fn);
    void register_op(const std::string& type, const std::string& subtype, TileOpFunc fn);

    std::optional<OpVariant> find(const std::string& type, const std::string& subtype) const;
    
    std::vector<std::string> get_keys() const;
    bool unregister_op(const std::string& type, const std::string& subtype);
    bool unregister_key(const std::string& key);
private:
    std::unordered_map<std::string, OpVariant> table_;
};

inline std::string make_key(const std::string& type, const std::string& subtype) {
    return type + ":" + subtype;
}

} // namespace ps