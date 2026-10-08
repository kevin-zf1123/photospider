#pragma once

#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "photospider/data/color_array.hpp"
#include "photospider/data/result.hpp"
#include "photospider/data/semantic.hpp"

namespace ps::input_internal {
/**
 * @brief Scoped binary32 nearest/gradual-underflow environment.
 * @note Floating state is thread-local. Save and restore the embedding's
 * rounding, denormal controls and exception flags even on callback failure.
 */
class Float32Environment final {
 public:
  Float32Environment() noexcept;
  ~Float32Environment() noexcept;
  Float32Environment(const Float32Environment&) = delete;
  Float32Environment& operator=(const Float32Environment&) = delete;
  [[nodiscard]] bool active() const noexcept { return active_; }

 private:
  std::fenv_t previous_{};
  bool saved_ = false;
  bool active_ = false;
};

/** @brief Exact 1..128-byte printable ASCII input name without spaces. */
bool valid_input_name(const std::string& name) noexcept;

/** @brief Checked dense metadata; never allocates a payload. */
struct DenseMetadata final {
  StridedLayout layout;
  std::uint64_t bytes = 0;
};

/** @brief Checks descriptor, signed canonical strides and host byte bounds. */
Result<DenseMetadata> dense_metadata(const ValueDescriptor& descriptor);

/** @brief Canonicalizes the same bounded facet vocabulary as Value::create. */
Status canonicalize_facets(std::vector<ValueFacet>* facets);

/** @brief Exact canonical facet comparison without coercion. */
bool same_facets(const std::vector<ValueFacet>& left,
                 const std::vector<ValueFacet>& right) noexcept;

/** @brief Detects structural image metadata independent of port kind. */
inline bool structural_image_metadata(const ValueDescriptor& descriptor,
                                      const std::vector<ValueFacet>& facets) {
  for (const auto& facet : facets) {
    if (facet.key == "photospider.image" ||
        (facet.key == "photospider.color-array" &&
         descriptor.shape.size() >= 3))
      return true;
    if (facet.key == "photospider.semantic") {
      auto semantic = decode_semantic(facet);
      if (semantic.ok() && (semantic.value().kind == SemanticKind::Image ||
                            semantic.value().kind == SemanticKind::ImagePlane ||
                            semantic.value().kind == SemanticKind::Mask))
        return true;
    }
  }
  return false;
}

/** @brief Tests exact whole logical coverage without allocation. */
bool whole_region(const Region& region,
                  const std::vector<std::uint64_t>& shape) noexcept;

/** @brief Returns the canonical image-v2 RGBA facet. */
ValueFacet image_facet();

/** @brief Validate recognized typed numeric domains over exact tensor samples.
 * Only explicit Validation obligations call this; empty sets and raw facets
 * read no payload. Batch and cell axes remain distinct. Windows retain exact
 * captured coverage and finite scans observe stop plus owning cancellation.
 */
Status validate_tensor_samples(const ResultRef& result,
                               const ResultDescriptor& descriptor,
                               uint32_t slot, const Footprint& samples,
                               const ResourceBudget& resources,
                               ErrorCode numeric_failure,
                               const CancellationToken& cancellation,
                               const std::function<ErrorCode()>& stop,
                               const std::function<Status(uint64_t)>& consume);

/** @brief Checks nonempty image demand including complete channel coverage. */
bool image_demand(const Region& region) noexcept;

/** @brief Recognized semantic keys; ColorArray is independent of SemanticKind.
 */
inline bool typed_facet(const std::string& key) noexcept {
  return key == "photospider.image" || key == "photospider.semantic" ||
         key == "photospider.color-array";
}

/** @brief Recognizes the independent generic ColorArray facet. */
inline bool color_array(const std::vector<ValueFacet>& facets) noexcept {
  return std::any_of(facets.begin(), facets.end(), [](const auto& facet) {
    return facet.key == "photospider.color-array";
  });
}

/** @brief Validates a requested Region and closes ColorArray output channels.
 * Image requests retain their existing all-channel admission rule. This is an
 * output observation rule, not authorization to widen an input read.
 */
Result<Region> color_output_region(const ValueDescriptor& descriptor,
                                   const std::vector<ValueFacet>& facets,
                                   const Region& requested);

/** @brief ColorArray output-observation closure; preserves other sample sets.
 */
Result<Footprint> color_output_samples(const ValueDescriptor& descriptor,
                                       const std::vector<ValueFacet>& facets,
                                       const Footprint& requested,
                                       const FootprintLimits& limits = {});

/** @brief Last channel axis for validated Image/ColorArray, otherwise absent.
 */
std::optional<std::size_t> tuple_channel_axis(
    const ValueDescriptor& descriptor,
    const std::vector<ValueFacet>& facets) noexcept;

/** @brief Requires full logical C for validated Image/ColorArray metadata.
 * @note Generic and other typed kinds add no channel-coverage restriction.
 */
bool complete_tuple_channels(const ValueDescriptor& descriptor,
                             const std::vector<ValueFacet>& facets,
                             const Region& region) noexcept;

/** @brief Expands only Validation to full colors; Data/Control stay local.
 * @note Caller has validated metadata. Empty remains Empty; limits and
 * cancellation propagate from Footprint operations without hidden supply reads.
 */
Result<Footprint> validation_closure(
    const ValueDescriptor& descriptor, const std::vector<ValueFacet>& facets,
    const Footprint& support, const FootprintLimits& limits,
    const std::function<Status(std::uint64_t)>& consume = {});

// Structural admission precedes port predicates; semantic descriptor checks
// follow them so a rejected predicate performs no additional decoding work.
Status validate_value_structure(const ValueDescriptor&,
                                const std::vector<ValueFacet>&);
Status validate_value_semantics(const ValueDescriptor&,
                                const std::vector<ValueFacet>&);
Status validate_value_metadata(const ValueDescriptor&,
                               const std::vector<ValueFacet>&);

// Borrowed Value stays alive through the synchronous typed-domain scan.
Status validate_value_samples(const Value& value, ErrorCode numeric_failure,
                              const std::function<ErrorCode()>& stop);
}  // namespace ps::input_internal
