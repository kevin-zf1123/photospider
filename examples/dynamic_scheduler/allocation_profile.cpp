#include "allocation_profile.hpp"  // NOLINT(build/include_subdir)

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <new>
#include <ostream>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<bool> active{false}, overflow{false};
std::atomic<std::uint64_t> writers{0}, calls{0}, bytes{0}, failures{0};
void add(std::atomic<std::uint64_t>* counter, std::uint64_t amount) noexcept {
  if (counter->fetch_add(amount, std::memory_order_relaxed) >
      UINT64_MAX - amount)
    overflow.store(true, std::memory_order_relaxed);
}
void record(std::size_t size, bool failed) noexcept {
  if (!active.load())
    return;
  // Sequentially consistent gates make stop observe each writer admitted
  // before it closes. A late entrant observes the closed gate and skips writes.
  writers.fetch_add(1);
  if (active.load()) {
    if (failed) {
      add(&failures, 1);
    } else {
      add(&calls, 1);
      add(&bytes, size);
    }
  }
  writers.fetch_sub(1);
}
void* allocate(std::size_t size, std::size_t alignment = 0) {
  try {
    for (;;) {
      void* pointer = nullptr;
      if (alignment) {
        if (posix_memalign(&pointer, std::max(alignment, sizeof(void*)),
                           size ? size : 1) != 0)
          pointer = nullptr;
      } else {
        pointer = std::malloc(size ? size : 1);
      }
      if (pointer) {
        record(size, false);
        return pointer;
      }
      const auto handler = std::get_new_handler();
      if (!handler)
        throw std::bad_alloc();
      handler();
    }
  } catch (...) {
    record(0, true);
    throw;
  }
}
}  // namespace

namespace allocation_profile {
void begin() noexcept {
  calls.store(0, std::memory_order_relaxed);
  bytes.store(0, std::memory_order_relaxed);
  failures.store(0, std::memory_order_relaxed);
  overflow.store(false, std::memory_order_relaxed);
  active.store(true);
}
Snapshot end() noexcept {
  active.store(false);
  while (writers.load())
    std::this_thread::yield();
  return {calls.load(std::memory_order_relaxed),
          bytes.load(std::memory_order_relaxed),
          failures.load(std::memory_order_relaxed),
          overflow.load(std::memory_order_relaxed)};
}
void write(std::ostream& output, const Snapshot& snapshot,
           std::uint64_t attempts) {
  if (snapshot.overflow)
    throw std::runtime_error("allocation profile counters overflowed");
  output
      << ",\"allocation_profile\":{\"scope\":\"measured_interval_all_threads\","
         "\"events\":\"routed_successful_cpp_new_returns\","
         "\"timings_are_profiled\":true,\"calls\":"
      << snapshot.calls << ",\"requested_bytes\":" << snapshot.requested_bytes
      << ",\"failures\":" << snapshot.failures
      << ",\"calls_per_attempt\":" << double(snapshot.calls) / attempts
      << ",\"requested_bytes_per_attempt\":"
      << double(snapshot.requested_bytes) / attempts << '}';
}
}  // namespace allocation_profile

void* operator new(std::size_t size) {
  return allocate(size);
}
void* operator new[](std::size_t size) {
  return allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
  return ::operator new(size, tag);
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  try {
    return allocate(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
  return ::operator new(size, alignment, tag);
}
void operator delete(void* pointer) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
  std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
  std::free(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  std::free(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
void operator delete(void* pointer, std::align_val_t,
                     const std::nothrow_t&) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::align_val_t,
                       const std::nothrow_t&) noexcept {
  std::free(pointer);
}
