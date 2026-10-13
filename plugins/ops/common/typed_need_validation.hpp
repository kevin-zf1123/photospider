#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <vector>

#include "data/typed_sample_validation.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::plugin_internal::ops_validation {
// Typed domain checks belong to the requesting operation. The granted input
// already contains complete tiles; validation must include their unused
// samples.
inline Status typed_input(const ResultProgramPhase& phase,
                          const ResultTensorInput& input) try {
  const auto& spec = input.spec();
  const auto& samples = input.coverage();
  const auto& resources = phase.resources;
  const auto& cancellation = phase.query.cancellation;
  const auto& consume = phase.consume_work;
  const auto stop = [&] {
    return cancellation.cancelled() ? ErrorCode::Cancelled : ErrorCode::Ok;
  };
  constexpr auto numeric_failure = ErrorCode::InvalidArgument;
  if (samples.empty())
    return Status::success();
  const ValueFacet* typed = nullptr;
  for (const auto& facet : spec.facets)
    if (input_internal::typed_facet(facet.key)) {
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
      auto acquired = input.acquire(region, cancellation);
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
          color ? input_internal::validate_color_samples(
                      *color, spec.descriptor, region, reader, numeric_failure,
                      stop)
                : input_internal::validate_semantic_samples(
                      *semantic, spec.descriptor, region,
                      spec.batch_axes.size(), reader, numeric_failure, stop);
      if (!checked.ok())
        return checked;
      if (cancellation.cancelled())
        return {ErrorCode::Cancelled, {}};
      {
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

inline Status typed_inputs(const ResultProgramPhase& phase) {
  if (!phase.tensors)
    return Status::success();
  for (const auto& entry : *phase.tensors) {
    auto status = typed_input(phase, entry.second);
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal::ops_validation
