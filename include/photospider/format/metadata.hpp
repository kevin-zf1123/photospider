#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "photospider/format/channel.hpp"

namespace ps::format {
/** @brief Closed typed values for the registered tensor-description-v3 schema.
 * Strings never acquire resources or certify samples. Profile identities must
 * resolve in the ResourceBindings supplied to Compiler/ExecutionContext.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
using MetadataValue = std::variant<
    std::string, std::uint64_t, std::int64_t, double, TensorEncoding,
    TensorSampling, TensorConfiguredSpace, TensorAnalyticBinding,
    std::array<double, 2>, std::array<double, 6>, ColorProfileIdentity,
    TensorInterpretation, TensorChannelDescription, TensorAxisDescription,
    TensorColorGroup, TensorDescription, std::vector<std::uint64_t>,
    std::vector<TensorChannelDescription>, std::vector<TensorAxisDescription>,
    std::vector<TensorColorGroup>, ValueFacet>;
// NOLINTEND
/** @brief Atomic subtree replacement. Paths start with /semantic or
 * /annotations/<facet-key>. Segments escape '~' as ~0 and '/' as ~1.
 * Channel entries accept index:N, name:TEXT or role:TEXT selectors resolved
 * against the original source. Groups use their exact unique name; axes use
 * canonical decimal indices. See the metadata operation guide for leaf paths.
 */
struct MetadataSet final {
  std::string path;
  MetadataValue value;
};
/** @brief Static FMT-08 edit transaction. Defaults are explicit in generated
 * nodes. Replace requires description (possibly empty); patch forbids it.
 * Replace permits only annotation set/remove entries. Cascade can delete only
 * affected old dependent descriptions and must preserve all explicit sets.
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
/** @brief Append native FMT-08A without reading samples. Validates static
 * syntax/types/options and stages the entire node before append; malformed
 * edits return InvalidArgument and allocation failures ResourceExhausted with
 * the graph unchanged. Source-relative selectors and target structure are
 * validated at compile time. Input/returned edges belong to document; callers
 * serialize document writes. Immutable execution is thread-safe, preserves
 * exact bits/coverage and resource ownership, and disables sample-only caches.
 * Strict and named CPU profiles have identical metadata and byte semantics.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> assign_metadata(
    WorkflowDocument& document, WorkflowInput input,
    const MetadataOptions& options = {});
/** @brief FMT-08B deletion-only transactional lowering to FMT-08A. Options must
 * remain patch with no set/description/remove entries; targets supply remove.
 * Other failure, ownership, concurrency and execution rules match A.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> remove_metadata(
    WorkflowDocument& document, WorkflowInput input,
    const std::vector<std::string>& targets,
    const MetadataOptions& options = {});
}  // namespace ps::format
