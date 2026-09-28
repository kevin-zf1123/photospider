#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>

namespace copy_profile {
struct Entry final {
  std::uint64_t calls = 0, bytes = 0, ns = 0, failures = 0, failure_ns = 0;
};
struct Snapshot final {
  std::array<Entry, 6> entries{};
  bool overflow = false;
};
#ifdef PHOTOSPIDER_PROFILE_COPIES
void initialize() noexcept;
void begin();
Snapshot end();
void write(std::ostream& output, const Snapshot& snapshot);
#else
inline void initialize() noexcept {}
inline void begin() noexcept {}
inline Snapshot end() noexcept {
  return {};
}
inline void write(std::ostream&, const Snapshot&) {}
#endif
}  // namespace copy_profile
