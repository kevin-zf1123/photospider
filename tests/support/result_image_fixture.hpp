#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace ps::test_image {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok()) {
    throw std::runtime_error(result.status().message);
  }
  return result.take_value();
}
inline void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
inline SchemaTemplate schema(bool mask = false) {
  SchemaTemplate schema;
  schema.id = "photospider.image";
  ResultTensorSpec slot;
  slot.batch_axes = {1, 1};
  slot.layout.spatial = true;
  slot.key = "pixels";
  slot.batch_axes[0] = slot.batch_axes[1] = 2;
  slot.descriptor = {ElementType::Float32,
                     mask ? std::vector<std::uint64_t>{3, 5}
                          : std::vector<std::uint64_t>{3, 5, 4}};
  if (mask) {
    slot.layout.channel_axis.reset();
  }
  slot.facets = {
      take(encode_semantic(mask ? coverage_semantics() : rgba_semantics()))};
  schema.tensors.push_back(std::move(slot));
  return schema;
}
inline ResultRef image(const ResourceBudget& root, bool mask = false,
                       float bias = 0) {
  const auto s = schema(mask);
  auto builder = take(ResultBuilder::start(root, s, "test.source"));

  const auto shape = s.tensors[0].sample_shape();
  const auto count = take(s.tensors[0].sample_count());
  std::vector<float> pixels(count);
  for (std::uint64_t i = 0; i < count; ++i) {
    pixels[i] = mask ? .25F : i % 4 == 3 ? .5F : bias + (i / 4) * .125F;
  }
  require(builder
              .publish_tensor(
                  0, Region::whole(shape),
                  ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                           pixels.size() * 4))
              .ok(),
          "source publication");
  return take(builder.seal());
}
inline ResultRef scalar(const ResourceBudget& root, float number) {
  SchemaTemplate schema;
  schema.id = "test.scalar";
  ResultTensorSpec tensor;
  tensor.key = "control";
  tensor.descriptor = {ElementType::Float32, {1}};
  schema.tensors.push_back(tensor);
  auto builder = take(ResultBuilder::start(root, schema, "test.control"));

  auto bytes = take(root.allocator().allocate(5));
  std::memcpy(bytes.data() + 1, &number, 4);
  require(builder
              .publish_tensor(0, Region::whole({1}), {1, {INT64_MIN}},
                              std::move(bytes).freeze())
              .ok(),
          "control publication");
  return take(builder.seal());
}
inline float read(const ResultRef& result,
                  std::vector<std::uint64_t> coordinate) {
  float value = 0;
  require(
      result.read_tensor(take(result.descriptor()), 0, coordinate, &value, 4)
          .ok(),
      "read image output");
  return value;
}
}  // namespace ps::test_image
