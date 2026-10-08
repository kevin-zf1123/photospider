#include "runtime.hpp"  // NOLINT(build/include_subdir)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "gpu_runtime.hpp"  // NOLINT(build/include_subdir)
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif
namespace px {
namespace {
thread_local const ps_result_services_v2* argument_services = nullptr;
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
void admit_arguments(size_t count) {
  if (!argument_services)
    return;
  if (count > 64 || argument_services->consume_work(argument_services->context,
                                                    4096 + 2048 * count) != 0)
    throw Failure(4, "PixelOE argument construction budget exhausted");
}
Context::Context(const ps_result_services_v2* services)
    : services_(services),
      simd_(services && !services->gpu && simd_available()) {
  if (!services_ || services_->struct_size != sizeof(*services_) ||
      !services_->allocate_scratch || !services_->release_scratch ||
      !services_->cancelled)
    throw Failure(6, "PixelOE incompatible Result services");
  if (services_->gpu != nullptr &&
      (!services_->gpu || services_->cpu_parallel || !services_->consume_work ||
       services_->gpu->struct_size != sizeof(*services_->gpu) ||
       services_->gpu->abi_version != PS_GPU_ABI_VERSION_1 ||
       !services_->gpu->buffer || !services_->gpu->execute ||
       !services_->gpu->release))
    throw Failure(6, "PixelOE incompatible GPU services");
  if (services_->cpu_tiles &&
      (services_->gpu != nullptr || services_->cpu_parallel || services_->gpu ||
       !services_->consume_work ||
       services_->cpu_tiles->struct_size != sizeof(*services_->cpu_tiles) ||
       services_->cpu_tiles->abi_version != PS_CPU_TILES_ABI_VERSION_1 ||
       !services_->cpu_tiles->maximum_workers || !services_->cpu_tiles->run))
    throw Failure(6, "PixelOE incompatible CPU tile service");
  if (services_->consume_work) {
    previous_services_ = argument_services;
    argument_services = services_;
  }
}
Context::~Context() {
  if (services_->consume_work)
    argument_services = previous_services_;
}
void Context::check() const {
  if (services_->cancelled(services_->context)) {
    throw Failure(2, "PixelOE cancelled");
  }
}
void Context::charge(uint64_t units) const {
  check();
  if (services_->consume_work &&
      services_->consume_work(services_->context, units) != 0)
    throw Failure(4, "PixelOE work admission failed");
}
Array Context::empty(uint64_t count, uint32_t bytes) {
  check();
  if (!count || count > SIZE_MAX / bytes) {
    throw Failure(4, "PixelOE allocation overflow");
  }
  {
    if (count * bytes > (UINT64_MAX - 128) / 2)
      throw Failure(4, "PixelOE construction work overflow");
    charge(2 * count * bytes + 128);
  }
  uint8_t* p = nullptr;
  const int allocated =
      services_->allocate_scratch(services_->context, count * bytes, &p);
  if (allocated || !p) {
    throw Failure(4, "PixelOE scratch budget exhausted");
  }
  Array a;
  a.data = p;
  a.count = count;
  if (gpu_enabled()) {
    const int code = services_->gpu->buffer(services_->gpu->context, p,
                                            count * bytes, 1, &a.token);
    if (code) {
      services_->release_scratch(services_->context, p);
      throw Failure(code, "PixelOE native buffer admission failed");
    }
  }
  a.owner =
      std::shared_ptr<void>(p, [s = services_, token = a.token](void* data) {
        if (token)
          s->gpu->release(s->gpu->context, token);
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
void Context::parallel_for(
    uint64_t count, uint64_t grain,
    const std::function<void(uint64_t, uint64_t)>& block) {
  check();
  if (!grain)
    throw Failure(6, "PixelOE range grain must be positive");
  if (const auto* tiles = services_->cpu_tiles) {
    const ps_cpu_tile_stage_v1 stage{sizeof(stage),
                                     {count, 1, 1},
                                     {grain, 1, 1},
                                     0};
    const auto execute = [](void* raw, const ps_cpu_tile_v1* tile) noexcept {
      try {
        (*static_cast<const std::function<void(uint64_t, uint64_t)>*>(raw))(
            tile->begin[0], tile->end[0]);
        return 0;
      } catch (const Failure& failure) {
        return failure.code;
      } catch (const std::bad_alloc&) {
        return 4;
      } catch (...) {
        return 1;
      }
    };
    const int code = tiles->run(
        tiles->context, &stage, execute,
        const_cast<std::function<void(uint64_t, uint64_t)>*>(&block));
    if (code)
      throw Failure(code, "PixelOE CPU tile stage failed");
    check();
    return;
  }
  const auto* parallel = services_->cpu_parallel;
  if (!parallel) {
    for (uint64_t begin = 0; begin < count;) {
      check();
      const auto end = begin + std::min(grain, count - begin);
      block(begin, end);
      begin = end;
    }
    return;
  }
  if (parallel->struct_size != sizeof(*parallel) ||
      parallel->abi_version != PS_CPU_PARALLEL_ABI_VERSION_1 ||
      !parallel->run || !parallel->maximum_workers)
    throw Failure(6, "PixelOE incompatible CPU range service");
  struct Work {
    const std::function<void(uint64_t, uint64_t)>& block;
  } work{block};
  const auto execute = [](void* raw, uint64_t begin, uint64_t end,
                          uint32_t) noexcept -> int {
    try {
      static_cast<Work*>(raw)->block(begin, end);
      return 0;
    } catch (const Failure& failure) {
      return failure.code;
    } catch (const std::bad_alloc&) {
      return 4;
    } catch (...) {
      return 1;
    }
  };
  const int code =
      parallel->run(parallel->context, count, grain, 0, execute, &work);
  if (code)
    throw Failure(code, "PixelOE CPU range failed");
  check();
}
void Context::flat(const char* entry, uint32_t count, Arguments args) {
  constexpr uint32_t row = 65535 * 256;
  admit_arguments(args.items.size() + 1);
  args.items.emplace("row_len", Argument(row));
  dispatch(entry, {std::min(count, row), count / row + (count % row != 0), 1},
           std::move(args));
}
void Context::dispatch(const char* entry, std::array<uint32_t, 3> grid,
                       Arguments args) {
  check();
  if (gpu_enabled()) {
    const auto start = std::chrono::steady_clock::now();
    dispatch_gpu(services_, entry, grid, args);
    const auto ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    kernel_ms += ms;
    kernel_times[entry] += ms;
    ++dispatches;
    check();
    return;
  }
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
  // The shared source-work upper bound covers scalar and SIMD execution.
  // CPU uses its compiled kernels; this lookup performs no native dispatch.
  charge(16384);
  const auto bound = std::find_if(
      gpu_kernels().begin(), gpu_kernels().end(),
      [&](const auto& k) { return std::strcmp(entry, k.name) == 0; });
  if (bound == gpu_kernels().end())
    throw Failure(6, "PixelOE CPU stage lacks a work bound");
  charge(gpu_work_bound(*bound, grid, args));
  std::array<uint32_t, 3> groups;
  for (int i = 0; i < 3; ++i) {
    groups[i] = grid[i] / kernel->group[i] + (grid[i] % kernel->group[i] != 0);
  }
  const auto start = std::chrono::steady_clock::now();
  // Each block is a fixed group range. Global reductions retain their pinned
  // group topology, and all buffers stay alive until the host barrier returns.
  if (!simd_ || !dispatch_simd(*this, entry, grid, args)) {
    const uint64_t columns = groups[0] / 8 + (groups[0] % 8 != 0);
    if (groups[1] && columns > UINT64_MAX / groups[1])
      throw Failure(4, "PixelOE group range overflow");
    const uint64_t plane = columns * groups[1];
    if (groups[2] && plane > UINT64_MAX / groups[2])
      throw Failure(4, "PixelOE group range overflow");
    parallel_for(plane * groups[2], 1, [&](uint64_t begin, uint64_t end) {
      for (uint64_t index = begin; index < end; ++index) {
        check();
        const auto z = static_cast<uint32_t>(index / plane);
        const auto y = static_cast<uint32_t>((index % plane) / columns);
        const auto x = static_cast<uint32_t>((index % columns) * 8);
        Range range{{x, y, z}, {x + std::min(8U, groups[0] - x), y + 1, z + 1}};
        kernel->run(range, args);
      }
    });
  }
  const auto ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  kernel_ms += ms;
  kernel_times[entry] += ms;
  ++dispatches;
}
}  // namespace px
