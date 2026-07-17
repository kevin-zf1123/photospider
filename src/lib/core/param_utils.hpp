#pragma once

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "photospider/plugin/node_view.hpp"

namespace ps {

/**
 * @brief 查找 format-neutral 参数 map 中的一个值。
 *
 * @param parameters 参数 map。
 * @param key 要查找的键。
 * @return 找到时返回借用指针，否则返回 nullptr。
 * @throws Nothing.
 * @note 返回指针仅在 parameters 未被修改且仍存活时有效。
 */
inline const plugin::ParameterValue* find_parameter(
    const plugin::ParameterMap& parameters, std::string_view key) noexcept {
  const auto value = parameters.find(key);
  return value == parameters.end() ? nullptr : &value->second;
}

/**
 * @brief 将整数或可精确表示为 int 的 double 读取为 int。
 *
 * @param value 要检查的 format-neutral 参数值。
 * @return 可表示且无小数部分时的 int，否则返回 std::nullopt。
 * @throws Nothing.
 * @note Boolean 和字符串不会隐式转换为数值。
 */
inline std::optional<int> parameter_value_as_int(
    const plugin::ParameterValue& value) noexcept {
  if (value.is_int64()) {
    const std::int64_t integer = value.as_int64();
    if (integer >= std::numeric_limits<int>::min() &&
        integer <= std::numeric_limits<int>::max()) {
      return static_cast<int>(integer);
    }
    return std::nullopt;
  }
  if (value.is_double()) {
    const double number = value.as_double();
    if (std::isfinite(number) && std::trunc(number) == number &&
        number >= static_cast<double>(std::numeric_limits<int>::min()) &&
        number <= static_cast<double>(std::numeric_limits<int>::max())) {
      return static_cast<int>(number);
    }
  }
  return std::nullopt;
}

/**
 * @brief 将整数或 double 参数读取为 double。
 *
 * @param value 要检查的 format-neutral 参数值。
 * @return 对应 double；非数值参数返回 std::nullopt。
 * @throws Nothing.
 * @note Int64 转换遵循 C++ 的 double 精度规则；Boolean 和字符串不转换。
 */
inline std::optional<double> parameter_value_as_double(
    const plugin::ParameterValue& value) noexcept {
  if (value.is_double()) {
    return value.as_double();
  }
  if (value.is_int64()) {
    return static_cast<double>(value.as_int64());
  }
  return std::nullopt;
}

/**
 * @brief 从 ParameterMap 提取 double，并对类型不匹配使用默认值。
 *
 * @param parameters 静态或执行期参数 map。
 * @param key 要查找的键。
 * @param defv 键不存在或类型不匹配时的默认值。
 * @return 成功读取的 double，否则返回 defv。
 * @throws Nothing.
 * @note 只接受 Int64 和 Double，不解析字符串。
 */
inline double as_double_flexible(const plugin::ParameterMap& parameters,
                                 std::string_view key, double defv) noexcept {
  const plugin::ParameterValue* value = find_parameter(parameters, key);
  if (value == nullptr) {
    return defv;
  }
  return parameter_value_as_double(*value).value_or(defv);
}

/**
 * @brief 从 ParameterMap 提取 int，并对类型不匹配使用默认值。
 *
 * @param parameters 静态或执行期参数 map。
 * @param key 要查找的键。
 * @param defv 键不存在、溢出或类型不匹配时的默认值。
 * @return 成功读取的 int，否则返回 defv。
 * @throws Nothing.
 * @note Double 仅在有限、无小数且可表示为 int 时被接受。
 */
inline int as_int_flexible(const plugin::ParameterMap& parameters,
                           std::string_view key, int defv) noexcept {
  const plugin::ParameterValue* value = find_parameter(parameters, key);
  if (value == nullptr) {
    return defv;
  }
  return parameter_value_as_int(*value).value_or(defv);
}

/**
 * @brief 从 ParameterMap 提取面向既有内核选项的字符串表示。
 *
 * @param parameters 静态或执行期参数 map。
 * @param key 要查找的键。
 * @param defv 键不存在或值不是标量时的默认值。
 * @return String 原值，或 Bool/Int64/Double 的稳定文本；否则返回 defv。
 * @throws std::bad_alloc 如果结果字符串或格式化流分配失败。
 * @note 该宽松转换只服务于既有内核选项；operation SDK 的精确访问器
 *       仍保持严格类型检查。
 */
inline std::string as_str(const plugin::ParameterMap& parameters,
                          std::string_view key, const std::string& defv = {}) {
  const plugin::ParameterValue* value = find_parameter(parameters, key);
  if (value == nullptr) {
    return defv;
  }
  if (value->is_string()) {
    return value->as_string();
  }
  if (value->is_bool()) {
    return value->as_bool() ? "true" : "false";
  }
  if (value->is_int64()) {
    return std::to_string(value->as_int64());
  }
  if (value->is_double()) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10)
           << value->as_double();
    return stream.str();
  }
  return defv;
}

}  // namespace ps
