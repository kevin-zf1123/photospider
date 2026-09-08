#include "data/input_validation.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "plugin/dense_layout_validation.hpp"

namespace ps::input_internal {
namespace {
Status failure(ErrorCode code, const char* message) {
  return Status::failure(code, message);
}
std::uint32_t float_bits(float value) noexcept {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                "Float32 requires IEEE binary32");
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
bool valid_constraint(const OperationPortConstraint& port) noexcept {
  switch (port.kind) {
    case OperationPortKind::Value:
    case OperationPortKind::Float32Mask:
    case OperationPortKind::LinearPremultipliedRgbaFloat32:
      return float_bits(port.minimum) == 0 && float_bits(port.maximum) == 0;
    case OperationPortKind::Float32Scalar:
      return std::isfinite(port.minimum) && std::isfinite(port.maximum) &&
             port.minimum <= port.maximum;
  }
  return false;
}
}  // namespace

Float32Environment::Float32Environment() noexcept {
  saved_ = std::fegetenv(&previous_) == 0;
  active_ = saved_ && std::fesetenv(FE_DFL_ENV) == 0;
}
Float32Environment::~Float32Environment() noexcept {
  if (saved_)
    std::fesetenv(&previous_);
}

bool valid_input_name(const std::string& name) noexcept {
  return !name.empty() && name.size() <= 128 &&
         std::none_of(name.begin(), name.end(), [](unsigned char byte) {
           return byte < 0x21 || byte > 0x7e;
         });
}

Result<DenseMetadata> dense_metadata(const ValueDescriptor& descriptor) {
  if (descriptor.shape.empty() || descriptor.shape.size() > 8 ||
      std::any_of(descriptor.shape.begin(), descriptor.shape.end(),
                  [](std::uint64_t extent) { return extent == 0; })) {
    return Result<DenseMetadata>(
        failure(ErrorCode::InvalidArgument, "invalid dense descriptor shape"));
  }
  std::uint64_t stride = 0;
  try {
    stride = Value::element_size(descriptor.element_type);
  } catch (const std::invalid_argument&) {
    return Result<DenseMetadata>(
        failure(ErrorCode::InvalidArgument, "unknown dense element type"));
  }
  DenseMetadata result;
  result.layout.byte_strides.resize(descriptor.shape.size());
  for (std::size_t reverse = descriptor.shape.size(); reverse > 0; --reverse) {
    const auto axis = reverse - 1;
    if (stride > INT64_MAX || stride > UINT64_MAX / descriptor.shape[axis]) {
      return Result<DenseMetadata>(failure(ErrorCode::ResourceExhausted,
                                           "dense stride or product overflow"));
    }
    result.layout.byte_strides[axis] = static_cast<std::int64_t>(stride);
    stride *= descriptor.shape[axis];
  }
  if (!plugin_internal::dense_byte_size_representable<std::size_t>(stride)) {
    return Result<DenseMetadata>(failure(
        ErrorCode::ResourceExhausted, "dense bytes are not host addressable"));
  }
  result.bytes = stride;
  return Result<DenseMetadata>(std::move(result));
}

Status canonicalize_facets(std::vector<ValueFacet>* facets) {
  if (facets->size() > 64) {
    return failure(ErrorCode::InvalidArgument, "too many facets");
  }
  std::set<std::string> keys;
  std::size_t total = 0;
  for (const auto& facet : *facets) {
    if (facet.key.empty() || facet.key.size() > 256 || facet.version == 0 ||
        std::any_of(
            facet.key.begin(), facet.key.end(),
            [](unsigned char byte) { return byte < 0x21 || byte > 0x7e; }) ||
        !keys.insert(facet.key).second) {
      return failure(ErrorCode::InvalidArgument, "invalid or duplicate facet");
    }
    if (facet.payload.size() > 64 * 1024 ||
        facet.payload.size() > 1024 * 1024 - total) {
      return failure(ErrorCode::ResourceExhausted,
                     "facet payload bound exceeded");
    }
    total += facet.payload.size();
  }
  std::sort(
      facets->begin(), facets->end(),
      [](const ValueFacet& a, const ValueFacet& b) { return a.key < b.key; });
  return Status::success();
}

bool same_facets(const std::vector<ValueFacet>& left,
                 const std::vector<ValueFacet>& right) noexcept {
  if (left.size() != right.size())
    return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].key != right[i].key || left[i].version != right[i].version ||
        left[i].payload != right[i].payload)
      return false;
  }
  return true;
}

