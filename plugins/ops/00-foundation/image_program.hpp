#pragma once

#include <functional>
#include <string>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::plugin_internal::image_ops {
enum class ImageKind { Rgba, Coverage, Tensor };
enum class ImageAlgorithm {
  Exposure,
  Opacity,
  Mask,
  SourceOver,
  Downsample,
  Brush,
  Mix,
  Split,
  Gaussian
};

// All repository image operations exchange one typed image slot. The actual
// tensor description, frame/layer counts and physical axes are inferred from
// the input schema, independently of this stable schema identity.
SchemaTemplate image_schema(ImageKind kind = ImageKind::Rgba);
Status check_image(const OperationMetadata& metadata, ImageKind kind);
Status register_image_algorithm(OperationRegistry* registry,
                                const std::string& key,
                                ImageAlgorithm algorithm,
                                ImageKind kind = ImageKind::Rgba);
}  // namespace ps::plugin_internal::image_ops
