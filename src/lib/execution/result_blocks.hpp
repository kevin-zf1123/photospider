#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "data/content_digest.hpp"
#include "execution/result_native.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::execution_internal {
inline Status result_block_state(const ResultRef& state,
                                 const ResourceBudget& root) {
  if (!state.owned_by(root) || !state.schema().fields.empty() ||
      state.schema().tensors.size() != 1 ||
      state.schema().publication != PublishPolicy::CompleteBundle)
    return Status{ErrorCode::InvalidArgument, "invalid Result block state"};
  auto facts = state.descriptor();
  if (!facts.ok())
    return facts.status();
  const auto& coverage = facts.value().tensor_coverage(0);
  auto count = state.schema().tensors[0].sample_count();
  if (!count.ok())
    return count.status();
  auto actual = coverage.element_count();
  return actual.ok() && actual.value() == count.value()
             ? Status::success()
             : Status{ErrorCode::InvalidArgument,
                      "incomplete Result block state"};
}
inline void append_block_footprint(content_internal::Sha256* hash,
                                   const Footprint& samples) {
  hash->integer(samples.shape().size());
  for (auto extent : samples.shape())
    hash->integer(extent);
  hash->integer(samples.boxes().size());
  for (const auto& box : samples.boxes())
    for (const auto& axis : box.dimensions()) {
      hash->integer(axis.offset);
      hash->integer(axis.extent);
    }
}
template <class Reader>
Status append_block_samples(content_internal::Sha256* hash,
                            const Footprint& samples, ElementType type,
                            Reader&& read,
                            const std::function<Status(std::uint64_t)>& work,
                            const CancellationToken& cancellation) {
  append_block_footprint(hash, samples);
  const auto width = Value::element_size(type);
  hash->integer(static_cast<std::uint32_t>(type));
  return samples.visit(
      [&](const auto& at) {
        auto stopped = work(0);
        if (!stopped.ok())
          return stopped;
        std::uint8_t bytes[8]{};
        auto status = read(at, bytes, width);
        if (!status.ok())
          return status;
        std::uint64_t bits = 0;
        if (width == 1) {
          bits = bytes[0];
        } else if (width == 4) {
          std::uint32_t word;
          std::memcpy(&word, bytes, 4);
          bits = word;
        } else {
          std::memcpy(&bits, bytes, 8);
        }
        hash->integer(bits);
        return Status::success();
      },
      UINT64_MAX, cancellation);
}
inline Result<Value> pack_block_state(const ResultRef& state,
                                      const ResultProgramPhase& phase) {
  const auto& spec = state.schema().tensors[0];
  const ValueDescriptor descriptor{spec.descriptor.element_type,
                                   spec.sample_shape()};
  if (phase.query.backend == Backend::Gpu) {
    auto facts = state.descriptor();
    if (!facts.ok())
      return Result<Value>(facts.status());
    auto window =
        state.acquire_tensor(facts.value(), 0, Region::whole(descriptor.shape),
                             phase.query.cancellation);
    if (!window.ok())
      return Result<Value>(window.status());
    return ResultNativeScope::input(window.value(), phase.consume_work,
                                    phase.query.cancellation);
  }
  auto made = MutableValue::allocate(
      descriptor, Region::whole(descriptor.shape), phase.resources.allocator());
  if (!made.ok())
    return Result<Value>(made.status());
  auto writer = made.take_value();
  auto facts = state.descriptor();
  if (!facts.ok())
    return Result<Value>(facts.status());
  std::uint64_t position = 0;
  const auto width = Value::element_size(descriptor.element_type);
  auto status = facts.value().tensor_coverage(0).visit(
      [&](const auto& at) {
        auto stopped = phase.consume_work(0);
        if (!stopped.ok())
          return stopped;
        auto read = state.read_tensor(facts.value(), 0, at,
                                      writer.data() + position * width, width,
                                      phase.query.cancellation);
        ++position;
        return read;
      },
      UINT64_MAX, phase.query.cancellation);
  if (!status.ok())
    return Result<Value>(status);
  return std::move(writer).publish();
}
inline Result<ResultRef> unpack_block_state(const Value& state,
                                            const SchemaTemplate& schema,
                                            const ResultProgramPhase& phase) {
  auto builder =
      ResultBuilder::start(phase.resources, schema, "result.block.state");
  if (!builder.ok())
    return Result<ResultRef>(builder.status());
  auto writer = builder.take_value();
  auto descriptor = ResultRelation::cartesian(phase.resources, 1, {});
  if (!descriptor.ok())
    return Result<ResultRef>(descriptor.status());
  auto status = writer.bind_descriptor_relation(descriptor.take_value());
  if (!status.ok())
    return Result<ResultRef>(status);
  auto count = schema.tensors[0].sample_count();
  if (!count.ok())
    return Result<ResultRef>(count.status());
  auto relation = ResultRelation::cartesian(phase.resources, count.value(), {});
  if (!relation.ok())
    return Result<ResultRef>(relation.status());
  status = writer.publish_tensor(
      0, Region::whole(schema.tensors[0].sample_shape()), state.layout(),
      state.storage(), relation.take_value(), {true, true, true, true},
      phase.query.cancellation);
  return status.ok() ? writer.seal() : Result<ResultRef>(status);
}
}  // namespace ps::execution_internal
