#pragma once

#include "support/result_image_fixture.hpp"

namespace ps::test_image {
struct ScalarPreludeState {
  ScalarPreludeState(bool input, float number)
      : consumer(input), value(number) {}
  bool consumer;
  float value;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (consumer) {
      auto status = phase.tensors->at({0, 2}).read({0}, &value, sizeof(value),
                                                   phase.query.cancellation);
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
    }
    auto started =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key);
    if (!started.ok())
      return Result<ResultProgramPoll>(started.status());
    auto builder = started.take_value();
    auto descriptor = ResultRelation::cartesian(
        phase.resources, 1, {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0});
    if (!descriptor.ok())
      return Result<ResultProgramPoll>(descriptor.status());
    auto status = builder.bind_descriptor_relation(descriptor.take_value());
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    auto relation = ResultRelation::cartesian(
        phase.resources, 1,
        {0, 1, 0, consumer ? 1U : 0U, ResultSupportTarget::Tensor,
         consumer ? 2U : 0U});
    if (!relation.ok())
      return Result<ResultProgramPoll>(relation.status());
    for (uint32_t slot = 0;
         slot < phase.query.output.result_schema->tensors.size(); ++slot) {
      if (consumer) {
        // A real output view pins the completed producer in the weak sharing
        // directory. Merely retaining its dependency facts does not pin bytes.
        auto window = phase.tensors->at({0, 2}).acquire(
            Region::whole({1}), phase.query.cancellation);
        if (!window.ok())
          return Result<ResultProgramPoll>(window.status());
        ResultTensorViewTransform transform;
        transform.source_axes = {{0, 0, 1, 1}};
        status = builder.publish_tensor_view(
            slot, Region::whole({1}), window.value(), transform,
            relation.value(), {true, true, true, true});
      } else {
        status = builder.publish_tensor(
            slot, Region::whole({1}),
            ByteView(reinterpret_cast<const uint8_t*>(&value), sizeof(value)),
            relation.value(), {true, true, true, true});
      }
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
    }
    auto result = builder.seal();
    return result.ok() ? Result<ResultProgramPoll>(
                             ResultPublication{result.take_value(), true})
                       : Result<ResultProgramPoll>(result.status());
  }
};
}  // namespace ps::test_image
