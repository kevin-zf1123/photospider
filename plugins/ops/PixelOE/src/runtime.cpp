#include "runtime.hpp"  // NOLINT(build/include_subdir)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif
namespace px {
namespace {
bool simd_available() {
  const char* mode = std::getenv("PIXELOE_CPU_SIMD");
  if (mode && std::strcmp(mode, "0") == 0) {
    return false;
  }
#if defined(PIXELOE_NEON)
  return true;
#elif defined(PIXELOE_AVX2) && !defined(_MSC_VER)
  return __builtin_cpu_supports("avx2");
#elif defined(PIXELOE_AVX2) && defined(_MSC_VER)
  int info[4];
  __cpuid(info, 0);
  if (info[0] < 7)
    return false;
  __cpuidex(info, 1, 0);
  if ((info[2] & (1 << 27)) == 0 || (info[2] & (1 << 28)) == 0 ||
      (_xgetbv(0) & 6) != 6)
    return false;
  __cpuidex(info, 7, 0);
  return (info[1] & (1 << 5)) != 0;
#else
  return false;
#endif
}
}  // namespace
Context::Context(const ps_planar_services_v1* services)
    : services_(services), simd_(simd_available()) {}
void Context::check() const {
  if (services_->cancelled(services_->context)) {
    throw Failure(2, "PixelOE cancelled");
  }
}
Array Context::empty(uint64_t count, uint32_t bytes) {
  check();
  if (!count || count > SIZE_MAX / bytes) {
    throw Failure(4, "PixelOE allocation overflow");
  }
  auto* p = services_->allocate_scratch(services_->context, count * bytes);
  if (!p) {
    throw Failure(4, "PixelOE scratch budget exhausted");
  }
  Array a;
  a.data = p;
  a.count = count;
  a.owner = std::shared_ptr<void>(p, [s = services_](void* data) {
    s->release_scratch(s->context, static_cast<uint8_t*>(data));
  });
  return a;
}
Array Context::image(uint32_t h, uint32_t w, uint32_t c) {
  auto a = empty(static_cast<uint64_t>(h) * w * c);
  a.height = h;
  a.width = w;
  a.channels = c;
  return a;
}
Array Context::constant(const float* p, size_t count) {
  auto a = empty(count);
  std::memcpy(a.data, p, count * 4);
  return a;
}
void Context::flat(const char* entry, uint32_t count, Arguments args) {
  constexpr uint32_t row = 65535 * 256;
  args.items.emplace("row_len", Argument(row));
  dispatch(entry, {std::min(count, row), (count + row - 1) / row, 1},
           std::move(args));
}
void Context::dispatch(const char* entry, std::array<uint32_t, 3> grid,
                       Arguments args) {
  check();
  const Kernel* kernel = nullptr;
  for (const auto& k : kernels()) {
    if (std::strcmp(entry, k.name) == 0) {
      kernel = &k;
      break;
    }
  }
  if (!kernel) {
    throw Failure(1, std::string("missing kernel ") + entry);
  }
  std::array<uint32_t, 3> groups;
  for (int i = 0; i < 3; ++i) {
    groups[i] = (grid[i] + kernel->group[i] - 1) / kernel->group[i];
  }
  const auto start = std::chrono::steady_clock::now();
  // All work stays on the host-assigned callback thread. SIMD processes
  // independent pixels; it neither creates workers nor changes FP reductions.
  if (!simd_ || !dispatch_simd(*this, entry, grid, args)) {
    for (uint32_t z = 0; z < groups[2]; ++z) {
      for (uint32_t y = 0; y < groups[1]; ++y) {
        for (uint32_t x = 0; x < groups[0]; x += 8) {
          check();
          Range range{{x, y, z}, {std::min(x + 8, groups[0]), y + 1, z + 1}};
          kernel->run(range, args);
        }
      }
    }
  }
  const auto ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  kernel_ms += ms;
  kernel_times[entry] += ms;
  ++dispatches;
}
}  // namespace px
