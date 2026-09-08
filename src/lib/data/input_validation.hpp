#pragma once

#include <cfenv>  // NOLINT(build/c++11)
#include <functional>
#include <string>
#include <vector>

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
/** @brief Checks scalar/image descriptors and exact profile facet metadata. */
Status validate_port_metadata(const OperationPortConstraint& port,
                              const ValueDescriptor& descriptor,
                              const std::vector<ValueFacet>& facets);
/** @brief Returns the exact S1 image profile facet. */
ValueFacet image_facet();
/**
 * @brief Checks dense port metadata and numeric domain without coercion.
 * @note stop is observed periodically while scanning, and returns Ok or a
 * prioritized Cancelled/Stale code without allocating diagnostic strings.
 */
Status validate_port_value(const OperationPortConstraint& port,
                           const Value& value, ErrorCode numeric_failure,
                           const std::function<ErrorCode()>& stop);
/** @brief Checks nonempty image demand including complete channel coverage. */
bool image_demand(const Region& region) noexcept;

}  // namespace ps::input_internal
