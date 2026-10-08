#include "00-foundation/image_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_mask_downsample_box(OperationRegistry* registry) {
  return image_ops::register_image_algorithm(
      registry, "mask.downsample_box", image_ops::ImageAlgorithm::Downsample,
      image_ops::ImageKind::Coverage);
}
}  // namespace ps::plugin_internal
