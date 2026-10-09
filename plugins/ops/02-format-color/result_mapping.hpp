#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/tensor_program.hpp"
#include "data/content_digest.hpp"
#include "data/result_window_access.hpp"
#include "photospider/ops/format/channel.hpp"

namespace ps::plugin_internal::format_result {
using tensor_ops::require;
using tensor_ops::take;
using Params = std::map<std::string, ParameterValue>;
inline std::string text(const Params& parameters, const char* key,
                        const std::string& fallback = {}) {
  const auto found = parameters.find(key);
  return found == parameters.end() ? fallback
                                   : std::get<std::string>(found->second);
}
inline std::string source_assertion(
    const std::vector<OperationMetadata>& inputs) {
  std::string result = "result-v1";
  for (const auto& input : inputs) {
    require(tensor_ops::check_tensor(input));
    const auto schema = format::detail::schema_assertion(*input.result_schema);
    const auto layout = format::detail::layout_assertion(
        input.result_schema->tensors[0].layout);
    result += ":" + std::to_string(schema.size()) + ":";
    result.append(schema.data(), schema.size());
    result += ":" + std::to_string(layout.size()) + ":" + layout;
  }
  return result;
}
inline std::string assembly_source_assertion(
    const std::vector<OperationMetadata>& inputs) {
  content_internal::Sha256 digest;
  digest.text("photospider.fmt.assembly-inputs.v1");
  digest.text(std::to_string(inputs.size()));
  for (const auto& input : inputs) {
    require(tensor_ops::check_tensor(input));
    digest.text(input.result_schema->canonical());
    digest.text(format::detail::layout_assertion(
        input.result_schema->tensors[0].layout));
  }
  return "result-inputs-v1:" + digest.finish();
}
inline std::vector<ResultMappedAxis> extraction_axes(std::size_t source_rank,
                                                     std::uint32_t axis,
                                                     std::uint64_t index,
                                                     bool keepdims) {
  std::vector<ResultMappedAxis> result(source_rank);
  for (std::size_t i = 0; i < source_rank; ++i) {
    if (i == axis)
      result[i].source_origin = index;
    else
      result[i].output_axis =
          static_cast<std::int32_t>(i < axis || keepdims ? i : i - 1);
  }
  return result;
}
inline Region mapped_region(const Region& output,
                            const std::vector<ResultMappedAxis>& axes) {
  std::vector<RegionDimension> source;
  for (const auto& axis : axes) {
    const auto dimension = axis.output_axis < 0
                               ? RegionDimension{0, 1}
                               : output.dimensions()[axis.output_axis];
    const auto first = take(axis.source_coordinate(dimension.offset));
    const auto last =
        take(axis.source_coordinate(dimension.offset + dimension.extent - 1));
    source.push_back({std::min(first, last),
                      std::max(first, last) - std::min(first, last) + 1});
  }
  return Region(std::move(source));
}
inline bool view_unavailable(const Status& status) {
  return status.code == ErrorCode::InvalidArgument &&
         status.message.find("ViewUnavailable") != std::string::npos;
}
struct PublicationCounts {
  std::uint64_t copied = 0, viewed = 0;
};
inline Status publish(const ResultProgramPhase& phase, ResultBuilder* builder,
                      const Region& box, const ResultTensorReadWindow& window,
                      const std::vector<ResultMappedAxis>& axes,
                      ResultRelation relation, const std::string& policy,
                      std::uint32_t port = 0,
                      PublicationCounts* counts = nullptr) {
  const auto& tensor = phase.query.output.result_schema->tensors[0];
  Status status{ErrorCode::InvalidArgument, "ViewUnavailable"};
  if (policy != "materialize") {
    ResultTensorViewTransform transform;
    transform.source_axes = axes;
    status = builder->publish_tensor_view(0, box, window, transform, relation,
                                          {true, true, true, true},
                                          phase.query.cancellation);
    if (tensor.layout.spatial && view_unavailable(status))
      status = builder->publish_tensor_view(0, box, {&window}, relation,
                                            {true, true, true, true},
                                            phase.query.cancellation);
  }
  if (policy != "materialize" && view_unavailable(status)) {
    const auto partitioned =
        execution_internal::ResultWindowAccess::visit_backing_regions(
            window, phase.resources, [&](const Region& source_region) {
              auto work =
                  phase.consume_work(source_region.rank() + box.rank() + 1);
              if (!work.ok())
                return work;
              const auto outputs =
                  take(Footprint::from_regions(tensor.sample_shape(), {box}));
              const auto source = take(Footprint::from_regions(
                  window.spec().sample_shape(), {source_region}));
              const auto parts = take(relation.preimage(
                  outputs, {port, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                  source));
              for (const auto& part : parts.boxes()) {
                auto input = phase.tensors->at({port, 0}).acquire(
                    mapped_region(part, axes), phase.query.cancellation);
                if (!input.ok())
                  return input.status();
                auto published = publish(phase, builder, part, input.value(),
                                         axes, relation, policy, port, counts);
                if (!published.ok())
                  return published;
              }
              return Status::success();
            });
    if (!partitioned.ok())
      return partitioned.status();
    if (partitioned.value())
      return Status::success();
  }
  if (policy == "view" && view_unavailable(status))
    return {ErrorCode::InvalidArgument,
            status.message,
            FailureReason::InvalidDomain,
            {FailureOrigin::Schema, FailureScope::Unspecified}};
  if (!view_unavailable(status)) {
    if (status.ok() && counts)
      counts->viewed += take(box.element_count());
    return status;
  }
  const auto width = Value::element_size(tensor.descriptor.element_type);
  auto work = execution_internal::ResultWindowAccess::read_work(window);
  if (!work.ok())
    return work.status();
  auto copied = builder->publish_tensor_kernel(
      0, box,
      [&](const auto& writers) {
        for (const auto& writer : writers) {
          const auto& dims = writer.region().dimensions();
          const auto sample_axis = writer.sample_axis();
          const auto& source_axis = axes[window.sample_axis()];
          const bool row_mapping = source_axis.output_axis ==
                                       static_cast<std::int32_t>(sample_axis) &&
                                   source_axis.step == 1;
          const bool row_repeat =
              std::none_of(axes.begin(), axes.end(), [&](const auto& axis) {
                return axis.output_axis ==
                           static_cast<std::int32_t>(sample_axis) &&
                       axis.step != 0;
              });
          std::vector<std::uint64_t> at, source(axes.size());
          for (auto d : dims)
            at.push_back(d.offset);
          for (;;) {
            const auto count =
                (row_mapping || row_repeat)
                    ? std::min<std::uint64_t>(
                          256, dims[sample_axis].offset +
                                   dims[sample_axis].extent - at[sample_axis])
                    : 1;
            const auto units = static_cast<unsigned __int128>(count) *
                               (static_cast<unsigned __int128>(work.value()) +
                                width + dims.size() + axes.size());
            if (units > UINT64_MAX)
              return Status{ErrorCode::ResourceExhausted,
                            "format copy work overflow"};
            auto status = phase.consume_work(static_cast<std::uint64_t>(units));
            if (!status.ok())
              return status;
            if (phase.query.cancellation.cancelled())
              return Status{ErrorCode::Cancelled, "format copy cancelled"};
            for (std::size_t i = 0; i < axes.size(); ++i) {
              const auto& axis = axes[i];
              auto coordinate = axis.source_coordinate(
                  axis.output_axis < 0 ? 0 : at[axis.output_axis]);
              if (!coordinate.ok())
                return coordinate.status();
              source[i] = coordinate.value();
            }
            auto read = window.row_run(source);
            if (!read.ok())
              return read.status();
            auto write = writer.row_run(at);
            if (!write.ok())
              return write.status();
            const auto n =
                std::min({count, row_repeat ? count : read.value().samples,
                          write.value().samples});
            if (row_repeat && write.value().sample_stride_bytes ==
                                  static_cast<std::int64_t>(width)) {
              std::memcpy(write.value().data, read.value().data, width);
              for (std::uint64_t ready = 1; ready < n;) {
                const auto copied = std::min(ready, n - ready);
                std::memcpy(write.value().data + ready * width,
                            write.value().data, copied * width);
                ready += copied;
              }
            } else if (!row_repeat &&
                       read.value().sample_stride_bytes ==
                           static_cast<std::int64_t>(width) &&
                       write.value().sample_stride_bytes ==
                           static_cast<std::int64_t>(width)) {
              std::memcpy(write.value().data, read.value().data, n * width);
            } else {
              for (std::uint64_t i = 0; i < n; ++i)
                std::memcpy(
                    write.value().data + static_cast<std::ptrdiff_t>(
                                             static_cast<__int128>(i) *
                                             write.value().sample_stride_bytes),
                    read.value().data +
                        static_cast<std::ptrdiff_t>(
                            static_cast<__int128>(i) *
                            (row_repeat ? 0
                                        : read.value().sample_stride_bytes)),
                    width);
            }
            at[sample_axis] += n;
            if (at[sample_axis] <
                dims[sample_axis].offset + dims[sample_axis].extent)
              continue;
            at[sample_axis] = dims[sample_axis].offset;
            bool advanced = false;
            for (std::size_t axis = dims.size(); axis;) {
              --axis;
              if (axis == sample_axis)
                continue;
              if (++at[axis] < dims[axis].offset + dims[axis].extent) {
                advanced = true;
                break;
              }
              at[axis] = dims[axis].offset;
            }
            if (!advanced)
              break;
          }
        }
        return Status::success();
      },
      relation, {true, true, true, true}, phase.query.cancellation);
  if (copied.ok() && counts)
    counts->copied += take(box.element_count());
  return copied;
}
template <class Function>
Status planes(const ResultTensorSpec& tensor, const Footprint& footprint,
              Function&& visit) {
  for (const auto& requested : footprint.boxes()) {
    auto dims = requested.dimensions();
    const auto batches = tensor.layout.spatial ? tensor.batch_axes.size() : 0;
    for (std::size_t i = 0; i < batches; ++i)
      dims[i].extent = 1;
    for (;;) {
      auto status = visit(Region(dims));
      if (!status.ok())
        return status;
      bool next = false;
      for (std::size_t i = batches; i;) {
        --i;
        const auto original = requested.dimensions()[i];
        if (++dims[i].offset < original.offset + original.extent) {
          next = true;
          break;
        }
        dims[i].offset = original.offset;
      }
      if (!next)
        break;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal::format_result
