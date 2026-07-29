#pragma once

#include <optional>
#include <string>

#include "compute/dirty_region_snapshot.hpp"
#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps::compute {

/**
 * @brief Resolves operation metadata for a single HP or RT compute domain.
 *
 * The resolver inspects atomic scalar implementation slots in the same order
 * used by executable selection. Metadata therefore always belongs to the
 * callback and identity selected from that slot. Legacy callers that
 * registered only one metadata record still fall back to
 * OpRegistry::get_metadata().
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
    if (domain == DirtyDomain::RealTime) {
      if (impls->tiled_rt) {
        return impls->tiled_rt->metadata;
      }
      if (impls->tiled_hp) {
        return impls->tiled_hp->metadata;
      }
      if (impls->monolithic_hp) {
        return impls->monolithic_hp->metadata;
      }
    }
    if (domain == DirtyDomain::HighPrecision) {
      if (impls->monolithic_hp) {
        return impls->monolithic_hp->metadata;
      }
      if (impls->tiled_hp) {
        return impls->tiled_hp->metadata;
      }
    }
  }
  return OpRegistry::instance().get_metadata(type, subtype);
}

}  // namespace ps::compute
