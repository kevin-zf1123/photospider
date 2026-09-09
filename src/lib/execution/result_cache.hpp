#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "execution/memory_budget.hpp"
#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
/** @brief Tracks only opaque metadata that a generic Drop producer may publish.
 * @note Declarations and explicit semantic rules have known facets. Following
 * PreserveInput cannot introduce typed guarantees absent from the compiled IR.
 */
inline bool dynamic_opaque_output(const ExecutionPlan& plan,
                                  std::size_t index) {
  for (std::size_t remaining = plan.steps().size(); remaining; --remaining) {
    if (index >= plan.steps().size())
      return false;
    const auto& step = plan.steps()[index];
    const auto& traits = step.traits;
    if (traits.output_schema.kind != OperationPortKind::Value ||
        !step.output_facets.empty())
      return false;
    if (traits.output_semantic_rule == OperationSemanticRule::Drop)
      return true;
    if (traits.output_semantic_rule != OperationSemanticRule::PreserveInput ||
        traits.output_semantic_input >= step.inputs.size())
      return false;
    const auto* producer =
        std::get_if<PlanStepInput>(&step.inputs[traits.output_semantic_input]);
    if (!producer)
      return false;
    index = producer->step_index;
  }
  return false;
}
/**
 * @brief Context-owned LRU and bounded coordinators for shared regional work.
 * @note Coordinators wait for the existing CPU callbacks, never occupy CPU
 * callback threads themselves. Destruction cancels and joins before CPU pools.
 */
