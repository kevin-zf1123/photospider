#include "00-foundation/image_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_image_opacity(OperationRegistry* registry) {
  return image_ops::register_image_algorithm(registry, "image.opacity",
                                             image_ops::ImageAlgorithm::Opacity,
                                             image_ops::ImageKind::Rgba);
}
}  // namespace ps::plugin_internal
