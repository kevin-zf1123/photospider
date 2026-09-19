#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "data/dependency_metadata.hpp"
#include "data/input_validation.hpp"

namespace ps::dependency_internal {
// ColorArray validation is a per-observation certificate obligation. A fetch
// union can accidentally borrow another atom's validation and is not a proof.
inline bool color_input(const OperationMetadata& input) {
  return std::any_of(
      input.facets.begin(), input.facets.end(),
      [](const auto& f) { return f.key == "photospider.color-array"; });
}
inline Status color_closure_missing() {
  return {ErrorCode::InvalidArgument,
          "ColorArray dependency omits complete-color Validation",
          FailureReason::None,
          {FailureOrigin::Protocol, FailureScope::Unspecified}};
}
inline Status validate_color_needs(const std::vector<OperationMetadata>& inputs,
                                   const std::vector<DependencyNeed>& needs,
                                   const FootprintLimits& limits) {
  for (std::size_t port = 0; port < inputs.size(); ++port) {
    if (!color_input(inputs[port]))
      continue;
    auto empty = Footprint::none(inputs[port].descriptor.shape, limits);
    if (!empty.ok())
      return empty.status();
    auto observed = empty.value(), validation = empty.take_value();
    for (const auto& need : needs) {
      if (limits.consume_work) {
        auto charged = limits.consume_work(1);
        if (!charged.ok())
          return charged;
      }
      if (need.port != port)
        continue;
      if (need.roles & 3) {
        auto joined = observed.unite(need.samples, limits);
        if (!joined.ok())
          return joined.status();
        observed = joined.take_value();
      }
      if (need.roles & 4) {
        auto joined = validation.unite(need.samples, limits);
        if (!joined.ok())
          return joined.status();
        validation = joined.take_value();
      }
    }
    auto closure =
        input_internal::validation_closure(inputs[port], observed, limits);
    if (!closure.ok())
      return closure.status();
    auto missing = closure.value().subtract(validation, limits);
    if (!missing.ok())
      return missing.status();
    if (!missing.value().empty())
      return color_closure_missing();
  }
  return Status::success();
}
// Same copied-axis skeleton reduces the relation proof to union/subtract of
// fixed input intervals. Copied coordinates use a dummy singleton. The channel
// axis is fixed by closure. This proves arbitrary-sized pieces without visiting
// their atoms, and permits multiple Validation maps to cover one color.
inline Result<bool> color_map_proved(
    const OperationMetadata& input, const DependencyMappedNeed& data,
    const std::vector<DependencyMappedNeed>& maps,
    const FootprintLimits& limits) {
  std::array<DependencyAxis, 8> required{};
  std::copy(data.axes.begin(), data.axes.end(), required.begin());
  const auto channel =
      *input_internal::tuple_channel_axis(input.descriptor, input.facets);
  required[channel] = {-1, {0, input.descriptor.shape[channel]}};
  std::vector<RegionDimension> dimensions;
  for (std::size_t i = 0; i < data.axes.size(); ++i) {
    const auto& axis = required[i];
    dimensions.push_back(axis.observation_axis < 0 ? axis.fixed
                                                   : RegionDimension{0, 1});
  }
  auto remainder = Footprint::from_regions(input.descriptor.shape,
                                           {Region(dimensions)}, limits);
  if (!remainder.ok())
    return Result<bool>(remainder.status());
  for (const auto& candidate : maps) {
    if (limits.consume_work) {
      auto charged = limits.consume_work(1 + candidate.axes.size());
      if (!charged.ok())
        return Result<bool>(charged);
    }
    if (candidate.port != data.port || !(candidate.roles & 4) ||
        candidate.axes.empty())
      continue;
    bool compatible = true;
    for (std::size_t i = 0; i < data.axes.size(); ++i) {
      const auto& r = required[i];
      const auto& c = candidate.axes[i];
      if (r.observation_axis != c.observation_axis ||
          (r.observation_axis >= 0 && r.translation != c.translation)) {
        compatible = false;
        break;
      }
      dimensions[i] = c.observation_axis < 0 ? c.fixed : RegionDimension{0, 1};
    }
    if (!compatible)
      continue;
    auto coverage = Footprint::from_regions(input.descriptor.shape,
                                            {Region(dimensions)}, limits);
    if (!coverage.ok())
      return Result<bool>(coverage.status());
    auto missing = remainder.value().subtract(coverage.value(), limits);
    if (!missing.ok())
      return Result<bool>(missing.status());
    remainder = std::move(missing);
    if (remainder.value().empty())
      return Result<bool>(true);
  }
  return Result<bool>(false);
}
inline Status validate_color_certificate(
    const DependencyCertificate& certificate,
    const std::vector<OperationMetadata>& inputs, FootprintLimits limits,
    const std::function<Status(std::uint64_t)>& consume,
    const std::vector<AtomCertificate>* history = nullptr,
    const std::vector<DependencyNeed>* terminal_history = nullptr) {
  if (std::none_of(inputs.begin(), inputs.end(), color_input))
    return Status::success();
  auto remaining = limits.maximum_work;
  const auto prior = limits.consume_work;
  limits.consume_work = [&](std::uint64_t count) {
    if (limits.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (count > remaining)
      return Status{ErrorCode::ResourceExhausted,
                    "color relation proof work limit",
                    FailureReason::WorkLimit};
    remaining -= count;
    auto status = consume(count);
    return status.ok() && prior ? prior(count) : status;
  };
  if (!certificate.mapped()) {
    for (const auto& row : certificate.rows()) {
      auto charged = limits.consume_work(row.inputs.size() + 1);
      if (!charged.ok())
        return charged;
      const auto* prior_needs = terminal_history;
      if (history) {
        const auto found =
            std::lower_bound(history->begin(), history->end(), row.output,
                             [](const auto& prior, const auto& coordinate) {
                               return prior.output < coordinate;
                             });
        if (found != history->end() && found->output == row.output)
          prior_needs = &found->inputs;
      }
      std::shared_ptr<const MetadataOwner> retained;
      std::vector<DependencyNeed> combined;
      if (prior_needs && !prior_needs->empty()) {
        MetadataBytes capacity;
        capacity.needs(row.inputs, true);
        capacity.needs(*prior_needs, true);
        if (capacity.bytes > UINT64_MAX / 4)
          return {ErrorCode::ResourceExhausted, "color proof history capacity",
                  FailureReason::CapacityLimit};
        retained = metadata_owner(capacity.bytes * 4);
        charged = limits.consume_work(prior_needs->size() + row.inputs.size() +
                                      capacity.bytes / 8);
        if (!charged.ok())
          return charged;
        combined = row.inputs;
        combined.insert(combined.end(), prior_needs->begin(),
                        prior_needs->end());
      }
      auto status = validate_color_needs(
          inputs, combined.empty() ? row.inputs : combined, limits);
      if (!status.ok())
        return status;
    }
    return Status::success();
  }
  // create_mapped already proved disjoint, complete piece coverage and valid
  // axis bounds. A neighboring piece can never supply this piece's evidence.
  for (const auto& piece : certificate.mapping_pieces()) {
    bool proved = true;
    for (const auto& map : piece.inputs) {
      auto charged = limits.consume_work(1 + map.axes.size());
      if (!charged.ok())
        return charged;
      if (!(map.roles & 3) || map.axes.empty() ||
          !color_input(inputs[map.port]))
        continue;
      auto fast = color_map_proved(inputs[map.port], map, piece.inputs, limits);
      if (!fast.ok())
        return fast.status();
      if (!fast.value()) {
        proved = false;
        break;
      }
    }
    if (proved)
      continue;
    // Different copied-axis skeletons can still cover each other on restricted
    // domains. Exact row fallback accepts them within the admitted work budget;
    // uncertainty is ResourceExhausted, never a false protocol rejection.
    auto status = piece.coverage.visit(
        [&](const auto& coordinate) {
          auto charged = limits.consume_work(1);
          if (!charged.ok())
            return charged;
          auto row = certificate.row(coordinate, limits);
          if (!row.ok())
            return row.status();
          return validate_color_needs(inputs, row.value().inputs, limits);
        },
        remaining, limits.cancellation);
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::dependency_internal
