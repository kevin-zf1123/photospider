#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "photospider/format/channel_assembly.hpp"

namespace ps::format {
/** @brief Static source structure for one FMT-03 input.
 *
 * Select exactly one of `scalar`, `component`, or channel-tensor structure.
 * Scalar and component structures have no axis. A channel tensor has both
 * flags false and may assert its channel axis; metadata resolves an omitted
 * axis except in raw mode, which requires an explicit axis. Axis indices
 * address cell axes and exclude the Result batch prefix; the complete sample
 * rank is at most 8.
 */
struct ChannelEditStructure final {
  bool component = false;
  std::optional<std::uint32_t> axis;
  bool scalar = false;
};
/** @brief Static edge metadata and explicit component/channels/scalar
 * structure. Input 0 is the base channel tensor. Metadata must equal actual
 * inference; generated nodes recheck it at compile time, including forward
 * references.
 */
struct ChannelEditInput final {
  WorkflowInput input;
  OperationMetadata metadata;
  ChannelEditStructure structure;
};
/** @brief Exact index (canonical unsigned decimal), name or role selector. */
struct ChannelSelector final {
  std::string match = "index";
  std::string value = "0";
};
/** @brief Typed literal in native byte order; exactly sizeof(dtype) bytes.
 * No floating conversion is performed. Copy bits with memcpy, including NaNs
 * and Int64 endpoints. The serialized scalar provider owns the exact bytes.
 */
struct ChannelLiteral final {
  ElementType dtype = ElementType::Float32;
  std::vector<std::uint8_t> bytes;
};
/** @brief Source ordinal and selector, or an explicit typed literal.
 * Scalars/components select index 0. Literals ignore input/selector fields.
 */
struct ChannelEditSource final {
  std::uint32_t input = 0;
  ChannelSelector selector;
  std::optional<ChannelLiteral> literal;
};
/** @brief One simultaneous assignment to an original base slot. */
struct ChannelReplacement final {
  ChannelSelector destination;
  ChannelEditSource source;
};
/** @brief Append FMT-03A as an ordered channel-slot mapping.
 *
 * Input 0 establishes the base cell-channel axis and sample grid. Axis indices
 * exclude the Result batch prefix. Every nonscalar input has the same batch
 * prefix as the base; scalar inputs are unbatched shape `[1]`. Complete sample
 * rank, including batch and cell axes, is at most 8. Additional inputs are
 * explicit scalar sources. The nonempty `slots` list can select, omit, or
 * repeat base channels and can select scalar/literal sources. The helper reads
 * no samples; all connected input descriptors, including unused inputs, are
 * checked when the generated graph is compiled and executed. Invalid structure
 * or selectors return `InvalidArgument`; incompatible shape or dtype returns
 * `TypeMismatch`. Expansion is transactional on returned errors and allocation
 * exceptions. Callers serialize document writes. The returned edge is owned by
 * the document. Execution copies exact bits over requested regions and honors
 * cancellation.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> swizzle_channels(
    WorkflowDocument& document, const std::vector<ChannelEditInput>& inputs,
    const std::vector<ChannelEditSource>& slots,
    const ChannelAssemblyOptions& options = {});
/** @brief Append FMT-03B simultaneous assignments to original base slots.
 *
 * Destinations are unique; an empty `replacements` list is identity. Axis
 * declarations in source structures index cell axes and exclude the Result
 * batch prefix. Every
 * nonscalar source has the base batch prefix; scalar sources are unbatched
 * shape `[1]`. Complete sample rank, including batch and cell axes, is at most
 * 8. Every source reads the original immutable inputs. Unlisted destinations
 * retain their samples and semantics; replacements inherit destination
 * semantics, subject to explicit target fields. External component, channel and
 * scalar sources require explicit structure, the exact sample grid and the base
 * dtype. Transaction, exception, ownership and execution guarantees match
 * `swizzle_channels`.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> replace_channels(
    WorkflowDocument& document, const std::vector<ChannelEditInput>& inputs,
    const std::vector<ChannelReplacement>& replacements,
    const ChannelAssemblyOptions& options = {});
}  // namespace ps::format
