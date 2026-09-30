#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/data/dependency.hpp"

namespace ps {
/** @brief One typed source observation set, preserving roles and slot identity.
 * Its name and sample owners retain their managed root through final release.
 */
struct SourceObservation final {
  ResourceString input;
  ResultSupportTarget target = ResultSupportTarget::Value;
  std::uint32_t slot = 0, roles = 0;
  Footprint samples;
};
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
   * @return Root-accounted owned coverage map, or an empty map for a default
   * object.
   * @throws std::bad_alloc For metadata copying.
   */
  ResourceMap<Footprint> coverage() const;
  /** @brief Number of directly observed operation records, excluding sources.
   */
  std::size_t record_count() const noexcept;
  /** @brief Root-accounted guarantees per observation; Unknown stays
   * unresolved. */
  ResourceMap<DependencyGuarantee> guarantees() const;
  /** @brief Retrieves merged per-observation evidence for an Atomic result.
   * @return Certificate or NotFound if no such resolved Atomic record exists.
   * Whole and terminal request records do not masquerade as sample rows.
   * @throws std::bad_alloc For copied metadata.
   */
  Result<DependencyCertificate> certificate(ValueRef result) const;
  /** @brief Restricts roots and every contributing Atomic certificate together.
   * @param outputs Named subsets of coverage(); omitted names are removed.
   * @param limits Bounds copied records and backward traversal.
   * @return Independent immutable evidence or a typed limit/domain failure.
   * Nonempty Whole subsets retain their complete global observation; Empty
   * subsets have no payload support. RequestRecord roots accept only the
   * identical complete query. Unknown rows reject.
   * @throws std::bad_alloc For metadata allocation; no pixel reads occur.
   */
  Result<ExecutionDependencies> restrict(
      const ResourceMap<Footprint>& outputs,
      const FootprintLimits& limits = {}) const;
  /** @brief Projects required root support onto named source payload sets.
   * @note This fetch union is for bounded content verification, not a
   * replacement for per-output certificates or their transpose relation.
   * @return Exact support for recorded roots, or a typed metadata/work failure.
   */
  Result<ResourceMap<Footprint>> source_support(
      const FootprintLimits& limits = {}) const;
  /** @brief Projects typed descriptor/field/image/Value support with roles.
   * @return Root-accounted owned observations, or NotFound for Unknown support
   * and a typed error for exhausted work/capacity or invalid evidence.
   * The returned names and vector capacities outlive this object and context.
   */
  Result<ResourceVector<SourceObservation>> source_observations(
      const FootprintLimits& limits = {}) const;
  /** @brief Transposes a source payload edit through recorded direct
   * associations.
   * @param input Exact workflow input name.
   * @param samples Potentially changed input samples in its descriptor domain.
   * @param roles Nonempty Data=1/Control=2/Validation=4/Descriptor=8
   * mask, 1..15 for Result inputs. Numeric Value payload edits use 1..7;
   * their static descriptor/facets require recompilation.
   * @param target Typed source observation domain; defaults to Value for a
   * numeric input, first Image for an image Result, or Descriptor otherwise.
   * @param slot Image/field index; Value and Descriptor require zero.
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
  Result<ResourceMap<Footprint>> potential_dirty(
      const std::string& input, const Footprint& samples,
      std::uint32_t roles = 7, const FootprintLimits& limits = {},
      std::optional<ResultSupportTarget> target = {},
      std::uint32_t slot = 0) const;

 private:
  friend class execution_internal::DependencyRecords;
  struct Impl;
  explicit ExecutionDependencies(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
};
}  // namespace ps
