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
#include <variant>
#include <opencv2/opencv.hpp>
#include <yaml-cpp/yaml.h>
#include "image_buffer.hpp"

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

struct NodeOutput {
    ps::ImageBuffer image_buffer;
    std::unordered_map<std::string, OutputValue> data;
};

#if defined(_WIN32)
    #if defined(PHOTOSPIDER_LIB_BUILD)
        #define PHOTOSPIDER_API __declspec(dllexport)
    #else
        #define PHOTOSPIDER_API __declspec(dllimport)
    #endif
#else // Non-Windows platforms
    #if defined(PHOTOSPIDER_LIB_BUILD)
        #define PHOTOSPIDER_API __attribute__((visibility("default")))
    #else
        #define PHOTOSPIDER_API
    #endif
#endif

enum class GraphErrc {
    Unknown = 1, NotFound, Cycle, Io, InvalidYaml,
    MissingDependency, NoOperation, InvalidParameter, ComputeError,
};
struct PHOTOSPIDER_API GraphError : public std::runtime_error {
    explicit GraphError(const std::string& what)
        : std::runtime_error(what), code_(GraphErrc::Unknown) {}
    GraphError(GraphErrc code, const std::string& what)
        : std::runtime_error(what), code_(code) {}
    GraphErrc code() const noexcept { return code_; }
private:
    GraphErrc code_;
};

// struct GraphError : public std::runtime_error {
//     explicit GraphError(const std::string& what)
//         : std::runtime_error(what), code_(GraphErrc::Unknown) {}
//     GraphError(GraphErrc code, const std::string& what)
//         : std::runtime_error(what), code_(code) {}
//     GraphErrc code() const noexcept { return code_; }
// private:
//     GraphErrc code_;
// };

class Node;
class NodeGraph;

// --- 新增: 定义操作元数据 ---
// 用于描述一个操作对分块大小的偏好
enum class TileSizePreference {
    UNDEFINED, // 未定义或不适用 (例如 Monolithic 操作)
    MICRO,     // 偏好小分块 (例如 16x16)，适用于交互式、低延迟任务
    MACRO      // 偏好大分块 (例如 256x256)，适用于吞吐量优先的批处理任务
};

// [核心修复] 移除此处的 Device 枚举定义，因为它已在 image_buffer.hpp 中定义


struct OpMetadata {
    TileSizePreference tile_preference = TileSizePreference::UNDEFINED;
    // [新增] 新增 device_preference 字段，默认为 CPU
    Device device_preference = Device::CPU; 
};


using MonolithicOpFunc = std::function<NodeOutput(const Node&, const std::vector<const NodeOutput*>&)>;
using TileOpFunc = std::function<void(const Node&, const Tile&, const std::vector<Tile>&)>;

class OpRegistry {
public:
    static OpRegistry& instance();
    
    using OpVariant = std::variant<MonolithicOpFunc, TileOpFunc>;

    // [修改] 重载 register_op 以接收元数据
    void register_op(const std::string& type, const std::string& subtype, MonolithicOpFunc fn, OpMetadata meta = {});
    void register_op(const std::string& type, const std::string& subtype, TileOpFunc fn, OpMetadata meta); // Tiled 操作必须提供元数据

    std::optional<OpVariant> find(const std::string& type, const std::string& subtype) const;
    
    // [新增] 获取元数据
    std::optional<OpMetadata> get_metadata(const std::string& type, const std::string& subtype) const;

    std::vector<std::string> get_keys() const;
    bool unregister_op(const std::string& type, const std::string& subtype);
    bool unregister_key(const std::string& key);
private:
    std::unordered_map<std::string, OpVariant> table_;
    // [修改] 元数据表现在可以存储包含设备偏好的完整 OpMetadata
    std::unordered_map<std::string, OpMetadata> metadata_table_;
};

inline std::string make_key(const std::string& type, const std::string& subtype) {
    return type + ":" + subtype;
}

} // namespace ps