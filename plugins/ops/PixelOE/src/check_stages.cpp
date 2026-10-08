// Independent scalar witnesses run through the public executor and real
// kernels.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "runtime.hpp"  // NOLINT(build/include_subdir)

namespace {
void check(bool good, const char* message) {
  if (!good) {
    throw std::runtime_error(message);
  }
}
template <class T>
T* data(const px::Array& a) {
  return static_cast<T*>(a.data);
}
void exact(const px::Array& actual, const std::vector<float>& expected,
           const char* message) {
  check(actual.count == expected.size() &&
            std::memcmp(actual.data, expected.data(), expected.size() * 4) == 0,
        message);
}
void diffusion(px::Context& c) {
  // Distinct binary fractions make wrong row/channel/edge reads observable.
  for (uint32_t w : {1, 2, 3, 17}) {
    auto img = c.image(5, w), err = c.empty(9 * w);
    std::vector<float> expected(img.count, .25f);
    std::copy(expected.begin(), expected.end(), data<float>(img));
    for (uint32_t i = 0; i < err.count; ++i) {
      data<float>(err)[i] = (static_cast<int>(i % 17) - 8) / 64.0f;
    }
    for (uint32_t y : {0, 2}) {
      for (uint32_t ch = 0; ch < 3; ++ch) {
        for (uint32_t row = 0; row < 2; ++row) {
          for (uint32_t x = 0; x < w; ++x) {
            float sum = 0;
            const float taps[2][3] = {{0, 0, 7.0f / 16},
                                      {3.0f / 16, 5.0f / 16, 1.0f / 16}};
            for (uint32_t r = 0; r < 2; ++r) {
              for (int k = 0; k < 3; ++k) {
                int j = static_cast<int>(x) + k - 1;
                if (w == 1) {
                  j = 0;
                } else if (j < 0) {
                  j = -j;
                } else if (j >= static_cast<int>(w)) {
                  j = 2 * static_cast<int>(w - 1) - j;
                }
                sum +=
                    taps[r][k] * data<float>(err)[((row + r) * 3 + ch) * w + j];
              }
            }
            expected[ch * 5 * w + (y + 1 + row) * w + x] += sum;
          }
        }
      }
      c.dispatch(
          "ed_apply", {w, 2, 1},
          {{"img", img}, {"err", err}, {"y", y}, {"height", 5}, {"width", w}});
      exact(img, expected, "error diffusion edge or channel mismatch");
    }
  }
}
void underflow(px::Context& c) {
  auto src = c.image(2, 2), dst = c.image(1, 1);
  const float tiny = std::numeric_limits<float>::denorm_min();
  for (uint32_t ch = 0; ch < 3; ++ch) {
    for (uint32_t i = 0; i < 4; ++i) {
      data<float>(src)[ch * 4 + i] = static_cast<float>(4 * (i + 1)) * tiny;
    }
  }
  c.dispatch("interpolate", {1, 1, 3},
             {{"src", src},
              {"dst", dst},
              {"mode", 2},
              {"in_h", 2},
              {"in_w", 2},
              {"out_h", 1},
              {"out_w", 1},
              {"scale_h", 2.0f},
              {"scale_w", 2.0f}});
  if (c.gpu_enabled()) {
    // Native profiles permit either preservation or signed-zero flushing.
    // Record which this device actually implements; selection/copy bit
    // preservation is checked independently by the public workflow contract
    // test.
    const bool preserved = data<float>(dst)[0] == 10 * tiny;
    const bool flushed = data<float>(dst)[0] == 0.0f;
    check(preserved || flushed, "invalid native underflow result");
    for (uint32_t ch = 1; ch < 3; ++ch) {
      check(data<uint32_t>(dst)[ch] == data<uint32_t>(dst)[0],
            "inconsistent native underflow");
    }
    std::cout << (c.gpu_backend() == PS_GPU_BACKEND_VULKAN_V1
                      ? "vulkan_native_fp32_underflow="
                      : "metal_native_fp32_underflow=")
              << (preserved ? "preserved" : "flushed") << '\n';
  } else {
    exact(dst, std::vector<float>(3, 10 * tiny), "bilinear gradual underflow");
  }
}
void centroid_iterations(px::Context& c) {
  if (!c.gpu_enabled()) {
    return;
  }
  // Compare every iteration, including all eight centroid fields and the
  // global stop state. Shapes cross both 16x16 groups and 1024-item folds.
  for (auto shape : {std::array<uint32_t, 3>{2, 65, 17},
                     {3, 17, 2},
                     {6, 17, 17},
                     {64, 2, 1}}) {
    const auto p = shape[0], w = shape[1], h = shape[2], n = w * h;
    auto img = c.image(h * p, w * p);
    auto old_cent = c.empty(n * 8), new_cent = c.empty(n * 8);
    auto old_diff = c.empty(4), new_diff = c.empty(4), item = c.empty(n);
    const auto chunks = n / 1024 + (n % 1024 != 0);
    auto part = c.empty(chunks);
    for (uint32_t fixture = 0; fixture < 6; ++fixture) {
      for (uint32_t j = 0; j < img.count; ++j) {
        // Uniform values exercise empty clusters and global stop. The
        // nonuniform cases include signed zero and flushed subnormal inputs.
        const float value = static_cast<float>((j * 73 + j / 17) % 1025) / 1024;
        data<float>(img)[j] =
            fixture == 0   ? .25f
            : fixture == 1 ? (j % 7 ? value : -0.0f)
            : fixture == 2
                ? (j % 7 ? value : std::numeric_limits<float>::denorm_min())
            : fixture == 3
                ? 8 * value - 4
                : std::array<float, 4>{
                      -0.0f, std::numeric_limits<float>::denorm_min(),
                      std::nextafter(std::numeric_limits<float>::min(), 0.0f),
                      std::numeric_limits<float>::min()}[j % 4];
        if (fixture == 5) {
          // Only the final block changes, beyond every full workgroup/fold.
          const auto x = j % (w * p), y = (j / (w * p)) % (h * p);
          data<float>(img)[j] =
              x >= (w - 1) * p && y >= (h - 1) * p
                  ? std::array<float, 4>{0, .25f, .25f, 1}[j % 4]
                  : .25f;
        }
      }
      px::Arguments args{
          {"img", img}, {"cent", old_cent}, {"height", h * p}, {"width", w * p},
          {"p", p},     {"out_h", h},       {"out_w", w}};
      c.dispatch("kc_init", {w, h, 1}, args);
      std::memcpy(new_cent.data, old_cent.data, n * 8 * sizeof(float));
      std::memset(old_diff.data, 0, 4 * sizeof(float));
      std::memset(new_diff.data, 0, 4 * sizeof(float));
      for (uint32_t it = 0; it < 4; ++it) {
        auto portable = args;
        portable.items.insert(
            {{"item_diff", item}, {"diff", old_diff}, {"it", it}});
        c.dispatch("kc_iter", {w, h, 1}, portable);
        c.dispatch("diff_partial", {chunks, 1, 1},
                   {{"item_diff", item},
                    {"part", part},
                    {"diff", old_diff},
                    {"it", it},
                    {"items", n},
                    {"chunk", 1024},
                    {"chunks", chunks}});
        c.dispatch("diff_final", {1, 1, 1},
                   {{"part", part},
                    {"diff", old_diff},
                    {"it", it},
                    {"chunks", chunks}});
        auto atomic = args;
        atomic.items.at("cent") = px::Argument(new_cent);
        atomic.items.insert({{"diff_bits", new_diff}, {"it", it}});
        c.dispatch("kc_iter_max", {w, h, 1}, atomic);
        check(std::memcmp(old_cent.data, new_cent.data,
                          n * 8 * sizeof(float)) == 0,
              "atomic centroid iteration fields differ");
        for (uint32_t slot = 0; slot < 4; ++slot) {
          const auto before = data<float>(old_diff)[slot];
          const auto after = data<float>(new_diff)[slot];
          // Native float max may flush subnormals that uint max preserves.
          // Both produce the same strict comparison against normal STOP_EPS.
          const bool tiny = before >= 0 && after >= 0 &&
                            before < std::numeric_limits<float>::min() &&
                            after < std::numeric_limits<float>::min();
          check(tiny || data<uint32_t>(old_diff)[slot] ==
                            data<uint32_t>(new_diff)[slot],
                "atomic centroid global max differs");
          check((before < 1.0f / 256) == (after < 1.0f / 256),
                "atomic centroid stop state differs");
        }
        if (fixture == 5 && it == 0) {
          check(data<float>(new_diff)[0] > 1.0f / 256,
                "final workgroup centroid maximum was lost");
        }
        if (fixture == 0) {
          check(data<float>(new_diff)[it] == 0,
                "uniform centroid failed to stop");
          for (uint32_t j = 0; j < n; ++j) {
            for (uint32_t ch = 0; ch < 6; ++ch) {
              check(data<float>(new_cent)[8 * j + ch] == .25f,
                    "uniform centroid mean");
            }
            check(data<float>(new_cent)[8 * j + 6] == p * p &&
                      data<float>(new_cent)[8 * j + 7] == 0,
                  "centroid distance tie or empty cluster");
          }
        }
      }
    }
    // The comparison is strict at STOP_EPS; a skipped iteration leaves
    // both the centroid bytes and its initially zero control slot intact.
    const float eps = 1.0f / 256;
    for (float previous :
         {std::nextafter(eps, 0.0f), eps, std::nextafter(eps, 1.0f)}) {
      std::fill_n(data<float>(new_cent), n * 8, .5f);
      std::memset(new_diff.data, 0, 4 * sizeof(float));
      data<float>(new_diff)[0] = previous;
      c.dispatch("kc_iter_max", {w, h, 1},
                 {{"img", img},
                  {"cent", new_cent},
                  {"diff_bits", new_diff},
                  {"it", 1},
                  {"height", h * p},
                  {"width", w * p},
                  {"p", p},
                  {"out_h", h},
                  {"out_w", w}});
      for (uint32_t j = 0; j < n; ++j) {
        check(
            data<float>(new_cent)[8 * j + 6] == (previous < eps ? .5f : p * p),
            "atomic centroid stop threshold");
      }
    }
  }
}
void assignment(px::Context& c) {
  const std::vector<float> samples{0,
                                   .25f,
                                   .5f,
                                   .75f,
                                   1,
                                   std::nextafter(.25f, 0.0f),
                                   std::nextafter(.25f, 1.0f),
                                   std::nextafter(.75f, 0.0f),
                                   std::nextafter(.75f, 1.0f)};
  const uint32_t n = samples.size(), k = 3;
  auto img = c.empty(n * 3), cent = c.empty(k * 3), weights = c.empty(n),
       labels = c.empty(n), control = c.empty(4), diff = c.empty(k);
  for (uint32_t ch = 0; ch < 3; ++ch) {
    std::copy(samples.begin(), samples.end(), data<float>(img) + ch * n);
  }
  for (uint32_t j = 0; j < k; ++j) {
    std::fill_n(data<float>(cent) + j * 3, 3, .5f * j);
  }
  for (uint32_t i = 0; i < n; ++i) {
    data<float>(weights)[i] = i % 2 ? 0.0f : .5f;
  }
  for (uint32_t weighted : {0, 1}) {
    // Exact threshold, its adjacent representable numbers, and prior stop.
    const float eps = 1.0f / 256;
    for (float delta :
         {std::nextafter(eps, 0.0f), eps, std::nextafter(eps, 1.0f)}) {
      for (uint32_t previous : {0, 1}) {
        std::fill_n(data<uint32_t>(labels), n, 12345);
        data<uint32_t>(control)[0] = previous;
        std::fill_n(data<float>(diff), k, delta);
        c.dispatch("km_assign", {n, 1, 1},
                   {{"pix", img},
                    {"weight", weights},
                    {"cent", cent},
                    {"labels", labels},
                    {"item_diff", diff},
                    {"run", control},
                    {"it", 1},
                    {"K", k},
                    {"count", n},
                    {"batch", 1},
                    {"weighted", weighted}});
        const bool running = previous && delta >= eps;
        check(data<uint32_t>(control)[1] == static_cast<unsigned>(running),
              "kmeans stop boundary mismatch");
        for (uint32_t i = 0; i < n; ++i) {
          uint32_t expected = 12345;
          if (running) {
            float minimum = std::numeric_limits<float>::infinity();
            for (uint32_t j = 0; j < k; ++j) {
              float distance = samples[i] - .5f * j;
              distance *= distance;
              if (weighted) {
                distance *= data<float>(weights)[i];
              }
              const float total = (distance + distance) + distance;
              if (total < minimum) {
                expected = j;
                minimum = total;
              }
            }
          }
          check(data<uint32_t>(labels)[i] == expected,
                "palette tie, label or skipped write mismatch");
        }
      }
    }
  }
}
void ordered(px::Context& c) {
  auto img = c.image(1, 1), pal = c.empty(6), bayer = c.empty(64),
       dst = c.image(1, 1);
  std::fill_n(data<float>(img), 3, .5f);
  std::fill_n(data<float>(pal), 3, 0);
  std::fill_n(data<float>(pal) + 3, 3, 1);
  const float denominator = 1.5f + 1e-6f;
  const float ratio = .75f / denominator;
  const auto evaluate = [&](float threshold) {
    data<float>(bayer)[0] = threshold;
    c.dispatch("dither_ordered", {1, 1, 1},
               {{"img", img},
                {"pal", pal},
                {"bayer", bayer},
                {"dst", dst},
                {"K", 2},
                {"height", 1},
                {"width", 1}});
  };
  if (c.gpu_backend() == PS_GPU_BACKEND_VULKAN_V1) {
    // Vulkan OpFDiv permits 2.5 ULP, while comparisons are exact. Infer the
    // transition from adjacent thresholds; require one monotone palette switch
    // within the native addition and division bounds. CPU/Metal retain their
    // original gate.
    std::array<float, 17> thresholds{};
    thresholds[8] = ratio;
    for (unsigned i = 0; i < 8; ++i) {
      thresholds[7 - i] = std::nextafter(thresholds[8 - i], 0.0f);
      thresholds[9 + i] = std::nextafter(thresholds[8 + i], 1.0f);
    }
    unsigned first_nearest = thresholds.size();
    bool nearest = false;
    for (unsigned i = 0; i < thresholds.size(); ++i) {
      evaluate(thresholds[i]);
      const float selected = data<float>(dst)[0];
      check(selected == 0 || selected == 1, "ordered palette output");
      exact(dst, std::vector<float>(3, selected), "ordered channel mismatch");
      check(!nearest || selected == 0, "ordered threshold is not monotone");
      if (selected == 0 && !nearest) {
        nearest = true;
        first_nearest = i;
      }
    }
    check(first_nearest > 0 && first_nearest < thresholds.size(),
          "native division threshold outside scan");
    const double boundary = thresholds[first_nearest - 1];
    const double reference = .75 / static_cast<double>(denominator);
    const double ulp = static_cast<double>(thresholds[9]) - ratio;
    // Without an explicit device RTE execution mode the denominator addition
    // may round to either adjacent float. Its exact sum fits binary64 here.
    const double exact_denominator = 1.5 + static_cast<double>(1e-6f);
    const float denominator_low = denominator <= exact_denominator
                                      ? denominator
                                      : std::nextafter(denominator, 0.0f);
    const float denominator_high = denominator >= exact_denominator
                                       ? denominator
                                       : std::nextafter(denominator, 2.0f);
    check(boundary >= .75 / denominator_high - 2.5 * ulp &&
              boundary <= .75 / denominator_low + 2.5 * ulp,
          "native ordered arithmetic exceeds Vulkan rounding/division bounds");
    std::cout << "vulkan_ordered_transition_ulp="
              << (boundary - reference) / ulp << '\n';
  } else {
    for (float threshold :
         {std::nextafter(ratio, 0.0f), ratio, std::nextafter(ratio, 1.0f)}) {
      evaluate(threshold);
      exact(dst, std::vector<float>(3, threshold > ratio ? 0.0f : 1.0f),
            "ordered dither equality or adjacent threshold mismatch");
    }
  }
  // A zero distance gives ratio zero. Exercise strict > and equality on every
  // profile independently of the preceding inexact quotient.
  std::fill_n(data<float>(img), 3, 0);
  for (float threshold : {-std::numeric_limits<float>::min(), 0.0f,
                          std::numeric_limits<float>::min()}) {
    evaluate(threshold);
    exact(dst, std::vector<float>(3, threshold > 0 ? 0.0f : 1.0f),
          "ordered zero-ratio equality mismatch");
  }
}
void centroid_selection(px::Context& c) {
  auto img = c.image(2, 2), cent = c.empty(8), dst = c.image(1, 1);
  std::fill_n(data<float>(cent), 3, .75f);
  std::fill_n(data<float>(cent) + 3, 3, .25f);
  for (float pixel : {.25f, .5f, .75f}) {
    std::fill_n(data<float>(img), 12, pixel);
    for (auto sizes :
         {std::array<float, 2>{2, 2}, {3, 1}, {1, 3}, {4, 0}, {0, 4}, {0, 0}}) {
      data<float>(cent)[6] = sizes[0];
      data<float>(cent)[7] = sizes[1];
      // For populated clusters the pinned criterion uses their counts.
      // For an empty cluster this fixture's SSE comparison is exact.
      const bool second =
          sizes[0] > 0 && sizes[1] > 0 ? sizes[1] > sizes[0] : pixel < .5f;
      c.dispatch("kc_final", {1, 1, 1},
                 {{"img", img},
                  {"cent", cent},
                  {"dst", dst},
                  {"height", 2},
                  {"width", 2},
                  {"p", 2},
                  {"out_h", 1},
                  {"out_w", 1}});
      exact(dst, std::vector<float>(3, second ? .25f : .75f),
            "centroid majority tie or empty-cluster SSE mismatch");
    }
  }
  constexpr uint32_t n = 2051, chunk = 1024, chunks = 3;
  auto item = c.empty(n), part = c.empty(chunks), diff = c.empty(4);
  const float eps = 1.0f / 256;
  for (float prior :
       {std::nextafter(eps, 0.0f), eps, std::nextafter(eps, 1.0f)}) {
    std::fill_n(data<float>(item), n, 0);
    data<float>(item)[n - 1] = .5f;
    std::fill_n(data<float>(part), chunks, .25f);
    data<float>(diff)[0] = prior;
    data<float>(diff)[1] = 0;
    c.dispatch("diff_partial", {chunks, 1, 1},
               {{"item_diff", item},
                {"part", part},
                {"diff", diff},
                {"it", 1},
                {"items", n},
                {"chunk", chunk},
                {"chunks", chunks}});
    c.dispatch("diff_final", {1, 1, 1},
               {{"part", part}, {"diff", diff}, {"it", 1}, {"chunks", chunks}});
    check(data<float>(diff)[1] == (prior >= eps ? .5f : 0),
          "global centroid stopping or tail reduction mismatch");
  }
}
void contrast_selection(px::Context& c) {
  auto img = c.image(3, 3), dst = c.image(1, 1);
  const std::array<float, 9> blocks[] = {{0, 10, 10, 10, 20, 10, 10, 10, 100},
                                         {0, 90, 90, 90, 80, 90, 90, 90, 100},
                                         {0, 25, 25, 25, 50, 75, 75, 75, 100}};
  for (const auto& block : blocks) {
    std::fill_n(data<float>(img), img.count, 0);
    std::copy(block.begin(), block.end(), data<float>(img));
    auto sorted = block;
    std::sort(sorted.begin(), sorted.end());
    const float median = sorted[4];
    const float mean = std::accumulate(block.begin(), block.end(), 0.0f) / 9;
    const bool low = median < mean && sorted[8] - median > median - sorted[0];
    const bool high = median > mean && sorted[8] - median < median - sorted[0];
    c.dispatch("contrast_downscale", {1, 1, 1},
               {{"lab_img", img},
                {"dst", dst},
                {"height", 3},
                {"width", 3},
                {"p", 3},
                {"out_h", 1},
                {"out_w", 1}});
    // Neutral Lab endpoints are far apart. Classify the selected endpoint;
    // do not treat a continuous RGB tolerance as an unobserved branch check.
    for (uint32_t ch = 0; ch < 3; ++ch) {
      const float v = data<float>(dst)[ch];
      check(low    ? v < .001f
            : high ? v > .999f
                   : v > .4f && v < .6f,
            "contrast median/mean endpoint selection mismatch");
    }
  }
}
void repeat_ties(px::Context& c) {
  constexpr uint32_t n = 257, chunk = 128, chunks = 3;
  auto rem = c.empty(n), counts = c.empty(n), fl = c.empty(1),
       state = c.empty(4), hist = c.empty(chunks * 16), gt = c.empty(chunks),
       eq = c.empty(chunks);
  for (uint32_t i = 0; i < n; ++i) {
    data<float>(rem)[i] = (i % 5) / 8.0f;
  }
  std::vector<uint32_t> order(n);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
    return data<float>(rem)[a] > data<float>(rem)[b];
  });
  for (uint32_t need : {0, 1, 51, 128, 256, 257}) {
    std::fill_n(data<uint32_t>(counts), n, 4);
    data<float>(fl)[0] = 3 * n - need;
    c.dispatch("rp_select_init", {1, 1, 1},
               {{"fl_sum", fl},
                {"state", state},
                {"count", n},
                {"batch", 1},
                {"extra", static_cast<float>(3 * n)}});
    for (int shift = 28; shift >= 0; shift -= 4) {
      c.dispatch("rp_hist", {chunks, 1, 1},
                 {{"rem", rem},
                  {"state", state},
                  {"hist", hist},
                  {"shift", shift},
                  {"count", n},
                  {"batch", 1},
                  {"chunk", chunk},
                  {"chunks", chunks}});
      c.dispatch("rp_select_step", {1, 1, 1},
                 {{"hist", hist},
                  {"state", state},
                  {"shift", shift},
                  {"batch", 1},
                  {"chunks", chunks}});
    }
    c.dispatch("rp_tie_count", {chunks, 1, 1},
               {{"rem", rem},
                {"state", state},
                {"chunk_gt", gt},
                {"chunk_eq", eq},
                {"count", n},
                {"batch", 1},
                {"chunk", chunk},
                {"chunks", chunks}});
    c.dispatch("rp_tie_scan", {1, 1, 1},
               {{"chunk_gt", gt},
                {"chunk_eq", eq},
                {"state", state},
                {"batch", 1},
                {"chunks", chunks}});
    c.dispatch("rp_mark", {chunks, 1, 1},
               {{"rem", rem},
                {"state", state},
                {"tie_prefix", eq},
                {"repeat", counts},
                {"count", n},
                {"batch", 1},
                {"chunk", chunk},
                {"chunks", chunks}});
    std::vector<uint32_t> expected(n, 4);
    for (uint32_t i = 0; i < need; ++i) {
      ++expected[order[i]];
    }
    check(std::memcmp(counts.data, expected.data(), n * 4) == 0,
          "repeat stable selection mismatch");
  }
}
void fixed_sums(px::Context& c) {
  constexpr uint32_t n = 259, k = 3, chunk = 128, chunks = 3;
  auto img = c.empty(n * 3), counts = c.empty(n), labels = c.empty(n),
       part = c.empty(chunks * k * 4, 8), control = c.empty(1),
       cent = c.empty(k * 3), diff = c.empty(k);
  data<uint32_t>(control)[0] = 1;
  for (uint32_t i = 0; i < n; ++i) {
    data<uint32_t>(counts)[i] = i % 7 + 1;
    data<uint32_t>(labels)[i] = i % 2;  // Last cluster stays empty.
    for (uint32_t ch = 0; ch < 3; ++ch) {
      data<float>(img)[ch * n + i] = ((i * 17 + ch * 53) % 256) / 256.0f;
    }
  }
  for (uint32_t repeat : {0, 1}) {
    c.dispatch("km_partial", {chunks * k, 1, 1},
               {{"pix", img},
                {"repeat", counts},
                {"labels", labels},
                {"part", part},
                {"run", control},
                {"it", 0},
                {"K", k},
                {"count", n},
                {"batch", 1},
                {"chunk", chunk},
                {"chunks", chunks},
                {"use_repeat", repeat}});
    std::vector<int64_t> expected(chunks * k * 4);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t index = ((i / chunk) * k + i % 2) * 4;
      const int64_t m = repeat ? i % 7 + 1 : 1;
      expected[index + 3] += m;
      for (uint32_t ch = 0; ch < 3; ++ch) {
        expected[index + ch] +=
            static_cast<int64_t>(
                static_cast<double>(data<float>(img)[ch * n + i]) *
                4294967296.0) *
            m;
      }
    }
    check(std::memcmp(part.data, expected.data(), expected.size() * 8) == 0,
          "32.32 integer partial sums mismatch");
    std::fill_n(data<float>(cent), k * 3, .25f);
    c.dispatch("km_update", {k, 1, 1},
               {{"part", part},
                {"cent", cent},
                {"item_diff", diff},
                {"run", control},
                {"it", 0},
                {"K", k},
                {"batch", 1},
                {"chunks", chunks}});
    std::vector<float> means(k * 3, .25f);
    for (uint32_t j = 0; j < k; ++j) {
      int64_t sums[4]{};
      for (uint32_t block = 0; block < chunks; ++block) {
        for (uint32_t ch = 0; ch < 4; ++ch) {
          sums[ch] += expected[(block * k + j) * 4 + ch];
        }
      }
      if (sums[3]) {
        for (uint32_t ch = 0; ch < 3; ++ch) {
          means[j * 3 + ch] = (static_cast<float>(sums[ch]) * 0x1p-32f) /
                              static_cast<float>(sums[3]);
        }
      }
    }
    exact(cent, means, "32.32 centroid mean or empty cluster mismatch");
  }
}
struct Memory {
  const ps::ResultProgramPhase& call;
  ps::ResourceBudget budget;
  std::map<uint8_t*, ps::MutableBuffer> blocks;
};
ps::Result<ps::ResultProgramPoll> run(const ps::ResultProgramPhase& call,
                                      const ps::ResourceBudget& budget) {
  Memory memory{call, budget, {}};
  ps_result_services_v2 services{};
  services.struct_size = sizeof(services);
  services.abi_version = PS_RESULT_OPERATION_ABI_VERSION_2;
  services.context = &memory;
  services.gpu = call.gpu;
  services.cancelled = [](void* p) {
    return static_cast<Memory*>(p)->call.query.cancellation.cancelled() ? 1 : 0;
  };
  services.consume_work = [](void* p, uint64_t units) {
    return static_cast<Memory*>(p)->budget.consume({units}).ok() ? 0 : 4;
  };
  services.allocate_scratch = [](void* p, uint64_t bytes,
                                 uint8_t** destination) -> int {
    auto& m = *static_cast<Memory*>(p);
    auto buffer = m.call.allocator.allocate(bytes);
    if (!buffer.ok()) {
      return 4;
    }
    auto value = buffer.take_value();
    auto* address = value.data();
    std::memset(address, 0, bytes);
    m.blocks.emplace(address, std::move(value));
    *destination = address;
    return 0;
  };
  services.release_scratch = [](void* p, uint8_t* bytes) {
    return static_cast<Memory*>(p)->blocks.erase(bytes) ? 0 : 6;
  };
  {
    px::Environment environment;
    px::Context context(&services);
    underflow(context);
    diffusion(context);
    assignment(context);
    ordered(context);
    centroid_selection(context);
    centroid_iterations(context);
    contrast_selection(context);
    repeat_ties(context);
    fixed_sums(context);
  }
  check(memory.blocks.empty(), "stage scratch owner leak");
  auto builder =
      ps::ResultBuilder::start(call.resources, *call.query.output.result_schema,
                               call.query.semantic_key)
          .take_value();
  auto relation = ps::ResultRelation::cartesian(call.resources, 1, {0, 1, 0, 0})
                      .take_value();
  check(builder.bind_descriptor_relation(relation).ok(), "stage descriptor");
  const float value = 1;
  check(builder
            .publish_tensor(
                0, ps::Region::whole({1}),
                ps::ByteView(reinterpret_cast<const uint8_t*>(&value), 4),
                relation, {true, true, true, true})
            .ok(),
        "stage Result publication");
  return ps::Result<ps::ResultProgramPoll>(
      ps::ResultPublication{builder.seal().take_value(), true});
}
struct StageProgram {
  uint32_t expected_backend;
  explicit StageProgram(uint32_t backend) : expected_backend(backend) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    if (expected_backend &&
        (!phase.gpu || phase.gpu->backend != expected_backend))
      return ps::Result<ps::ResultProgramPoll>(
          ps::Status{ps::ErrorCode::BackendUnavailable,
                     "stage profile requires its named native backend"});
    return run(phase, phase.resources);
  }
};

}  // namespace
int main(int argc, char** argv) try {
  const std::string backend = argc > 1 ? argv[1] : "cpu";
  check(backend == "cpu" || backend == "gpu" || backend == "vulkan",
        "unknown backend");
  const bool gpu = backend != "cpu";
  const uint32_t expected_backend = backend == "vulkan"
                                        ? PS_GPU_BACKEND_VULKAN_V1
                                    : gpu ? PS_GPU_BACKEND_METAL_V1
                                          : 0;
  auto registry = std::make_shared<ps::OperationRegistry>();
  ps::ResourceBudget budget;
  ps::OperationDefinition operation;
  operation.key = "test.pixeloe_stages";
  operation.traits.supports_cpu = !gpu;
  operation.traits.supports_gpu = gpu;
  operation.traits.workspace_bytes = 8 << 20;
  auto& output = operation.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = "test.pixeloe.stage";
  output.output_schema.result_schema_version = 1;
  ps::SchemaTemplate schema;
  schema.id = "test.pixeloe.stage";
  ps::ResultTensorSpec tensor;
  tensor.key = "passed";
  tensor.descriptor = {ps::ElementType::Float32, {1}};
  schema.tensors.push_back(tensor);
  output.result_schema = schema;
  output.region_rule = ps::OperationRegionRule::Whole;
  output.continuation_bytes = sizeof(StageProgram);
  output.maximum_dependency_stages = 1;
  operation.start_result = [expected_backend](
                               const ps::ResultProgramQuery&,
                               const ps::BufferAllocator& allocator) {
    return ps::ResultContinuation::make<StageProgram>(allocator,
                                                      expected_backend);
  };
  const auto registered = registry->register_operation(std::move(operation));
  check(registered.ok(), registered.message.c_str());
  check(registry->freeze().ok(), "freeze");
  ps::WorkflowDocument document;
  document.nodes = {{1, "test.pixeloe_stages", {}, {}}};
  document.outputs = {{"result", 1, "value"}};
  ps::GraphContext graph(document);
  ps::PlanningOptions planning;
  planning.execution_mode =
      gpu ? ps::ExecutionMode::NativeGpu : ps::ExecutionMode::CpuExact;
  const auto plan =
      ps::Compiler(registry).compile(graph, planning).take_value();
  ps::ExecutionContextConfig config;
  config.gpu_enabled = gpu;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  if (gpu && !execution.gpu_enabled()) {
    std::cout << "native GPU unavailable\n";
    return 77;
  }
  budget = execution.resource_budget().take_value();
  {
    const auto result = execution.execute(plan.plan);
    if (!result.ok()) {
      throw std::runtime_error(result.status().message);
    }
    check(result.value().diagnostics.fallback_reasons.empty(), "CPU fallback");
    check(!gpu || result.value().diagnostics.native_dispatch_count > 100,
          "missing native stage dispatches");
    std::cout << "PASS "
              << (backend == "vulkan" ? "vulkan_native_fp32"
                  : gpu               ? "metal_native_fp32"
                                      : "CPU")
              << ": exact diffusion edges, palette labels and stop threshold, "
                 "ordered threshold, centroid and contrast selection, stable "
                 "repeat ties, int64 partial sums and means; "
              << "native_dispatches="
              << result.value().diagnostics.native_dispatch_count << '\n';
  }
  check(budget.statistics().live[ps::ResourceKind::Payload] == 0,
        "payload retained after result release");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
