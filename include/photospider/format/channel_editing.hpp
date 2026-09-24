#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "photospider/format/channel_assembly.hpp"

namespace ps::format {
/** @brief FMT-03 explicit source kind. Exactly one of scalar/component/channels
 * is selected: scalar and component are mutually exclusive and forbid axis;
 * channels has both flags false, with optional metadata-resolved axis. */
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
/** @brief FMT-03A expands a nonempty ordered slot list to FMT-02C.
 * Input 0 establishes the base axis/grid. Other inputs must be explicit
 * scalars. Options inherit FMT-02 metadata/layout/profile semantics. Default
 * descriptions follow selected base components; only uniquely remapped complete
 * groups survive. No samples are read. All supplied input descriptors are
 * checked, even unused. Invalid structure/selectors return InvalidArgument;
 * shape/dtype mismatch returns TypeMismatch. Graph/metadata capacity returns a
 * bounded preflight failure. Expansion is transactional (also on bad_alloc);
 * callers must serialize writes to document. Returned edge and declarations are
 * owned by document. Execution preserves exact bits, regional support,
 * immutable ownership and cancellation; no completed-result cache or new
 * swizzle operation key is introduced.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> swizzle_channels(
    WorkflowDocument& document, const std::vector<ChannelEditInput>& inputs,
    const std::vector<ChannelEditSource>& slots,
    const ChannelAssemblyOptions& options = {});
/** @brief FMT-03B replaces unique destinations; an empty list is identity.
 * Sources read original inputs simultaneously. Unlisted destinations retain
 * samples and semantics; replacements inherit destination semantics. Explicit
 * target fields may change only listed slots; invalidated old groups are
 * dropped. External component/channel/scalar sources use explicit structure,
 * exact grids and the base dtype. Other preflight, exception, ownership,
 * thread, cache and execution contracts match swizzle_channels.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> replace_channels(
    WorkflowDocument& document, const std::vector<ChannelEditInput>& inputs,
    const std::vector<ChannelReplacement>& replacements,
    const ChannelAssemblyOptions& options = {});
}  // namespace ps::format
