// Differential checks against the actual AOT Slang entrypoints.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <thread>

#include "runtime.hpp"  // NOLINT(build/include_subdir)

namespace {
void mode(bool enabled) {
#if defined(_WIN32)
  _putenv_s("PIXELOE_CPU_SIMD", enabled ? "1" : "0");
#else
  setenv("PIXELOE_CPU_SIMD", enabled ? "1" : "0", 1);
#endif
}
struct Memory {
  std::map<uint8_t*, uint64_t> blocks;
  std::thread::id thread = std::this_thread::get_id();
  uint32_t polls = 0, cancel_after = UINT32_MAX;
};
void equal(const px::Array& a, const px::Array& b, const std::string& name) {
  if (a.count != b.count || std::memcmp(a.data, b.data, a.count * 4)) {
    const auto* x = static_cast<const uint32_t*>(a.data);
    const auto* y = static_cast<const uint32_t*>(b.data);
    for (size_t i = 0; i < a.count; ++i) {
      if (x[i] != y[i]) {
        throw std::runtime_error(name + " at " + std::to_string(i) + ": " +
                                 std::to_string(x[i]) +
                                 " != " + std::to_string(y[i]));
      }
    }
  }
}
}  // namespace
int main() try {
  px::Environment environment;
  Memory memory;
  ps_planar_services_v1 services{};
  services.struct_size = sizeof(services);
  services.context = &memory;
  services.cancelled = [](void* raw) {
    auto& m = *static_cast<Memory*>(raw);
    if (m.thread != std::this_thread::get_id())
      std::abort();
    return ++m.polls >= m.cancel_after ? 1 : 0;
  };
  services.allocate_scratch = [](void* raw, uint64_t size) -> uint8_t* {
    auto& m = *static_cast<Memory*>(raw);
    if (m.thread != std::this_thread::get_id())
      std::abort();
    auto* p = static_cast<uint8_t*>(std::calloc(size, 1));
    if (p)
      m.blocks[p] = size;
    return p;
  };
  services.release_scratch = [](void* raw, uint8_t* p) {
    auto& m = *static_cast<Memory*>(raw);
    if (m.thread != std::this_thread::get_id() || m.blocks.erase(p) != 1)
      std::abort();
    std::free(p);
    return 1;
  };
  mode(false);
  px::Context scalar(&services);
  mode(true);
  px::Context simd(&services);
  if (!simd.simd_enabled()) {
    std::cout << "SKIP: no supported SIMD ISA on this host\n";
    return 77;
  }
  std::mt19937 rng(48319);
  auto fill = [&](px::Array& a, bool signed_values) {
    auto* p = static_cast<float*>(a.data);
    for (size_t i = 0; i < a.count; ++i) {
      p[i] = static_cast<float>(rng() % 1024) / 1024;
      if (signed_values && i % 2)
        p[i] = -p[i];
      if (i % 17 == 0)
        p[i] = -0.0f;
      if (i % 23 == 0)
        p[i] = std::numeric_limits<float>::denorm_min();
    }
  };
  size_t cases = 0;
  for (uint32_t w : {1, 2, 3, 7, 8, 15, 16, 31, 32, 33, 65, 128}) {
    for (uint32_t h : {1, 2, 7, 33, 66}) {
      auto src = scalar.image(h, w), a = scalar.image(h, w),
           b = scalar.image(h, w), extra = scalar.image(h, w);
      fill(src, true);
      fill(extra, true);
      for (int r : {2, 4, 8, 16, 32}) {
        auto taps = scalar.empty(2 * r + 4);
        fill(taps, true);
        for (bool rows : {true, false}) {
          for (uint32_t mode_value : {0, 1, 2}) {
            if (rows && mode_value != 0)
              continue;
            std::memcpy(a.data, extra.data, a.count * 4);
            std::memcpy(b.data, extra.data, b.count * 4);
            const std::string entry =
                std::string(rows ? "lr_rows_r" : "lr_cols_r") +
                std::to_string(r);
            auto args = [&](const px::Array& out) {
              return px::Arguments{
                  {"src", src},
                  {"tmp", rows ? out : src},
                  {"dst", out},
                  {"add_src", extra},
                  {"tables", taps},
                  {"offset", 3},
                  {"height", h},
                  {"width", w},
                  {"reflect",
                   h > static_cast<uint32_t>(r) && w > static_cast<uint32_t>(r)
                       ? 1
                       : 0},
                  {"mode", mode_value}};
            };
            std::array<uint32_t, 3> grid =
                rows ? std::array<uint32_t, 3>{(w + 7) / 8, h, 3}
                     : std::array<uint32_t, 3>{w, (h + 7) / 8, 3};
            scalar.dispatch(entry.c_str(), grid, args(a));
            simd.dispatch(entry.c_str(), grid, args(b));
            equal(a, b,
                  entry + " " + std::to_string(w) + "x" + std::to_string(h));
            ++cases;
          }
        }
      }
      // Use upstream structuring element coefficients plus adversarial samples.
      for (uint32_t thickness : {1, 2, 3, 4, 5, 6}) {
        const auto& values = px::table("se" + std::to_string(thickness));
        const uint32_t ks = static_cast<uint32_t>(std::sqrt(values.size()));
        auto se = scalar.constant(values.data(), values.size());
        for (uint32_t direction : {0, 1}) {
          for (uint32_t clamp : {0, 1}) {
            auto args = [&](const px::Array& out) {
              return px::Arguments{{"src", src},        {"dst", out},
                                   {"se", se},          {"ks", ks},
                                   {"height", h},       {"width", w},
                                   {"mode", direction}, {"do_clamp", clamp}};
            };
            scalar.dispatch("morph", {w, h, 3}, args(a));
            simd.dispatch("morph", {w, h, 3}, args(b));
            equal(a, b, "morph");
            ++cases;
          }
        }
        auto raw = scalar.image(h, w, 1), wa = scalar.image(h, w, 1),
             wb = scalar.image(h, w, 1), lo = scalar.empty(1),
             hi = scalar.empty(1);
        fill(raw, false);
        *static_cast<float*>(lo.data) = -0.125f;
        *static_cast<float*>(hi.data) = 1.25f;
        for (uint32_t norm : {0, 1, 2}) {
          auto args = [&](const px::Array& out, const px::Array& weight) {
            return px::Arguments{
                {"img", src},       {"dst", out},     {"se_erode", se},
                {"se_dilate", se},  {"ks_erode", ks}, {"ks_dilate", ks},
                {"height", h},      {"width", w},     {"w_raw", raw},
                {"w_out", weight},  {"wmin", lo},     {"wmax", hi},
                {"norm_mode", norm}};
          };
          scalar.dispatch("oe_blend", {w, h, 3}, args(a, wa));
          simd.dispatch("oe_blend", {w, h, 3}, args(b, wb));
          equal(a, b, "blend");
          equal(wa, wb, "weight");
          ++cases;
        }
      }
    }
  }
  // Cancellation interrupts a specialization without requiring a second thread.
  auto src = scalar.image(128, 128), dst = scalar.image(128, 128);
  const auto& values = px::table("se3");
  auto se = scalar.constant(values.data(), values.size());
  memory.cancel_after = memory.polls + 3;
  try {
    simd.dispatch("morph", {128, 128, 3},
                  {{"src", src},
                   {"dst", dst},
                   {"se", se},
                   {"ks", 3},
                   {"height", 128},
                   {"width", 128},
                   {"mode", 0},
                   {"do_clamp", 0}});
    throw std::runtime_error("cancellation was ignored");
  } catch (const px::Failure& failure) {
    if (failure.code != 2)
      throw;
  }
  std::cout << "PASS " << cases
            << " bitwise SIMD/Slang stage comparisons; callback thread "
               "ownership; cancellation\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << "\n";
  return 1;
}
