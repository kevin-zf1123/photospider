#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace channel_benchmark {
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
inline ps::ExecutionContextConfig config() {
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 2ULL * 1024 * 1024 * 1024;
  config.managed_resources = ps::ResourceLimits{};
  config.managed_resources->capacity[ps::ResourceKind::Host] =
      config.maximum_live_bytes;
  config.managed_resources->capacity[ps::ResourceKind::Metadata] =
      64 * 1024 * 1024;
  return config;
}
inline ps::ResultRef source(
    const ps::ResourceBudget& root, const ps::ValueDescriptor& descriptor,
    const ps::ResultTensorLayout& layout,
    const std::function<float(const std::vector<std::uint64_t>&)>& sample) {
  ps::SchemaTemplate schema;
  schema.id = "example.channel-benchmark";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = descriptor;
  tensor.layout = layout;
  schema.tensors.push_back(tensor);
  auto builder = take(ps::ResultBuilder::start(root, schema, "source"));
  require(builder.bind_descriptor_relation(take(ps::ResultRelation::cartesian(
      root, 1, {0, 8, 0, 0, ps::ResultSupportTarget::Descriptor, 0}))));
  const auto relation = take(ps::ResultRelation::cartesian(
      root, take(tensor.sample_count()),
      {0, 1, 0, 0, ps::ResultSupportTarget::Tensor, 0}));
  require(builder.publish_tensor_kernel(
      0, ps::Region::whole(descriptor.shape),
      [&](const auto& writers) {
        for (const auto& writer : writers) {
          const auto& dims = writer.region().dimensions();
          const auto axis = writer.sample_axis();
          std::vector<std::uint64_t> at;
          for (auto dim : dims)
            at.push_back(dim.offset);
          for (;;) {
            auto run = writer.row_run(at);
            if (!run.ok())
              return run.status();
            const auto n = std::min<std::uint64_t>(256, run.value().samples);
            auto charged = root.consume({n});
            if (!charged.ok())
              return charged;
            for (std::uint64_t i = 0; i < n; ++i) {
              const auto value = sample(at);
              std::memcpy(
                  run.value().data + static_cast<std::ptrdiff_t>(
                                         static_cast<__int128>(i) *
                                         run.value().sample_stride_bytes),
                  &value, sizeof(value));
              ++at[axis];
            }
            if (at[axis] < dims[axis].offset + dims[axis].extent)
              continue;
            at[axis] = dims[axis].offset;
            bool next = false;
            for (std::size_t i = dims.size(); i;) {
              --i;
              if (i == axis)
                continue;
              if (++at[i] < dims[i].offset + dims[i].extent) {
                next = true;
                break;
              }
              at[i] = dims[i].offset;
            }
            if (!next)
              break;
          }
        }
        return ps::Status::success();
      },
      relation, {true, true, true, true}));
  return take(builder.seal());
}
inline std::uint64_t logical_bytes(const ps::ExecutionResult& run,
                                   const ps::ExecutionBindings& bindings) {
  const auto support = take(run.dependencies.source_support());
  std::uint64_t bytes = 0;
  for (const auto& binding : bindings.inputs) {
    const auto found = support.find(binding.name);
    if (found != support.end())
      bytes += take(found->second.element_count()) * sizeof(float);
  }
  return bytes;
}
inline std::uint64_t extra(std::uint64_t live, std::uint64_t baseline) {
  return live > baseline ? live - baseline : 0;
}
}  // namespace channel_benchmark
