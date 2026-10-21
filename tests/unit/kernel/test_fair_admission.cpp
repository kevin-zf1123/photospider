#include <array>
#include <atomic>
#include <chrono>
#include <thread>

#include "execution/fair_admission.hpp"
#include "support/status_check.hpp"

using ps::execution_internal::FairAdmission;

int main() {
  FairAdmission queue;
  FairAdmission::Waiter a(queue), b(queue), c(queue);
  unsigned calls = 0;
  const auto busy = [&] {
    ++calls;
    return false;
  };
  const auto accept = [&] {
    ++calls;
    return true;
  };
  PS_CHECK(!queue.try_acquire(a, busy));
  PS_CHECK(!queue.try_acquire(b, busy));
  PS_CHECK(!queue.try_acquire(c, busy));
  PS_CHECK(calls == 1);
  for (unsigned round = 0; round < 20; ++round) {
    PS_CHECK(queue.try_acquire(a, accept));
    PS_CHECK(!queue.try_acquire(a, accept));
    PS_CHECK(queue.try_acquire(b, accept));
    PS_CHECK(!queue.try_acquire(b, accept));
    PS_CHECK(queue.try_acquire(c, accept));
    PS_CHECK(!queue.try_acquire(c, accept));
  }
  PS_CHECK(calls == 61);
  // Remove a middle waiter, then the head and the remaining tail.
  queue.cancel(b);
  queue.cancel(a);
  PS_CHECK(queue.try_acquire(c, accept));
  queue.cancel(c);
  queue.cancel(c);
  {
    FairAdmission::Waiter temporary(queue);
    PS_CHECK(!queue.try_acquire(temporary, busy));
    PS_CHECK(!queue.try_acquire(a, accept));
  }
  PS_CHECK(queue.try_acquire(a, accept));

  // Concurrent coordinators share one acquisition critical section. Each
  // embedded waiter is removed before its owning thread returns.
  std::array<std::thread, 4> threads;
  std::array<unsigned, 4> counts{};
  unsigned total = 0;
  std::atomic<bool> start{false};
  for (unsigned id = 0; id < threads.size(); ++id) {
    threads[id] = std::thread([&, id] {
      FairAdmission::Waiter waiter(queue);
      while (!start.load())
        std::this_thread::yield();
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (counts[id] < 100 && std::chrono::steady_clock::now() < deadline) {
        queue.try_acquire(waiter, [&] {
          ++total;
          ++counts[id];
          return true;
        });
        std::this_thread::yield();
      }
    });
  }
  start = true;
  for (auto& thread : threads)
    thread.join();
  PS_CHECK(total == 400);
  for (auto count : counts)
    PS_CHECK(count == 100);
  return 0;
}
