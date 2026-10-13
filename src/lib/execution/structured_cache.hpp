#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include "execution/tile_cache.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::execution_internal {
struct StructuredCacheNeed final {
  ResultProgramNeed request;
  ResourceString facts;
};
struct StructuredCacheManifest final {
  ResourceLease lease;
  ResourceString key, content;
  ResultRef result;
  std::shared_ptr<const TileCacheSnapshot> tiles;
  ResourceVector<StructuredCacheNeed> replay;
  Backend backend = Backend::Cpu;
  std::uint64_t metadata = 0, epoch = 0;
};
}  // namespace ps::execution_internal
