#include "00-foundation/image_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_image_mask(OperationRegistry* registry) {
  return image_ops::register_image_algorithm(registry, "image.mask",
                                             image_ops::ImageAlgorithm::Mask,
                                             image_ops::ImageKind::Rgba);
}
}  // namespace ps::plugin_internal
