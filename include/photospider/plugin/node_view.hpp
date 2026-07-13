#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/**
 * @file node_view.hpp
 * @brief Host-independent parameter snapshots and operation identity views.
 *
 * The declarations in this header use only the C++17 standard library. They
 * deliberately separate callback-lifetime identity strings from the owned
 * parameter tree that a plugin may retain after one invocation.
 */

namespace ps::plugin {

/**
 * @brief Identifies one public parameter-value alternative.
 *
 * @throws Nothing.
 * @note Values and the `uint32_t` representation are stable across the v2 C++
 * DSO boundary. `ParameterValue` remains a C++ value contract rather than a
 * pure-C wire representation.
 */
enum class ParameterKind : std::uint32_t {
  /** @brief Null alternative. */
  Null = 0U,
  /** @brief Boolean alternative. */
  Bool = 1U,
  /** @brief Signed 64-bit integer alternative. */
  Int64 = 2U,
  /** @brief Double-precision alternative. */
  Double = 3U,
  /** @brief Owned string alternative. */
  String = 4U,
  /** @brief Owned array alternative. */
  Array = 5U,
  /** @brief Owned object alternative. */
  Object = 6U,
};

/**
 * @brief Reports an explicit `ParameterValue` type mismatch without allocation.
 *
 * @throws Nothing. Construction, inspection, and `what()` use enum values and
 *         static storage only.
 * @note Callers can distinguish a malformed parameter contract from resource
 *       exhaustion because `std::bad_alloc` is never translated to this type.
 */
class ParameterTypeError final : public std::exception {
 public:
  /**
   * @brief Creates a no-allocation type mismatch record.
   *
   * @param expected Alternative required by the accessor.
   * @param actual Alternative currently stored by the value.
   * @throws Nothing.
   * @note Both labels are copied enum values and outlive all parameter storage.
   */
  ParameterTypeError(ParameterKind expected, ParameterKind actual) noexcept
      : expected_(expected), actual_(actual) {}

  /**
   * @brief Returns a static type-mismatch diagnostic.
   * @return Process-lifetime text containing no dynamic type labels.
   * @throws Nothing.
   * @note Use expected_kind() and kind() for programmatic details.
   */
  const char* what() const noexcept override {
    return "ParameterValue type mismatch";
  }

  /**
   * @brief Returns the alternative required by the failed accessor.
   * @return Expected parameter kind.
   * @throws Nothing.
   */
  ParameterKind expected_kind() const noexcept { return expected_; }

  /**
   * @brief Returns the alternative actually stored by the value.
   * @return Actual parameter kind.
   * @throws Nothing.
   */
  ParameterKind kind() const noexcept { return actual_; }

 private:
  /** @brief Accessor-required alternative. */
  ParameterKind expected_;
  /** @brief Value-stored alternative. */
  ParameterKind actual_;
};

/**
 * @brief Deep-owned recursive parameter value used at the operation boundary.
 *
 * A value is exactly one of null, Boolean, signed 64-bit integer, double,
 * string, array, or string-keyed object. Arrays, objects, keys, and strings own
 * all storage, so copying a value recursively detaches it from its source.
 *
 * @throws std::bad_alloc from construction, copy, or container mutation.
 * @note Conversion from implementation configuration formats is host-owned.
 *       A plugin receives no alias to the host configuration tree.
 */
class ParameterValue final {
 public:
  /** @brief Deep-owned array alternative. */
  using Array = std::vector<ParameterValue>;
  /** @brief Ordered, deep-owned object alternative. */
  using Object = std::map<std::string, ParameterValue, std::less<>>;
  /** @brief Complete recursive storage variant. */
  using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double,
                               std::string, Array, Object>;

  /**
   * @brief Creates the null alternative.
   * @throws Nothing.
   */
  ParameterValue() noexcept : storage_(nullptr) {}

  /**
   * @brief Creates the null alternative explicitly.
   * @param value Null marker; its value is ignored.
   * @throws Nothing.
   */
  ParameterValue(std::nullptr_t value) noexcept : storage_(value) {}

  /**
   * @brief Creates a Boolean value.
   * @param value Boolean payload.
   * @throws Nothing.
   */
  ParameterValue(bool value) noexcept : storage_(value) {}

