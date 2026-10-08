#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace example_result {
struct Failure : std::runtime_error {
  ps::Status status;
  explicit Failure(ps::Status value)
      : std::runtime_error(value.message), status(std::move(value)) {}
};
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw Failure(result.status());
  return result.take_value();
}
inline void check(ps::Status status) {
  if (!status.ok())
    throw Failure(std::move(status));
}
inline ps::SchemaTemplate schema(ps::ElementType type,
                                 std::vector<std::uint64_t> shape) {
  ps::SchemaTemplate result;
  result.id = "example.paged.tensor";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {type, std::move(shape)};
  result.tensors.push_back(std::move(tensor));
  return result;
}
inline ps::WorkflowInputDeclaration declaration(
    std::uint64_t id, std::string name, const ps::SchemaTemplate& schema) {
  ps::WorkflowInputDeclaration result;
  result.id = id;
  result.name = std::move(name);
  result.result_schema = std::make_shared<const ps::SchemaTemplate>(schema);
  return result;
}
inline ps::OperationOutputTraits output(const ps::SchemaTemplate& schema,
                                        std::uint64_t state_bytes,
                                        std::uint32_t stages) {
  ps::OperationOutputTraits result;
  result.key = "value";
  result.output_schema.kind = ps::OperationPortKind::Result;
  result.output_schema.result_schema_id = std::string(schema.id);
  result.output_schema.result_schema_version = schema.version;
  result.result_schema = schema;
  result.region_rule = ps::OperationRegionRule::Dependency;
  result.dependency_version = 2;
  result.continuation_bytes = state_bytes;
  result.maximum_dependency_stages = stages;
  return result;
}
template <class Fill>
ps::ExecutionBinding input(const ps::ResourceBudget& root, std::string name,
                           const ps::SchemaTemplate& schema, Fill fill,
                           bool reverse = false, bool broadcast = false) {
  check(schema.validate(true));
  if (schema.tensors.size() != 1 || !schema.fields.empty() ||
      (reverse && broadcast))
    throw Failure({ps::ErrorCode::InvalidArgument,
                   "example input requires one tensor and one layout"});
  const auto& tensor = schema.tensors.at(0);
  const auto count = take(tensor.sample_count());
  const auto width = ps::Value::element_size(tensor.descriptor.element_type);
  if (count > std::numeric_limits<std::uint64_t>::max() / width)
    throw Failure({ps::ErrorCode::ResourceExhausted,
                   "example input byte count overflow"});
  auto bytes =
      take(ps::BufferAllocator{}.allocate((broadcast ? 1 : count) * width));
  for (std::uint64_t i = 0; i < (broadcast ? 1 : count); ++i)
    fill(i, bytes.data() + (reverse ? count - i - 1 : i) * width);
  if (broadcast) {
    std::vector<std::uint8_t> sample(width);
    for (std::uint64_t i = 1; i < count; ++i) {
      fill(i, sample.data());
      if (!std::equal(sample.begin(), sample.end(), bytes.data()))
        throw Failure({ps::ErrorCode::InvalidArgument,
                       "broadcast input requires constant samples"});
    }
  }
  auto storage = take(root.reference(std::move(bytes).freeze()));
  ps::StridedLayout layout;
  layout.origin.resize(tensor.sample_shape().size());
  layout.byte_strides.resize(layout.origin.size());
  std::uint64_t stride = width;
  for (auto axis = layout.origin.size(); axis-- > 0;) {
    layout.byte_strides[axis] = broadcast ? 0
                                : reverse ? -static_cast<std::int64_t>(stride)
                                          : static_cast<std::int64_t>(stride);
    stride *= tensor.sample_shape()[axis];
  }
  if (reverse)
    layout.byte_offset = (count - 1) * width;
  auto builder = take(ps::ResultBuilder::start(root, schema, name));
  check(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {}))));
  check(builder.publish_tensor(
      0, ps::Region::whole(tensor.sample_shape()), std::move(layout),
      std::move(storage), take(ps::ResultRelation::cartesian(root, count, {})),
      {true, true, true, true}));
  ps::ExecutionBinding binding;
  binding.name = std::move(name);
  binding.result = take(builder.seal());
  return binding;
}
}  // namespace example_result
