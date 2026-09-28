#pragma once

#include <cstdint>
#include <iosfwd>

namespace allocation_profile {
struct Snapshot final {
  std::uint64_t calls = 0, requested_bytes = 0, failures = 0;
  bool overflow = false;
};
#ifdef PHOTOSPIDER_PROFILE_ALLOCATIONS
void begin() noexcept;
Snapshot end() noexcept;
void write(std::ostream& output, const Snapshot& snapshot,
           std::uint64_t attempts);
#else
inline void begin() noexcept {}
inline Snapshot end() noexcept {
  return {};
}
inline void write(std::ostream&, const Snapshot&, std::uint64_t) {}
#endif
}  // namespace allocation_profile
