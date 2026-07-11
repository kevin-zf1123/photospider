#pragma once

#include <optional>
#include <string>

#include "compute/dirty_region_snapshot.hpp"
#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Resolves operation metadata for a single HP or RT compute domain.
 *
 * The resolver inspects the multi-implementation registry first so RT planning
 * can use `meta_rt` when an operator has separate HP and RT metadata. Legacy
 * callers that registered only one metadata record still fall back to
 * OpRegistry::get_metadata(), preserving the existing HP-first compatibility
 * behavior outside domain-aware planning and dirty execution code.
 *
 * @param type Operation type, such as `"image_process"`.
 * @param subtype Operation subtype, such as `"gaussian_blur"`.
 * @param domain Compute domain whose implementation metadata is requested.
 * @return Domain-specific metadata when present, otherwise the legacy metadata
 * fallback or nullopt when no metadata is registered.
 * @throws std::bad_alloc if implementation or metadata snapshot copying cannot
 *         allocate.
 * @throws Any exception raised while copying a registered callback target.
 * @note This helper does not resolve the executable operation. It only selects
 * metadata used for tile shape, dependency ROI, and tiled execution config.
 */
inline std::optional<OpMetadata> metadata_for_domain(const std::string& type,
                                                     const std::string& subtype,
                                                     DirtyDomain domain) {
  const auto impls = OpRegistry::instance().get_implementations(type, subtype);
  if (impls) {
    if (domain == DirtyDomain::RealTime && impls->meta_rt) {
      return impls->meta_rt;
    }
    if (domain == DirtyDomain::HighPrecision && impls->meta_hp) {
      return impls->meta_hp;
    }
  }
  return OpRegistry::instance().get_metadata(type, subtype);
}

}  // namespace ps::compute
