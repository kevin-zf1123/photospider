#include "adapters/yaml/parameter_value_yaml.hpp"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ps::adapters::yaml::internal {
namespace {

/**
 * @brief Checks whether one YAML tag explicitly denotes a string.
 * @param tag yaml-cpp tag text.
 * @return True for quoted/non-specific or standard string tags.
 * @throws Nothing.
 */
bool is_string_tag(const std::string& tag) noexcept {
  return tag == "!" || tag == "!!str" || tag == "tag:yaml.org,2002:str";
}

/**
 * @brief Checks whether yaml-cpp marked a scalar for plain implicit inference.
 * @param tag yaml-cpp tag text.
 * @return True for empty or non-specific plain tags.
 * @throws Nothing.
 * @note Quoted `!` and explicit/custom tags are excluded.
 */
bool is_plain_tag(const std::string& tag) noexcept {
  return tag.empty() || tag == "?";
}

/**
 * @brief Checks the YAML core null lexical forms.
 * @param text Scalar text to validate.
 * @return True for empty, tilde, or case-standard null spelling.
 * @throws Nothing.
 */
bool is_null_text(const std::string& text) noexcept {
  return text.empty() || text == "~" || text == "null" || text == "Null" ||
         text == "NULL";
}

/**
 * @brief Checks whether a plain scalar has YAML integer lexical shape.
 * @param text Scalar text after yaml-cpp tokenization.
 * @return True for signed decimal, binary, octal, or hexadecimal integers.
 * @throws Nothing.
 * @note This classifier runs after failed int64 conversion so an out-of-range
 * integer cannot silently fall through to a rounded double alternative.
 */
bool is_integer_lexeme(const std::string& text) noexcept {
  std::size_t index = 0;
  if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
    ++index;
  }
  if (index == text.size()) {
    return false;
  }
  int base = 10;
  if (text.size() - index > 2 && text[index] == '0') {
    const char prefix = text[index + 1];
    if (prefix == 'x' || prefix == 'X') {
      base = 16;
      index += 2;
    } else if (prefix == 'o' || prefix == 'O') {
      base = 8;
      index += 2;
    } else if (prefix == 'b' || prefix == 'B') {
      base = 2;
      index += 2;
    }
  }
  bool saw_digit = false;
  for (; index < text.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(text[index]);
    if (character == '_') {
      continue;
    }
    int digit = -1;
    if (std::isdigit(character)) {
      digit = character - static_cast<unsigned char>('0');
    } else if (std::isxdigit(character)) {
      digit = std::tolower(character) - static_cast<unsigned char>('a') + 10;
    }
    if (digit < 0 || digit >= base) {
      return false;
    }
    saw_digit = true;
  }
  return saw_digit;
}

/**
 * @brief Checks whether a plain scalar has decimal real lexical shape.
 * @param text Scalar text after yaml-cpp tokenization.
 * @return True for a decimal mantissa containing a dot or exponent.
 * @throws Nothing.
 * @note This classifier runs only after yaml-cpp cannot decode a double. It
 * distinguishes overflowing real numbers from ordinary strings so numeric
 * overflow cannot silently change the public parameter kind.
 */
bool is_real_lexeme(const std::string& text) noexcept {
  std::size_t index = 0;
  if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
    ++index;
  }

  bool saw_mantissa_digit = false;
  while (index < text.size() &&
         (std::isdigit(static_cast<unsigned char>(text[index])) ||
          text[index] == '_')) {
    saw_mantissa_digit = saw_mantissa_digit || text[index] != '_';
    ++index;
  }

  bool saw_dot = false;
  if (index < text.size() && text[index] == '.') {
    saw_dot = true;
    ++index;
    while (index < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[index])) ||
            text[index] == '_')) {
      saw_mantissa_digit = saw_mantissa_digit || text[index] != '_';
      ++index;
    }
  }

  bool saw_exponent = false;
  if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
    saw_exponent = true;
    ++index;
    if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
      ++index;
    }
    bool saw_exponent_digit = false;
    while (index < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[index])) ||
            text[index] == '_')) {
      saw_exponent_digit = saw_exponent_digit || text[index] != '_';
      ++index;
    }
    if (!saw_exponent_digit) {
      return false;
    }
  }

  return index == text.size() && saw_mantissa_digit &&
         (saw_dot || saw_exponent);
}

/**
 * @brief Checks explicit YAML spellings for infinity and NaN.
 * @param text Scalar text to inspect.
 * @return True only for signed `.inf` or `.nan`, case-insensitively.
 * @throws Nothing.
 */
