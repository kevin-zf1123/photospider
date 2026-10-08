#include "00-foundation/image_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_image_downsample_box(OperationRegistry* registry) {
  return image_ops::register_image_algorithm(
      registry, "image.downsample_box", image_ops::ImageAlgorithm::Downsample,
      image_ops::ImageKind::Rgba);
}
}  // namespace ps::plugin_internal
