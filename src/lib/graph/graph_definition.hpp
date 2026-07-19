#pragma once

/**
 * @file graph_definition.hpp
 * @brief Defines detached persistent graph-document values.
 */

#include <string>
#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)

namespace ps {

/**
 * @brief Deep-owned persistent definition of one graph node.
 *
 * NodeDefinition contains only fields that belong to a graph document.
 * Runtime parameters, computed outputs, revisions, ROI/LUT state, timing, and
 * cache results remain exclusively on GraphModel/Node runtime state.
 *
 * @throws std::bad_alloc when copied strings, recursive parameter values, or
 *         vectors cannot allocate.
 * @note Format translators may construct this value independently of a live
 *       graph. InMemoryGraphDocumentAdapter validates definition-only schema
 *       rules before publishing any Node values.
 */
struct NodeDefinition {
  /** @brief Stable graph-local identifier, or -1 when unspecified. */
  int id = -1;
  /** @brief Human-readable node name persisted by document adapters. */
  std::string name;
  /** @brief Operation family used for private registry lookup. */
  std::string type;
  /** @brief Operation subtype used for private registry lookup. */
  std::string subtype;
  /** @brief Declared image edges in destination-input order. */
  std::vector<ImageInput> image_inputs;
  /** @brief Declared parameter edges in effective-merge order. */
  std::vector<ParameterInput> parameter_inputs;
  /** @brief Deep-owned static operation parameter document. */
  plugin::ParameterMap parameters;
  /** @brief Persistent output-port descriptors. */
  std::vector<OutputPort> outputs;
  /** @brief Persistent external-cache descriptors. */
  std::vector<CacheEntry> caches;
  /** @brief Whether force-compute paths preserve reusable HP output. */
  bool preserved = false;
};

/**
 * @brief Detached ordered definition of a complete graph document.
 *
 * @throws std::bad_alloc when node definitions or their nested values cannot
 *         allocate.
 * @note Input order is preserved so duplicate identifiers can be rejected
 *       explicitly. Model capture emits nodes in ascending id order for
 *       deterministic serialization.
 */
struct GraphDefinition {
  /** @brief Complete ordered node definitions owned by this document value. */
  std::vector<NodeDefinition> nodes;
};

}  // namespace ps
