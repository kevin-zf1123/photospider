#include "copy_profile.hpp"  // NOLINT(build/include_subdir)

#include <cstdio>
#include <mutex>
#include <ostream>
#include <stdexcept>

#include "execution/execution_test_hooks.hpp"

namespace {
std::mutex mutex;
bool active = false;
copy_profile::Snapshot counters;
void add(std::uint64_t* value, std::uint64_t amount) noexcept {
  if (*value > UINT64_MAX - amount)
    counters.overflow = true;
  else
    *value += amount;
}
void observed(ps::execution_testing::TimingKind kind, std::uint64_t bytes,
              std::uint64_t ns, bool successful) noexcept {
  std::lock_guard<std::mutex> lock(mutex);
  if (!active)
    return;
  auto& entry = counters.entries[static_cast<unsigned>(kind)];
  if (successful) {
    add(&entry.calls, 1);
    add(&entry.bytes, bytes);
    add(&entry.ns, ns);
  } else {
    add(&entry.failures, 1);
    add(&entry.failure_ns, ns);
  }
}
}  // namespace

namespace copy_profile {
void initialize() noexcept {
  static const auto hooks = [] {
    ps::execution_testing::ExecutionTestHooks result;
    result.native_device = true;
    result.execution_timing = observed;
    return result;
  }();
  ps::execution_testing::install_execution_test_hooks(&hooks);
}
void begin() {
  std::lock_guard<std::mutex> lock(mutex);
  counters = {};
  std::fputs("PS_COPY_PROFILE_BEGIN\n", stderr);
  active = true;
}
Snapshot end() {
  std::lock_guard<std::mutex> lock(mutex);
  active = false;
  std::fputs("PS_COPY_PROFILE_END\n", stderr);
  return counters;
}
void write(std::ostream& output, const Snapshot& snapshot) {
  if (snapshot.overflow)
    throw std::runtime_error("copy profile counters overflowed");
  const char* names[]{"value_materialization", "native_gather",
                      "atlas_materialization"};
  output << ",\"copy_profile\":{\"scope\":\"measured_interval_all_threads\","
            "\"timings_are_profiled\":true";
  for (unsigned i = 0; i < 3; ++i) {
    const auto& entry = snapshot.entries[i];
    output << ",\"" << names[i] << "\":{\"calls\":" << entry.calls
           << ",\"logical_output_bytes\":" << entry.bytes
           << ",\"host_ns\":" << entry.ns << ",\"failures\":" << entry.failures
           << ",\"failure_host_ns\":" << entry.failure_ns << '}';
  }
  output << '}';
  const char* stages[]{"ready_dispatch", "completion_publication",
                       "final_assembly"};
  output << ",\"execution_stages\":{\"scope\":\"ExecutionRun_only\"";
  for (unsigned i = 0; i < 3; ++i) {
    const auto& entry = snapshot.entries[i + 3];
    output << ",\"" << stages[i] << "\":{\"calls\":" << entry.calls
           << ",\"host_ns\":" << entry.ns << ",\"failures\":" << entry.failures
           << ",\"failure_host_ns\":" << entry.failure_ns << '}';
  }
  output << '}';
}
}  // namespace copy_profile