bool whole_region(const Region& region,
                  const std::vector<std::uint64_t>& shape) noexcept {
  if (region.rank() != shape.size())
    return false;
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (region.dimensions()[i].offset != 0 ||
        region.dimensions()[i].extent != shape[i])
      return false;
  }
  return true;
}

Status validate_declaration(WorkflowInputDeclaration* declaration) {
  auto dense = dense_metadata(declaration->descriptor);
  if (!dense.ok())
    return dense.status();
  if ((!declaration->layout.origin.empty() &&
       declaration->layout.origin.size() !=
           declaration->descriptor.shape.size()) ||
      !whole_region(declaration->region, declaration->descriptor.shape) ||
      declaration->layout.byte_offset != 0 ||
      std::any_of(declaration->layout.origin.begin(),
                  declaration->layout.origin.end(),
                  [](std::uint64_t value) { return value != 0; }) ||
      declaration->layout.byte_strides != dense.value().layout.byte_strides) {
    return failure(ErrorCode::InvalidArgument,
                   "input requires whole dense layout");
  }
  return canonicalize_facets(&declaration->facets);
}

Status validate_binding(const WorkflowInputDeclaration& declaration,
                        const Value& value) {
  if (!value.valid())
    return failure(ErrorCode::InvalidArgument, "invalid bound Value");
  if (value.descriptor().element_type != declaration.descriptor.element_type ||
      value.descriptor().shape != declaration.descriptor.shape ||
      !whole_region(value.region(), declaration.descriptor.shape)) {
    return failure(ErrorCode::TypeMismatch,
                   "binding descriptor or Region differs");
  }
  auto dense = dense_metadata(declaration.descriptor);
  if (!dense.ok())
    return dense.status();
  if (value.layout().byte_offset != 0 ||
      value.layout().byte_strides != declaration.layout.byte_strides ||
      value.bytes().size() != dense.value().bytes) {
    return failure(ErrorCode::TypeMismatch,
                   "binding layout or storage differs");
  }
  if (!same_facets(value.facets(), declaration.facets)) {
    return failure(ErrorCode::TypeMismatch, "binding facets differ");
  }
  return Status::success();
}

Status validate_port_schema(const OperationTraits& traits) {
  Float32Environment environment;
  if (!environment.active())
    return failure(ErrorCode::OperationFailed,
                   "cannot set binary32 environment");
  if (traits.input_count > 1024 ||
      traits.input_schema.size() != traits.input_count ||
      !valid_constraint(traits.output_schema) ||
      (traits.output_schema.kind == OperationPortKind::Float32Scalar ||
       traits.output_schema.kind == OperationPortKind::Float32Mask)) {
    return failure(ErrorCode::InvalidArgument,
                   "invalid port schema count or output");
  }
  const bool image_output = traits.output_schema.kind ==
                            OperationPortKind::LinearPremultipliedRgbaFloat32;
  if (image_output &&
      (traits.output_element_type != ElementType::Float32 ||
       (traits.shape_rule != OperationShapeRule::PreserveFirstInput &&
        traits.shape_rule != OperationShapeRule::MatchAllInputs) ||
       traits.input_schema.empty() ||
       traits.input_schema.front().kind !=
           OperationPortKind::LinearPremultipliedRgbaFloat32)) {
    return failure(ErrorCode::InvalidArgument,
                   "image output must preserve first image");
  }
  for (std::size_t i = 0; i < traits.input_schema.size(); ++i) {
    const auto& port = traits.input_schema[i];
    if (!valid_constraint(port) ||
        (port.kind == OperationPortKind::Float32Scalar &&
         (traits.shape_rule == OperationShapeRule::MatchAllInputs ||
          (i == 0 &&
           traits.shape_rule == OperationShapeRule::PreserveFirstInput))) ||
        ((port.kind == OperationPortKind::LinearPremultipliedRgbaFloat32 ||
          port.kind == OperationPortKind::Float32Mask) &&
         traits.region_rule != OperationRegionRule::Whole && !image_output)) {
      return failure(ErrorCode::InvalidArgument,
                     "invalid input port combination");
    }
  }
  return Status::success();
}