bool is_explicit_nonfinite_lexeme(const std::string& text) noexcept {
  std::size_t index = 0;
  if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
    ++index;
  }
  if (text.size() - index != 4 || text[index] != '.') {
    return false;
  }
  const char first = static_cast<char>(
      std::tolower(static_cast<unsigned char>(text[index + 1])));
  const char second = static_cast<char>(
      std::tolower(static_cast<unsigned char>(text[index + 2])));
  const char third = static_cast<char>(
      std::tolower(static_cast<unsigned char>(text[index + 3])));
  return (first == 'i' && second == 'n' && third == 'f') ||
         (first == 'n' && second == 'a' && third == 'n');
}

/**
 * @brief Decodes a double without accepting accidental overflow to infinity.
 * @param value YAML scalar to convert.
 * @param text Exact scalar spelling used to distinguish explicit non-finite
 * values.
 * @return Parsed double.
 * @throws YAML::BadConversion for an invalid value or non-finite overflow.
 */
double validated_double(const YAML::Node& value, const std::string& text) {
  const double result = value.as<double>();
  if (!std::isfinite(result) && !is_explicit_nonfinite_lexeme(text)) {
    throw YAML::BadConversion(value.Mark());
  }
  return result;
}

/**
 * @brief Checks whether one YAML tag explicitly denotes a scalar kind.
 * @param tag yaml-cpp tag text.
 * @param short_name Standard YAML scalar kind name.
 * @return True for the short or full standard tag.
 * @throws std::bad_alloc if comparison text allocation fails.
 */
bool has_yaml_tag(const std::string& tag, const std::string& short_name) {
  return tag == "!!" + short_name || tag == "tag:yaml.org,2002:" + short_name;
}

/**
 * @brief Tries yaml-cpp's complete scalar conversion for an untagged value.
 * @tparam Value Numeric scalar type to decode.
 * @param value Scalar YAML node.
 * @return Decoded value, or nullopt when the plain scalar is not that type.
 * @throws std::bad_alloc unchanged from yaml-cpp diagnostic/storage work.
 * @note Only ordinary YAML conversion errors are treated as a type miss.
 * Explicitly tagged scalars bypass this helper and propagate conversion errors.
 */
template <typename Value>
std::optional<Value> try_plain_scalar(const YAML::Node& value) {
  try {
    return value.as<Value>();
  } catch (const YAML::Exception&) {
    return std::nullopt;
  }
}

/**
 * @brief Converts one scalar YAML node to its public alternative.
 * @param value Scalar YAML node.
 * @return Public scalar value.
 * @throws YAML::Exception for an invalid explicitly tagged scalar.
 * @throws std::bad_alloc unchanged from string/tag allocation.
 * @note Explicit standard tags suppress plain inference. Unknown explicit
 * tags are rejected instead of silently changing their value to a string.
 */
plugin::ParameterValue scalar_parameter_value(const YAML::Node& value) {
  const std::string text = value.Scalar();
  const std::string tag = value.Tag();
  if (is_string_tag(tag)) {
    return plugin::ParameterValue(text);
  }
  if (has_yaml_tag(tag, "null")) {
    if (!is_null_text(text)) {
      throw YAML::BadConversion(value.Mark());
    }
    return plugin::ParameterValue(nullptr);
  }
  if (has_yaml_tag(tag, "bool")) {
    return plugin::ParameterValue(value.as<bool>());
  }
  if (has_yaml_tag(tag, "int")) {
    return plugin::ParameterValue(value.as<std::int64_t>());
  }
  if (has_yaml_tag(tag, "float")) {
    return plugin::ParameterValue(validated_double(value, text));
  }
  if (!is_plain_tag(tag)) {
    throw YAML::BadConversion(value.Mark());
  }
  if (is_null_text(text)) {
    return plugin::ParameterValue(nullptr);
  }
  if (text == "true" || text == "True" || text == "TRUE" || text == "false" ||
      text == "False" || text == "FALSE") {
    return plugin::ParameterValue(text == "true" || text == "True" ||
                                  text == "TRUE");
  }
  if (const auto integer = try_plain_scalar<std::int64_t>(value)) {
    return plugin::ParameterValue(*integer);
  }
  if (is_integer_lexeme(text)) {
    throw YAML::BadConversion(value.Mark());
  }
  if (const auto real = try_plain_scalar<double>(value)) {
    if (!std::isfinite(*real) && !is_explicit_nonfinite_lexeme(text)) {
      throw YAML::BadConversion(value.Mark());
    }
    return plugin::ParameterValue(*real);
  }
  if (is_real_lexeme(text)) {
    throw YAML::BadConversion(value.Mark());
  }
  return plugin::ParameterValue(text);
}

/**
 * @brief Formats one numeric key deterministically.
 * @param value Integer or double key value.
 * @return Canonical decimal key text.
 * @throws std::invalid_argument for a non-numeric value.
 * @throws std::bad_alloc if text storage allocation fails.
 * @note Negative zero is normalized to `0`; doubles use max_digits10.
 */
