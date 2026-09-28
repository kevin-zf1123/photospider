#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "execution/disk_cache.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

int main(int argc, char** argv) {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(argc == 2);
  const std::filesystem::path directory = argv[1];
  std::filesystem::remove_all(directory);
  auto budget = std::make_shared<execution_internal::MemoryBudget>(12);
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, release = false;
  std::atomic<bool> gate{true};
  {
    execution_internal::DiskCache cache(
        DiskCacheConfig{directory.string(), 512, 1, 4}, "pending-owner-test",
        budget, 4,
        {[&] {
           if (!gate)
             return;
           std::unique_lock<std::mutex> lock(mutex);
           entered = true;
           cv.notify_all();
           cv.wait(lock, [&] { return release; });
         },
         {}});
    auto reservation = budget->reserve(4).take_value();
    auto writer =
        MutableValue::allocate({ElementType::Float32, {1, 1}},
                               Region::whole({1, 1}), reservation->allocator())
            .take_value();
    const float sample = .5F;
    std::memcpy(writer.data(), &sample, sizeof(sample));
    auto value =
        std::move(writer)
            .publish({encode_semantic(coverage_semantics()).take_value()})
            .take_value();
    reservation->seal();
    cache.put("blocked", value);
    value = {};
    bool reached;
    {
      std::unique_lock<std::mutex> lock(mutex);
      reached =
          cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered; });
    }
    // Always release the writer before assertions can leave this scope.
    const bool retained = budget->available() == 8;
    cache.drop_pending();
    auto full = budget->reserve(12);
    const bool admitted = full.ok();
    if (full.ok())
      full.value()->seal();
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    cv.notify_all();
    cache.flush();
    PS_CHECK(reached && retained && admitted);
    PS_CHECK(cache.statistics().entries == 0 &&
             cache.statistics().queued_writes == 0);
    PS_CHECK(budget->available() == 12);
    gate = false;
    auto unaccounted =
        MutableValue::allocate({ElementType::Float32, {1, 1}},
                               Region::whole({1, 1}), BufferAllocator{})
            .take_value();
    std::memcpy(unaccounted.data(), &sample, sizeof(sample));
    value = std::move(unaccounted)
                .publish({encode_semantic(coverage_semantics()).take_value()})
                .take_value();
    cache.put("cancelled", value, [] { return ErrorCode::Cancelled; });
    cache.flush();
    PS_CHECK(cache.statistics().entries == 0);
    cache.put("retry", value);
    cache.flush();
    PS_CHECK(cache.statistics().entries == 1);
    cache.clear();
  }
  std::filesystem::remove_all(directory);
  return 0;
}
