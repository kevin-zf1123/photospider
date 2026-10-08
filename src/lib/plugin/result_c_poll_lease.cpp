#include "plugin/result_c_poll_lease.hpp"

namespace ps::plugin_internal::result_c {
void CResultPollLease::retire() noexcept {
  active = false;
  native_views.clear();
  atlases.clear();
  checkpoints.clear();
}
}  // namespace ps::plugin_internal::result_c