std::string canonical_numeric_key(const plugin::ParameterValue& value) {
  if (value.is_int64()) {
    return std::to_string(value.as_int64());
  }
  if (!value.is_double()) {
    throw std::invalid_argument("YAML mapping key is not numeric");
  }
  const double number = value.as_double();
  if (number == 0.0) {
    return "0";
  }
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(std::numeric_limits<double>::max_digits10)
         << number;
  return output.str();
}

/**
 * @brief Converts one scalar YAML key to public string-key form.
 * @param key YAML mapping key.
 * @return Deterministic owned string key.
 * @throws std::invalid_argument for a non-scalar or unsupported key.
 * @throws std::bad_alloc unchanged from scalar conversion.
 */
std::string normalized_mapping_key(const YAML::Node& key) {
  if (!key.IsScalar()) {
    throw std::invalid_argument(
        "Operation parameters require scalar YAML mapping keys");
  }
  plugin::ParameterValue converted = scalar_parameter_value(key);
  if (converted.is_int64() || converted.is_double()) {
    return canonical_numeric_key(converted);
  }
  if (converted.is_string()) {
    return converted.as_string();
  }
  if (converted.is_bool()) {
    return converted.as_bool() ? "true" : "false";
  }
  if (converted.is_null()) {
    return "null";
  }
  throw std::invalid_argument("Unsupported YAML mapping key kind");
}

}  // namespace

/** @copydoc ps::adapters::yaml::internal::parameter_value_from_yaml */
plugin::ParameterValue parameter_value_from_yaml(const YAML::Node& value) {
  if (!value || value.IsNull()) {
    return plugin::ParameterValue(nullptr);
  }
  if (value.IsScalar()) {
    return scalar_parameter_value(value);
  }
  if (value.IsSequence()) {
    plugin::ParameterValue::Array result;
    result.reserve(value.size());
    for (const auto& element : value) {
      result.push_back(parameter_value_from_yaml(element));
    }
    return plugin::ParameterValue(std::move(result));
  }
  if (value.IsMap()) {
    return plugin::ParameterValue(parameter_map_from_yaml(value));
  }
  throw std::invalid_argument("Unsupported YAML parameter node kind");
}

/** @copydoc ps::adapters::yaml::internal::parameter_map_from_yaml */
plugin::ParameterMap parameter_map_from_yaml(const YAML::Node& value) {
  plugin::ParameterMap result;
  if (!value || value.IsNull()) {
    return result;
  }
  if (!value.IsMap()) {
    throw std::invalid_argument("Operation parameters must be a YAML mapping");
  }
  for (const auto& entry : value) {
    std::string key = normalized_mapping_key(entry.first);
    const auto inserted =
        result.emplace(std::move(key), parameter_value_from_yaml(entry.second));
    if (!inserted.second) {
      throw std::invalid_argument(
          "Operation parameter keys collide after numeric normalization");
    }
  }
  return result;
}

/** @copydoc ps::adapters::yaml::internal::parameter_value_to_yaml */
YAML::Node parameter_value_to_yaml(const plugin::ParameterValue& value) {
  switch (value.kind()) {
    case plugin::ParameterKind::Null: {
      YAML::Node result(YAML::NodeType::Null);
      result.SetTag("!!null");
      return result;
    }
    case plugin::ParameterKind::Bool: {
      YAML::Node result(value.as_bool());
      result.SetTag("!!bool");
      return result;
    }
    case plugin::ParameterKind::Int64: {
      YAML::Node result(value.as_int64());
      result.SetTag("!!int");
      return result;
    }
    case plugin::ParameterKind::Double: {
      YAML::Node result(value.as_double());
      result.SetTag("!!float");
      return result;
    }
    case plugin::ParameterKind::String: {
      YAML::Node result(value.as_string());
      result.SetTag("!!str");
      return result;
    }
    case plugin::ParameterKind::Array: {
      YAML::Node result(YAML::NodeType::Sequence);
      for (const auto& element : value.as_array()) {
        result.push_back(parameter_value_to_yaml(element));
      }
      return result;
    }
    case plugin::ParameterKind::Object: {
      YAML::Node result(YAML::NodeType::Map);
      for (const auto& [key, element] : value.as_object()) {
        YAML::Node yaml_key(key);
        yaml_key.SetTag("!!str");
        result[yaml_key] = parameter_value_to_yaml(element);
      }
      return result;
    }
  }
  throw std::invalid_argument("Unknown public parameter kind");
}

/** @copydoc ps::adapters::yaml::internal::parameter_map_to_yaml */
YAML::Node parameter_map_to_yaml(const plugin::ParameterMap& values) {
  YAML::Node result(YAML::NodeType::Map);
  for (const auto& [key, value] : values) {
    YAML::Node yaml_key(key);
    yaml_key.SetTag("!!str");
    result[yaml_key] = parameter_value_to_yaml(value);
  }
  return result;
}

}  // namespace ps::adapters::yaml::internal
