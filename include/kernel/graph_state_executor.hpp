#pragma once

#include <functional>
#include <future>
#include <mutex>
#include <type_traits>
#include <utility>

#include "graph_model.hpp"

namespace ps {

class GraphStateExecutor {
 public:
  explicit GraphStateExecutor(GraphModel& model) : model_(model) {}

  GraphStateExecutor(const GraphStateExecutor&) = delete;
  GraphStateExecutor& operator=(const GraphStateExecutor&) = delete;

  template <typename Fn>
  auto submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn, GraphModel&>> {
    using Ret = std::invoke_result_t<Fn, GraphModel&>;
    auto task = [this, f = std::forward<Fn>(fn)]() mutable -> Ret {
      std::lock_guard<std::mutex> lock(mutex_);
      if constexpr (std::is_void_v<Ret>) {
        std::invoke(f, model_);
      } else {
        return std::invoke(f, model_);
      }
    };
    return std::async(std::launch::async, std::move(task));
  }

 private:
  GraphModel& model_;
  std::mutex mutex_;
};

}  // namespace ps
