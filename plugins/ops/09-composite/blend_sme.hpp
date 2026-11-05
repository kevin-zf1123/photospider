// Copyright 2026 Photospider contributors
#pragma once

#include <cstddef>

namespace ps::plugin_internal::blend {
enum class Kind;
unsigned sme_vector_bytes();
void sme_copy(const void* source, void* output, std::size_t bytes);
void sme_binary(Kind kind, const float* background, const float* source,
                float* output, std::size_t count);
#if defined(PHOTOSPIDER_HAS_BLEND_SME_F64)
void sme_difference(const double* b, const double* s, double* y,
                    std::size_t count);
#endif
}  // namespace ps::plugin_internal::blend
