#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "photospider/format/channel.hpp"

namespace ps::format {
/** @brief Closed typed values accepted by the tensor-description-v4/v5
 * metadata editor. Strings remain data: they do not load resources or certify
 * sample values. Profile and configuration identities must resolve through
 * ResourceBindings during compilation and execution.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
using MetadataValue = std::variant<
    std::string, std::uint64_t, std::int64_t, double, TensorEncoding,
    TensorSampling, TensorConfiguredSpace, TensorAnalyticBinding,
    TensorModelCoordinates, std::array<double, 2>, std::array<double, 6>,
    ColorProfileIdentity, TensorInterpretation, TensorChannelDescription,
    TensorAxisDescription, TensorColorGroup, TensorDescription,
    std::vector<std::uint64_t>, std::vector<TensorChannelDescription>,
    std::vector<TensorAxisDescription>, std::vector<TensorColorGroup>,
    ValueFacet>;
// NOLINTEND
/** @brief One typed metadata assignment.
 *
 * A path begins with `/semantic` or `/annotations/<facet-key>`. Escape `~` as
 * `~0` and `/` as `~1` within a segment. Channel entries accept `index:N`,
 * `name:TEXT` or `role:TEXT`, resolved against the original source; group names
 * must be exact and unique, and axis indices use canonical decimal notation.
 * Setting a subtree replaces it atomically. See the metadata operation guide
 * for the registered leaf paths and value types.
 */
struct MetadataSet final {
  std::string path;
  MetadataValue value;
};
/** @brief Static FMT-08 edit options copied into the generated workflow node.
 *
 * `mode` is `patch` or `replace`; replace requires `description` (possibly
 * empty), while patch forbids it. Replace edits may target annotations only.
 * `dependencies` is `error` or `cascade`; cascade removes affected prior
 * descriptions but preserves explicit assignments. `missing` is `error` or
 * `ignore` and applies only to absent deletion targets. `layout` is `auto`,
 * `view` or `materialize`. `profile` selects `strict`,
 * `accelerated_apple_silicon` or `accelerated_x86_64`.
 */
struct MetadataOptions final {
  std::string mode = "patch";
  std::vector<MetadataSet> set;
  std::optional<TensorDescription> description;
  std::vector<std::string> remove;
  std::string dependencies = "error";
  std::string missing = "error";
  std::string layout = "auto";
  std::string profile = "strict";
};
/** @brief Append a Result-based FMT-08A metadata edit node to `document`.
 *
 * The source must be a Result schema with one tensor member and no fields. The
 * helper validates typed edit syntax and options and appends transactionally;
 * a failure leaves `document` unchanged. An internal optional canonical-source
 * assertion can constrain the schema; this helper does not set it. Compilation
 * resolves selectors, validates the complete target description and required
 * resource identities, without reading sample values. Published views retain
 * source backing and resource owners; materialized Results own copied backing.
 * The Result operation preserves tensor structure and sample bits, requests
 * Data plus Descriptor support, has no Validation or Control role, and disables
 * result caching. An empty output
 * request publishes an empty Result without requesting payload. `auto` uses a
 * legal Result view and copies only when that view is unavailable; `view`
 * reports `ViewUnavailable`; `materialize` copies requested samples with
 * cancellation checks at most every 256 samples. Named CPU profiles preserve
 * the same metadata and byte semantics.
 *
 * Calls that mutate the same `document` must be serialized by the caller.
 * Compiled immutable workflows can execute concurrently.
 *
 * @param document Workflow to extend; unchanged if authoring fails.
 * @param input Existing Result input edge.
 * @param options Static metadata edits and execution policy.
 * @return The new node's `values` output edge, or an error such as
 *         `InvalidArgument` or `ResourceExhausted`.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> assign_metadata(
    WorkflowDocument& document, WorkflowInput input,
    const MetadataOptions& options = {});
/** @brief Lower a deletion-only metadata edit to the Result-based FMT-08A node.
 *
 * `options` must remain in patch mode with empty `set`, `description` and
 * `remove`; `targets` supplies the deletion paths. The helper has the same
 * source schema requirement, transaction behavior and execution semantics as
 * assign_metadata.
 *
 * @param document Workflow to extend; unchanged if authoring fails.
 * @param input Existing Result input edge.
 * @param targets Static paths to remove.
 * @param options Shared dependency, missing, layout and CPU profile policy.
 * @return The lowered node's `values` edge, or an error such as
 *         `InvalidArgument` or `ResourceExhausted`.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> remove_metadata(
    WorkflowDocument& document, WorkflowInput input,
    const std::vector<std::string>& targets,
    const MetadataOptions& options = {});
}  // namespace ps::format
