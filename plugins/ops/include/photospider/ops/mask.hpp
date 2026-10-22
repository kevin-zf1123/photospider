#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "photospider/data/result.hpp"

namespace ps::mask {
namespace detail {
inline ResultTensorSpec color_member(const char* name, ElementType type,
                                     std::vector<std::uint64_t> shape,
                                     unsigned atomic = 0) {
  ResultTensorSpec result;
  result.key = name;
  result.descriptor = {type, std::move(shape)};
  result.atomic_trailing_axes = atomic;
  result.layout.channel_axis = {};
  return result;
}
inline Result<SchemaTemplate> color_bundle(
    const char* id, std::vector<ResultTensorSpec> fields) {
  SchemaTemplate result;
  result.id = id;
  ResultPublicationScope scope;
  scope.policy = ResultPublicationPolicy::CompleteBundle;
  for (unsigned j = 0; j < fields.size(); ++j) {
    std::uint64_t count = 1;
    for (auto n : fields[j].sample_shape()) {
      if (!n || n > (UINT64_C(1) << 40) / count)
        return Result<SchemaTemplate>(
            Status{ErrorCode::InvalidArgument, "mask model capacity limit"});
      count *= n;
    }
    scope.members.push_back(j);
    result.tensors.push_back(std::move(fields[j]));
  }
  result.publication_scopes.push_back(std::move(scope));
  const auto status = result.validate();
  if (!status.ok())
    return Result<SchemaTemplate>(status);
  return Result<SchemaTemplate>(std::move(result));
}
}  // namespace detail

/** @brief MASK-04 grouped-color model v1, complete fixed-capacity publication.
 * Directory: count:Int64[1], ids:Int64[capacity], mean:Float64[capacity,C],
 * cholesky:Float64[capacity,C,C]. Count and canonical padding are payload
 * premises. A logical empty model has count=0 and positive backing capacity.
 * Coordinates, interpretation and source pairing are caller-owned
 * configuration. This factory validates structure only and allocates no tensor
 * backing. Returned metadata owns its directory and is independent of the
 * arguments. Allocation may throw std::bad_alloc; independent calls are
 * thread-safe.
 * @param capacity Positive fixed row capacity.
 * @param components Positive coordinate count per row.
 * @return Owned schema, or InvalidArgument for a zero dimension or a field
 * exceeding 2^40 samples. Other schema validation failures are preserved.
 */
inline Result<SchemaTemplate> color_group_model_schema(
    std::uint64_t capacity, std::uint64_t components) {
  using detail::color_member;
  return detail::color_bundle(
      "photospider.ops.mask.color_groups",
      {color_member("count", ElementType::Int64, {1}),
       color_member("ids", ElementType::Int64, {capacity}),
       color_member("mean", ElementType::Float64, {capacity, components}, 1),
       color_member("cholesky", ElementType::Float64,
                    {capacity, components, components}, 2)});
}
/** @brief MASK-04 zero-total-weight group diagnostics, sorted ID prefix.
 * Directory: count:Int64[1], ids:Int64[capacity]. Both members publish
 * together. The sole diagnostic reason is zero_total_weight, encoded by this
 * Result kind. Capacity is positive even for an empty prefix. Ownership,
 * exceptions and concurrency follow color_group_model_schema.
 * @param capacity Positive fixed row capacity, at most 2^40.
 * @return Owned schema, or InvalidArgument for an invalid capacity.
 */
inline Result<SchemaTemplate> skipped_color_groups_schema(
    std::uint64_t capacity) {
  using detail::color_member;
  return detail::color_bundle(
      "photospider.ops.mask.skipped_color_groups",
      {color_member("count", ElementType::Int64, {1}),
       color_member("ids", ElementType::Int64, {capacity})});
}
}  // namespace ps::mask
