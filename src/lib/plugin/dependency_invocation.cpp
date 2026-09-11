#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps {
namespace {
Result<std::string> invocation_identity() {
  static std::atomic<std::uint64_t> next{1};
  auto value = next.load();
  do {
    if (value == UINT64_MAX)
      return Result<std::string>(
          Status::failure(ErrorCode::ResourceExhausted,
                          "direct invocation identities exhausted"));
  } while (!next.compare_exchange_weak(value, value + 1));
  return Result<std::string>("direct-" + std::to_string(value));
}
}  // namespace
Result<Value> OperationRegistry::invoke_dependency_current(
    const std::string& key, const OperationInvocation& invocation,
    const std::function<bool()>& current) const {
  const auto stop = [&] {
    if (invocation.cancellation.cancelled())
      return ErrorCode::Cancelled;
    return current && !current() ? ErrorCode::Stale : ErrorCode::Ok;
  };
  const auto failure = [&](Status status) {
    const auto code = stop();
    if (code != ErrorCode::Ok) {
      status.code = code;
      status.message.clear();
    }
    return Result<Value>(std::move(status));
  };
  try {
    if (stop() != ErrorCode::Ok)
      return failure(Status{stop(), {}});
    auto found = find_traits(key);
    if (!found.ok())
      return failure(found.status());
    auto selected =
        select_operation_output(found.value(), invocation.output_index);
    if (!selected.ok())
      return failure(selected.status());
    auto resolved = resolve_operation_traits(
        selected.value(),
        invocation.input_metadata.empty() ? invocation.inputs.size()
                                          : invocation.input_metadata.size(),
        invocation.parameters);
    if (!resolved.ok())
      return failure(resolved.status());
    if (invocation.input_demands.size() != invocation.inputs.size())
      return failure(
          Status::failure(ErrorCode::InvalidArgument,
                          "dependency invocation demand count mismatch"));
    auto metadata = invocation.input_metadata;
    if (metadata.empty())
      metadata.resize(invocation.inputs.size());
    if (!invocation.input_indices.empty() &&
        invocation.input_indices.size() != invocation.inputs.size())
      return failure(
          Status{ErrorCode::InvalidArgument, "input index count mismatch"});
    std::vector<std::size_t> slots(metadata.size(), SIZE_MAX);
    for (std::size_t i = 0; i < invocation.inputs.size(); ++i) {
      const auto port =
          invocation.input_indices.empty() ? i : invocation.input_indices[i];
      if (port >= metadata.size() || slots[port] != SIZE_MAX)
        return failure(
            Status{ErrorCode::InvalidArgument, "invalid input projection"});
      slots[port] = i;
      const auto& input = invocation.inputs[i];
      if (!input.valid())
        return failure(Status{ErrorCode::InvalidArgument,
                              "invalid direct dependency input"});
      auto view = input.view(invocation.input_demands[i]);
      if (!view.ok())
        return failure(view.status());
      if (invocation.input_metadata.empty())
        metadata[port] = {input.descriptor(), input.facets()};
      else if (input.descriptor().element_type !=
                   metadata[port].descriptor.element_type ||
               input.descriptor().shape != metadata[port].descriptor.shape ||
               !input_internal::same_facets(input.facets(),
                                            metadata[port].facets))
        return failure(Status{ErrorCode::TypeMismatch,
                              "projected input metadata mismatch"});
    }
    auto inferred = infer_operation_output(resolved.value(), metadata,
                                           invocation.parameters);
    if (!inferred.ok())
      return failure(inferred.status());
    const auto region = invocation.output_region.rank()
                            ? invocation.output_region
                            : Region::whole(inferred.value().descriptor.shape);
    if (region.empty())
      return failure(Status::failure(ErrorCode::InvalidArgument,
                                     "empty direct output Region"));
    auto samples =
        Footprint::from_regions(inferred.value().descriptor.shape, {region});
    if (!samples.ok())
      return failure(samples.status());
    auto observations =
        operation_observations(inferred.value(), samples.value());
    if (!observations.ok())
      return failure(observations.status());
    auto identity = invocation_identity();
    if (!identity.ok())
      return failure(identity.status());
    std::map<std::uint32_t, DependencyCheckpoint> completed;
    DependencyCheckpointServices checkpoints;
    checkpoints.identity = identity.value();
    checkpoints.find = [&](std::uint32_t phase, std::uint64_t before)
        -> Result<std::optional<DependencyCheckpoint>> {
      const auto found = completed.find(phase);
      if (found == completed.end() || found->second.sequence() > before)
        return Result<std::optional<DependencyCheckpoint>>(
            std::optional<DependencyCheckpoint>{});
      return Result<std::optional<DependencyCheckpoint>>(found->second);
    };
    checkpoints.publish = [&](const DependencyCheckpoint& checkpoint) {
      if (checkpoint.metadata_entries() <= 65536 &&
          (completed.count(checkpoint.phase()) || completed.size() < 64))
        completed.insert_or_assign(checkpoint.phase(), checkpoint);
      return Status::success();
    };
    auto run = [&](const Footprint& outputs) -> Result<DependencyResult> {
      DependencyRequest request{metadata,
                                invocation.parameters,
                                outputs,
                                identity.value(),
                                invocation.backend,
                                invocation.cancellation,
                                {}};
      request.output_index = invocation.output_index;
      auto started =
          start_dependency(key, std::move(request), invocation.allocator);
      if (!started.ok())
        return Result<DependencyResult>(started.status());
      auto session = started.take_value();
      for (;;) {
        if (stop() != ErrorCode::Ok)
          return Result<DependencyResult>(Status{stop(), {}});
        auto progress = session->poll(invocation.allocator, checkpoints);
        if (!progress.ok())
          return Result<DependencyResult>(progress.status());
        auto event = progress.take_value();
        if (auto* result = std::get_if<DependencyResult>(&event))
          return Result<DependencyResult>(std::move(*result));
        auto pending = session->pending_reads();
        if (!pending.ok())
          return Result<DependencyResult>(pending.status());
        std::vector<ValueFragments> ready;
        for (std::size_t port = 0; port < metadata.size(); ++port) {
          auto needed = Footprint::none(metadata[port].descriptor.shape);
          if (!needed.ok())
            return Result<DependencyResult>(needed.status());
          for (const auto& read : pending.value())
            if (read.port == port) {
              auto joined = needed.value().unite(read.samples);
              if (!joined.ok())
                return Result<DependencyResult>(joined.status());
              needed = std::move(joined);
            }
          std::vector<Value> values;
          if (!needed.value().empty()) {
            if (slots[port] == SIZE_MAX)
              return Result<DependencyResult>(
                  Status{ErrorCode::InvalidArgument,
                         "required input was not supplied"});
            const auto slot = slots[port];
            auto supplied =
                invocation.inputs[slot].view(invocation.input_demands[slot]);
            if (!supplied.ok())
              return Result<DependencyResult>(supplied.status());
            values.push_back(supplied.take_value());
          }
          auto fragments = ValueFragments::create(
              metadata[port].descriptor, metadata[port].facets,
              needed.take_value(), std::move(values));
          if (!fragments.ok())
            return Result<DependencyResult>(fragments.status());
          ready.push_back(fragments.take_value());
        }
        auto status = session->supply(std::move(ready), identity.value());
        if (!status.ok())
          return Result<DependencyResult>(status);
      }
    };
    if (resolved.value().outputs[0].observation_kind ==
        ObservationKind::RequestRecord) {
      auto terminal = run(samples.value());
      if (!terminal.ok())
        return failure(terminal.status());
      auto result =
          terminal.value().value.collect(region, invocation.allocator);
      if (!result.ok())
        return failure(result.status());
      if (stop() != ErrorCode::Ok)
        return failure(Status{stop(), {}});
      return result;
    }
    auto allocation = MutableValue::allocate(inferred.value().descriptor,
                                             region, invocation.allocator);
    if (!allocation.ok())
      return failure(allocation.status());
    auto output = allocation.take_value();
    const auto width =
        Value::element_size(inferred.value().descriptor.element_type);
    auto status = observations.value().visit(
        [&](const auto& coordinate) {
          std::vector<RegionDimension> dims;
          for (auto c : coordinate)
            dims.push_back({c, 1});
          auto atom = Footprint::from_regions(observations.value().shape(),
                                              {Region(std::move(dims))});
          if (!atom.ok())
            return atom.status();
          auto requested = observation_samples(inferred.value(), atom.value());
          if (!requested.ok())
            return requested.status();
          auto result = run(requested.value());
          if (!result.ok())
            return result.status();
          return requested.value().visit(
              [&](const auto& sample) {
                std::uint64_t offset = 0;
                for (std::size_t axis = 0; axis < sample.size(); ++axis)
                  offset += (sample[axis] - region.dimensions()[axis].offset) *
                            static_cast<std::uint64_t>(
                                output.layout().byte_strides[axis]);
                if (offset > output.size() || width > output.size() - offset)
                  return Status::failure(
                      ErrorCode::Internal,
                      "dependency collector address overflow");
                return result.value().value.read(sample, output.data() + offset,
                                                 width);
              },
              64, invocation.cancellation);
        },
        1048576, invocation.cancellation);
    if (!status.ok())
      return failure(status);
    if (stop() != ErrorCode::Ok)
      return failure(Status{stop(), {}});
    return std::move(output).publish(inferred.value().facets);
  } catch (const std::bad_alloc&) {
    return failure(Status::failure(ErrorCode::ResourceExhausted,
                                   "direct dependency allocation failed"));
  } catch (const std::exception& error) {
    return failure(Status::failure(ErrorCode::OperationFailed,
                                   error.what() ? error.what() : ""));
  } catch (...) {
    return failure(Status::failure(ErrorCode::OperationFailed,
                                   "direct dependency invocation exception"));
  }
}
}  // namespace ps
