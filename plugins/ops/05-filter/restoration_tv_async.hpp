// Copyright 2026 Photospider contributors
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "05-filter/restoration_tv_math.hpp"
#include "common/floating_environment.hpp"

namespace ps::plugin_internal::restoration::tv {
enum class Reason { Running, Converged, MaxIterations };

struct Outcome {
  Reason reason;
  std::uint64_t round;
};

// One invocation-local predicate worker borrows only declared scratch and the
// immutable packed input. It is quiescent before the invocation releases any
// view. Normal completion is committed before aborting/joining the worker;
// cleanup never waits for an unfinished objective reduction to complete.
template <bool Isotropic, class T, class Poll>
class Checker final {
 public:
  Checker(const T* input, T* snapshot, std::size_t height, std::size_t width,
          std::size_t planes, Parameters<T> parameters, T atol, T rtol,
          Poll poll)
      : input_(input),
        snapshot_(snapshot),
        height_(height),
        width_(width),
        planes_(planes),
        count_(height * width * planes),
        parameters_(parameters),
        atol_(atol),
        rtol_(rtol),
        poll_(std::move(poll)),
        worker_([this] { run(); }) {}
  Checker(const Checker&) = delete;
  Checker& operator=(const Checker&) = delete;
  ~Checker() { stop(); }

  template <class Admit>
  void trigger(State<T> state, std::uint64_t round, Admit admit) {
    std::lock_guard<std::mutex> lock(mutex_);
    rethrow();
    if (busy_ || outcome_.reason != Reason::Running)
      return;
    // Admission and snapshot acquisition start the check in the solver's
    // coherent round. Busy triggers are discarded rather than queued.
    admit();
    for (std::size_t i = 0; i < count_; i += 4096) {
      poll_();
      const auto bytes = std::min<std::size_t>(4096, count_ - i) * sizeof(T);
      std::memcpy(snapshot_ + i, state.primal + i, bytes);
      std::memcpy(snapshot_ + count_ + i, state.px + i, bytes);
      std::memcpy(snapshot_ + 2 * count_ + i, state.py + i, bytes);
    }
    checked_round_ = round;
    busy_ = true;
    ready_ = true;
    ready_condition_.notify_one();
  }

  Outcome outcome() {
    std::lock_guard<std::mutex> lock(mutex_);
    rethrow();
    return outcome_;
  }

  Outcome finish(std::uint64_t round) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (outcome_.reason == Reason::Running)
        outcome_ = {Reason::MaxIterations, round};
    }
    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    rethrow();
    return outcome_;
  }

  const T* result() const { return snapshot_; }

 private:
  struct Aborted {};
  void rethrow() const {
    if (failure_)
      std::rethrow_exception(failure_);
  }
  void stop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_.store(true, std::memory_order_release);
    }
    ready_condition_.notify_one();
    if (worker_.joinable())
      worker_.join();
  }
  void run() noexcept {
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_condition_.wait(lock, [&] {
          return ready_ || stopping_.load(std::memory_order_acquire);
        });
        if (stopping_.load(std::memory_order_acquire))
          return;
        ready_ = false;
      }
      bool passes = false;
      std::exception_ptr failure;
      try {
        FloatingEnvironment environment;
        if (!environment.active())
          throw std::runtime_error("TV predicate floating environment");
        const auto poll = [&] {
          if (stopping_.load(std::memory_order_acquire))
            throw Aborted{};
          poll_();
        };
        passes = gap_passes<Isotropic>(
            input_, snapshot_, snapshot_ + count_, snapshot_ + 2 * count_,
            height_, width_, planes_, parameters_, atol_, rtol_, poll);
      } catch (const Aborted&) {
      } catch (...) {
        failure = std::current_exception();
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure && !failure_)
          failure_ = failure;
        if (passes && outcome_.reason == Reason::Running)
          outcome_ = {Reason::Converged, checked_round_};
        busy_ = false;
      }
    }
  }

  const T* input_;
  T* snapshot_;
  std::size_t height_, width_, planes_, count_;
  Parameters<T> parameters_;
  T atol_, rtol_;
  Poll poll_;
  std::mutex mutex_;
  std::condition_variable ready_condition_;
  std::atomic<bool> stopping_{false};
  bool busy_ = false, ready_ = false;
  std::uint64_t checked_round_ = 0;
  Outcome outcome_{Reason::Running, 0};
  std::exception_ptr failure_;
  std::thread worker_;
};
}  // namespace ps::plugin_internal::restoration::tv
