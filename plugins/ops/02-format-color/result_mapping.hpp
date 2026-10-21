#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/tensor_program.hpp"
#include "photospider/core/content_digest.hpp"
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
inline Region mapped_region(const Region& output,
                            const std::vector<ResultViewAxis>& axes) {
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
}  // namespace ps::plugin_internal::format_result