  /**
   * @brief Creates a signed 64-bit value from any non-Boolean integral type.
   *
   * @tparam Integer Standard signed or unsigned integral input type, excluding
   * Boolean.
   * @param value Integral payload to normalize without truncation.
   * @throws std::overflow_error when an unsigned or extended-width signed
   * value lies outside the signed 64-bit storage range.
   * @note The template avoids overload ambiguity for ordinary integer literals
   * across LP64 and LLP64 platforms. Boolean and floating-point inputs continue
   * to select their exact non-template constructors.
   */
  template <
      typename Integer,
      std::enable_if_t<std::is_integral_v<std::remove_cv_t<Integer>> &&
                           !std::is_same_v<std::remove_cv_t<Integer>, bool>,
                       int> = 0>
  ParameterValue(Integer value) : storage_(normalize_integral(value)) {}

  /**
   * @brief Creates a floating-point value.
   * @param value Double-precision payload.
   * @throws Nothing.
   */
  ParameterValue(double value) noexcept : storage_(value) {}

  /**
   * @brief Creates a double value from another floating-point type.
   * @tparam Floating Standard floating type other than double.
   * @param value Floating payload to normalize to public double storage.
   * @throws std::overflow_error when a finite extended-precision value lies
   * outside the finite double range.
   * @note NaN and infinities remain supported double alternatives. This exact
   * constrained overload prevents long-double ambiguity with bool while the
   * ordinary double constructor remains the preferred exact match.
   */
  template <
      typename Floating,
      std::enable_if_t<std::is_floating_point_v<std::remove_cv_t<Floating>> &&
                           !std::is_same_v<std::remove_cv_t<Floating>, double>,
                       int> = 0>
  ParameterValue(Floating value) : storage_(normalize_floating(value)) {}

  /**
   * @brief Creates an owned string value.
   * @param value String payload to copy.
   * @throws std::bad_alloc if copying storage fails.
   */
  ParameterValue(const char* value)
      : storage_(std::string(value ? value : "")) {}

  /**
   * @brief Creates an owned string value.
   * @param value String payload to move.
   * @throws Nothing when the standard string move is non-throwing.
   */
  ParameterValue(std::string value) : storage_(std::move(value)) {}

  /**
   * @brief Creates an owned array value.
   * @param value Array payload to move.
   * @throws Nothing when the standard vector move is non-throwing.
   */
  ParameterValue(Array value) : storage_(std::move(value)) {}

  /**
   * @brief Creates an owned object value.
   * @param value Object payload to move.
   * @throws Nothing when the standard map move is non-throwing.
   */
  ParameterValue(Object value) : storage_(std::move(value)) {}

  /**
   * @brief Checks whether this value stores null.
   * @return True for the null alternative.
   * @throws Nothing.
   */
  bool is_null() const noexcept {
    return std::holds_alternative<std::nullptr_t>(storage_);
  }
  /**
   * @brief Checks whether this value stores a Boolean.
   * @return True for the Boolean alternative.
   * @throws Nothing.
   */
  bool is_bool() const noexcept {
    return std::holds_alternative<bool>(storage_);
  }
  /**
   * @brief Checks whether this value stores a signed integer.
   * @return True for the signed 64-bit integer alternative.
   * @throws Nothing.
   */
  bool is_int64() const noexcept {
    return std::holds_alternative<std::int64_t>(storage_);
  }
  /**
   * @brief Checks whether this value stores a double.
   * @return True for the double alternative.
   * @throws Nothing.
   */
  bool is_double() const noexcept {
    return std::holds_alternative<double>(storage_);
  }
  /**
   * @brief Checks whether this value stores a string.
   * @return True for the owned string alternative.
   * @throws Nothing.
   */
  bool is_string() const noexcept {
    return std::holds_alternative<std::string>(storage_);
  }
  /**
   * @brief Checks whether this value stores an array.
   * @return True for the owned array alternative.
   * @throws Nothing.
   */
  bool is_array() const noexcept {
    return std::holds_alternative<Array>(storage_);
  }
  /**
   * @brief Checks whether this value stores an object.
   * @return True for the owned object alternative.
   * @throws Nothing.
   */
  bool is_object() const noexcept {
    return std::holds_alternative<Object>(storage_);
  }

  /**
   * @brief Reads the Boolean payload.
   * @return Stored Boolean value.
   * @throws ParameterTypeError if this value is not Boolean.
   */
  bool as_bool() const { return require<bool>(ParameterKind::Bool); }