/**
 * @brief Derives one legal input demand from an operation Region rule.
 * @param traits Canonical compiler-visible operation traits.
 * @param output_demand Valid demanded coverage of the operation output.
 * @param output_shape Statically inferred output shape.
 * @param input_shape Producer descriptor shape for this input.
 * @return Whole, exact, or overflow-safe clipped-halo input demand.
 * @throws std::bad_alloc If result or diagnostic allocation fails.
 * @note Halo expansion clips without evaluating overflowing addition.
 */
Result<Region> derive_input_demand(
    const OperationTraits& traits, const Region& output_demand,
    const std::vector<std::uint64_t>& output_shape,
    const std::vector<std::uint64_t>& input_shape, OperationPortKind kind) {
  const Status output_status = output_demand.validate(output_shape);
  if (!output_status.ok() || output_demand.empty()) {
    return Result<Region>(Status::failure(
        ErrorCode::InvalidArgument,
        "physical planning output demand is empty or out of bounds"));
  }
  if (kind == OperationPortKind::Float32Scalar) {
    return Result<Region>(Region::whole(input_shape));
  }
  if (kind == OperationPortKind::Float32Mask &&
      traits.region_rule != OperationRegionRule::Whole) {
    if (input_shape.size() != 2 || output_shape.size() != 3 ||
        input_shape[0] != output_shape[0] || input_shape[1] != output_shape[1])
      return Result<Region>(Status::failure(
          ErrorCode::TypeMismatch, "mask/image spatial shapes differ"));
    return derive_input_demand(
        traits,
        Region({output_demand.dimensions()[0], output_demand.dimensions()[1]}),
        input_shape, input_shape, OperationPortKind::Value);
  }
  switch (traits.region_rule) {
    case OperationRegionRule::Whole:
      return Result<Region>(Region::whole(input_shape));
    case OperationRegionRule::Elementwise:
      if (input_shape != output_shape) {
        return Result<Region>(Status::failure(
            ErrorCode::TypeMismatch,
            "elementwise Region rule requires matching input/output shapes"));
      }
      return Result<Region>(output_demand);
    case OperationRegionRule::Halo:
      if (input_shape != output_shape || traits.halo_radius == 0U) {
        return Result<Region>(Status::failure(
            ErrorCode::TypeMismatch,
            "halo Region rule requires matching shapes and positive radius"));
      }
      break;
  }
  std::vector<RegionDimension> dimensions;
  dimensions.reserve(input_shape.size());
  const std::uint64_t radius = traits.halo_radius;
  for (std::size_t axis = 0U; axis < input_shape.size(); ++axis) {
    const RegionDimension& requested = output_demand.dimensions()[axis];
    if (kind == OperationPortKind::LinearPremultipliedRgbaFloat32 &&
        axis == 2) {
      dimensions.push_back(RegionDimension{0, 4});
      continue;
    }
    const std::uint64_t start =
        requested.offset > radius ? requested.offset - radius : 0U;
    const std::uint64_t requested_end = requested.offset + requested.extent;
    const std::uint64_t right_room = input_shape[axis] - requested_end;
    const std::uint64_t end = requested_end + std::min(radius, right_room);
    dimensions.push_back(RegionDimension{start, end - start});
  }
  return Result<Region>(Region(std::move(dimensions)));
}

ValueFacet image_facet() {
  const std::string profile = "rgba;linear-srgb;premultiplied;hwc";
  return ValueFacet{"photospider.image", 1, {profile.begin(), profile.end()}};
}

