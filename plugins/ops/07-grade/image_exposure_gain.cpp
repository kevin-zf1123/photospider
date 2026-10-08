#include "00-foundation/image_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_image_exposure_gain(OperationRegistry* registry) {
  return image_ops::register_image_algorithm(
      registry, "image.exposure_gain", image_ops::ImageAlgorithm::Exposure,
      image_ops::ImageKind::Rgba);
}
}  // namespace ps::plugin_internal
