// Node YAML (de)serialization moved to kernel node_module
#include "node.hpp"

namespace ps {

/**
 * @brief 将 ImageInput 对象的向量转换为 YAML 序列节点。
 *
 * 此函数遍历所提供 vector 中的每个 ImageInput 对象，并为每个对象创建相应的 YAML
 * 节点，节点包含以下键：
 * - "from_node_id"：设置为 ImageInput 对象的 from_node_id 值。
 * - "from_output_name"：如果 ImageInput 对象的 from_output_name 不为
 * "image"，则设置为该值。
 *
 * 然后将每个 YAML 节点添加到一个 YAML 序列节点中，最终返回该序列节点。
 *
 * @param inputs 对 ImageInput 对象的常量引用向量。
 * @return YAML::Node 一个包含每个 ImageInput 对象对应节点的 YAML 序列节点。
 */
static YAML::Node image_inputs_to_yaml(const std::vector<ImageInput>& inputs) {
  YAML::Node arr(YAML::NodeType::Sequence);
  for (const auto& p : inputs) {
    YAML::Node n;
    n["from_node_id"] = p.from_node_id;
    if (p.from_output_name != "image")
      n["from_output_name"] = p.from_output_name;
    arr.push_back(n);
  }
  return arr;
}
/**
 * @brief 将参数输入向量转换为 YAML 节点。
 *
 * 本函数处理一组 ParameterInput 对象，并构造一个包含这些数据的
 * YAML::Node，从而使其适合 YAML 序列化。
 *
 * @param inputs 待转换的 ParameterInput 对象向量。
 * @return YAML::Node 表示提供输入的结果 YAML 节点。
 */
static YAML::Node parameter_inputs_to_yaml(
    const std::vector<ParameterInput>& inputs) {
  YAML::Node arr(YAML::NodeType::Sequence);
  for (const auto& p : inputs) {
    YAML::Node n;
    n["from_node_id"] = p.from_node_id;
    n["from_output_name"] = p.from_output_name;
    n["to_parameter_name"] = p.to_parameter_name;
    arr.push_back(n);
  }
  return arr;
}
/**
 * @brief 将 OutputPort 对象向量转换为 YAML 序列
 *
 * 此函数遍历传入的 OutputPort 向量，为每个对象创建一个 YAML 节点，并设置
 * "output_id" 和 "output_type" 字段。 如果 output_parameters
 * 字段有效，也会将其添加到对应的 YAML 节点中。
 *
 * @param outs 一个包含 OutputPort 实例的向量，用于序列化。
 * @return YAML::Node 一个 YAML 序列，其中每个元素对应一个 OutputPort 对象。
 */
static YAML::Node ports_to_yaml(const std::vector<OutputPort>& outs) {
  YAML::Node arr(YAML::NodeType::Sequence);
  for (const auto& p : outs) {
    YAML::Node n;
    n["output_id"] = p.output_id;
    n["output_type"] = p.output_type;
    if (p.output_parameters)
      n["output_parameters"] = p.output_parameters;
    arr.push_back(n);
  }
  return arr;
}
/**
 * @brief 将 CacheEntry 对象的向量转换为 YAML 序列节点。
 *
 * 本函数遍历提供的 CacheEntry 对象向量，对于每个条目，
 * 它创建一个 YAML 节点，并根据 CacheEntry 的属性设置键 "cache_type" 和
 * "location"。 所有这些节点会被聚合到一个 YAML 序列节点中并返回。
 *
 * @param caches 包含待转换的 CacheEntry 对象的向量。
 * @return YAML::Node 一个 YAML 序列节点，其中每个元素表示一个 CacheEntry。
 */
static YAML::Node caches_to_yaml(const std::vector<CacheEntry>& caches) {
  YAML::Node arr(YAML::NodeType::Sequence);
  for (const auto& c : caches) {
    YAML::Node n;
    n["cache_type"] = c.cache_type;
    n["location"] = c.location;
    arr.push_back(n);
  }
  return arr;
}

/**
 * @brief 从 YAML 节点构造一个 Node 对象。
 *
 * 该函数解析输入的 YAML::Node，并提取各个字段来初始化 Node 对象。
 *
 * 提取和处理如下字段：
 * - id: 必须作为整数提取。
 * - name, type, subtype: 提取为字符串，如果不存在则使用默认值。
 * - preserved: 如果提供，则可选地提取布尔值。
 * - image_inputs: 如果存在，迭代序列以提取图像输入的详细信息，
 *   包括起始节点 ID 和输出名称。
 * - parameter_inputs: 如果存在，迭代序列以提取参数映射，
 *   其中 'from_output_name' 和 'to_parameter_name' 均不能为空；否则抛出
 * GraphError 异常。
 * - parameters: 如果存在，则直接赋值。
 * - outputs: 如果存在，迭代序列以提取输出端口的详细信息，
 *   包括 output_id、output_type 以及可选的 output_parameters。
 * - caches: 如果存在，迭代序列以提取缓存条目的详细信息，
 *   包括 cache_type 和 location。
 *
 * 如果任何必需的参数输入字段缺失，则该函数会抛出带有
 * GraphErrc::InvalidParameter 的 GraphError 异常。
 *
 * @param n 包含 Node 配置信息的 YAML::Node 对象。
 * @return Node 从指定 YAML 节点构造的 Node 对象。
 *
 * @throws GraphError 如果参数输入缺少必需字段。
 */
Node Node::from_yaml(const YAML::Node& n) {
  Node node;
  node.id = n["id"].as<int>();
  node.name = n["name"].as<std::string>("");
  node.type = n["type"].as<std::string>("");
  node.subtype = n["subtype"].as<std::string>("");

  if (n["preserved"]) {
    node.preserved = n["preserved"].as<bool>(false);
  }

  if (n["image_inputs"]) {
    for (const auto& it : n["image_inputs"]) {
      ImageInput p;
      p.from_node_id = it["from_node_id"].as<int>(-1);
      p.from_output_name = it["from_output_name"].as<std::string>("image");
      node.image_inputs.push_back(std::move(p));
    }
  }

  if (n["parameter_inputs"]) {
    for (const auto& it : n["parameter_inputs"]) {
      ParameterInput p;
      p.from_node_id = it["from_node_id"].as<int>(-1);
      p.from_output_name = it["from_output_name"].as<std::string>();
      p.to_parameter_name = it["to_parameter_name"].as<std::string>();
      if (p.from_output_name.empty() || p.to_parameter_name.empty()) {
        throw GraphError(GraphErrc::InvalidParameter,
                         "Parameter input for node " + std::to_string(node.id) +
                             " is missing required fields.");
      }
      node.parameter_inputs.push_back(std::move(p));
    }
  }

  if (n["parameters"]) {
    node.parameters = n["parameters"];
  }

  if (n["outputs"]) {
    for (const auto& ot : n["outputs"]) {
      OutputPort p;
      p.output_id = ot["output_id"].as<int>(-1);
      p.output_type = ot["output_type"].as<std::string>("");
      if (ot["output_parameters"]) {
        p.output_parameters = ot["output_parameters"];
      }
      node.outputs.push_back(std::move(p));
    }
  }

  if (n["caches"]) {
    for (const auto& ct : n["caches"]) {
      CacheEntry c;
      c.cache_type = ct["cache_type"].as<std::string>("");
      c.location = ct["location"].as<std::string>("");
      node.caches.push_back(std::move(c));
    }
  }

  return node;
}

/**
 * @brief 将 Node 实例转换为 YAML 表示形式。
 *
 * 此方法将节点的属性序列化为一个 YAML::Node 对象。处理的字段包括：
 * - "id": 节点的唯一标识符。
 * - "name": 节点名称。
 * - "type": 节点的主类别或类型。
 * - "subtype": 节点的子类别或子类型。
 * - "image_inputs": 如果不为空，则使用 image_inputs_to_yaml 序列化。
 * - "parameter_inputs": 如果不为空，则使用 parameter_inputs_to_yaml 序列化。
 * - "parameters": 如果 parameters 字段为非空映射，则直接赋值；否则创建一个空的
 * YAML 映射。
 * - "outputs": 如果存在，则使用 ports_to_yaml 序列化输出端口信息。
 * - "caches": 如果存在，则使用 caches_to_yaml 序列化缓存项。
 *
 * @return 一个包含节点完整配置的 YAML::Node 对象。
 */
YAML::Node Node::to_yaml() const {
  YAML::Node n;
  n["id"] = id;
  n["name"] = name;
  n["type"] = type;
  n["subtype"] = subtype;

  if (preserved)
    n["preserved"] = preserved;

  if (!image_inputs.empty())
    n["image_inputs"] = image_inputs_to_yaml(image_inputs);

  if (!parameter_inputs.empty())
    n["parameter_inputs"] = parameter_inputs_to_yaml(parameter_inputs);

  if (parameters && parameters.IsMap() && parameters.size() > 0)
    n["parameters"] = parameters;
  else
    n["parameters"] = YAML::Node(YAML::NodeType::Map);

  if (!outputs.empty())
    n["outputs"] = ports_to_yaml(outputs);

  if (!caches.empty())
    n["caches"] = caches_to_yaml(caches);

  return n;
}

}  // namespace ps