  /**
   * @brief Reads the signed integer payload.
   * @return Stored signed 64-bit value.
   * @throws ParameterTypeError if this value is not the Int64 alternative.
   * @note Double values, including exactly integral values, require explicit
   *       caller-side kind handling and are never converted implicitly.
   */
  std::int64_t as_int64() const {
    return require<std::int64_t>(ParameterKind::Int64);
  }

  /**
   * @brief Reads the double payload.
   * @return Stored double value.
   * @throws ParameterTypeError if this value is not the Double alternative.
   * @note Int64 values require explicit caller-side kind handling and are never
   *       converted implicitly.
   */
  double as_double() const { return require<double>(ParameterKind::Double); }

  /**
   * @brief Reads the owned string payload.
   * @return Const reference valid while this value is alive and unchanged.
   * @throws ParameterTypeError if this value is not a string.
   */
  const std::string& as_string() const {
    return require<std::string>(ParameterKind::String);
  }

  /**
   * @brief Reads the owned array payload.
   * @return Const reference valid while this value is alive and unchanged.
   * @throws ParameterTypeError if this value is not an array.
   */
  const Array& as_array() const { return require<Array>(ParameterKind::Array); }

  /**
   * @brief Reads the owned object payload.
   * @return Const reference valid while this value is alive and unchanged.
   * @throws ParameterTypeError if this value is not an object.
   */
  const Object& as_object() const {
    return require<Object>(ParameterKind::Object);
  }

  /**
   * @brief Returns the currently stored alternative label.
   * @return Exact public parameter kind.
   * @throws Nothing.
   */
  ParameterKind kind() const noexcept {
    return static_cast<ParameterKind>(storage_.index());
  }

  /**
   * @brief Returns the complete storage variant for inspection.
   * @return Const storage reference valid while this value remains alive.
   * @throws Nothing.
   * @note Prefer the explicit predicates and accessors in plugin logic.
   */
  const Storage& storage() const noexcept { return storage_; }

  /**
   * @brief Compares complete recursive value content.
   * @param other Value to compare.
   * @return True only when alternatives and all owned content match.
   * @throws Nothing under standard value/container equality.
   * @note Floating-point comparison follows ordinary double equality, including
   *       NaN behavior.
   */
  bool operator==(const ParameterValue& other) const noexcept {
    return storage_ == other.storage_;
  }

  /**
   * @brief Checks whether recursive value content differs.
   * @param other Value to compare.
   * @return Negation of operator==.
   * @throws Nothing.
   */
  bool operator!=(const ParameterValue& other) const noexcept {
    return !(*this == other);
  }

 private:
  /**
   * @brief Normalizes one non-Boolean integral input without truncation.
   *
   * @tparam Integer Standard signed or unsigned integral input type.
   * @param value Input value selected by the constrained public constructor.
   * @return Exact signed 64-bit representation.
   * @throws std::overflow_error when value is outside the signed 64-bit range.
   * @note Width comparisons are compile-time branches, so no narrowing cast is
   * evaluated until the value is proven representable.
   */
  template <typename Integer>
  static std::int64_t normalize_integral(Integer value) {
    using Value = std::remove_cv_t<Integer>;
    static_assert(std::is_integral_v<Value> && !std::is_same_v<Value, bool>,
                  "ParameterValue integral normalization excludes bool");
    if constexpr (std::is_signed_v<Value>) {
      if constexpr (std::numeric_limits<Value>::digits >
                    std::numeric_limits<std::int64_t>::digits) {
        if (value <
                static_cast<Value>(std::numeric_limits<std::int64_t>::min()) ||
            value >
                static_cast<Value>(std::numeric_limits<std::int64_t>::max())) {
          throw std::overflow_error(
              "ParameterValue integer is outside signed 64-bit range");
        }
      }
    } else {
      if constexpr (std::numeric_limits<Value>::digits >
                    std::numeric_limits<std::uint64_t>::digits) {
        if (value >
            static_cast<Value>(std::numeric_limits<std::int64_t>::max())) {
          throw std::overflow_error(
              "ParameterValue integer is outside signed 64-bit range");
        }
      } else if (static_cast<std::uint64_t>(value) >
                 static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())) {
        throw std::overflow_error(
            "ParameterValue integer is outside signed 64-bit range");
      }
    }
    return static_cast<std::int64_t>(value);
  }

