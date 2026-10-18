#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/error.hpp"
#include "yyjson.h"  // NOLINT(build/include_subdir)

namespace ps::tools {
/** @brief Borrowed validated DOM view; its Document must remain alive. */
class Json final {
 public:
  explicit Json(yyjson_val* value = nullptr) : value_(value) {}
  explicit operator bool() const { return value_ != nullptr; }
  Json optional(const std::string& key) const;
  Json at(const std::string& key) const;
  std::string text() const;
  std::uint64_t u64() const;
  std::int64_t i64() const;
  double real() const;
  bool boolean() const;
  std::vector<Json> array() const;
  std::vector<std::pair<std::string, Json>> object() const;
  void fields(std::initializer_list<const char*> allowed) const;
  bool is_object() const { return yyjson_is_obj(value_); }
  bool is_array() const { return yyjson_is_arr(value_); }
  bool null() const { return yyjson_is_null(value_); }
  bool is_string() const { return yyjson_is_str(value_); }
  bool is_bool() const { return yyjson_is_bool(value_); }
  bool is_raw() const { return yyjson_is_raw(value_); }
  bool is_int() const { return yyjson_is_int(value_); }

 private:
  yyjson_val* value_;
};
class JsonDocument final {
 public:
  explicit JsonDocument(const std::string& text);
  Json root() const { return Json(yyjson_doc_get_root(owner_.get())); }

 private:
  std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> owner_{
      nullptr, yyjson_doc_free};
};
/** @brief Owned JSON writer; numeric values retain signedness and width. */
class JsonWriter final {
 public:
  enum class InvalidUtf8 { Reject, Replace };
  explicit JsonWriter(InvalidUtf8 invalid_utf8 = InvalidUtf8::Reject);
  yyjson_mut_val* object() { return yyjson_mut_obj(owner_.get()); }
  yyjson_mut_val* array() { return yyjson_mut_arr(owner_.get()); }
  yyjson_mut_val* str(const std::string& value);
  yyjson_mut_val* integer(std::uint64_t value) {
    return yyjson_mut_uint(owner_.get(), value);
  }
  yyjson_mut_val* integer(std::int64_t value) {
    return yyjson_mut_sint(owner_.get(), value);
  }
  yyjson_mut_val* boolean(bool value) {
    return yyjson_mut_bool(owner_.get(), value);
  }
  void put(yyjson_mut_val* object, const std::string& key,
           yyjson_mut_val* value);
  void push(yyjson_mut_val* array, yyjson_mut_val* value);
  std::string finish(yyjson_mut_val* root, bool pretty = false);

 private:
  InvalidUtf8 invalid_utf8_;
  std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> owner_{
      nullptr, yyjson_mut_doc_free};
};
}  // namespace ps::tools
