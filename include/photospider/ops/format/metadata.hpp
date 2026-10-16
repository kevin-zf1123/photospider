#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "photospider/ops/export.hpp"
#include "photospider/ops/tensor_description.hpp"

namespace ps::format {
/** @brief Closed typed values accepted by the tensor-description-v4/v5
 * configuration editor. Strings remain data: they do not load resources or
 * certify sample values. An operation needing profile or configuration bytes
 * explicitly requests their identities during preparation for host resolution
 * through ResourceSet. The editor itself performs no resource lookup.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
using MetadataValue = std::variant<
    std::string, std::uint64_t, std::int64_t, double, TensorEncoding,
    TensorSampling, TensorConfiguredSpace, TensorAnalyticBinding,
    TensorModelCoordinates, std::array<double, 2>, std::array<double, 6>,
    ResourceIdentity, TensorInterpretation, TensorChannelDescription,
    TensorAxisDescription, TensorColorGroup, TensorDescription,
    std::vector<std::uint64_t>, std::vector<TensorChannelDescription>,
    std::vector<TensorAxisDescription>, std::vector<TensorColorGroup>>;
// NOLINTEND
/** @brief One typed metadata assignment.
 *
 * A path begins with `/semantic`. Escape `~` as
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
/** @brief Pure SDK edit options for explicitly supplied descriptions.
 *
 * `mode` is `patch` or `replace`; replace requires `description` (possibly
 * empty), while patch forbids it.
 * `dependencies` is `error` or `cascade`; cascade removes affected prior
 * descriptions but preserves explicit assignments. `missing` is `error` or
 * `ignore` and applies only to absent deletion targets.
 */
struct MetadataOptions final {
  std::string mode = "patch";
  std::vector<MetadataSet> set;
  std::optional<TensorDescription> description;
  std::vector<std::string> remove;
  std::string dependencies = "error";
  std::string missing = "error";
};
/** @brief Pure transactional edit of explicitly supplied operator
 * configuration. Descriptor alignment and edit conflicts are validated; failure
 * leaves source unchanged. No graph node, payload read, or resource lookup is
 * performed. Only /semantic paths are accepted. Allocation failure may throw
 * bad_alloc.
 */
PHOTOSPIDER_OPS_SDK_API Result<TensorDescription> edit_description(
    const TensorDescription& source, const TensorDescriptor& descriptor,
    const MetadataOptions& options = {});
}  // namespace ps::format
