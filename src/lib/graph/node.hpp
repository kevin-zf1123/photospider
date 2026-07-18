#pragma once

#include <string>
#include <vector>

#include "core/ps_types.hpp"
#include "photospider/plugin/node_view.hpp"

namespace ps {

/**
 * @brief Private image-input source identity used by dependency-LUT reuse.
 *
 * @throws std::bad_alloc when source_output storage is copied.
 * @note The value mirrors topology visible to a public RoiContext without
 *       exposing this private cache record through the SDK.
 */
struct DependencyImageInputIdentity {
  /** @brief Upstream node id, or -1 for a disconnected input. */
  int source_node_id = -1;
  /** @brief Upstream output-port name. */
  std::string source_output;

  /**
   * @brief Compares the complete upstream edge identity.
   * @param other Identity to compare.
   * @return True when source node and output name both match.
   * @throws Nothing under string equality.
   */
  bool operator==(const DependencyImageInputIdentity& other) const noexcept {
    return source_node_id == other.source_node_id &&
           source_output == other.source_output;
  }
};

/**
 * @brief Private cache identity for one validated dependency LUT.
 *
 * The identity captures every host-owned input that can change dependency
 * semantics without exposing those revisions through the public plugin SDK.
 *
 * @throws std::bad_alloc when extent or revision vectors are copied.
 * @note Static and parameter-input revisions plus every image-input extent
 *       always participate. Image-input content revisions participate only for
 *       data-dependent operations.
 */
struct DependencyLutCacheIdentity {
  /**
   * @brief Exact deep-owned effective parameter content used to build the LUT.
   * @note Static parameters and currently cached parameter-input values are
   *       merged before this snapshot is created; no hash collision is
   * possible.
   */
  plugin::ParameterMap effective_parameters;
  /** @brief Revision of the node's static parameter document. */
  uint64_t static_parameter_revision = 0;
  /** @brief Parameter-parent HP revisions in declaration order. */
  std::vector<uint64_t> parameter_input_content_revisions;
  /** @brief Image source node/output identities by destination input index. */
  std::vector<DependencyImageInputIdentity> image_input_sources;
  /** @brief Image-input extents indexed by destination input index. */
  std::vector<PixelSize> input_extents;
  /** @brief Data-dependent image-parent HP revisions by input index. */
  std::vector<uint64_t> upstream_content_revisions;
  /** @brief Whether upstream content revisions participate in equality. */
  bool data_dependent = false;
  /** @brief Registry ownership revision of the builder callback. */
  uint64_t dependency_builder_revision = 0;
  /** @brief Registry revision of the resolved data-dependency flag state. */
  uint64_t data_dependent_revision = 0;

  /**
   * @brief Compares all cache-identity components.
   * @param other Identity to compare.
   * @return True only when exact parameters, effective-parameter revisions,
   *         extents, dependency mode, and relevant image revisions match.
   * @throws Nothing under vector/value equality.
   */
  bool operator==(const DependencyLutCacheIdentity& other) const noexcept {
    return effective_parameters == other.effective_parameters &&
           static_parameter_revision == other.static_parameter_revision &&
           parameter_input_content_revisions ==
               other.parameter_input_content_revisions &&
           image_input_sources == other.image_input_sources &&
           input_extents == other.input_extents &&
           data_dependent == other.data_dependent &&
           dependency_builder_revision == other.dependency_builder_revision &&
           data_dependent_revision == other.data_dependent_revision &&
           upstream_content_revisions == other.upstream_content_revisions;
  }

  /**
   * @brief Checks whether two identities differ.
   * @param other Identity to compare.
   * @return Negation of operator==.
   * @throws Nothing.
   */
  bool operator!=(const DependencyLutCacheIdentity& other) const noexcept {
    return !(*this == other);
  }
};

/**
 * @brief Atomically replaceable private dependency LUT and its exact identity.
 *
 * @throws std::bad_alloc when table or identity storage is copied.
 * @note Keeping both values in one optional prevents clear, copy, move, and
 *       publication paths from separating a LUT from the inputs that built it.
 */
struct DependencyLutCache {
  /** @brief Validated private dependency lookup table. */
  SpatialDependencyMap lut;
  /** @brief Exact host-owned reuse identity paired with lut. */
  DependencyLutCacheIdentity identity;
};

/**
 * @brief Owns one private graph node's topology, configuration, and HP state.
 *
 * A Node stores declared image/parameter edges, static and request-local
 * `ParameterValue` maps, output/cache descriptors, the authoritative reusable
 * HP output, and dependency-LUT identity. Real-time proxy outputs remain
 * outside Node in compute-service runtime state.
 *
 * @throws std::bad_alloc when copied strings, parameter trees, vectors,
 * optional output payloads, or cache identities cannot allocate.
 * @note GraphModel owns Node objects and serializes structural mutation. Cache
 * fields are private backend state, not operation SDK values; callers must use
 * GraphModel/runtime synchronization rather than mutate a shared Node from
 * concurrent execution callbacks.
 */
class Node {
 public:
  /** @brief Stable graph-local identifier, or -1 before graph assignment. */
  int id = -1;
  /** @brief Human-readable node name persisted in graph YAML. */
  std::string name;
  /** @brief Operation type used as the first private registry key segment. */
  std::string type;
  /** @brief Operation subtype used as the second registry key segment. */
  std::string subtype;

  /** @brief Declared image edges indexed by destination input position. */
  std::vector<ImageInput> image_inputs;
  /** @brief Declared parameter edges in effective-parameter merge order. */
  std::vector<ParameterInput> parameter_inputs;

  /**
   * @brief Static format-neutral parameter map owned by the graph definition.
   * @note Graph document adapters populate this value exactly once during
   * ingestion; Graph and compute code never retain the source YAML tree.
   */
  plugin::ParameterMap parameters;

  /**
   * @brief Execution-local effective parameters for the current request.
   * @note Executors populate this snapshot from parameters and connected
   * parameter inputs; it must not be treated as committed reusable graph state.
   */
  plugin::ParameterMap runtime_parameters;

  /** @brief Declared output ports serialized with the graph definition. */
  std::vector<OutputPort> outputs;
  /** @brief Declared external cache locations associated with the node. */
  std::vector<CacheEntry> caches;
  /** @brief Whether force-compute requests preserve the reusable HP result. */
  bool preserved = false;

  /**
   * @brief Authoritative reusable high-precision output for this graph node.
   * @note Subsequent HP execution and persistent-cache logic may reuse this
   * value; real-time proxy output is stored separately by compute service.
   */
  std::optional<NodeOutput> cached_output_high_precision;
  /** @brief Monotonic content revision of the reusable HP output. */
  int hp_version = 0;
  /** @brief Most recent dirty or updated ROI committed to the HP output. */
  std::optional<PixelRect> hp_roi;

  /** @brief Last committed full-resolution input extent for ROI propagation. */
  std::optional<PixelSize> last_input_size_hp;

  /**
   * @brief Validated dependency LUT and exact private reuse identity.
   * @note The pair is replaced or cleared as one optional value.
   */
  mutable std::optional<DependencyLutCache> dependency_lut_cache;
  /** @brief Monotonic diagnostic count of successful LUT replacements. */
  mutable uint64_t dependency_lut_version = 0;
  /** @brief Monotonic revision of the node's static parameter document. */
  uint64_t parameters_version = 0;
};

}  // namespace ps
