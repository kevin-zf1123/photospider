#pragma once
#include <string>
#include <yaml-cpp/yaml.h>

namespace ps {

/**
 * @brief 从 YAML 节点中安全地提取一个 double 值。
 * @param n YAML 节点 (通常是 runtime_parameters)。
 * @param key 要查找的键。
 * @param defv 如果键不存在或类型不匹配，返回的默认值。
 * @return 提取到的值或默认值。
 */
inline double as_double_flexible(const YAML::Node& n, const std::string& key, double defv) {
    if (!n || !n[key]) return defv;
    try {
        if (n[key].IsScalar()) return n[key].as<double>();
        return defv;
    } catch (...) {
        return defv;
    }
}

/**
 * @brief 从 YAML 节点中安全地提取一个 int 值。
 */
inline int as_int_flexible(const YAML::Node& n, const std::string& key, int defv) {
    if (!n || !n[key]) return defv;
    try {
        return n[key].as<int>();
    } catch (...) {
        return defv;
    }
}

/**
 * @brief 从 YAML 节点中安全地提取一个 string 值。
 */
inline std::string as_str(const YAML::Node& n, const std::string& key, const std::string& defv = {}) {
    if (!n || !n[key]) return defv;
    try {
        return n[key].as<std::string>();
    } catch (...) {
        return defv;
    }
}

} // namespace ps