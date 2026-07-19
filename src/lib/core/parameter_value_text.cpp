#include "core/parameter_value_text.hpp"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ps::core {
namespace {

/**
 * @brief Appends one quoted string using the inspection escape grammar.
 *
 * @param value Raw string or object-key text.
 * @param output Destination display buffer.
 * @return Nothing.
 * @throws std::bad_alloc when destination growth is exhausted.
 * @note Quotation marks, backslashes, common controls, and remaining C0
 * controls are escaped; other bytes are preserved verbatim.
 */
void append_quoted(const std::string& value, std::string& output) {
  static constexpr char kHex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (character < 0x20U) {
          output += "\\u00";
          output.push_back(kHex[(character >> 4U) & 0x0fU]);
          output.push_back(kHex[character & 0x0fU]);
        } else {
          output.push_back(static_cast<char>(character));
        }
        break;
    }
  }
  output.push_back('"');
}

/**
 * @brief Formats one double with the stable scalar inspection policy.
 *
 * @param value Floating-point value, including supported NaN or infinities.
 * @return Locale-independent text with round-trip precision.
 * @throws std::bad_alloc if stream storage allocation is exhausted.
 */
std::string format_double(double value) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(std::numeric_limits<double>::max_digits10)
         << value;
  return output.str();
}

/**
 * @brief Recursively appends one parameter value to inspection text.
 *
 * @param value Value to format.
 * @param quote_strings Whether a string alternative must be quoted.
 * @param output Destination display buffer.
 * @return Nothing.
 * @throws std::bad_alloc when output/stream allocation is exhausted.
 * @throws std::logic_error for an unknown parameter kind.
 * @note Object iteration follows `ParameterValue::Object` ordering and is
 * therefore deterministic.
 */
void append_parameter_value(const plugin::ParameterValue& value,
                            bool quote_strings, std::string& output) {
  switch (value.kind()) {
    case plugin::ParameterKind::Null:
      output += "null";
      return;
    case plugin::ParameterKind::Bool:
      output += value.as_bool() ? "true" : "false";
      return;
    case plugin::ParameterKind::Int64:
      output += std::to_string(value.as_int64());
      return;
    case plugin::ParameterKind::Double:
      output += format_double(value.as_double());
      return;
    case plugin::ParameterKind::String:
      if (quote_strings) {
        append_quoted(value.as_string(), output);
      } else {
        output += value.as_string();
      }
      return;
    case plugin::ParameterKind::Array: {
      output.push_back('[');
      bool first = true;
      for (const auto& element : value.as_array()) {
        if (!first) {
          output += ", ";
        }
        first = false;
        append_parameter_value(element, true, output);
      }
      output.push_back(']');
      return;
    }
    case plugin::ParameterKind::Object: {
      output.push_back('{');
      bool first = true;
      for (const auto& [key, element] : value.as_object()) {
        if (!first) {
          output += ", ";
        }
        first = false;
        append_quoted(key, output);
        output += ": ";
        append_parameter_value(element, true, output);
      }
      output.push_back('}');
      return;
    }
  }
  throw std::logic_error("Unknown public parameter kind");
}

}  // namespace

/** @copydoc format_parameter_value_for_inspection */
std::string format_parameter_value_for_inspection(
    const plugin::ParameterValue& value) {
  std::string output;
  append_parameter_value(value, false, output);
  return output;
}

}  // namespace ps::core
