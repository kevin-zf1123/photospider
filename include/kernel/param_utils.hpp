#pragma once
#include <yaml-cpp/yaml.h>

#include <new>
#include <string>

namespace ps {

/**
 * @brief 从 YAML 节点中提取 double，并对可恢复转换失败使用默认值。
 *
 * @param n YAML 节点（通常是 runtime_parameters）。
 * @param key 要查找的键。
 * @param defv 键不存在或类型不匹配时的默认值。
 * @return 成功转换的值，否则返回 defv。
 * @throws std::bad_alloc 如果 YAML 查找或转换耗尽内存。
 * @note 其他 YAML/标准异常保留既有宽松参数语义并转换为默认值。
 */
inline double as_double_flexible(const YAML::Node& n, const std::string& key,
                                 double defv) {
  if (!n || !n[key])
    return defv;
  try {
    if (n[key].IsScalar())
      return n[key].as<double>();
    return defv;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return defv;
  }
}

/**
 * @brief 从 YAML 节点中提取 int，并对可恢复转换失败使用默认值。
 *
 * @param n YAML 节点（通常是 runtime_parameters）。
 * @param key 要查找的键。
 * @param defv 键不存在或类型不匹配时的默认值。
 * @return 成功转换的值，否则返回 defv。
 * @throws std::bad_alloc 如果 YAML 查找或转换耗尽内存。
 * @note 其他 YAML/标准异常保留既有宽松参数语义并转换为默认值。
 */
inline int as_int_flexible(const YAML::Node& n, const std::string& key,
                           int defv) {
  if (!n || !n[key])
    return defv;
  try {
    return n[key].as<int>();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return defv;
  }
}

/**
 * @brief 从 YAML 节点中提取 string，并对可恢复转换失败使用默认值。
 *
 * @param n YAML 节点（通常是 runtime_parameters）。
 * @param key 要查找的键。
 * @param defv 键不存在或类型不匹配时的默认值。
 * @return 成功转换的字符串，否则返回 defv 的副本。
 * @throws std::bad_alloc 如果 YAML 查找、转换或字符串复制耗尽内存。
 * @note 其他 YAML/标准异常保留既有宽松参数语义并转换为默认值。
 */
inline std::string as_str(const YAML::Node& n, const std::string& key,
                          const std::string& defv = {}) {
  if (!n || !n[key])
    return defv;
  try {
    return n[key].as<std::string>();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return defv;
  }
}

}  // namespace ps
