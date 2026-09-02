#include "photospider/data/value.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps {
namespace {

/**
 * @brief Computes the inclusive addressed byte range of a strided dense view.
 * @param descriptor Validated logical descriptor.
 * @param layout Candidate byte layout with one stride per axis.
 * @param element_size Physical scalar width.
 * @return Pair of signed minimum and maximum byte offsets, or a failure.
 * @throws std::bad_alloc If a diagnostic allocation fails.
 * @note Zero-stride and singleton axes contribute no address span and are
 * skipped before any extent conversion or stride-magnitude arithmetic. Every
 * contributing axis is checked before signed addition/multiplication.
 */
Result<std::pair<std::int64_t, std::int64_t>> addressed_range(
    const ValueDescriptor& descriptor, const StridedLayout& layout,
    std::size_t element_size) {
  if (layout.byte_offset >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return Result<std::pair<std::int64_t, std::int64_t>>(Status::failure(
        ErrorCode::InvalidArgument, "Value byte offset exceeds int64"));
  }
  std::int64_t minimum = static_cast<std::int64_t>(layout.byte_offset);
  std::int64_t maximum = minimum;
  for (std::size_t axis = 0; axis < descriptor.shape.size(); ++axis) {
    const std::uint64_t steps = descriptor.shape[axis] - 1U;
    const std::int64_t stride = layout.byte_strides[axis];
    if (steps == 0U || stride == 0) {
      continue;
    }
    if (steps >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return Result<std::pair<std::int64_t, std::int64_t>>(Status::failure(
          ErrorCode::InvalidArgument, "Value stride span overflows int64"));
    }
    if (stride == std::numeric_limits<std::int64_t>::min()) {
      return Result<std::pair<std::int64_t, std::int64_t>>(
          Status::failure(ErrorCode::InvalidArgument,
                          "Value stride uses unsupported INT64_MIN"));
    }
    const std::int64_t magnitude = stride < 0 ? -stride : stride;
    if (steps > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max() / magnitude)) {
      return Result<std::pair<std::int64_t, std::int64_t>>(Status::failure(
          ErrorCode::InvalidArgument, "Value stride span overflows int64"));
    }
    const std::int64_t span = static_cast<std::int64_t>(steps) * magnitude;
    if (stride < 0) {
      if (span > minimum) {
        return Result<std::pair<std::int64_t, std::int64_t>>(
            Status::failure(ErrorCode::InvalidArgument,
                            "Value negative stride precedes buffer"));
      }
      minimum -= span;
    } else {
      if (span > std::numeric_limits<std::int64_t>::max() - maximum) {
        return Result<std::pair<std::int64_t, std::int64_t>>(Status::failure(
            ErrorCode::InvalidArgument, "Value positive stride exceeds int64"));
      }
      maximum += span;
    }
  }
  if (element_size - 1U >
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() -
                               maximum)) {
    return Result<std::pair<std::int64_t, std::int64_t>>(Status::failure(
        ErrorCode::InvalidArgument, "Value element tail exceeds int64"));
  }
  maximum += static_cast<std::int64_t>(element_size - 1U);
  return Result<std::pair<std::int64_t, std::int64_t>>(
      std::make_pair(minimum, maximum));
}

/**
 * @brief Validates and canonicalizes bounded Value facets.
 * @param facets Candidate owned facet records.
 * @return Success or a precise invalid-argument/resource failure.
 * @throws std::bad_alloc If canonical key tracking or diagnostics allocate.
 * @note At most 64 records, 64 KiB each, and 1 MiB total payload are accepted.
 */
Status validate_facets(std::vector<ValueFacet>* facets) {
  constexpr std::size_t kMaximumFacetCount = 64U;
  constexpr std::size_t kMaximumKeyBytes = 256U;
  constexpr std::size_t kMaximumFacetPayloadBytes = 64U * 1024U;
  constexpr std::size_t kMaximumTotalPayloadBytes = 1024U * 1024U;
  if (!facets || facets->size() > kMaximumFacetCount) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "Value facet count exceeds 64");
  }
  std::set<std::string> keys;
  std::size_t total_payload = 0U;
  for (const ValueFacet& facet : *facets) {
    if (facet.key.empty() || facet.key.size() > kMaximumKeyBytes ||
        facet.version == 0U) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "Value facet key/version is invalid");
    }
    for (unsigned char byte : facet.key) {
      if (byte < 0x21U || byte > 0x7eU) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "Value facet key is not printable ASCII");
      }
    }
    if (!keys.insert(facet.key).second) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "Value facet keys must be unique");
    }
    if (facet.payload.size() > kMaximumFacetPayloadBytes ||
        facet.payload.size() > kMaximumTotalPayloadBytes - total_payload) {
      return Status::failure(ErrorCode::ResourceExhausted,
                             "Value facet payload bounds are exceeded");
    }
    total_payload += facet.payload.size();
  }
  std::sort(facets->begin(), facets->end(),
            [](const ValueFacet& left, const ValueFacet& right) {
              return left.key < right.key;
            });
  return Status::success();
}

}  // namespace

/**
 * @brief Implements atomic validated Value publication.
 * @copydetails Value::create
 */
