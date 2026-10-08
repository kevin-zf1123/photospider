#include "00-foundation/image_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_image_split_horizontal(OperationRegistry* registry) {
  return image_ops::register_image_algorithm(registry, "image.split_horizontal",
                                             image_ops::ImageAlgorithm::Split,
                                             image_ops::ImageKind::Tensor);
}
}  // namespace ps::plugin_internal
