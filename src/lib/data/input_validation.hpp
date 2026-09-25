#pragma once

#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "photospider/data/color_array.hpp"
#include "photospider/plugin/operation_registry.hpp"

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
inline bool structural_image_metadata(const OperationMetadata& metadata) {
  if (metadata.planar_layout)
    return true;
  for (const auto& facet : metadata.facets) {
    if (facet.key == "photospider.image" ||
        (facet.key == "photospider.color-array" &&
         metadata.descriptor.shape.size() >= 3))
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
/** @brief Validates and canonicalizes a metadata-only declaration. */
Status validate_declaration(WorkflowInputDeclaration* declaration);
/** @brief Checks a bound Value against already canonical declaration facts. */
Status validate_binding(const WorkflowInputDeclaration& declaration,
                        const Value& value);
/** @brief Checks all closed port schema combinations before registration. */
Status validate_port_schema(const OperationTraits& traits);
/** @brief Checks declarative output/repeated-input records before publication.
 */
Status validate_operation_contract(const OperationTraits& traits);
/** @brief Checks scalar/image descriptors and exact profile facet metadata. */
Status validate_port_metadata(const OperationPortConstraint& port,
                              const OperationMetadata& metadata);
Status validate_port_metadata(const OperationPortConstraint& port,
                              const ValueDescriptor& descriptor,
                              const std::vector<ValueFacet>& facets);
/** @brief Shared checked Whole/elementwise/halo demand rule for planning and
 * direct invocation. Halo traits must have their static parameter resolved.
 */
Result<Region> derive_input_demand(
    const OperationTraits& traits, const Region& output_demand,
    const std::vector<std::uint64_t>& output_shape,
    const std::vector<std::uint64_t>& input_shape, OperationPortKind kind,
    std::uint32_t input_port = 0);
/** @brief Returns the canonical image-v2 RGBA facet. */
ValueFacet image_facet();
/**
 * @brief Checks dense port metadata and numeric domain without coercion.
 * @note stop is observed periodically while scanning, and returns Ok or a
 * prioritized Cancelled/Stale code without allocating diagnostic strings.
 */
Status validate_port_value(const OperationPortConstraint& port,
                           const Value& value, ErrorCode numeric_failure,
                           const std::function<ErrorCode()>& stop);
/** @brief Storage eligibility for image-v2 or canonical coverage-mask metadata.
 * @note Float32 only; image channels/roles come from the canonical descriptor.
 */
inline Status validate_image_storage_metadata(
    const ValueDescriptor& descriptor, const std::vector<ValueFacet>& facets) {
  if (descriptor.element_type != ElementType::Float32 || facets.size() != 1)
    return Status::failure(
        ErrorCode::TypeMismatch,
        "image storage requires Float32 and one typed facet");
  auto semantic = decode_semantic(facets[0]);
  if (!semantic.ok())
    return semantic.status();
  if (semantic.value().kind != SemanticKind::Image) {
    const auto expected = encode_semantic(coverage_semantics()).take_value();
    if (facets[0].key != expected.key ||
        facets[0].version != expected.version ||
        facets[0].payload != expected.payload)
      return Status::failure(
          ErrorCode::TypeMismatch,
          "image storage requires image or coverage semantics");
  }
  return validate_semantic_descriptor(semantic.value(), descriptor);
}
/** @brief Checks storage eligibility, complete image channels and samples. */
inline Status validate_image_storage_value(
    const Value& value, const std::function<ErrorCode()>& stop = {}) {
  if (!value.valid())
    return Status::failure(ErrorCode::InvalidArgument,
                           "invalid image storage Value");
  auto status =
      validate_image_storage_metadata(value.descriptor(), value.facets());
  if (!status.ok())
    return status;
  return validate_semantic_value(
      decode_semantic(value.facets()[0]).take_value(), value,
      ErrorCode::InvalidArgument, stop);
}
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
Result<Footprint> color_output_samples(const OperationMetadata& metadata,
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
    const OperationMetadata& metadata, const Footprint& support,
    const FootprintLimits& limits,
    const std::function<Status(std::uint64_t)>& consume = {});
/** @brief Copies a static need as Validation with fixed full channel interval.
 * @note Caller has validated metadata and mapping rank. Source tags and all
 * leading-axis relations remain unchanged.
 */
DependencyMappedNeed validation_map(DependencyMappedNeed support,
                                    const OperationMetadata& metadata);

}  // namespace ps::input_internal
