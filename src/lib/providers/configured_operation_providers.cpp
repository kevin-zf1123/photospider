#include "providers/configured_operation_providers.hpp"

#include "core/ops.hpp"
#if defined(PHOTOSPIDER_HAS_OPENCV_OPERATION_PROVIDER)
#include "providers/opencv/opencv_operation_provider.hpp"
#endif

namespace ps::providers {

/** @copydoc register_configured_operation_providers */
void register_configured_operation_providers() {
  ops::register_core_operations();
#if defined(PHOTOSPIDER_HAS_OPENCV_OPERATION_PROVIDER)
  opencv::register_provider();
#endif
}

}  // namespace ps::providers
