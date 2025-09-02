#include "cli/benchmark_yaml_generator.hpp"

namespace ps {

YAML::Node YamlGenerator::Generate(const GraphGenConfig& config) {
    YAML::Node root(YAML::NodeType::Sequence);
    
    // 1. 创建输入节点
    YAML::Node input_node;
    size_t colon_pos = config.input_op_type.find(':');
    input_node["id"] = 0;
    input_node["name"] = "GeneratedInput";
    input_node["type"] = config.input_op_type.substr(0, colon_pos);
    input_node["subtype"] = config.input_op_type.substr(colon_pos + 1);
    input_node["parameters"]["width"] = config.width;
    input_node["parameters"]["height"] = config.height;
    root.push_back(input_node);

    int last_node_id = 0;

    // 2. 创建主操作节点链
    for (int i = 0; i < config.chain_length; ++i) {
        int current_node_id = i + 1;
        YAML::Node main_node;
        size_t op_colon_pos = config.main_op_type.find(':');
        main_node["id"] = current_node_id;
        main_node["name"] = "GeneratedMainOp_" + std::to_string(i);
        main_node["type"] = config.main_op_type.substr(0, op_colon_pos);
        main_node["subtype"] = config.main_op_type.substr(op_colon_pos + 1);

        YAML::Node image_inputs(YAML::NodeType::Sequence);
        YAML::Node input_ref;
        input_ref["from_node_id"] = last_node_id;
        image_inputs.push_back(input_ref);
        
        // 如果操作需要两个输入 (如 image_mixing)，则复制第一个输入
        if (main_node["type"].as<std::string>() == "image_mixing") {
            image_inputs.push_back(input_ref);
        }

        main_node["image_inputs"] = image_inputs;
        root.push_back(main_node);
        last_node_id = current_node_id;
    }

    // 3. 创建输出/分析节点 (扇出)
    for (int i = 0; i < config.num_outputs; ++i) {
        int current_node_id = last_node_id + 1 + i;
        YAML::Node output_node;
        output_node["id"] = current_node_id;
        output_node["name"] = "GeneratedOutput_" + std::to_string(i);
        output_node["type"] = "analyzer"; // 使用一个轻量级操作作为终点
        output_node["subtype"] = "get_dimensions";

        YAML::Node image_inputs(YAML::NodeType::Sequence);
        YAML::Node input_ref;
        input_ref["from_node_id"] = last_node_id;
        image_inputs.push_back(input_ref);
        output_node["image_inputs"] = image_inputs;
        
        // 为输出节点添加缓存配置，以便 `save` 等操作可以找到它
        YAML::Node caches(YAML::NodeType::Sequence);
        YAML::Node cache_entry;
        cache_entry["cache_type"] = "image";
        cache_entry["location"] = "output_" + std::to_string(i) + ".png";
        caches.push_back(cache_entry);
        output_node["caches"] = caches;

        root.push_back(output_node);
    }

    return root;
}

} // namespace ps