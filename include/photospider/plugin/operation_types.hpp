#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "photospider/data/planar_image.hpp"
#include "photospider/data/result.hpp"
#include "photospider/data/value.hpp"

namespace ps {
/** @brief Local physical backend; unknown numeric representations reject. */
enum class Backend : std::uint32_t { Cpu = 1, Gpu = 2 };
/** @brief Local observation declaration, distinct from a graph's effective
 * kind.
 * @note Atomic promises restriction-stable values/errors. RequestRecord is a
 * terminal complete-query boundary: no data/control/scalar/validation consumer
 * may reference it, including another RequestRecord or a source wrapper.
 */
enum class ObservationKind : std::uint32_t { Atomic = 0, RequestRecord = 1 };
/** @brief Error delivery across every resolver/read/validation/compute phase.
 * @note RequestFailureOnly executes one observation per atomic invocation.
 * PerAtomOutcome requires a complete outcome protocol before batching is legal.
 */
enum class FailureDelivery : std::uint32_t {
  RequestFailureOnly = 0,
  PerAtomOutcome = 1
};
/** @brief Owned static metadata, shared by compiler and direct invocation. */
struct PHOTOSPIDER_API OperationMetadata final {
  ValueDescriptor descriptor;
  std::vector<ValueFacet> facets;
  /** @brief Alternative paged edge type; descriptor/facets are empty when set.
   */
  std::shared_ptr<const SchemaTemplate> result_schema = {};
  /** @brief Inferred producer tuple grouping, independent of Value facets.
   * Only producer output contracts establish this field. Input declarations
   * and standalone Values default to sample observations. No payload is held.
   */
  std::uint32_t atomic_trailing_axes = 0;
  /** @brief Structural image layout, independent of semantic facets. */
  std::optional<PlanarImageLayout> planar_layout = {};
};
}  // namespace ps
