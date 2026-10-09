#include "data/value_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/dense_layout_validation.hpp"
#include "data/typed_sample_validation.hpp"
#include "photospider/data/tensor_description.hpp"

namespace ps::input_internal {
namespace {
Status failure(ErrorCode code, const char* message) {
  return Status::failure(code, message);
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
  if (!core_internal::dense_byte_size_representable<std::size_t>(stride)) {
    return Result<DenseMetadata>(failure(
        ErrorCode::ResourceExhausted, "dense bytes are not host addressable"));
  }
  result.bytes = stride;
  return Result<DenseMetadata>(std::move(result));
}

Status validate_facets(const std::vector<ValueFacet>& facets, bool canonical) {
  if (facets.size() > 64) {
    return failure(ErrorCode::InvalidArgument, "too many facets");
  }
  ResourceLease decoder_workspace;
  if (auto* root = resource_internal::metadata_budget()) {
    std::uint64_t bytes = 0;
    for (const auto& facet : facets)
      if (typed_facet(facet.key) ||
          facet.key == "photospider.tensor-description")
        bytes = std::max<std::uint64_t>(bytes, 512 + 16 * facet.payload.size());
    auto work = root->consume({facets.size() * facets.size() + bytes});
    if (!work.ok())
      return work;
    if (bytes) {
      auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
      if (!admitted.ok())
        return admitted.status();
      decoder_workspace = admitted.take_value();
    }
  }
  std::size_t total = 0;
  unsigned typed_count = 0;
  bool tensor_description = false;
  for (std::size_t index = 0; index < facets.size(); ++index) {
    const auto& facet = facets[index];
    if (facet.key.empty() || facet.key.size() > 256 || facet.version == 0 ||
        std::any_of(
            facet.key.begin(), facet.key.end(),
            [](unsigned char byte) { return byte < 0x21 || byte > 0x7e; }) ||
        std::any_of(
            facets.begin(), facets.begin() + index,
            [&](const auto& prior) { return prior.key == facet.key; }) ||
        (canonical && index && facets[index - 1].key >= facet.key)) {
      return failure(ErrorCode::InvalidArgument, "invalid or duplicate facet");
    }
    if (facet.payload.size() > 64 * 1024 ||
        facet.payload.size() > 1024 * 1024 - total) {
      return failure(ErrorCode::ResourceExhausted,
                     "facet payload bound exceeded");
    }
    if (typed_facet(facet.key)) {
      if (++typed_count > 1)
        return failure(ErrorCode::InvalidArgument, "multiple typed semantics");
      const auto status = facet.key == "photospider.color-array"
                              ? decode_color_array(facet).status()
                              : decode_semantic(facet).status();
      if (!status.ok())
        return status;
    }
    if (facet.key == "photospider.tensor-description") {
      auto decoded = decode_tensor_description(facet);
      if (!decoded.ok())
        return decoded.status();
      tensor_description = true;
    }
    total += facet.payload.size();
  }
  if (tensor_description && typed_count)
    return failure(
        ErrorCode::InvalidArgument,
        "tensor-description v3 cannot mix legacy typed coordinate conventions");
  return Status::success();
}
Status canonicalize_facets(std::vector<ValueFacet>* facets) {
  auto status = validate_facets(*facets, false);
  if (!status.ok())
    return status;
  std::sort(facets->begin(), facets->end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
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

ValueFacet image_facet() {
  return encode_semantic(rgba_semantics()).take_value();
}

bool image_demand(const Region& region) noexcept {
  return region.rank() == 3 && !region.empty() &&
         region.dimensions()[2].offset == 0 &&
         region.dimensions()[2].extent == 4;
}

Result<Region> color_output_region(const ValueDescriptor& descriptor,
                                   const std::vector<ValueFacet>& facets,
                                   const Region& requested) {
  auto status = requested.validate(descriptor.shape);
  if (!status.ok())
    return Result<Region>(status);
  if (!color_array(facets) || requested.empty())
    return Result<Region>(requested);
  auto dimensions = requested.dimensions();
  dimensions.back() = {0, descriptor.shape.back()};
  return Result<Region>(Region(std::move(dimensions)));
}
std::optional<std::size_t> tuple_channel_axis(
    const ValueDescriptor& descriptor,
    const std::vector<ValueFacet>& facets) noexcept {
  for (const auto& facet : facets) {
    if (facet.key == "photospider.image" && descriptor.shape.size() == 3)
      return 2;
    if (facet.key == "photospider.color-array" &&
        descriptor.shape.size() >= 2 && descriptor.shape.size() <= 8)
      return descriptor.shape.size() - 1;
  }
  return std::nullopt;
}
bool complete_tuple_channels(const ValueDescriptor& descriptor,
                             const std::vector<ValueFacet>& facets,
                             const Region& region) noexcept {
  const auto axis = tuple_channel_axis(descriptor, facets);
  return !axis ||
         (region.rank() == descriptor.shape.size() && !region.empty() &&
          region.dimensions()[*axis].offset == 0 &&
          region.dimensions()[*axis].extent == descriptor.shape[*axis]);
}
Result<Footprint> validation_closure(
    const ValueDescriptor& descriptor, const std::vector<ValueFacet>& facets,
    const Footprint& support, const FootprintLimits& limits,
    const std::function<Status(std::uint64_t)>& consume) {
  const auto axis = tuple_channel_axis(descriptor, facets);
  if (!axis || support.empty())
    return Result<Footprint>(support);
  std::vector<Region> regions;
  regions.reserve(support.boxes().size());
  for (const auto& box : support.boxes()) {
    const auto& charge = consume ? consume : limits.consume_work;
    if (charge) {
      auto status = charge(1 + descriptor.shape.size());
      if (!status.ok())
        return Result<Footprint>(status);
    }
    auto dimensions = box.dimensions();
    dimensions[*axis] = {0, descriptor.shape[*axis]};
    regions.emplace_back(std::move(dimensions));
  }
  return Footprint::from_regions(descriptor.shape, regions, limits);
}
Result<Footprint> color_output_samples(const ValueDescriptor& descriptor,
                                       const std::vector<ValueFacet>& facets,
                                       const Footprint& requested,
                                       const FootprintLimits& limits) {
  if (!requested.valid() || requested.shape() != descriptor.shape)
    return Result<Footprint>(
        Status{ErrorCode::InvalidArgument, "output footprint domain mismatch"});
  if (!color_array(facets))
    return Result<Footprint>(requested);
  return validation_closure(descriptor, facets, requested, limits);
}
Status validate_tensor_samples(
    const ResultRef& result, const ResultDescriptor& descriptor, uint32_t slot,
    const Footprint& samples, const ResourceBudget& resources,
    ErrorCode numeric_failure, const CancellationToken& cancellation,
    const std::function<ErrorCode()>& stop,
    const std::function<Status(uint64_t)>& consume) try {
  if (!result.valid() || slot >= result.schema().tensors.size() ||
      !samples.valid())
    return {ErrorCode::TypeMismatch, "invalid typed tensor samples"};
  const auto& spec = result.schema().tensors[slot];
  if (samples.empty())
    return Status::success();
  const ValueFacet* typed = nullptr;
  for (const auto& facet : spec.facets)
    if (typed_facet(facet.key)) {
      typed = &facet;
      break;
    }
  if (!typed)
    return Status::success();
  auto scratch = resources.reserve(ResourceCapacity::host(
      4096 + 4 * typed->payload.size(), 4096 + 4 * typed->payload.size()));
  if (!scratch.ok())
    return scratch.status();
  ResourceAllocationScope scope(resources);
  std::optional<SemanticDescriptor> semantic;
  std::optional<ColorArrayDescriptor> color;
  if (typed->key == "photospider.color-array") {
    auto decoded = decode_color_array(*typed);
    if (!decoded.ok())
      return decoded.status();
    color = decoded.take_value();
  } else {
    auto decoded = decode_semantic(*typed);
    if (!decoded.ok())
      return decoded.status();
    semantic = decoded.take_value();
  }
  for (const auto& box : samples.boxes()) {
    auto dimensions = box.dimensions();
    const std::size_t batches = 0;
    for (size_t axis = 0; axis < batches; ++axis)
      dimensions[axis].extent = 1;
    for (;;) {
      const Region region(dimensions);
      auto acquired =
          result.acquire_tensor(descriptor, slot, region, cancellation);
      if (!acquired.ok())
        return acquired.status();
      auto window = acquired.take_value();
      struct Cached {
        std::array<uint64_t, 8> at{};
        ResultTensorRun run;
        bool valid = false;
      };
      std::array<Cached, 4> cache{};
      const auto sample_axis = window.sample_axis();
      auto count = region.element_count();
      if (!count.ok())
        return count.status();
      uint64_t remaining = count.value(), credit = 0;
      const auto reader = [&](const auto& at) -> Result<double> {
        if (cancellation.cancelled())
          return Result<double>(Status{ErrorCode::Cancelled, {}});
        if (!credit) {
          const auto next = std::min<uint64_t>(256, remaining);
          if (consume) {
            // Precharge a bounded batch without changing the per-sample
            // validation. Successful regions still consume exactly N units;
            // a rejected charge never falls back to a smaller retry.
            auto charged = consume(next);
            if (!charged.ok())
              return Result<double>(charged);
          }
          credit = next;
          remaining -= credit;
          if (cancellation.cancelled())
            return Result<double>(Status{ErrorCode::Cancelled, {}});
        }
        --credit;
        auto& row = cache[at.back() % cache.size()];
        bool hit = row.valid && at[sample_axis] >= row.at[sample_axis] &&
                   at[sample_axis] - row.at[sample_axis] < row.run.samples;
        for (size_t axis = 0; hit && axis < at.size(); ++axis)
          if (axis != sample_axis && at[axis] != row.at[axis])
            hit = false;
        if (!hit) {
          auto run = window.row_run(at);
          if (!run.ok())
            return Result<double>(run.status());
          row.run = run.take_value();
          std::copy(at.begin(), at.end(), row.at.begin());
          row.valid = true;
        }
        const auto offset =
            static_cast<__int128>(at[sample_axis] - row.at[sample_axis]) *
            row.run.sample_stride_bytes;
        const auto* bytes = row.run.data + static_cast<std::ptrdiff_t>(offset);
        double value = 0;
        if (spec.descriptor.element_type == ElementType::Float32) {
          float narrow;
          std::memcpy(&narrow, bytes, 4);
          value = narrow;
        } else if (spec.descriptor.element_type == ElementType::Float64) {
          std::memcpy(&value, bytes, 8);
        } else {
          int64_t integer;
          std::memcpy(&integer, bytes, 8);
          value = static_cast<double>(integer);
        }
        return Result<double>(value);
      };
      auto checked =
          color ? validate_color_samples(*color, spec.descriptor, region,
                                         reader, numeric_failure, stop)
                : validate_semantic_samples(*semantic, spec.descriptor, region,
                                            spec.batch_axes.size(), reader,
                                            numeric_failure, stop);
      if (!checked.ok())
        return checked;
      if (cancellation.cancelled())
        return {ErrorCode::Cancelled, {}};
      if (stop) {
        const auto code = stop();
        if (code != ErrorCode::Ok)
          return {code, {}};
      }
      bool advanced = false;
      for (size_t axis = batches; axis-- > 0;) {
        if (++dimensions[axis].offset <
            box.dimensions()[axis].offset + box.dimensions()[axis].extent) {
          advanced = true;
          break;
        }
        dimensions[axis].offset = box.dimensions()[axis].offset;
      }
      if (!advanced)
        break;
    }
  }
  return Status::success();
} catch (const std::bad_alloc&) {
  return {ErrorCode::ResourceExhausted, {}};
}

Status validate_value_structure(const ValueDescriptor& descriptor,
                                const std::vector<ValueFacet>& facets) {
  const auto element = static_cast<std::uint32_t>(descriptor.element_type);
  if (element < 1 || element > 7 || descriptor.shape.empty() ||
      descriptor.shape.size() > 8 ||
      std::any_of(descriptor.shape.begin(), descriptor.shape.end(),
                  [](auto n) { return n == 0; }))
    return failure(ErrorCode::TypeMismatch, "invalid port descriptor");
  auto canonical_status = validate_facets(facets, true);
  if (!canonical_status.ok())
    return canonical_status;
  return Status::success();
}
Status validate_value_semantics(const ValueDescriptor& descriptor,
                                const std::vector<ValueFacet>& facets) {
  for (const auto& facet : facets) {
    if (facet.key == "photospider.tensor-description") {
      auto decoded = decode_tensor_description(facet);
      if (!decoded.ok())
        return decoded.status();
      auto status = validate_tensor_description(decoded.value(), descriptor);
      if (!status.ok())
        return status;
      continue;
    }
    if (facet.key == "photospider.color-array") {
      auto color = decode_color_array(facet);
      if (!color.ok())
        return color.status();
      auto status = validate_color_array_descriptor(color.value(), descriptor);
      if (!status.ok())
        return status;
      continue;
    }
    if (facet.key != "photospider.image" && facet.key != "photospider.semantic")
      continue;
    auto semantic = decode_semantic(facet);
    if (!semantic.ok())
      return semantic.status();
    auto status = validate_semantic_descriptor(semantic.value(), descriptor);
    if (!status.ok())
      return status;
  }
  return Status::success();
}
Status validate_value_metadata(const ValueDescriptor& descriptor,
                               const std::vector<ValueFacet>& facets) {
  auto valid = validate_value_structure(descriptor, facets);
  return valid.ok() ? validate_value_semantics(descriptor, facets) : valid;
}
Status validate_value_samples(const Value& value, ErrorCode numeric_failure,
                              const std::function<ErrorCode()>& stop) {
  for (const auto& facet : value.facets()) {
    if (facet.key == "photospider.color-array") {
      auto color = decode_color_array(facet);
      if (!color.ok())
        return color.status();
      return validate_color_array_value(color.value(), value, numeric_failure,
                                        stop);
    }
    if (facet.key != "photospider.image" && facet.key != "photospider.semantic")
      continue;
    auto semantic = decode_semantic(facet);
    if (!semantic.ok())
      return semantic.status();
    return validate_semantic_value(semantic.value(), value, numeric_failure,
                                   stop);
  }
  return Status::success();
}
}  // namespace ps::input_internal
