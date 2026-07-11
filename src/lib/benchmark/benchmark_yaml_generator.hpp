#pragma once

#include <yaml-cpp/yaml.h>

#include <string>

#include "benchmark/benchmark_types.hpp"  // <--- 关键修改：包含定义而不是重新定义

namespace ps {

// <--- 关键修改：删除这里的 GraphGenConfig 结构体定义

/**
 * @class YamlGenerator
 * @brief 根据配置动态生成图的YAML定义。
 */
class YamlGenerator {
 public:
  static YAML::Node Generate(const GraphGenConfig& config);
};

}  // namespace ps