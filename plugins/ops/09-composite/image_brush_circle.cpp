#include "00-foundation/image_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_image_brush_circle(OperationRegistry* registry) {
  return image_ops::register_image_algorithm(registry, "image.brush_circle",
                                             image_ops::ImageAlgorithm::Brush,
                                             image_ops::ImageKind::Rgba);
}
}  // namespace ps::plugin_internal
