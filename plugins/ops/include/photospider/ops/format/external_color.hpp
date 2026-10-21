#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "photospider/ops/format/channel.hpp"
#include "photospider/ops/icc_profile.hpp"
#include "photospider/ops/ocio_config_resource.hpp"
#include "photospider/ops/tensor_description.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps::format {
/** Explicit binding: add resource to the workflow ResourceSet and pass
 * parameter to the corresponding operator. Neither field is a filesystem path.
 */
struct ColorResourceBinding final {
  ImmutableResource resource;
  std::string parameter;
};
/** Encode immutable identity for an OCIO snapshot or standalone file/tree. */
PHOTOSPIDER_OPS_API std::string color_resource_parameter(
    const ImmutableResource&);
/** Encode validated ICC identity and endpoint header assertions. */
PHOTOSPIDER_OPS_API std::string icc_profile_parameter(const IccProfile&);
struct IccChainStage final {
  IccProfile profile;
  std::string direction;
  std::string intent;
  std::optional<bool> bpc;
};
/** Seal 2..255 stages, including the exact frozen profile bytes. Construction
 * does no CMM work; the operation verifies and constructs the complete chain.
 */
PHOTOSPIDER_OPS_API Result<ColorResourceBinding> icc_chain(
    const std::vector<IccChainStage>&, const ResourceBudget&);
/** Identifies the pinned engines, private runtime and host integration build.
 */
PHOTOSPIDER_OPS_API std::string external_color_engine_identity();
/** Seal an explicit OCIO snapshot with this build's engine/settings identity.
 */
PHOTOSPIDER_OPS_API Result<OcioConfigResource> ocio_config(
    OcioConfigSnapshot, const ResourceBudget&);
/** Bind standalone immutable LUT/CLF/CTF bytes to their explicit logical name.
 */
PHOTOSPIDER_OPS_API Result<ColorResourceBinding> ocio_file(
    const std::string& logical_name, const ImmutableResource&);
/** Seal a declarative OCIO YAML Transform/GroupTransform fragment. Preparation
 * checks typed node structure, cycles, depth and selected resources. Numeric
 * parameter domains and finite LUT values are unchecked caller premises. */
PHOTOSPIDER_OPS_API Result<ColorResourceBinding> ocio_transform_tree(
    const std::string& yaml, const ResourceBudget&);
/** Encode a complete endpoint using a canonical one-dimensional color group.
 * Units follow model order; an endpoint is an assertion, never a conversion. */
PHOTOSPIDER_OPS_API Result<std::string> color_endpoint_parameter(
    const TensorInterpretation&, const std::vector<std::string>& units);
/** @brief Validate and inspect one known external color operation.
 *
 * Synchronously constructs the actual pinned engine using the same static
 * parameters and immutable resources as execution. construction_bytes and
 * construction_work bound this independent inspection. Construction has no
 * cancellation token; it does not modify the input metadata or ResourceSet.
 * The returned owned text records actual linking, selectors, files, effective
 * interpolation and frozen properties; its human-readable format is not a
 * serialization protocol. Engine objects are released before return.
 * Unknown keys or invalid resources return InvalidArgument; resource refusal
 * preserves ResourceExhausted. Concurrent calls own independent engine state.
 */
PHOTOSPIDER_OPS_API Result<std::string> inspect_external_color(
    const std::string& operation, const OperationMetadata& input,
    const std::map<std::string, ParameterValue>& parameters,
    const ResourceSet& resources);
}  // namespace ps::format
