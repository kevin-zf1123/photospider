// Copyright 2026 Photospider contributors
#pragma once

#include <cstdint>

#include "photospider/plugin/fixed_invocation_api.h"

namespace ps::plugin_internal::frequency {
// These declarations use the ordinary, non-streaming ABI. The definitions
// change mode internally, and no SVE or ZA value crosses this interface.
std::uint64_t sme_stationary_vector_bytes();

// The caller has confirmed a 64-byte streaming vector, validated levels in
// 1..5 and admitted 16*levels*count work,
// including the initial cancellation query. False means cancellation only.
// Output is disjoint from original detail bands. Exact out == LL is allowed
// when all detail inputs are disjoint from that allocation.
bool sme_stationary_inverse_f32(const float* const* bands, float* out,
                                std::uint64_t count, unsigned levels,
                                const ps_kernel_cancellation_v8& cancellation);
bool sme_stationary_inverse_f64(const double* const* bands, double* out,
                                std::uint64_t count, unsigned levels,
                                const ps_kernel_cancellation_v8& cancellation);
}  // namespace ps::plugin_internal::frequency