Result<Value> Value::create(ValueDescriptor descriptor, Region region,
                            StridedLayout layout,
                            std::vector<std::uint8_t> bytes,
                            std::vector<ValueFacet> facets) {
  if (descriptor.shape.empty() || descriptor.shape.size() > 8U ||
      descriptor.shape.size() != layout.byte_strides.size()) {
    return Result<Value>(Status::failure(
        ErrorCode::InvalidArgument,
        "Value descriptor/layout rank must match and be in 1..8"));
  }
  for (std::uint64_t extent : descriptor.shape) {
    if (extent == 0U) {
      return Result<Value>(Status::failure(
          ErrorCode::InvalidArgument, "Value shape extents must be nonzero"));
    }
  }
  const Status region_status = region.validate(descriptor.shape);
  if (!region_status.ok()) {
    return Result<Value>(region_status);
  }
  std::size_t scalar_size = 0U;
  try {
    scalar_size = element_size(descriptor.element_type);
  } catch (const std::invalid_argument& error) {
    return Result<Value>(
        Status::failure(ErrorCode::InvalidArgument, error.what()));
  }
  auto range = addressed_range(descriptor, layout, scalar_size);
  if (!range.ok()) {
    return Result<Value>(range.status());
  }
  if (range.value().first < 0 || range.value().second < range.value().first ||
      static_cast<std::uint64_t>(range.value().second) >= bytes.size()) {
    return Result<Value>(Status::failure(
        ErrorCode::InvalidArgument, "Value layout addresses outside buffer"));
  }
  const Status facet_status = validate_facets(&facets);
  if (!facet_status.ok()) {
    return Result<Value>(facet_status);
  }

  Value value;
  value.descriptor_ = std::move(descriptor);
  value.region_ = std::move(region);
  value.layout_ = std::move(layout);
  value.facets_ = std::move(facets);
  value.bytes_ =
      std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
  return Result<Value>(std::move(value));
}

/**
 * @brief Implements canonical Float64 scalar construction.
 * @copydetails Value::from_float64
 */
Value Value::from_float64(double value) {
  std::vector<std::uint8_t> bytes(sizeof(double));
  std::memcpy(bytes.data(), &value, sizeof(double));
  auto result =
      create(ValueDescriptor{ElementType::Float64, {1U}}, Region::whole({1U}),
             StridedLayout{0U, {8}}, std::move(bytes));
  if (!result.ok()) {
    throw std::logic_error("internal scalar Value construction failed");
  }
  return result.take_value();
}

/**
 * @brief Implements checked Float64 scalar extraction.
 * @copydetails Value::as_float64
 */
Result<double> Value::as_float64() const {
  const bool exact_scalar_region = region_.rank() == 1U &&
                                   region_.dimensions()[0U].offset == 0U &&
                                   region_.dimensions()[0U].extent == 1U;
  if (!valid() || descriptor_.element_type != ElementType::Float64 ||
      descriptor_.shape != std::vector<std::uint64_t>{1U} ||
      !exact_scalar_region ||
      layout_.byte_strides != std::vector<std::int64_t>{8} ||
      layout_.byte_offset + sizeof(double) > bytes_->size()) {
    return Result<double>(Status::failure(
        ErrorCode::TypeMismatch, "Value is not a contiguous Float64 scalar"));
  }
  double value = 0.0;
  std::memcpy(&value, bytes_->data() + layout_.byte_offset, sizeof(double));
  return Result<double>(value);
}

/**
 * @brief Implements immutable descriptor access.
 * @copydetails Value::descriptor
 */
const ValueDescriptor& Value::descriptor() const {
  if (!valid()) {
    throw std::logic_error("default Value has no descriptor");
  }
  return descriptor_;
}

/**
 * @brief Implements immutable logical Region access.
 * @copydetails Value::region
 */
const Region& Value::region() const {
  if (!valid()) {
    throw std::logic_error("default Value has no Region");
  }
  return region_;
}

/**
 * @brief Implements immutable strided-layout access.
 * @copydetails Value::layout
 */
const StridedLayout& Value::layout() const {
  if (!valid()) {
    throw std::logic_error("default Value has no layout");
  }
  return layout_;
}

/**
 * @brief Implements canonical immutable facet access.
 * @copydetails Value::facets
 */
const std::vector<ValueFacet>& Value::facets() const {
  if (!valid()) {
    throw std::logic_error("default Value has no facets");
  }
  return facets_;
}

/**
 * @brief Implements immutable byte-storage access.
 * @copydetails Value::bytes
 */
const std::vector<std::uint8_t>& Value::bytes() const {
  if (!valid()) {
    throw std::logic_error("default Value has no bytes");
  }
  return *bytes_;
}

/**
 * @brief Implements closed element-width lookup.
 * @copydetails Value::element_size
 */
std::size_t Value::element_size(ElementType type) {
  switch (type) {
    case ElementType::UInt8:
      return 1U;
    case ElementType::Int64:
    case ElementType::Float64:
      return 8U;
  }
  throw std::invalid_argument("unknown ElementType");
}

}  // namespace ps
