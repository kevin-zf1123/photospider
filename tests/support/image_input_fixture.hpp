#pragma once

#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace s1_fixture {
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
inline void check(ps::Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
inline ps::SchemaTemplate schema(std::vector<std::uint64_t> shape = {2, 2, 4},
                                 bool image = true) {
  const bool spatial = image || shape.size() == 2;
  ps::SchemaTemplate result;
  result.id = spatial ? "photospider.image" : "example.numeric";
  ps::ResultTensorSpec tensor;
  tensor.key = spatial ? "pixels" : "value";
  tensor.descriptor = {ps::ElementType::Float32, std::move(shape)};
  if (spatial) {
    tensor.batch_axes = {1, 1};
    tensor.layout.spatial = true;
    if (!image)
      tensor.layout.channel_axis.reset();
    tensor.atomic_trailing_axes = image ? 1 : 0;
  }
  result.tensors.push_back(std::move(tensor));
  return result;
}
inline ps::ResultRef tensor(const ps::ResourceBudget& root,
                            const std::vector<float>& pixels,
                            std::vector<std::uint64_t> shape = {2, 2, 4},
                            bool image = true) {
  const auto layout = schema(std::move(shape), image);
  const auto count = take(layout.tensors[0].sample_count());
  if (pixels.size() != count)
    throw std::runtime_error("fixture sample count mismatch");
  auto builder = take(ps::ResultBuilder::start(root, layout, "example.source"));

  check(builder.publish_tensor(
      0, ps::Region::whole(layout.tensors[0].sample_shape()),
      ps::ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                   pixels.size() * 4)));
  return take(builder.seal());
}
inline ps::ResultRef scalar(const ps::ResourceBudget& root, float number) {
  return tensor(root, {number}, {1}, false);
}
inline ps::WorkflowInputDeclaration declaration(
    std::uint64_t id, std::string name, const ps::SchemaTemplate& schema) {
  ps::WorkflowInputDeclaration input;
  input.id = id;
  input.name = std::move(name);
  input.result_schema = std::make_shared<ps::SchemaTemplate>(schema);
  return input;
}
inline ps::WorkflowInputDeclaration declaration(std::uint64_t id,
                                                std::string name,
                                                const ps::ResultRef& input) {
  return declaration(id, std::move(name), input.schema());
}
}  // namespace s1_fixture
