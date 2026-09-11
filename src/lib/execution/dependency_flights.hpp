#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "execution/dependency_records.hpp"

namespace ps::execution_internal {
struct DependencyFlightValue final {
  ValueFragments value;
  std::shared_ptr<const DependencyRecord> record;
  std::uint64_t producer_peak = 0;
};
/** @brief Context-owned exact-observation directory without worker ownership.
 * @note A claim's driver remains on the requesting coordinator. Only actual
 * callback stages enter the existing execution pool. Keys bind the immutable
 * snapshot, implementation contract, geometry, observation and resource policy.
 */
class DependencyFlights final {
 private:
  using Outcome = Result<std::shared_ptr<const DependencyFlightValue>>;
  struct Waiter {
    // Internal currentness/token checks only; never plugin/user callbacks.
    std::function<ErrorCode()> stop;
    ErrorCode stopped = ErrorCode::Ok;
  };
  struct Flight {
    std::uint64_t id = 0, epoch = 0;
    std::string key;
    CancellationSource cancellation;
    std::map<std::uint64_t, std::shared_ptr<Waiter>> waiters;
    std::optional<Outcome> outcome;
  };
  struct State {
    explicit State(std::uint64_t maximum) : maximum(maximum) {}
    const std::uint64_t maximum;
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t sequence = 1, epoch = 0, pending = 0, waiters = 0, shared = 0;
    bool closing = false;
    std::map<std::string, std::shared_ptr<Flight>> flights;
    void refresh(Flight* flight) noexcept {
      std::uint64_t active = 0;
      for (auto& entry : flight->waiters) {
        const auto code = entry.second->stop();
        if (code == ErrorCode::Cancelled ||
            entry.second->stopped == ErrorCode::Ok)
          entry.second->stopped = code;
        active += entry.second->stopped == ErrorCode::Ok;
      }
      if (!flight->outcome && (!active || closing))
        flight->cancellation.cancel();
    }
    void complete(const std::shared_ptr<Flight>& flight, Outcome&& outcome) {
      if (flight->outcome)
        return;
      flight->outcome.emplace(std::move(outcome));
      auto found = flights.find(flight->key);
      if (found != flights.end() && found->second->id == flight->id)
        flights.erase(found);
      --pending;
      changed.notify_all();
    }
  };

 public:
  class Lease final {
   public:
    ~Lease() {
      if (!state_ || !waiter_)
        return;
      std::lock_guard<std::mutex> lock(state_->mutex);
      flight_->waiters.erase(waiter_id_);
      --state_->waiters;
      state_->refresh(flight_.get());
      if (producer_ && !flight_->outcome)
        state_->complete(flight_,
                         Outcome(Status{ErrorCode::OperationFailed, {}}));
      state_->changed.notify_all();
    }
    bool producer() const noexcept { return producer_; }
    std::uint64_t id() const noexcept { return flight_->id; }
    std::uint64_t epoch() const noexcept { return flight_->epoch; }
    CancellationToken token() const noexcept {
      return flight_->cancellation.token();
    }
    void refresh() const noexcept {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->refresh(flight_.get());
    }
    void complete(Outcome outcome) const {
      std::optional<Outcome> retired;
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->refresh(flight_.get());
      if (flight_->cancellation.token().cancelled()) {
        retired.emplace(std::move(outcome));
        outcome = Outcome(Status{ErrorCode::Cancelled, {}});
      }
      state_->complete(flight_, std::move(outcome));
    }
    /** @brief Waits only on a requesting coordinator, pumping ancestor stops.
     * @note No callback worker may call this. Independent waiter cancellation
     * does not overwrite an outcome required by another waiter.
     */
    Outcome wait(const std::function<void()>& pump = {}) const {
      for (;;) {
        if (pump)
          pump();
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->refresh(flight_.get());
        if (waiter_->stopped != ErrorCode::Ok)
          return Outcome(Status{waiter_->stopped, {}});
        if (flight_->outcome)
          return *flight_->outcome;
        state_->changed.wait_for(lock, std::chrono::milliseconds(2));
      }
    }

   private:
    friend class DependencyFlights;
    std::shared_ptr<State> state_;
    std::shared_ptr<Flight> flight_;
    std::shared_ptr<Waiter> waiter_;
    std::uint64_t waiter_id_ = 0;
    bool producer_ = false;
  };
  explicit DependencyFlights(std::uint64_t maximum)
      : state_(std::make_shared<State>(maximum)) {}
  Result<std::shared_ptr<Lease>> claim(std::string key,
                                       std::function<ErrorCode()> stop) {
    using Answer = Result<std::shared_ptr<Lease>>;
    if (key.empty() || key.size() > 4096 || !stop)
      return Answer(Status{ErrorCode::InvalidArgument, {}});
    const auto code = stop();
    if (code != ErrorCode::Ok)
      return Answer(Status{code, {}});
    auto lease = std::make_shared<Lease>();
    auto waiter = std::make_shared<Waiter>();
    waiter->stop = std::move(stop);
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closing)
      return Answer(Status{ErrorCode::Cancelled, {}});
    if (state_->waiters >= state_->maximum || state_->sequence == UINT64_MAX)
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    auto found = state_->flights.find(key);
    if (found != state_->flights.end())
      state_->refresh(found->second.get());
    const bool producer = found == state_->flights.end() ||
                          found->second->cancellation.token().cancelled();
    std::shared_ptr<Flight> flight;
    if (producer) {
      if (state_->pending >= state_->maximum)
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      flight = std::make_shared<Flight>();
      flight->id = state_->sequence;
      flight->epoch = state_->epoch;
      flight->key = key;
    } else {
      flight = found->second;
    }
    const auto waiter_id = state_->sequence++;
    flight->waiters.emplace(waiter_id, waiter);
    if (producer) {
      try {
        state_->flights.insert_or_assign(std::move(key), flight);
      } catch (...) {
        flight->waiters.erase(waiter_id);
        throw;
      }
      ++state_->pending;
    }
    ++state_->waiters;
    if (!producer)
      ++state_->shared;
    lease->state_ = state_;
    lease->flight_ = std::move(flight);
    lease->waiter_ = std::move(waiter);
    lease->waiter_id_ = waiter_id;
    lease->producer_ = producer;
    return Answer(std::move(lease));
  }
  void clear() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->epoch;
  }
  std::pair<std::uint64_t, std::uint64_t> statistics() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return {state_->pending, state_->shared};
  }
  void close() noexcept {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->closing = true;
    for (const auto& entry : state_->flights)
      entry.second->cancellation.cancel();
    state_->changed.notify_all();
  }

 private:
  std::shared_ptr<State> state_;
};
}  // namespace ps::execution_internal
