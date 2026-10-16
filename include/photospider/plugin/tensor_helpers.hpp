#pragma once
#include <utility>
#include <variant>
#include <vector>

#include "photospider/plugin/tile_helpers.hpp"

namespace ps::sdk {
/** @brief Owned complete member capabilities, normally fragmented or affine.
 * No dense allocation occurs while assembling these grants. Capabilities and
 * acquired windows retain their owners beyond the current callback.
 */
using WholeTensorInputs = ResourceVector<ResultTensorInput>;
using ToWholeStep = std::variant<ResultProgramNeed, WholeTensorInputs>;
/** @brief Nonblocking acquisition step for all static members of one input.
 * Return the Need alternative from the operation poll, then call again in its
 * next poll. A ready alternative owns complete grants. This function never
 * executes a producer, waits, fills missing samples, or collects dense bytes.
 */
inline Result<ToWholeStep> to_whole(const ResultProgramPhase& phase,
                                    std::uint32_t input) try {
  using Answer = Result<ToWholeStep>;
  if (input >= phase.query.inputs.size() ||
      !phase.query.inputs[input].result_schema)
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid whole acquisition input"});
  auto scratch = phase.resources.reserve(ResourceCapacity::host(8192, 8192));
  if (!scratch.ok())
    return Answer(scratch.status());
  const auto& schema = *phase.query.inputs[input].result_schema;
  WholeTensorInputs ready{
      ResourceAllocator<ResultTensorInput>(phase.resources)};
  ResultProgramNeed need;
  bool missing = false;
  need.tiles =
      ResourceVector<TileNeed>(ResourceAllocator<TileNeed>(phase.resources));
  for (std::uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
    FootprintLimits limits;
    limits.consume_work = phase.consume_work;
    limits.cancellation = phase.query.cancellation;
    auto full = Footprint::all(schema.tensors[slot].sample_shape(), limits);
    if (!full.ok())
      return Answer(full.status());
    if (phase.tensors) {
      const auto found = phase.tensors->find({input, slot});
      if (found != phase.tensors->end() &&
          found->second.coverage() == full.value()) {
        ready.push_back(found->second);
      } else {
        missing = true;
      }
    } else {
      missing = true;
    }
    auto request = tile_need_for_samples(phase.resources, phase.query, input,
                                         slot, full.value());
    if (!request.ok())
      return Answer(request.status());
    need.tiles.push_back(request.take_value());
  }
  if (missing)
    return Answer(ToWholeStep(std::move(need)));
  return Answer(ToWholeStep(std::move(ready)));
} catch (const std::bad_alloc&) {
  return Result<ToWholeStep>(Status{ErrorCode::ResourceExhausted, {}});
}
/** @brief Owned windows covering exactly the selected member rectangles. */
using WholeTensorWindows = ResourceVector<ResultTensorReadWindow>;
using ToWholeWindowStep = std::variant<ResultProgramNeed, WholeTensorWindows>;
/** @brief Nonblocking acquisition of a static member or disjoint ROI.
 * Returns a compact tile Need until all selected samples are authorized, then
 * owning read windows clipped to the requested rectangles. No payload copy or
 * worker wait occurs. Empty selection returns an empty ready vector and grants
 * no descriptor or sample access. The phase is borrowed for this call only.
 * Invalid input/member/domain and resource/cancellation failures are returned.
 */
inline Result<ToWholeWindowStep> to_whole(const ResultProgramPhase& phase,
                                          std::uint32_t input,
                                          std::uint32_t slot,
                                          const Footprint& samples) try {
  using Answer = Result<ToWholeWindowStep>;
  if (phase.query.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  auto request =
      tile_need_for_samples(phase.resources, phase.query, input, slot, samples);
  if (!request.ok())
    return Answer(request.status());
  WholeTensorWindows ready{
      ResourceAllocator<ResultTensorReadWindow>(phase.resources)};
  if (samples.empty())
    return Answer(ToWholeWindowStep(std::move(ready)));
  FootprintLimits limits;
  limits.consume_work = phase.consume_work;
  limits.cancellation = phase.query.cancellation;
  if (phase.tensors) {
    const auto found = phase.tensors->find({input, slot});
    if (found != phase.tensors->end()) {
      auto missing = samples.subtract(found->second.coverage(), limits);
      if (!missing.ok())
        return Answer(missing.status());
      if (missing.value().empty()) {
        for (const auto& box : samples.boxes()) {
          auto window = found->second.acquire(box, phase.query.cancellation);
          if (!window.ok())
            return Answer(window.status());
          ready.push_back(window.take_value());
        }
        return Answer(ToWholeWindowStep(std::move(ready)));
      }
    }
  }
  ResultProgramNeed need;
  need.tiles =
      ResourceVector<TileNeed>(ResourceAllocator<TileNeed>(phase.resources));
  need.tiles.push_back(request.take_value());
  return Answer(ToWholeWindowStep(std::move(need)));
} catch (const std::bad_alloc&) {
  return Result<ToWholeWindowStep>(Status{ErrorCode::ResourceExhausted, {}});
}
/** @brief Acquire one member's full static coverage with owning windows. */
inline Result<ToWholeWindowStep> to_whole(const ResultProgramPhase& phase,
                                          std::uint32_t input,
                                          std::uint32_t slot) {
  using Answer = Result<ToWholeWindowStep>;
  if (input >= phase.query.inputs.size() ||
      !phase.query.inputs[input].result_schema ||
      slot >= phase.query.inputs[input].result_schema->tensors.size())
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid whole acquisition member"});
  FootprintLimits limits;
  limits.consume_work = phase.consume_work;
  limits.cancellation = phase.query.cancellation;
  auto full = Footprint::all(
      phase.query.inputs[input].result_schema->tensors[slot].sample_shape(),
      limits);
  return full.ok() ? to_whole(phase, input, slot, full.value())
                   : Answer(full.status());
}
/** @brief Explicit dense collection of one authorized rectangle.
 * Allocation, metadata, sample lookup and copying use the phase Root. Missing
 * coverage fails before payload allocation. No implicit view fallback occurs.
 */
inline Result<TensorSpan> collect(const ResultProgramPhase& phase,
                                  const ResultTensorInput& input,
                                  const Region& region) {
  auto scratch = phase.resources.reserve(ResourceCapacity::host(8192, 8192));
  if (!scratch.ok())
    return Result<TensorSpan>(scratch.status());
  auto window = input.acquire(region, phase.query.cancellation);
  if (!window.ok())
    return Result<TensorSpan>(window.status());
  auto allocated = TensorWriter::allocate(
      {input.spec().descriptor.element_type, input.spec().sample_shape()},
      region, phase.allocator);
  if (!allocated.ok())
    return Result<TensorSpan>(allocated.status());
  auto writer = allocated.take_value();
  auto copied = window.value().copy_to(writer.data(), writer.size());
  if (!copied.ok())
    return Result<TensorSpan>(copied);
  return std::move(writer).publish();
}
/** @brief Explicitly allocate, fill and publish one dense affine rectangle.
 * The callback sees one row-major writable window. All workers must join before
 * returning; no writable pointer may escape. Output storage uses the phase Root
 * and is checked against the output/stage payload bound at publication. Failure
 * poisons the builder and publishes nothing. This helper never collects inputs.
 */
inline Status publish_dense_tensor(
    const ResultProgramPhase& phase, ResultBuilder& builder, std::uint32_t slot,
    const Region& region,
    const std::function<Status(const ResourceVector<ResultTensorWriteWindow>&)>&
        write,
    const CancellationToken& cancellation = {}) try {
  const auto reject = [&](Status status) {
    builder.fail(status);
    return status;
  };
  if (!phase.query.output.result_schema ||
      slot >= phase.query.output.result_schema->tensors.size() || !write)
    return reject(
        Status{ErrorCode::InvalidArgument, "invalid dense publication"});
  if (cancellation.cancelled() || phase.query.cancellation.cancelled())
    return reject(Status{ErrorCode::Cancelled, {}});
  auto scratch = phase.resources.reserve(ResourceCapacity::host(4096, 4096));
  if (!scratch.ok())
    return reject(scratch.status());
  const auto& spec = phase.query.output.result_schema->tensors[slot];
  auto allocated = TensorWriter::allocate(
      {spec.descriptor.element_type, spec.sample_shape()}, region,
      phase.resources.allocator());
  if (!allocated.ok())
    return reject(allocated.status());
  auto writer = allocated.take_value();
  auto view = ResultTensorWriteWindow::borrow_dense(spec, writer);
  if (!view.ok())
    return reject(view.status());
  ResourceVector<ResultTensorWriteWindow> windows{
      ResourceAllocator<ResultTensorWriteWindow>(phase.resources)};
  windows.push_back(view.take_value());
  auto written = write(windows);
  windows.clear();
  if (!written.ok())
    return reject(written);
  if (cancellation.cancelled() || phase.query.cancellation.cancelled())
    return reject(Status{ErrorCode::Cancelled, {}});
  auto frozen = std::move(writer).publish();
  if (!frozen.ok())
    return reject(frozen.status());
  const auto& span = frozen.value();
  return builder.publish_tensor(slot, region, span.layout(), span.storage(),
                                phase.query.cancellation);
} catch (const std::bad_alloc&) {
  Status status{ErrorCode::ResourceExhausted, {}};
  builder.fail(status);
  return status;
} catch (...) {
  Status status{ErrorCode::OperationFailed, {}};
  builder.fail(status);
  return status;
}
/** @brief Publish the selected complete tile as immutable views of a Result.
 * Source schema must match the output. All source owners are retained; borrowed
 * storage is never expanded or copied. The source must cover the requested
 * member tile. Caller propagates the result as a completed publication.
 */
inline Result<ResultRef> to_tile(
    const ResultProgramPhase& phase, const ResultRef& source,
    std::optional<std::uint32_t> source_member = {}) {
  using Answer = Result<ResultRef>;
  if (!source.valid() || !phase.query.output.result_schema)
    return Answer(
        Status{ErrorCode::TypeMismatch, "tile source schema mismatch"});
  const auto slot = phase.query.tensor_slot;
  const auto source_slot = source_member.value_or(slot);
  const auto& target = *phase.query.output.result_schema;
  if (slot >= target.tensors.size() ||
      source_slot >= source.schema().tensors.size())
    return Answer(Status{ErrorCode::InvalidArgument, "invalid tile member"});
  if (!source_member && !source.schema().same_schema(target))
    return Answer(
        Status{ErrorCode::TypeMismatch, "tile source schema mismatch"});
  const auto associations = phase.association ? phase.association->size() : 0;
  if (associations > (UINT64_MAX - 8192) / sizeof(std::uint64_t))
    return Answer(
        Status{ErrorCode::ResourceExhausted, "tile view metadata overflow"});
  const auto bytes = 8192 + associations * sizeof(std::uint64_t);
  auto scratch = phase.resources.reserve(ResourceCapacity::host(bytes, bytes));
  if (!scratch.ok())
    return Answer(scratch.status());
  SchemaTemplate actual, expected;
  actual.tensors = {source.schema().tensors[source_slot]};
  expected.tensors = {target.tensors[slot]};
  actual.tensors[0].key = expected.tensors[0].key;
  if (!actual.same_schema(expected))
    return Answer(
        Status{ErrorCode::TypeMismatch, "tile member structure mismatch"});
  auto grid =
      TileGrid::from_tensor(target.tensors[slot], phase.query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto region = grid.value().region(phase.query.tile_unit);
  if (!region.ok())
    return Answer(region.status());
  auto facts = source.descriptor(false);
  if (!facts.ok())
    return Answer(facts.status());
  auto window = source.acquire_tensor(facts.value(), source_slot,
                                      region.value(), phase.query.cancellation);
  if (!window.ok())
    return Answer(window.status());
  auto made = ResultBuilder::start(
      phase.resources, target, phase.query.semantic_key, {},
      phase.association ? std::vector<std::uint64_t>(phase.association->begin(),
                                                     phase.association->end())
                        : std::vector<std::uint64_t>{},
      phase.query.tile_extent);
  if (!made.ok())
    return Answer(made.status());
  auto builder = made.take_value();
  ResultTensorViewTransform identity;
  for (std::uint32_t axis = 0; axis < region.value().rank(); ++axis)
    identity.source_axes.push_back({static_cast<std::int32_t>(axis), 0, 1, 1});
  auto status = builder.publish_tensor_view(
      slot, region.value(), window.value(), identity, phase.query.cancellation);
  if (!status.ok())
    return Answer(status);
  return builder.seal();
}
}  // namespace ps::sdk
