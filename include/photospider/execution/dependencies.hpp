#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/data/dependency.hpp"

namespace ps {
namespace execution_internal {
class DependencyRecords;
}
/** @brief Immutable direct dependency evidence from a successful execution.
 * @note Records contain exact associations and their resolved coverage, no
 * pixel allocations or worker ownership. They outlive Values and the context.
 * Queries describe potential change under the captured relation, not changed
 * numeric values or completeness outside the recorded output coverage. Const
 * operations are concurrent-safe; metadata failures publish no partial answer.
 */
class PHOTOSPIDER_API ExecutionDependencies final {
 public:
  ExecutionDependencies() = default;
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Exact named sample coverage proved by this execution.
   * @return Owned coverage map, or an empty map for a default object.
   * @throws std::bad_alloc For metadata copying.
   */
  std::map<std::string, Footprint> coverage() const;
  /** @brief Number of directly observed operation records, excluding sources.
   */
  std::size_t record_count() const noexcept;
  /** @brief Retrieves merged per-observation evidence for an Atomic node.
   * @return Certificate or NotFound if no such resolved Atomic record exists.
   * Whole and terminal request records do not masquerade as sample rows.
   * @throws std::bad_alloc For copied metadata.
   */
  Result<DependencyCertificate> certificate(std::uint64_t node) const;
  /** @brief Restricts roots and every contributing Atomic certificate together.
   * @param outputs Named subsets of coverage(); omitted names are removed.
   * @param limits Bounds copied records and backward traversal.
   * @return Independent immutable evidence or a typed limit/domain failure.
   * Whole manifests retain their complete global observation. RequestRecord
   * roots accept only the identical complete query. Unknown rows reject.
   * @throws std::bad_alloc For metadata allocation; no pixel reads occur.
   */
  Result<ExecutionDependencies> restrict(
      const std::map<std::string, Footprint>& outputs,
      const FootprintLimits& limits = {}) const;
  /** @brief Projects required root support onto named source payload sets.
   * @note This fetch union is for bounded content verification, not a
   * replacement for per-output certificates or their transpose relation.
   * @return Exact support for recorded roots, or a typed metadata/work failure.
   */
  Result<std::map<std::string, Footprint>> source_support(
      const FootprintLimits& limits = {}) const;
  /** @brief Transposes a source payload edit through recorded direct
   * associations.
   * @param input Exact workflow input name.
   * @param samples Potentially changed input samples in its descriptor domain.
   * @param roles Matching Data/Control/Validation bit mask, 1..7.
   * @param limits Bounds all set/queue work; cancellation returns Cancelled.
   * @return Exact potentially dirty subsets of coverage(), including known
   * empty answers. Invalid input/domain/roles reject; unknown output regions
   * are absent from coverage() and are never reported clean. This query is not
   * a cache-validity grant or a trusted declaration of actual edits. Static
   * descriptor/schema changes require recompilation; this sample relation
   * does not represent cross-node metadata-output atoms. Tagged direct
   * relations remain available through certificate().
   * @throws std::bad_alloc For metadata allocation.
   */
  Result<std::map<std::string, Footprint>> potential_dirty(
      const std::string& input, const Footprint& samples,
      std::uint32_t roles = 7, const FootprintLimits& limits = {}) const;

 private:
  friend class execution_internal::DependencyRecords;
  struct Impl;
  explicit ExecutionDependencies(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
};
}  // namespace ps
