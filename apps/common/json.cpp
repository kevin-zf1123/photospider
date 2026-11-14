#include "common/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ps::tools {
namespace {
[[noreturn]] void type_error(const char* expected) {
  throw ToolError(std::string("expected JSON ") + expected);
}
void validate(Json value, std::size_t depth, std::size_t* remaining) {
  if (depth > 128 || !(*remaining)--)
    throw ToolError("JSON nesting or node limit exceeded");
  if (value.is_raw())
    throw ToolError("JSON number exceeds supported numeric range");
  // yyjson retains duplicate keys: reject before any object lookup.
  if (value.is_object()) {
    std::set<std::string> keys;
    for (const auto& item : value.object()) {
      if (!keys.insert(item.first).second)
        throw ToolError("duplicate JSON key: " + item.first);
      validate(item.second, depth + 1, remaining);
    }
  } else if (value.is_array()) {
    for (auto child : value.array())
      validate(child, depth + 1, remaining);
  }
}
}  // namespace
Json Json::optional(const std::string& key) const {
  if (!yyjson_is_obj(value_))
    type_error("object");
  return Json(yyjson_obj_getn(value_, key.data(), key.size()));
}
Json Json::at(const std::string& key) const {
  auto result = optional(key);
  if (!result)
    throw ToolError("missing JSON field: " + key);
  return result;
}
std::string Json::text() const {
  if (!yyjson_is_str(value_))
    type_error("string");
  return {yyjson_get_str(value_), yyjson_get_len(value_)};
}
std::uint64_t Json::u64() const {
  if (!yyjson_is_int(value_) ||
      (yyjson_is_sint(value_) && yyjson_get_sint(value_) < 0))
    type_error("unsigned integer");
  return yyjson_get_uint(value_);
}
std::int64_t Json::i64() const {
  if (!yyjson_is_int(value_) ||
      (yyjson_is_uint(value_) && yyjson_get_uint(value_) > INT64_MAX))
    type_error("signed 64-bit integer");
  return yyjson_get_sint(value_);
}
double Json::real() const {
  if (!yyjson_is_num(value_))
    type_error("number");
  if (yyjson_is_int(value_)) {
    auto magnitude = yyjson_is_sint(value_) && yyjson_get_sint(value_) < 0
                         ? std::uint64_t{0} - static_cast<std::uint64_t>(
                                                  yyjson_get_sint(value_))
                         : yyjson_get_uint(value_);
    auto reduced = magnitude;
    unsigned shift = 0;
    while (reduced > (std::uint64_t{1} << 53)) {
      reduced >>= 1;
      ++shift;
    }
    if (shift && (magnitude & ((std::uint64_t{1} << shift) - 1)))
      throw ToolError(
          "integer cannot be represented exactly as f64; use f64_bits or a "
          "floating literal");
  }
  const auto result = yyjson_get_num(value_);
  if (!std::isfinite(result))
    throw ToolError("nonfinite JSON number");
  return result;
}
bool Json::boolean() const {
  if (!yyjson_is_bool(value_))
    type_error("boolean");
  return yyjson_get_bool(value_);
}
std::vector<Json> Json::array() const {
  if (!yyjson_is_arr(value_))
    type_error("array");
  std::vector<Json> result;
  std::size_t i, size;
  yyjson_val* child;
  yyjson_arr_foreach(value_, i, size, child) result.emplace_back(child);
  return result;
}
std::vector<std::pair<std::string, Json>> Json::object() const {
  if (!yyjson_is_obj(value_))
    type_error("object");
  std::vector<std::pair<std::string, Json>> result;
  std::size_t i, size;
  yyjson_val *key, *value;
  yyjson_obj_foreach(value_, i, size, key, value) {
    result.emplace_back(std::string(yyjson_get_str(key), yyjson_get_len(key)),
                        Json(value));
  }
  return result;
}
void Json::fields(std::initializer_list<const char*> allowed) const {
  for (const auto& field : object())
    if (std::none_of(allowed.begin(), allowed.end(),
                     [&](const char* name) { return field.first == name; }))
      throw ToolError("unknown JSON field: " + field.first);
}
JsonDocument::JsonDocument(const std::string& text) {
  yyjson_read_err error{};
  owner_.reset(yyjson_read_opts(const_cast<char*>(text.data()), text.size(),
                                YYJSON_READ_BIGNUM_AS_RAW, nullptr, &error));
  if (!owner_) {
    std::size_t line = 1, column = 1;
    for (std::size_t i = 0; i < error.pos && i < text.size(); ++i) {
      if (text[i] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
    }
    throw ToolError("JSON " + std::to_string(line) + ":" +
                    std::to_string(column) + ": " +
                    (error.msg ? error.msg : "parse failed"));
  }
  std::size_t remaining = 4194304;
  validate(root(), 0, &remaining);
}
JsonWriter::JsonWriter(InvalidUtf8 invalid_utf8) : invalid_utf8_(invalid_utf8) {
  owner_.reset(yyjson_mut_doc_new(nullptr));
  if (!owner_)
    throw std::bad_alloc();
}
yyjson_mut_val* JsonWriter::str(const std::string& value) {
  // Semantic names and schema identities must never be silently rewritten.
  // Diagnostic renderers explicitly permit replacing non-UTF-8 system text.
  std::string safe;
  for (std::size_t i = 0; i < value.size();) {
    auto c = static_cast<unsigned char>(value[i]);
    std::size_t n = c < 128                ? 1
                    : c >= 194 && c <= 223 ? 2
                    : c >= 224 && c <= 239 ? 3
                    : c >= 240 && c <= 244 ? 4
                                           : 0;
    bool valid = n && i + n <= value.size();
    for (std::size_t j = 1; valid && j < n; ++j) {
      auto b = static_cast<unsigned char>(value[i + j]);
      valid = b >= 128 && b <= 191;
      if (j == 1)
        valid = valid && !(c == 224 && b < 160) && !(c == 237 && b >= 160) &&
                !(c == 240 && b < 144) && !(c == 244 && b > 143);
    }
    if (valid) {
      safe.append(value, i, n);
      i += n;
    } else {
      if (invalid_utf8_ == InvalidUtf8::Reject)
        throw ToolError("cannot encode non-UTF-8 semantic JSON text", 1,
                        "PS-JSON-UTF8");
      safe += "\xef\xbf\xbd";
      ++i;
    }
  }
  auto* result = yyjson_mut_strncpy(owner_.get(), safe.data(), safe.size());
  if (!result)
    throw std::bad_alloc();
  return result;
}
void JsonWriter::put(yyjson_mut_val* object, const std::string& key,
                     yyjson_mut_val* value) {
  if (!value || !yyjson_mut_obj_add(object, str(key), value))
    throw std::bad_alloc();
}
void JsonWriter::push(yyjson_mut_val* array, yyjson_mut_val* value) {
  if (!value || !yyjson_mut_arr_append(array, value))
    throw std::bad_alloc();
}
std::string JsonWriter::finish(yyjson_mut_val* root, bool pretty) {
  yyjson_mut_doc_set_root(owner_.get(), root);
  std::size_t size = 0;
  char* data = yyjson_mut_write(
      owner_.get(), pretty ? YYJSON_WRITE_PRETTY_TWO_SPACES : 0, &size);
  if (!data)
    throw std::bad_alloc();
  std::unique_ptr<char, decltype(&std::free)> owner(data, std::free);
  return std::string(data, size) + "\n";
}
}  // namespace ps::tools