  /**
   * @brief Normalizes a non-double floating input to double storage.
   * @tparam Floating Standard floating type other than double.
   * @param value Input value selected by the constrained public constructor.
   * @return Double representation, preserving NaN and infinity categories.
   * @throws std::overflow_error when a finite value exceeds double range.
   * @note Finite range comparison occurs in the wider input type before cast.
   */
  template <typename Floating>
  static double normalize_floating(Floating value) {
    using Value = std::remove_cv_t<Floating>;
    static_assert(
        std::is_floating_point_v<Value> && !std::is_same_v<Value, double>,
        "ParameterValue floating normalization excludes double");
    if (std::isfinite(value)) {
      const Value maximum =
          static_cast<Value>(std::numeric_limits<double>::max());
      if (value > maximum || value < -maximum) {
        throw std::overflow_error(
            "ParameterValue floating value is outside double range");
      }
    }
    return static_cast<double>(value);
  }

  /**
   * @brief Reads one exact alternative or reports a public type error.
   * @tparam Value Expected storage alternative.
   * @param expected Expected public alternative label.
   * @return Const reference to the stored alternative.
   * @throws ParameterTypeError when the alternative does not match.
   */
  template <typename Value>
  const Value& require(ParameterKind expected) const {
    const auto* value = std::get_if<Value>(&storage_);
    if (!value) {
      throw ParameterTypeError(expected, kind());
    }
    return *value;
  }

  /** @brief Recursive owned parameter storage. */
  Storage storage_;
};

/** @brief Canonical string-keyed effective parameter snapshot. */
using ParameterMap = ParameterValue::Object;

/**
 * @brief Borrowed operation identity plus an owned effective parameter map.
 *
 * Identity views refer to host strings that remain valid only for the current
 * callback. The parameter map is a recursive value copy and may be retained or
 * moved by plugin code independently of the callback.
 *
 * @throws std::bad_alloc when parameter construction or copying allocates.
 * @note The view is immutable to callback consumers and exposes no mutable
 *       graph, registry, cache, or configuration owner.
 */
class NodeView final {
 public:
  /**
   * @brief Creates an empty identity with an empty parameter snapshot.
   * @throws Nothing.
   */
  NodeView() = default;

  /**
   * @brief Creates a callback identity and takes an effective parameter map.
   * @param id Stable node id for the current graph lifetime.
   * @param name Borrowed display name valid through the callback.
   * @param type Borrowed operation type valid through the callback.
   * @param subtype Borrowed operation subtype valid through the callback.
   * @param parameters Deep-owned effective parameter snapshot.
   * @throws Nothing when standard container/string-view moves are non-throwing.
   */
  NodeView(int id, std::string_view name, std::string_view type,
           std::string_view subtype, ParameterMap parameters)
      : id_(id),
        name_(name),
        type_(type),
        subtype_(subtype),
        parameters_(std::move(parameters)) {}

  /**
   * @brief Returns the callback node id.
   * @return Stable id copied from the host node.
   * @throws Nothing.
   */
  int id() const noexcept { return id_; }
  /**
   * @brief Returns the callback-lifetime borrowed display name.
   * @return Immutable name view.
   * @throws Nothing.
   */
  std::string_view name() const noexcept { return name_; }
  /**
   * @brief Returns the callback-lifetime borrowed operation type.
   * @return Immutable type view.
   * @throws Nothing.
   */
  std::string_view type() const noexcept { return type_; }
  /**
   * @brief Returns the callback-lifetime borrowed operation subtype.
   * @return Immutable subtype view.
   * @throws Nothing.
   */
  std::string_view subtype() const noexcept { return subtype_; }

  /**
   * @brief Returns the complete owned effective parameter map.
   * @return Const parameter map reference valid while this view remains alive.
   * @throws Nothing; returning a reference performs no lookup or comparison.
   */
  const ParameterMap& parameters() const noexcept { return parameters_; }

  /**
   * @brief Finds one effective parameter by name.
   * @param key Parameter key to locate.
   * @return Pointer to the owned value, or nullptr when absent.
   * @throws Nothing.
   */
  const ParameterValue* find_parameter(std::string_view key) const noexcept {
    const auto found = parameters_.find(key);
    return found == parameters_.end() ? nullptr : &found->second;
  }

 private:
  /** @brief Stable id copied for callback inspection. */
  int id_ = -1;
  /** @brief Borrowed identity strings valid only through one callback. */
  std::string_view name_;
  std::string_view type_;
  std::string_view subtype_;
  /** @brief Deep-owned effective parameter snapshot. */
  ParameterMap parameters_;
};

}  // namespace ps::plugin
