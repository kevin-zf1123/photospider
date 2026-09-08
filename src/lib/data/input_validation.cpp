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
  if (!whole_region(declaration->region, declaration->descriptor.shape) ||
      declaration->layout.byte_offset != 0 ||
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
      traits.output_schema.kind == OperationPortKind::Float32Scalar) {
    return failure(ErrorCode::InvalidArgument,
                   "invalid port schema count or output");
  }
  const bool image_output = traits.output_schema.kind ==
                            OperationPortKind::LinearPremultipliedRgbaFloat32;
  if (image_output &&
      (traits.output_element_type != ElementType::Float32 ||
       traits.shape_rule != OperationShapeRule::PreserveFirstInput ||
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
        (port.kind == OperationPortKind::LinearPremultipliedRgbaFloat32 &&
         traits.region_rule != OperationRegionRule::Whole && !image_output)) {
      return failure(ErrorCode::InvalidArgument,
                     "invalid input port combination");
    }
  }
  return Status::success();
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
  auto dense = dense_metadata(value.descriptor());
  if (!dense.ok())
    return dense.status();
  if (!whole_region(value.region(), value.descriptor().shape) ||
      value.layout().byte_offset != 0 ||
      value.layout().byte_strides != dense.value().layout.byte_strides ||
      value.bytes().size() != dense.value().bytes) {
    return failure(ErrorCode::TypeMismatch,
                   "port requires exact whole dense Value");
  }
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
  for (std::size_t offset = 0; offset < value.bytes().size(); offset += 16) {
    if (offset % 16384 == 0 && stop) {
      const auto code = stop();
      if (code != ErrorCode::Ok) {
        Status stopped;
        stopped.code = code;
        return stopped;
      }
    }
    float rgba[4];
    std::memcpy(rgba, value.bytes().data() + offset, sizeof(rgba));
    for (float channel : rgba) {
      if (!std::isfinite(channel) || channel < 0) {
        return failure(numeric_failure,
                       "image channel is negative or nonfinite");
      }
    }
    if (rgba[3] > 1 ||
        (rgba[3] == 0 && (rgba[0] != 0 || rgba[1] != 0 || rgba[2] != 0))) {
      return failure(numeric_failure,
                     "image violates premultiplied alpha domain");
    }
  }
  return Status::success();
}
}  // namespace ps::input_internal
