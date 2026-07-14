#pragma once

#include <functional>
#include <future>
#include <mutex>
#include <type_traits>
#include <utility>

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {

/**
 * @brief Serializes all visible GraphModel access for one graph runtime.
 *
 * @throws std::system_error if asynchronous task creation or mutex operations
 * fail.
 * @note Each submitted callable owns one future and holds the executor mutex
 * for its complete invocation, including exceptions and result construction.
 */
class GraphStateExecutor {
 public:
  /**
   * @brief Binds an executor to one model with the same owning lifetime.
   * @param model Model serialized by every submitted callable.
   * @throws Nothing.
   */
  explicit GraphStateExecutor(GraphModel& model) : model_(model) {}

  /**
   * @brief Disables copying because mutex ownership cannot be duplicated.
   * @param other Executor that retains its model and mutex.
   * @throws Nothing because construction is unavailable.
   */
  GraphStateExecutor(const GraphStateExecutor&) = delete;

  /**
   * @brief Disables copy assignment because the bound model cannot change.
   * @param other Executor that retains its model and mutex.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  GraphStateExecutor& operator=(const GraphStateExecutor&) = delete;

  /**
   * @brief Launches one callable that owns exclusive model access.
   *
   * @tparam Fn Callable invocable as `fn(GraphModel&)`.
   * @param fn Callable captured by value into the asynchronous task.
   * @return Future carrying the callable's exact result or exception.
   * @throws std::bad_alloc if task/future capture allocation exhausts memory.
   * @throws std::system_error if `std::async` cannot launch the task.
   * @note The mutex covers the complete callable. Test-enabled builds may
   * publish a non-blocking observation after a real try_lock contention, then
   * acquire the same normal blocking lock; production builds compile out that
   * observer and use the blocking lock directly.
   */
  template <typename Fn>
  auto submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn, GraphModel&>> {
    using Ret = std::invoke_result_t<Fn, GraphModel&>;
    auto task = [this, f = std::forward<Fn>(fn)]() mutable -> Ret {
      std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
      lock_task(lock);
      if constexpr (std::is_void_v<Ret>) {
        std::invoke(f, model_);
      } else {
        return std::invoke(f, model_);
      }
    };
    return std::async(std::launch::async, std::move(task));
  }

 private:
  /**
   * @brief Acquires one deferred executor lock for submitted graph work.
   * @param lock Deferred unique lock bound to this executor's mutex.
   * @return Nothing.
   * @throws std::system_error if mutex acquisition fails.
   * @note Test-enabled builds may publish a non-blocking observation after a
   *       real failed try_lock before using the normal blocking lock.
   *       Production builds perform only the normal blocking lock.
   */
  static void lock_task(std::unique_lock<std::mutex>& lock);

  /** @brief Borrowed model whose owner also owns this executor. */
  GraphModel& model_;
  /** @brief Exclusive boundary held for every complete submitted callable. */
  std::mutex mutex_;
};

}  // namespace ps
