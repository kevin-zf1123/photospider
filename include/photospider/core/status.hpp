#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "photospider/core/export.hpp"

namespace ps {

/**
 * @brief Stable error categories returned by the public compiler/executor.
 *
 * @note Values classify recoverable product behavior. Diagnostic messages are
 * not a branching contract and may change between builds.
 */
enum class ErrorCode {
  Ok = 0,
  InvalidArgument,
  NotFound,
  Cycle,
  TypeMismatch,
  ResourceExhausted,
  Cancelled,
  Stale,
  BackendUnavailable,
  OperationFailed,
  Internal,
};

/**
 * @brief One success or recoverable failure status.
 *
 * @note The type owns its diagnostic and is safe to copy between threads.
 */
struct PHOTOSPIDER_API Status final {
  /** @brief Stable programmatic category. */
  ErrorCode code = ErrorCode::Ok;
  /** @brief Human-readable bounded diagnostic. */
  std::string message;

  /**
   * @brief Reports whether this status is canonical success.
   * @return True only when `code` is `ErrorCode::Ok`.
   * @throws Nothing.
   * @note A success should normally carry an empty message.
   */
  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }

  /**
   * @brief Constructs canonical success.
   * @return A status with `Ok` and no diagnostic.
   * @throws Nothing.
   * @note The returned value owns no external resource.
   */
  [[nodiscard]] static Status success() noexcept { return {}; }

  /**
   * @brief Constructs a recoverable failure.
   * @param error Stable non-success category.
   * @param diagnostic Human-readable diagnostic copied into the status.
   * @return An owned failure status.
   * @throws std::invalid_argument If `error` is `ErrorCode::Ok`.
   * @note Diagnostics longer than 4096 bytes are truncated by the definition.
   */
  [[nodiscard]] static Status failure(ErrorCode error, std::string diagnostic) {
    if (error == ErrorCode::Ok) {
      throw std::invalid_argument("failure status requires a non-Ok code");
    }
    if (diagnostic.size() > 4096U) {
      diagnostic.resize(4096U);
    }
    return Status{error, std::move(diagnostic)};
  }
};

/**
 * @brief Owns either a result value or one recoverable failure status.
 * @tparam T Published value type.
 *
 * @note Construction is all-or-nothing; a failure never contains a partial T.
 */
template <typename T>
class Result final {
 public:
  /**
   * @brief Constructs a successful result from an owned value.
   * @param value Value moved into this result.
   * @throws Any exception thrown by T's move construction.
   * @note The result becomes the sole owner of the moved value where T is
   * move-only.
   */
  explicit Result(T value) : value_(std::move(value)), status_() {}

  /**
   * @brief Constructs a failed result.
   * @param status Non-success status copied into this result.
   * @throws std::invalid_argument If `status` is success.
   * @note No T instance is published by this constructor.
   */
  explicit Result(Status status) : status_(std::move(status)) {
    if (status_.ok()) {
      throw std::invalid_argument("failed Result requires a failure status");
    }
  }

  /**
   * @brief Reports whether a complete value is present.
   * @return True for success and false for failure.
   * @throws Nothing.
   * @note This method is safe for read-only concurrent access.
   */
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }

  /**
   * @brief Returns the result status.
   * @return Canonical success or the owned failure.
   * @throws Nothing.
   * @note The returned reference is valid for this Result's lifetime.
   */
  [[nodiscard]] const Status& status() const noexcept { return status_; }

  /**
   * @brief Returns the published value.
   * @return Const reference to the complete value.
   * @throws std::logic_error If this Result is a failure.
   * @note The reference is invalidated when this Result is destroyed or moved.
   */
  [[nodiscard]] const T& value() const {
    if (!ok()) {
      throw std::logic_error("failed Result has no value");
    }
    return *value_;
  }

  /**
   * @brief Moves the published value out of this result.
   * @return The complete owned value.
   * @throws std::logic_error If this Result is a failure.
   * @note Call at most once when T's moved-from state is not useful.
   */
  [[nodiscard]] T take_value() {
    if (!ok()) {
      throw std::logic_error("failed Result has no value");
    }
    return std::move(*value_);
  }

 private:
  /** @brief Value storage engaged only for successful results. */
  std::optional<T> value_;
  /** @brief Canonical success or the sole failure diagnostic. */
  Status status_{};
};

}  // namespace ps
