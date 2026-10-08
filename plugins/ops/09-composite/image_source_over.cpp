#include "00-foundation/image_program.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
Status register_image_source_over(OperationRegistry* registry) {
  return image_ops::register_image_algorithm(
      registry, "image.source_over", image_ops::ImageAlgorithm::SourceOver,
      image_ops::ImageKind::Rgba);
}
}  // namespace ps::plugin_internal
