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

/** @brief MASK-12 fixed-capacity area index, schema version 1.
 * Members are rows:Int64[capacity,2] (id,area) and count:Int64[1].
 * Both publish as one complete bundle. The effective prefix is sorted by
 * positive ID; unused rows are zero. Counts, padding and source Labels
 * ObjectId/spatial-basis pairing are unchecked caller premises.
 * This factory validates structure and allocates metadata only. Returned
 * metadata owns its directory. Independent calls are thread-safe; allocation
 * may throw std::bad_alloc.
 * @param capacity Positive source pixel count, at most 2^40.
 * @return Owned schema or the original structural validation failure.
 */
inline Result<SchemaTemplate> component_area_index_schema(
    std::uint64_t capacity) {
  if (!capacity || capacity > (UINT64_C(1) << 40))
    return Result<SchemaTemplate>(
        Status{ErrorCode::InvalidArgument, "component capacity limit"});
  auto rows =
      detail::color_member("rows", ElementType::Int64, {capacity, 2}, 1);
  SchemaTemplate result;
  result.id = "photospider.ops.mask.component_areas";
  result.tensors = {std::move(rows),
                    detail::color_member("count", ElementType::Int64, {1})};
  result.publication_scopes.push_back(
      {ResultPublicationPolicy::CompleteBundle, {0, 1}});
  const auto status = result.validate();
  if (!status.ok())
    return Result<SchemaTemplate>(status);
  return Result<SchemaTemplate>(std::move(result));
}
/** @brief MASK-12 fixed-capacity bounding-box index, schema version 1.
 * Members are rows:Int64[capacity,5] and count:Int64[1], with columns
 * id,y_min,x_min,y_max_exclusive,x_max_exclusive. Rows are atomic records.
 * Ownership, association premises, exceptions and concurrency follow
 * component_area_index_schema.
 * @param capacity Positive source pixel count, at most 2^40.
 * @return Owned schema or the original structural validation failure.
 */
inline Result<SchemaTemplate> component_bbox_index_schema(
    std::uint64_t capacity) {
  auto value = component_area_index_schema(capacity);
  if (!value.ok())
    return value;
  auto result = value.take_value();
  result.id = "photospider.ops.mask.component_bboxes";
  result.tensors[0].descriptor.shape[1] = 5;
  const auto status = result.validate();
  if (!status.ok())
    return Result<SchemaTemplate>(status);
  return Result<SchemaTemplate>(std::move(result));
}
/** @brief MASK-12 MinPixel labels and complete attributes, schema version 1.
 * Members are labels:Int64[height,width], rows:Int64[height*width,7] and
 * count:Int64[1]. Columns are id,area,min_pixel,y_min,x_min,y_max_exclusive,
 * x_max_exclusive. All members publish together, with a sorted effective
 * prefix and zero padding. The containing Result owns the common identity.
 * Ownership, exceptions and concurrency follow component_area_index_schema.
 * @param height Positive canvas height.
 * @param width Positive canvas width; height*width must be at most 2^40.
 * @return Owned schema or the original structural validation failure.
 */
inline Result<SchemaTemplate> component_bundle_schema(std::uint64_t height,
                                                      std::uint64_t width) {
  if (!height || !width || height > (UINT64_C(1) << 40) / width)
    return Result<SchemaTemplate>(
        Status{ErrorCode::InvalidArgument, "component canvas limit"});
  SchemaTemplate result;
  result.id = "photospider.ops.mask.components";
  auto labels =
      detail::color_member("labels", ElementType::Int64, {height, width});
  labels.layout.spatial = true;
  labels.layout.height_axis = 0;
  labels.layout.width_axis = 1;
  result.tensors = {
      std::move(labels),
      detail::color_member("rows", ElementType::Int64, {height * width, 7}, 1),
      detail::color_member("count", ElementType::Int64, {1})};
  result.publication_scopes.push_back(
      {ResultPublicationPolicy::CompleteBundle, {0, 1, 2}});
  const auto status = result.validate();
  if (!status.ok())
    return Result<SchemaTemplate>(status);
  return Result<SchemaTemplate>(std::move(result));
}
}  // namespace ps::mask
