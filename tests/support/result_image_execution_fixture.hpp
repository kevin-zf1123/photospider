#pragma once

#include <utility>

#include "support/result_image_operation_fixture.hpp"

namespace ps::test_image {
struct ScalarPreludeState {
  ScalarPreludeState(bool input, float number, int* count)
      : consumer(input), value(number), entries(count) {}
  bool consumer, requested = false;
  float value;
  int* entries;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (consumer && !requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, 2, false, take(TileSet::range(phase.resources, 0, 1))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (consumer) {
      ++*entries;
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
    Status status;
    for (uint32_t slot = phase.query.tensor_slot;
         slot == phase.query.tensor_slot; ++slot) {
      if (consumer) {
        // The output view retains its completed producer backing.
        auto window = phase.tensors->at({0, 2}).acquire(
            Region::whole({1}), phase.query.cancellation);
        if (!window.ok())
          return Result<ResultProgramPoll>(window.status());
        ResultTensorViewTransform transform;
        transform.source_axes = {{0, 0, 1, 1}};
        status = builder.publish_tensor_view(slot, Region::whole({1}),
                                             window.value(), transform);
      } else {
        status = builder.publish_tensor(
            slot, Region::whole({1}),
            ByteView(reinterpret_cast<const uint8_t*>(&value), sizeof(value)));
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
