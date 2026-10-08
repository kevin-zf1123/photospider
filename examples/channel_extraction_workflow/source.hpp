#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace channel_fixture {
template <class T>
T take(ps::Result<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().message);
  return value.take_value();
}
inline void require(ps::Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
inline void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
inline ps::StridedLayout dense(ps::ElementType type,
                               const std::vector<std::uint64_t>& shape) {
  ps::StridedLayout layout;
  layout.byte_strides.resize(shape.size());
  std::uint64_t stride = ps::Value::element_size(type);
  for (std::size_t axis = shape.size(); axis-- > 0;) {
    layout.byte_strides[axis] = stride;
    stride *= shape[axis];
  }
  return layout;
}
struct Source {
  ps::SchemaTemplate schema;
  ps::StridedLayout layout;
  std::vector<std::uint8_t> bytes;
  std::vector<ps::Region> coverage;
  ps::ResourceBindings resources;
  std::uint64_t tile_height = 128, tile_width = 128;
};
inline Source source(ps::ValueDescriptor descriptor,
                     std::vector<ps::ValueFacet> facets = {},
                     ps::ResultTensorLayout physical = {},
                     std::vector<std::uint64_t> batches = {}) {
  Source input;
  input.schema.id = "example.channels";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = std::move(descriptor);
  std::sort(facets.begin(), facets.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  tensor.facets = std::move(facets);
  tensor.layout = std::move(physical);
  tensor.batch_axes.assign(batches.begin(), batches.end());
  const auto shape = tensor.sample_shape();
  input.layout = dense(tensor.descriptor.element_type, shape);
  input.bytes.resize(take(tensor.sample_count()) *
                     ps::Value::element_size(tensor.descriptor.element_type));
  for (std::size_t i = 0; i < input.bytes.size(); ++i)
    input.bytes[i] = (17 + 37 * i) & 255;
  input.schema.tensors.push_back(std::move(tensor));
  input.coverage = {ps::Region::whole(shape)};
  return input;
}
inline std::size_t address(const Source& source,
                           const std::vector<std::uint64_t>& at) {
  __int128 offset = source.layout.byte_offset;
  for (std::size_t axis = 0; axis < at.size(); ++axis)
    offset +=
        (static_cast<__int128>(at[axis]) -
         (source.layout.origin.empty() ? 0 : source.layout.origin[axis])) *
        source.layout.byte_strides[axis];
  check(offset >= 0 &&
            offset + ps::Value::element_size(
                         source.schema.tensors[0].descriptor.element_type) <=
                source.bytes.size(),
        "fixture address");
  return static_cast<std::size_t>(offset);
}
inline ps::ResultRef publish(const ps::ResourceBudget& root,
                             const Source& source) {
  auto builder = take(ps::ResultBuilder::start(
      root, source.schema, "channel.source", {}, {}, source.tile_height,
      source.tile_width, source.resources));
  require(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {0, 8, 0, 0}))));
  const auto& spec = source.schema.tensors[0];
  const auto shape = spec.sample_shape();
  const auto relation = take(ps::ResultRelation::cartesian(
      root, take(spec.sample_count()), {0, 1, 0, 0}));
  if (spec.layout.spatial) {
    for (const auto& region : source.coverage)
      require(builder.publish_tensor_kernel(
          0, region,
          [&](const auto& writers) {
            for (const auto& writer : writers) {
              auto samples =
                  take(ps::Footprint::from_regions(shape, {writer.region()}));
              auto status = samples.visit(
                  [&](const auto& at) {
                    auto output = writer.row_run(at);
                    if (!output.ok())
                      return output.status();
                    std::memcpy(
                        output.value().data,
                        source.bytes.data() + address(source, at),
                        ps::Value::element_size(spec.descriptor.element_type));
                    return ps::Status::success();
                  },
                  UINT64_MAX);
              if (!status.ok())
                return status;
            }
            return ps::Status::success();
          },
          relation, {true, true, true, true}));
  } else {
    auto buffer = take(root.allocator().allocate(source.bytes.size()));
    std::memcpy(buffer.data(), source.bytes.data(), source.bytes.size());
    auto storage = std::move(buffer).freeze();
    for (const auto& region : source.coverage)
      require(builder.publish_tensor(0, region, source.layout, storage,
                                     relation, {true, true, true, true}));
  }
  return take(builder.seal());
}
inline ps::WorkflowInputDeclaration declaration(const Source& source) {
  ps::WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "source";
  input.result_schema =
      std::make_shared<const ps::SchemaTemplate>(source.schema);
  return input;
}
inline std::vector<std::uint8_t> read(const ps::ResultRef& result,
                                      const ps::Region& region) {
  auto window =
      take(result.acquire_tensor(take(result.descriptor()), 0, region));
  const auto width = ps::Value::element_size(
      result.schema().tensors[0].descriptor.element_type);
  std::vector<std::uint8_t> bytes(take(region.element_count()) * width);
  auto samples = take(ps::Footprint::from_regions(
      result.schema().tensors[0].sample_shape(), {region}));
  std::size_t next = 0;
  require(samples.visit(
      [&](const auto& at) {
        auto row = window.row_run(at);
        if (!row.ok())
          return row.status();
        std::memcpy(bytes.data() + next, row.value().data, width);
        next += width;
        return ps::Status::success();
      },
      UINT64_MAX));
  return bytes;
}
inline const void* owner(const ps::ResultRef& result,
                         const ps::Region& region) {
  auto window =
      take(result.acquire_tensor(take(result.descriptor()), 0, region));
  return window.storage_owner_token();
}
}  // namespace channel_fixture
