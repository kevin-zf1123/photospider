#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include "execution/dependency_records.hpp"
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
  ResourceVector<SourceObservation> sources;
  ResourceVector<StructuredCacheNeed> replay;
  ResultRelation obligations;
  std::shared_ptr<const DependencyBundle> bundle;
  Backend backend = Backend::Cpu;
  std::uint64_t metadata = 0, epoch = 0;
};
}  // namespace ps::execution_internal
