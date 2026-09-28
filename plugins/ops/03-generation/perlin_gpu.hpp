#pragma once

#include <cstdint>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal {
// Fast tiers admit 256 lanes of <=32 words; the full tier uses four lanes of
// 544 words. Both fit this bound, independently of the complete output size.
inline constexpr std::uint64_t kPerlinGpuWorkspace = 256 * 17 * 32 * 4;
Result<Value> execute_perlin_gpu(const OperationInvocation& call);
}  // namespace ps::plugin_internal
