#include <functional>
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <utility>

#include "photospider/plugin/result_operation_plugin_api.h"
#include "photospider/plugin/result_program.hpp"

namespace ps::plugin_internal::result_c {
class CResultMemberBridge;
// Owns one poll's handle translation and service wrappers. owner and host
// services are borrowed until callback return; retirement invalidates the
// lease before releasing grants. The bridge retains inactive lease addresses
// as tombstones, so stale C pointers can be rejected until its destruction.
struct CResultPollLease {
  using NativeViews = std::map<
      std::uint64_t, std::uint64_t, std::less<std::uint64_t>,
      ResourceAllocator<std::pair<const std::uint64_t, std::uint64_t>>>;
  using AtlasKey = std::pair<std::uint32_t, std::uint32_t>;
  using Atlases = std::map<
      AtlasKey, ps_result_native_atlas_v2, std::less<AtlasKey>,
      ResourceAllocator<std::pair<const AtlasKey, ps_result_native_atlas_v2>>>;
  using Checkpoints = std::map<
      std::uint64_t, ResultCheckpoint, std::less<std::uint64_t>,
      ResourceAllocator<std::pair<const std::uint64_t, ResultCheckpoint>>>;
  NativeViews native_views;
  Atlases atlases;
  Checkpoints checkpoints;
  CResultMemberBridge* owner;
  CancellationToken cancellation;
  std::atomic<bool> active{true};
  ps_cpu_parallel_service_v1 parallel{};
  ps_cpu_tiles_service_v1 tiles{};
  ps_gpu_service_v1 gpu{};
  const ps_cpu_parallel_service_v1* host_parallel = nullptr;
  const ps_cpu_tiles_service_v1* host_tiles = nullptr;
  const ps_gpu_service_v1* host_gpu = nullptr;
  void retire() noexcept;
};
}  // namespace ps::plugin_internal::result_c
