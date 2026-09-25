#include <sys/sysctl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "execution/cancellation_poll.hpp"
#include "support/test_support.hpp"

namespace ps::plugin_internal::format_numeric {
std::uint64_t sme_conversion_vector_bytes();
std::uint64_t sme_f32_u8_tile(const std::uint8_t*, std::uint8_t*, std::uint64_t,
                              const execution_internal::CancellationPoll&);
}  // namespace ps::plugin_internal::format_numeric
int main(int argc, char** argv) {
  for (const char* name :
       {"hw.optional.arm.FEAT_SME", "hw.optional.arm.FEAT_SME_F64F64"}) {
    int available = 0;
    std::size_t size = sizeof(available);
    if (sysctlbyname(name, &available, &size, nullptr, 0) || !available) {
      std::cout << "SME unavailable: skipped\n";
      return 0;
    }
  }
  if (ps::plugin_internal::format_numeric::sme_conversion_vector_bytes() !=
      64) {
    std::cout
        << "SVL differs from 64 bytes: production fallback, test skipped\n";
    return 0;
  }
  using ps::plugin_internal::format_numeric::sme_f32_u8_tile;
  std::atomic<bool> primary{false}, secondary{false};
  ps::execution_internal::CancellationPoll poll;
  poll.flags[0] = &primary;
  poll.flags[1] = &secondary;
  poll.size = 2;
  constexpr std::uint64_t count = 4 * 1024 * 1024;
  std::vector<float> source(count, 0.5f);
  std::vector<std::uint8_t> output(count, 0xa5);
  const auto* input = reinterpret_cast<const std::uint8_t*>(source.data());
  const auto start = std::chrono::steady_clock::now();
  PS_CHECK(sme_f32_u8_tile(input, output.data(), count, poll) == count);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  PS_CHECK(std::all_of(output.begin(), output.end(),
                       [](auto x) { return x == 128; }));
  std::fill(output.begin(), output.end(), 0xa5);
  secondary.store(true, std::memory_order_release);
  PS_CHECK(sme_f32_u8_tile(input, output.data(), count, poll) == 0);
  PS_CHECK(std::all_of(output.begin(), output.end(),
                       [](auto x) { return x == 0xa5; }));

  std::array<std::atomic<bool>, 65> grouped;
  for (std::size_t i = 0; i < grouped.size(); ++i) {
    grouped[i].store(false, std::memory_order_relaxed);
    poll.flags[i] = &grouped[i];
  }
  poll.size = grouped.size();
  grouped.back().store(true, std::memory_order_release);
  PS_CHECK(sme_f32_u8_tile(input, output.data(), count, poll) == 0);
  PS_CHECK(std::all_of(output.begin(), output.end(),
                       [](auto x) { return x == 0xa5; }));
  poll.flags[0] = &primary;
  poll.flags[1] = &secondary;
  poll.size = 2;

  // Timing-dependent diagnostic is opt-in; the default CTest is deterministic.
  if (argc == 1)
    return 0;
  PS_CHECK(argc == 2 && std::strcmp(argv[1], "--midflight") == 0);
  // Exercise cancellation after conversion has actually written a prefix.
  // Inspect output only after join, avoiding concurrent non-atomic reads.
  bool observed_partial = false;
  for (unsigned fraction = 1; fraction <= 7 && !observed_partial; ++fraction) {
    secondary.store(false, std::memory_order_release);
    std::fill(output.begin(), output.end(), 0xa5);
    std::atomic<bool> started{false};
    std::uint64_t converted = count;
    std::thread worker([&] {
      started.store(true, std::memory_order_release);
      converted = sme_f32_u8_tile(input, output.data(), count, poll);
    });
    while (!started.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::this_thread::sleep_for(elapsed * fraction / 8);
    secondary.store(true, std::memory_order_release);
    worker.join();
    const auto first_unwritten = std::find(output.begin(), output.end(), 0xa5);
    const auto prefix =
        static_cast<std::uint64_t>(first_unwritten - output.begin());
    if (prefix && prefix < count) {
      PS_CHECK(converted == 0);
      PS_CHECK(std::all_of(output.begin(), first_unwritten,
                           [](auto x) { return x == 128; }));
      PS_CHECK(std::all_of(first_unwritten, output.end(),
                           [](auto x) { return x == 0xa5; }));
      observed_partial = true;
    }
  }
  std::cout << (observed_partial ? "midflight partial prefix observed\n"
                                 : "midflight timing inconclusive\n");
  if (!observed_partial)
    return 2;
  return 0;
}
