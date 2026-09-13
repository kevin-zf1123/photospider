#pragma once

#include "photospider/execution/cancellation.hpp"

namespace ps::plugin_internal::inpaint_ns {
struct Stopped {};
struct NumericFailure {};
inline void check_stop(const CancellationToken& token) {
  if (token.cancelled())
    throw Stopped{};
}
// Caller supplies N Float32 plane, N UInt8 mask, P UInt8 state, P Float32
// arrival times and 16*N heap bytes. Buffers are invocation-local host owners.
void native(float* pixels, const unsigned char* mask, int h, int w, int radius,
            unsigned char* state, float* times, void* entries,
            const CancellationToken& token);
#ifdef PHOTOSPIDER_HAS_INPAINT_OPENCV
void opencv(float* source, float* target, unsigned char* mask, int h, int w,
            int radius);
#endif
}  // namespace ps::plugin_internal::inpaint_ns
