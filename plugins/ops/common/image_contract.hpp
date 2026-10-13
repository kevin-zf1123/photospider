#pragma once

#include <memory>
#include <optional>
#include <utility>

#include "common/result_support.hpp"
#include "photospider/data/semantic.hpp"

namespace ps::plugin_internal::image_ops {
inline SchemaTemplate image_schema() {
  SchemaTemplate schema;
  schema.id = "photospider.image";
  ResultTensorSpec image;
  image.batch_axes = {1, 1};
  image.layout.spatial = true;
  image.key = "pixels";
  image.descriptor = {ElementType::Float32, {1, 1, 4}};
  image.facets = {numeric_ops::math_take(encode_semantic(rgba_semantics()))};
  schema.tensors.push_back(std::move(image));
  return schema;
}
inline Status check_image(const OperationMetadata& metadata) {
  if (!metadata.result_schema ||
      metadata.result_schema->id != "photospider.image" ||
      metadata.result_schema->version != 1 ||
      metadata.result_schema->tensors.size() != 1 ||
      !metadata.result_schema->fields.empty()) {
    return {ErrorCode::TypeMismatch,
            "operation requires photospider.image schema with one pixels slot"};
  }
  const auto& image = metadata.result_schema->tensors[0];
  if (image.key != "pixels" || image.batch_axes.size() != 2 ||
      !image.layout.spatial ||
      image.descriptor.element_type != ElementType::Float32 ||
      image.descriptor.shape.size() != 3 || image.layout.height_axis != 0 ||
      image.layout.width_axis != 1 ||
      image.layout.channel_axis != std::optional<std::uint32_t>{2} ||
      image.descriptor.shape[2] != 4) {
    return {
        ErrorCode::TypeMismatch,
        "image operation requires frame/layer batches and Float32 HWC RGBA"};
  }
  const auto expected =
      numeric_ops::math_take(encode_semantic(rgba_semantics()));
  if (image.facets.size() != 1 || image.facets[0].key != expected.key ||
      image.facets[0].version != expected.version ||
      image.facets[0].payload != expected.payload) {
    return {ErrorCode::TypeMismatch,
            "image operation requires canonical linear premultiplied RGBA "
            "semantics"};
  }
  return Status::success();
}
}  // namespace ps::plugin_internal::image_ops