class ResultCache final {
 public:
  ResultCache(std::uint64_t limit, std::shared_ptr<MemoryBudget> budget,
              std::size_t workers, std::size_t waiting)
      : limit_(limit),
        budget_(std::move(budget)),
        maximum_pending_(workers + waiting) {
    try {
      for (std::size_t i = 0; i < workers; ++i)
        workers_.emplace_back([this] { worker(); });
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
      }
      changed_.notify_all();
      for (auto& t : workers_)
        t.join();
      throw;
    }
  }
  ~ResultCache() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closing_ = true;
      for (auto& item : flights_)
        item.second->cancellation.cancel();
    }
    changed_.notify_all();
    for (auto& t : workers_)
      t.join();
  }
  ResultCacheStatistics statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stats = stats_;
    stats.retained_bytes = bytes_;
    stats.entries = entries_.size();
    stats.in_flight = pending_;
    std::set<const CpuStorage*> native_owners;
    for (const auto& entry : entries_)
      if (entry.second.native &&
          native_owners.insert(entry.second.value.storage().get()).second)
        stats.native_retained_bytes += entry.second.value.storage()->capacity();
    return stats;
  }
  std::uint64_t epoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return epoch_;
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    while (!entries_.empty())
      evict();
  }
  Value get(const std::string& key) {
    if (key.empty())
      return {};
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = entries_.find(key);
    if (found == entries_.end()) {
      ++stats_.misses;
      return {};
    }
    ++stats_.hits;
    lru_.splice(lru_.end(), lru_, found->second.order);
    return found->second.value;
  }
  /** @brief Revalidates a completed result against its resolved output
   * contract.
   * @note Generic Drop outputs retain their existing opaque-facet behavior.
   * Typed sample failures remain computed OperationFailed errors on hits.
   */
  Result<Value> get_output(const std::string& key, const PlanStep& step,
                           const std::function<ErrorCode()>& stop,
                           bool dynamic_opaque = false) {
    if (stop) {
      const auto code = stop();
      if (code != ErrorCode::Ok)
        return Result<Value>(Status::failure(code, "cached output stopped"));
    }
    auto value = get(key);
    if (!value.valid())
      return Result<Value>(Value{});
    const bool allow_opaque =
        dynamic_opaque && step.output_facets.empty() &&
        step.traits.output_schema.kind == OperationPortKind::Value;
    const bool exact =
        !allow_opaque &&
        (step.traits.output_schema.kind != OperationPortKind::Value ||
         step.traits.output_semantic_rule != OperationSemanticRule::Drop);
    const bool facets_match =
        exact ? value.facets().size() == step.output_facets.size() &&
                    std::equal(value.facets().begin(), value.facets().end(),
                               step.output_facets.begin(),
                               [](const auto& a, const auto& b) {
                                 return a.key == b.key &&
                                        a.version == b.version &&
                                        a.payload == b.payload;
                               })
              : std::none_of(value.facets().begin(), value.facets().end(),
                             [](const auto& f) {
                               return f.key == "photospider.image" ||
                                      f.key == "photospider.semantic";
                             });
    if (value.descriptor().element_type !=
            step.output_descriptor.element_type ||
        value.descriptor().shape != step.output_descriptor.shape ||
        !value.view(step.output_demand).ok() || !facets_match)
      return Result<Value>(Status::failure(ErrorCode::OperationFailed,
                                           "cached output contract mismatch"));
    for (const auto& facet : value.facets()) {
      if (facet.key != "photospider.image" &&
          facet.key != "photospider.semantic")
        continue;
      auto semantic = decode_semantic(facet);
      if (!semantic.ok())
        return Result<Value>(semantic.status());
      auto status = validate_semantic_value(semantic.value(), value,
                                            ErrorCode::OperationFailed, stop);
      if (!status.ok())
        return Result<Value>(status);
    }
    return Result<Value>(std::move(value));
  }
  /** @brief Optional retention is fenced; allocation failure cannot fail work.
   */
  void put(const std::string& key, const Value& value, std::uint64_t epoch,
           bool native = false) noexcept {
    if (key.empty() || !value.valid())
      return;
    try {
      const auto owner = value.storage();
      const auto capacity = owner->capacity();
      if (capacity > limit_)
        return;
      std::lock_guard<std::mutex> lock(mutex_);
      if (epoch != epoch_ || closing_ || entries_.count(key))
        return;
      while (!entries_.empty() &&
             (entries_.size() >= 4096 ||
              (!owners_.count(owner.get()) && bytes_ > limit_ - capacity)))
        evict();
      lru_.push_back(key);
      try {
        entries_.emplace(key, Entry{value, std::prev(lru_.end()), native});
      } catch (...) {
        lru_.pop_back();
        throw;
      }
      try {
        auto& count = owners_[owner.get()];
        if (count++ == 0)
          bytes_ += capacity;
      } catch (...) {
        entries_.erase(key);
        lru_.pop_back();
        throw;
      }
    } catch (...) {
    }
  }
  /** @brief Reclaims cache references before strict working-set admission. */
  void reclaim_for(std::uint64_t requested) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!entries_.empty() && budget_->available() < requested)
      evict();
  }
  using Work = std::function<Result<ExecutionResult>(const CancellationToken&)>;
  Result<ExecutionResult> compute(const std::string& key,
                                  const std::function<ErrorCode()>& stop,
                                  Work work) {
    std::unique_lock<std::mutex> lock(mutex_);
    std::shared_ptr<Flight> flight;
    bool shared = false;
    for (;;) {
      const auto code = stop();
      if (code != ErrorCode::Ok)
        return Result<ExecutionResult>(
            Status::failure(code, "shared waiter stopped"));
      if (closing_)
        return Result<ExecutionResult>(
            Status::failure(ErrorCode::Cancelled, "cache closing"));
      auto found = flights_.find(key);
      if (found != flights_.end() &&
          !found->second->cancellation.token().cancelled()) {
        flight = found->second;
        shared = true;
        ++stats_.shared_computations;
        break;
      }
      if (pending_ < maximum_pending_) {
        flight = std::make_shared<Flight>();
        flight->work = std::move(work);
        flight->key = key;
        queue_.push_back(flight);
        try {
          flights_.insert_or_assign(key, flight);
        } catch (...) {
          queue_.pop_back();
          throw;
        }
        ++pending_;
        changed_.notify_all();
        break;
      }
      changed_.wait_for(lock, std::chrono::milliseconds(2));
    }
    ++flight->subscribers;
    struct Subscriber {
      Flight* flight;
      ~Subscriber() {
        if (--flight->subscribers == 0 && !flight->done)
          flight->cancellation.cancel();
      }
    } subscriber{flight.get()};
    while (!flight->done) {
      const auto code = stop();
      if (code != ErrorCode::Ok) {
        if (flight->subscribers == 1) {
          flight->cancellation.cancel();
          while (!flight->done)
            changed_.wait(lock);
        }
        return Result<ExecutionResult>(
            Status::failure(code, "shared waiter stopped"));
      }
      changed_.wait_for(lock, std::chrono::milliseconds(2));
    }
    const auto code = stop();
    if (code != ErrorCode::Ok)
      return Result<ExecutionResult>(
          Status::failure(code, "shared publication stopped"));
    if (!flight->result->ok())
      return Result<ExecutionResult>(flight->result->status());
    auto result = flight->result->value();
    if (shared) {
      result.diagnostics.operation_timings.clear();
      result.diagnostics.native_dispatch_count = 0;
      result.diagnostics.native_submission_count = 0;
      result.diagnostics.native_compute_us = 0;
      result.diagnostics.native_constant_bytes = 0;
      result.diagnostics.transfer_count = 0;
      result.diagnostics.transfer_bytes = 0;
      result.diagnostics.host_access_count = 0;
      result.diagnostics.result_copy_bytes = 0;
      result.diagnostics.native_upload_hits = 0;
      result.diagnostics.source_read_count = 0;
      result.diagnostics.source_read_bytes = 0;
      result.diagnostics.shared_computations = 1;
    }
    return Result<ExecutionResult>(std::move(result));
  }

 private:
  struct Entry {
    Value value;
    std::list<std::string>::iterator order;
    bool native = false;
  };
  struct Flight {
    std::string key;
    Work work;
    CancellationSource cancellation;
    std::size_t subscribers = 0;
    bool done = false;
    std::optional<Result<ExecutionResult>> result;
  };
  void evict() {
    auto found = entries_.find(lru_.front());
    const auto owner = found->second.value.storage();
    auto count = owners_.find(owner.get());
    if (--count->second == 0) {
      bytes_ -= owner->capacity();
      owners_.erase(count);
    }
    entries_.erase(found);
    lru_.pop_front();
    ++stats_.evictions;
  }
  void worker() noexcept {
    for (;;) {
      std::shared_ptr<Flight> flight;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return closing_ || !queue_.empty(); });
        if (queue_.empty())
          return;
        flight = queue_.front();
        queue_.pop_front();
      }
      std::optional<Result<ExecutionResult>> result;
      try {
        result.emplace(flight->work(flight->cancellation.token()));
      } catch (...) {
        Status failure;
        failure.code = ErrorCode::OperationFailed;
        result.emplace(std::move(failure));
      }
      flight->work = {};
      {
        std::lock_guard<std::mutex> lock(mutex_);
        flight->result = std::move(result);
        flight->done = true;
        auto found = flights_.find(flight->key);
        if (found != flights_.end() && found->second == flight)
          flights_.erase(found);
        --pending_;
      }
      changed_.notify_all();
    }
  }
  const std::uint64_t limit_;
  std::shared_ptr<MemoryBudget> budget_;
  const std::size_t maximum_pending_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool closing_ = false;
  std::uint64_t bytes_ = 0, epoch_ = 0;
  std::size_t pending_ = 0;
  ResultCacheStatistics stats_;
  std::map<std::string, Entry> entries_;
  std::list<std::string> lru_;
  std::map<const CpuStorage*, std::size_t> owners_;
  std::map<std::string, std::shared_ptr<Flight>> flights_;
  std::deque<std::shared_ptr<Flight>> queue_;
  std::vector<std::thread> workers_;
};
}  // namespace ps::execution_internal
