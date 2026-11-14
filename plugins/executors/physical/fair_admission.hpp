#pragma once

#include <mutex>

namespace ps::execution_internal {
// Rotates eligible coordinators after each successful resource acquisition.
// Waiters are embedded in frames; enrollment and service never allocate.
class FairAdmission final {
 public:
  class Waiter final {
   public:
    explicit Waiter(FairAdmission& owner) noexcept : owner_(owner) {}
    ~Waiter() noexcept { owner_.cancel(*this); }
    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;

   private:
    friend class FairAdmission;
    FairAdmission& owner_;
    Waiter* previous_ = nullptr;
    Waiter* next_ = nullptr;
    bool queued_ = false;
  };

  // The waiter has a free execution lane and dependency-ready work. attempt
  // only acquires an immediate backend credit, without invoking user/device
  // code. False retains queue position; true removes this turn. A later
  // attempt joins the tail. Each waiter is used by one coordinator.
  template <class Attempt>
  bool try_acquire(Waiter& waiter, Attempt attempt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!waiter.queued_) {
      waiter.previous_ = tail_;
      if (tail_)
        tail_->next_ = &waiter;
      else
        head_ = &waiter;
      tail_ = &waiter;
      waiter.queued_ = true;
    }
    if (head_ != &waiter || !attempt())
      return false;
    remove(waiter);
    return true;
  }

  // Remove on loss of eligibility, abort or frame exit, before accepted work
  // drains. Destruction repeats this harmlessly, including failed construction.
  void cancel(Waiter& waiter) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (waiter.queued_)
      remove(waiter);
  }

 private:
  void remove(Waiter& waiter) noexcept {
    if (waiter.previous_)
      waiter.previous_->next_ = waiter.next_;
    else
      head_ = waiter.next_;
    if (waiter.next_)
      waiter.next_->previous_ = waiter.previous_;
    else
      tail_ = waiter.previous_;
    waiter.previous_ = waiter.next_ = nullptr;
    waiter.queued_ = false;
  }
  std::mutex mutex_;
  Waiter* head_ = nullptr;
  Waiter* tail_ = nullptr;
};
}  // namespace ps::execution_internal