Status validate_port_metadata(const OperationPortConstraint& port,
                              const ValueDescriptor& descriptor,
                              const std::vector<ValueFacet>& facets) {
  if (port.kind == OperationPortKind::Value)
    return Status::success();
  if (descriptor.element_type != ElementType::Float32) {
    return failure(ErrorCode::TypeMismatch, "port requires Float32");
  }
  if (port.kind == OperationPortKind::Float32Mask) {
    if (descriptor.shape.size() != 2 || descriptor.shape[0] == 0 ||
        descriptor.shape[1] == 0 || !facets.empty())
      return failure(ErrorCode::TypeMismatch, "mask requires HW and no facets");
    return Status::success();
  }
  if (port.kind == OperationPortKind::Float32Scalar) {
    if (descriptor.shape != std::vector<std::uint64_t>{1} || !facets.empty()) {
      return failure(ErrorCode::TypeMismatch,
                     "scalar requires shape one and no facets");
    }
  } else if (descriptor.shape.size() != 3 || descriptor.shape[0] == 0 ||
             descriptor.shape[1] == 0 || descriptor.shape[2] != 4 ||
             !same_facets(facets, {image_facet()})) {
    return failure(ErrorCode::TypeMismatch,
                   "image requires HWC RGBA and exact profile");
  }
  return Status::success();
}

bool image_demand(const Region& region) noexcept {
  return region.rank() == 3 && !region.empty() &&
         region.dimensions()[2].offset == 0 &&
         region.dimensions()[2].extent == 4;
}

Status validate_port_value(const OperationPortConstraint& port,
                           const Value& value, ErrorCode numeric_failure,
                           const std::function<ErrorCode()>& stop) {
  if (!value.valid())
    return failure(ErrorCode::InvalidArgument, "invalid port Value");
  if (port.kind == OperationPortKind::Value)
    return Status::success();
  auto status =
      validate_port_metadata(port, value.descriptor(), value.facets());
  if (!status.ok())
    return status;
  if (value.region().empty())
    return failure(ErrorCode::TypeMismatch, "port requires nonempty coverage");
  if (port.kind == OperationPortKind::Float32Scalar &&
      (!whole_region(value.region(), {1}) || value.bytes().size() != 4 ||
       value.layout().byte_offset != 0 ||
       value.layout().byte_strides != std::vector<std::int64_t>{4}))
    return failure(ErrorCode::TypeMismatch,
                   "scalar requires exact dense coverage");
  Float32Environment environment;
  if (!environment.active())
    return failure(ErrorCode::OperationFailed,
                   "cannot set binary32 environment");
  if (port.kind == OperationPortKind::Float32Scalar) {
    float scalar = 0;
    std::memcpy(&scalar, value.bytes().data(), sizeof(scalar));
    if (!std::isfinite(scalar) || scalar < port.minimum ||
        scalar > port.maximum) {
      return failure(numeric_failure,
                     "scalar is nonfinite or outside port interval");
    }
    return Status::success();
  }
  const auto y_region = value.region().dimensions()[0];
  const auto x_region = value.region().dimensions()[1];
  for (std::uint64_t y = y_region.offset; y < y_region.offset + y_region.extent;
       ++y) {
    if (stop) {
      const auto code = stop();
      if (code != ErrorCode::Ok) {
        Status stopped;
        stopped.code = code;
        return stopped;
      }
    }
    for (std::uint64_t x = x_region.offset;
         x < x_region.offset + x_region.extent; ++x) {
      if (port.kind == OperationPortKind::Float32Mask) {
        auto offset = value.byte_address({y, x});
        if (!offset.ok())
          return offset.status();
        float sample = 0;
        std::memcpy(&sample, value.bytes().data() + offset.value(),
                    sizeof(sample));
        if (!std::isfinite(sample) || sample < 0 || sample > 1)
          return failure(numeric_failure, "mask sample outside finite [0,1]");
        continue;
      }
      float rgba[4];
      for (std::uint64_t c = 0; c < 4; ++c) {
        auto offset = value.byte_address({y, x, c});
        if (!offset.ok())
          return offset.status();
        std::memcpy(&rgba[c], value.bytes().data() + offset.value(),
                    sizeof(float));
      }
      for (float channel : rgba) {
        if (!std::isfinite(channel) || channel < 0)
          return failure(numeric_failure,
                         "image channel is negative or nonfinite");
      }
      if (rgba[3] > 1 ||
          (rgba[3] == 0 && (rgba[0] != 0 || rgba[1] != 0 || rgba[2] != 0)))
        return failure(numeric_failure,
                       "image violates premultiplied alpha domain");
    }
  }
  return Status::success();
}
}  // namespace ps::input_internal
