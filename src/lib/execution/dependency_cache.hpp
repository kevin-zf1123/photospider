#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "data/content_digest.hpp"
#include "execution/dependency_records.hpp"

namespace ps::execution_internal {
/** @brief A pixel key binds logical geometry, independent of partition order.
 * @note Different legal fragment partitions must never reuse the same key
 * after partial LRU eviction. Shape/type/facets belong to the plan template.
 */
inline std::string dependency_fragment_key(const std::string& template_key,
                                           const std::string& content,
                                           const Region& region) {
  content_internal::Sha256 key;
  key.text("photospider.dependency-pixel.v2");
  key.text(template_key);
  key.text(content);
  key.integer(region.rank());
  for (const auto& dimension : region.dimensions()) {
    key.integer(dimension.offset);
    key.integer(dimension.extent);
  }
  return key.finish();
}
/** @brief Complete proof and pixel keys, without pixel or input owners.
 * @note Support is the full transitive Data/Control/Validation witness. Its
 * content digest proves reuse only under the same static plan contract.
 */
struct DependencyCacheManifest final {
  std::shared_ptr<const DependencyRecord> record;
  ValueDescriptor descriptor;
  std::vector<ValueFacet> facets;
  Footprint outputs;
  std::map<std::string, Footprint> support;
  std::string content_identity;
  std::vector<std::string> fragment_keys;
  std::uint64_t metadata_entries = 0, epoch = 0;
};
struct DependencyCacheProof final {
  std::map<std::string, Footprint> support;
  std::uint64_t metadata_entries = 0;
};
/** @brief Projects the actual retained record DAG with a single shared budget.
 * @note Pointer identity counts distinct retained owners, even when equal
 * observations were recomputed. Every walk/copy is charged before allocation.
 * A normalization receives a precharged finite work allowance; exhaustion is
 * an optional cache miss, never an unmetered retry at the next output record.
 */
inline Result<DependencyCacheProof> dependency_cache_proof(
    const ExecutionPlan& plan,
    const std::shared_ptr<const DependencyRecord>& root,
    std::uint64_t metadata_limit, std::uint64_t* work, std::uint64_t* visited,
    const FootprintLimits& limits) {
  using Answer = Result<DependencyCacheProof>;
  DependencyCacheProof result;
  const auto charge = [&](std::uint64_t count, bool retained = true) {
    if (count > *work) {
      *work = 0;
      return false;
    }
    *work -= count;
    if (retained) {
      if (count > metadata_limit ||
          result.metadata_entries > metadata_limit - count)
        return false;
      result.metadata_entries += count;
    }
    return true;
  };
  if (!root || !charge(1))
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  std::vector<const DependencyRecord*> pending{root.get()};
  std::set<const DependencyRecord*> seen;
  while (!pending.empty()) {
    if (limits.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    if (!charge(1, false))
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    const auto* record = pending.back();
    pending.pop_back();
    if (seen.count(record))
      continue;
    ++*visited;
    if (!record || record->step >= plan.steps().size())
      return Answer(Status{ErrorCode::InvalidArgument, {}});
    const auto& step = plan.steps()[record->step];
    if (!charge(2 + step.inputs.size()) ||
        !charge(record->identity.size() + record->samples.shape().size()) ||
        !charge(record->samples.boxes().size() *
                (1 + 2 * record->samples.shape().size())) ||
        !charge(record->upstream.size()))
      return Answer(Status{ErrorCode::ResourceExhausted, {}});
    seen.insert(record);
    const auto needs =
        [&](const std::vector<DependencyNeed>& inputs) -> Status {
      if (!charge(inputs.size()))
        return Status{ErrorCode::ResourceExhausted, {}};
      for (const auto& need : inputs) {
        if (!charge(need.tags.size() * 3 + need.samples.shape().size()) ||
            !charge(need.samples.boxes().size() *
                    (1 + 2 * need.samples.shape().size())))
          return Status{ErrorCode::ResourceExhausted, {}};
        if (need.port >= step.inputs.size())
          return Status{ErrorCode::InvalidArgument, {}};
        const auto* input =
            std::get_if<PlanWorkflowInput>(&step.inputs[need.port]);
        if (!input || !(need.roles & 7U) || need.samples.empty())
          continue;
        const auto& declaration =
            plan.input_declarations().at(input->declaration_index);
        auto found = result.support.find(declaration.name);
        if (found == result.support.end()) {
          if (!charge(1 + declaration.name.size() +
                      need.samples.shape().size() +
                      need.samples.boxes().size() *
                          (1 + 2 * need.samples.shape().size())))
            return Status{ErrorCode::ResourceExhausted, {}};
          result.support.emplace(declaration.name, need.samples);
        } else {
          if (!charge(1 + need.samples.boxes().size(), false))
            return Status{ErrorCode::ResourceExhausted, {}};
          if (found->second == need.samples)
            continue;
          const auto n =
              1 + found->second.boxes().size() + need.samples.boxes().size();
          const auto grant =
              n > 1048576 ? *work : std::min<std::uint64_t>(*work, 32 * n * n);
          if (!grant || !charge(grant, false))
            return Status{ErrorCode::ResourceExhausted, {}};
          auto bounded = limits;
          bounded.maximum_work = grant;
          auto combined = found->second.unite(need.samples, bounded);
          if (!combined.ok())
            return combined.status();
          const auto before = found->second.boxes().size();
          const auto after = combined.value().boxes().size();
          if (after > before &&
              !charge((after - before) * (1 + 2 * need.samples.shape().size())))
            return Status{ErrorCode::ResourceExhausted, {}};
          found->second = combined.take_value();
        }
      }
      return Status::success();
    };
    if (record->certificate) {
      if (!charge(record->certificate->identity().size()) ||
          !charge(record->certificate->rows().size()) ||
          !charge(record->certificate->input_shapes().size()))
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      if (!charge(record->certificate->coverage().boxes().size() *
                  (1 + 2 * record->samples.shape().size())))
        return Answer(Status{ErrorCode::ResourceExhausted, {}});
      for (const auto& shape : record->certificate->input_shapes())
        if (!charge(shape.size()))
          return Answer(Status{ErrorCode::ResourceExhausted, {}});
      for (const auto& row : record->certificate->rows()) {
        if (!charge(row.output.size()))
          return Answer(Status{ErrorCode::ResourceExhausted, {}});
        auto status = needs(row.inputs);
        if (!status.ok())
          return Answer(status);
      }
    } else {
      auto status = needs(record->manifest);
      if (!status.ok())
        return Answer(status);
    }
    for (const auto& child : record->upstream) {
      if (!child || child->step >= record->step)
        return Answer(Status{ErrorCode::InvalidArgument, {}});
      pending.push_back(child.get());
    }
  }
  return Answer(std::move(result));
}
}  // namespace ps::execution_internal
