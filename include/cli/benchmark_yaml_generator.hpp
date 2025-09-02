#pragma once

#include <string>
#include <yaml-cpp/yaml.h>

namespace ps {

/**
 * @struct GraphGenConfig
 * @brief 配置参数，用于动态生成计算图的YAML定义。
 */
struct GraphGenConfig {
    std::string input_op_type = "image_generator:constant";
    std::string main_op_type = "image_process:gaussian_blur";
    int width = 256;
    int height = 256;
    int chain_length = 1; // 图中主要操作节点的串联数量
    int num_outputs = 1;  // 从最后一个操作节点引出的输出节点数量 (用于测试扇出)
};

/**
 * @class YamlGenerator
 * @brief 根据配置动态生成图的YAML定义。
 */
class YamlGenerator {
public:
    static YAML::Node Generate(const GraphGenConfig& config);
};

} // namespace ps