#include <map>
#include <string>
#include <utility>
// Adapted orchestration of PixelOE 0239787, Apache-2.0; see third_party/.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "runtime.hpp"  // NOLINT(build/include_subdir)
namespace px {
namespace {
Array constant(Context& c, const std::string& key) {
  const auto& t = table(key);
  return c.constant(t.data(), t.size());
}
std::array<uint32_t, 3> grid(const Array& a) {
  return {a.width, a.height, a.channels};
}
std::pair<Array, Array> minmax(Context& c, Array src, uint32_t segments,
                               uint32_t length) {
  Array lo = src, hi = src;
  do {
    uint32_t chunks = (length + 63) / 64;
    auto a = c.empty(static_cast<uint64_t>(segments) * chunks),
         b = c.empty(static_cast<uint64_t>(segments) * chunks);
    c.dispatch("seg_minmax_level", {segments * chunks, 1, 1},
               {{"src_min", lo},
                {"src_max", hi},
                {"part_min", a},
                {"part_max", b},
                {"src_offset", 0},
                {"seg_count", segments},
                {"seg_len", length},
                {"chunk", 64},
                {"chunks", chunks}});
    lo = a;
    hi = b;
    length = chunks;
  } while (length > 1);
  return {lo, hi};
}
Array sum(Context& c, Array src, uint32_t segments, uint32_t length,
          float scale = 1) {
  do {
    uint32_t chunks = (length + 63) / 64;
    auto a = c.empty(static_cast<uint64_t>(segments) * chunks);
    c.dispatch("seg_sum_level", {segments * chunks, 1, 1},
               {{"src", src},
                {"part", a},
                {"src_offset", 0},
                {"seg_count", segments},
                {"seg_len", length},
                {"chunk", 64},
                {"chunks", chunks},
                {"scale", chunks == 1 ? scale : 1.0f}});
    src = a;
    length = chunks;
  } while (length > 1);
  return src;
}
Array lab(Context& c, const Array& src, bool lum) {
  auto out = c.image(src.height, src.width, lum ? 1 : 3);
  auto pixels = src.height * src.width;
  if (lum) {
    c.flat("luminance", pixels,
           {{"img", src}, {"lum", out}, {"pixels", pixels}, {"hw", pixels}});
  } else {
    c.flat(
        "lab_image", pixels,
        {{"img", src}, {"lab_out", out}, {"pixels", pixels}, {"hw", pixels}});
  }
  return out;
}
void moments(Context& c, const Array& src, const Array& stats, uint32_t slot) {
  uint32_t pixels = src.height * src.width, chunks = (pixels + 31) / 32;
  auto part = c.empty(static_cast<uint64_t>(chunks) * 3);
  c.dispatch("lab_moments_partial", {chunks, 1, 1},
             {{"img", src},
              {"part", part},
              {"pixels", pixels},
              {"hw", pixels},
              {"chunk", 32},
              {"chunks", chunks}});
  do {
    uint32_t merged = (chunks + 63) / 64;
    auto next = c.empty(static_cast<uint64_t>(merged) * 3);
    c.dispatch("moments_merge", {merged, 1, 1},
               {{"src", part},
                {"part", next},
                {"stats", stats},
                {"count", chunks},
                {"chunk", 64},
                {"chunks", merged},
                {"slot", slot}});
    part = next;
    chunks = merged;
  } while (chunks > 1);
}
Array blur(Context& c, const Array& src, const Array& add, uint32_t r,
           const Options& o) {
  auto out = c.image(src.height, src.width);
  auto g = grid(src);
  uint32_t reflect = src.height > r && src.width > r;
  if (o.colorfix_blur == "exact" && o.blur_impl == "lowrank") {
    auto taps = constant(c, "lr" + std::to_string(r));
    auto tmp = c.image(src.height, src.width);
    uint32_t k = 2 * r + 1, rank = std::min(o.blur_rank, k);
    for (uint32_t comp = 0; comp < rank; ++comp) {
      c.dispatch(("lr_rows_r" + std::to_string(r)).c_str(),
                 {(src.width + 7) / 8, src.height, 3},
                 {{"src", src},
                  {"tmp", tmp},
                  {"tables", taps},
                  {"offset", comp * k},
                  {"radius", r},
                  {"height", src.height},
                  {"width", src.width},
                  {"reflect", reflect}});
      c.dispatch(("lr_cols_r" + std::to_string(r)).c_str(),
                 {src.width, (src.height + 7) / 8, 3},
                 {{"tmp", tmp},
                  {"add_src", add.data ? add : src},
                  {"dst", out},
                  {"tables", taps},
                  {"offset", (k + comp) * k},
                  {"radius", r},
                  {"height", src.height},
                  {"width", src.width},
                  {"reflect", reflect},
                  {"mode", comp ? 2 : (add.data ? 1 : 0)}});
    }
  } else if (o.colorfix_blur == "separable") {
    auto taps = constant(c, "sep" + std::to_string(r));
    auto tmp = c.image(src.height, src.width);
    c.dispatch("blur_rows", g,
               {{"src", src},
                {"dst", tmp},
                {"weights", taps},
                {"radius", r},
                {"height", src.height},
                {"width", src.width},
                {"reflect", reflect}});
    c.dispatch("blur_cols", g,
               {{"src", tmp},
                {"add_src", add.data ? add : src},
                {"dst", out},
                {"weights", taps},
                {"radius", r},
                {"height", src.height},
                {"width", src.width},
                {"reflect", reflect},
                {"has_add", add.data ? 1 : 0}});
  } else {
    auto taps = constant(c, "exact" + std::to_string(r));
    c.dispatch(o.blur_impl == "sym"
                   ? ("blur2d_sym_r" + std::to_string(r)).c_str()
                   : "blur2d",
               o.blur_impl == "sym"
                   ? std::array<uint32_t, 3>{src.width, (src.height + 3) / 4, 3}
                   : g,
               {{"src", src},
                {"add_src", add.data ? add : src},
                {"dst", out},
                {"weights", taps},
                {"radius", r},
                {"height", src.height},
                {"width", src.width},
                {"reflect", reflect},
                {"has_add", add.data ? 1 : 0}});
  }
  return out;
}
Array match(Context& c, const Array& src, const Array& target,
            const Options& o) {
  auto stats = c.empty(4);
  moments(c, src, stats, 0);
  moments(c, target, stats, 1);
  const auto* values = static_cast<const float*>(stats.data);
  if (!std::isfinite(values[0]) || !std::isfinite(values[1]) ||
      !std::isfinite(values[2]) || !std::isfinite(values[3]) ||
      values[1] <= 0) {
    throw Failure(1, "PixelOE color matching has undefined Lab variance");
  }
  auto inp = c.image(src.height, src.width),
       diff = c.image(src.height, src.width);
  c.flat("match_apply", src.height * src.width,
         {{"src", src},
          {"tgt", target},
          {"stats", stats},
          {"inp", inp},
          {"diff", diff},
          {"pixels", src.height * src.width},
          {"hw", src.height * src.width}});
  for (uint32_t r = 2; r <= 32; r *= 2) {
    diff = blur(c, diff, r == 32 ? inp : Array{}, r, o);
  }
  return diff;
}
struct Weight {
  Array raw, lo, hi;
  uint32_t norm;
};
Weight weight(Context& c, const Array& img, const Options& o) {
  uint32_t h = img.height, w = img.width, k = o.pixel_size, s = k / 2,
           n = h * w;
  auto lum = lab(c, img, true), raw = c.image(h, w, 1), lc = c.image(h, w, 1);
  uint32_t mapping = o.weight_mapping == "contrast_ratio"
                         ? 1
                         : (o.weight_mapping == "contrast_gated" ? 2 : 0);
  if (o.local_stats == "lattice") {
    uint32_t mh = h / s + 1, mw = w / s + 1, ah = (h + 2 * (k / 2) - k) / s + 1,
             aw = (w + 2 * (k / 2) - k) / s + 1;
    auto med = c.empty(static_cast<uint64_t>(mh) * mw),
         lo = c.empty(static_cast<uint64_t>(ah) * aw),
         hi = c.empty(static_cast<uint64_t>(ah) * aw);
    if (k == 2 || k == 4 || k == 6) {
      c.dispatch(("lattice_stats_h" + std::to_string(k)).c_str(), {mw, mh, 1},
                 {{"lum", lum},
                  {"out_med", med},
                  {"out_min", lo},
                  {"out_max", hi},
                  {"height", h},
                  {"width", w},
                  {"stride", s},
                  {"lat_h", mh},
                  {"lat_w", mw}});
    } else {
      c.dispatch("lattice_median", {mw, mh, 1},
                 {{"lum", lum},
                  {"out_stat", med},
                  {"height", h},
                  {"width", w},
                  {"ksize", 2 * k},
                  {"stride", s},
                  {"pad", k},
                  {"lat_h", mh},
                  {"lat_w", mw}});
      c.dispatch("lattice_minmax", {aw, ah, 1},
                 {{"lum", lum},
                  {"out_min", lo},
                  {"out_max", hi},
                  {"height", h},
                  {"width", w},
                  {"ksize", k},
                  {"stride", s},
                  {"pad", k / 2},
                  {"lat_h", ah},
                  {"lat_w", aw}});
    }
    c.dispatch("weight_raw", {w, h, 1},
               {{"med_stat", med},
                {"min_stat", lo},
                {"max_stat", hi},
                {"w_out", raw},
                {"lc_out", lc},
                {"height", h},
                {"width", w},
                {"med_lat_h", mh},
                {"med_lat_w", mw},
                {"med_ksize", 2 * k},
                {"med_pad", k},
                {"mm_lat_h", ah},
                {"mm_lat_w", aw},
                {"mm_ksize", k},
                {"mm_pad", k / 2},
                {"stride", s},
                {"mapping", mapping},
                {"avg_scale", 10.0f},
                {"dist_scale", 3.0f}});
  } else {
    auto med = c.image(h, w, 1), lo = c.image(h, w, 1), hi = c.image(h, w, 1),
         rlo = c.image(h, w, 1), rhi = c.image(h, w, 1);
    uint32_t pad = o.stat_padding == "replicate";
    c.dispatch("sliding_median", {w, h, 1},
               {{"lum", lum},
                {"out_stat", med},
                {"height", h},
                {"width", w},
                {"ksize", 2 * k},
                {"pad_mode", pad}});
    c.dispatch("sliding_minmax_rows", {w, h, 1},
               {{"lum", lum},
                {"row_min", rlo},
                {"row_max", rhi},
                {"height", h},
                {"width", w},
                {"ksize", k},
                {"pad_mode", pad}});
    c.dispatch("sliding_minmax_cols", {w, h, 1},
               {{"row_min", rlo},
                {"row_max", rhi},
                {"out_min", lo},
                {"out_max", hi},
                {"height", h},
                {"width", w},
                {"ksize", k},
                {"pad_mode", pad}});
    c.flat("weight_dense", n,
           {{"med", med},
            {"mn", lo},
            {"mx", hi},
            {"w_out", raw},
            {"lc_out", lc},
            {"count", n},
            {"mapping", mapping},
            {"avg_scale", 10.0f},
            {"dist_scale", 3.0f}});
  }
  if (mapping == 2) {
    auto mean = sum(c, lc, 1, n, 1.0f / static_cast<float>(n));
    c.flat("weight_gate", n,
           {{"w", raw},
            {"lc", lc},
            {"lc_mean", mean},
            {"count", n},
            {"dist_scale", 3.0f}});
  }
  uint32_t norm = o.weight_normalize == "none" ? 0 : 1;
  auto mm = minmax(c, raw, 1, n);
  return {raw, mm.first, mm.second, norm};
}
Array normalize(Context& c, const Weight& w) {
  auto out = c.image(w.raw.height, w.raw.width, 1);
  uint32_t n = out.height * out.width;
  c.flat("normalize_map", n,
         {{"w_raw", w.raw},
          {"wmin", w.lo},
          {"wmax", w.hi},
          {"w_out", out},
          {"hw", n},
          {"count", n},
          {"norm_mode", w.norm}});
  return out;
}
Array outline(Context& c, const Array& img, const Weight& weight,
              const Options& o) {
  uint32_t h = img.height, w = img.width;
  auto se = constant(c, "se" + std::to_string(o.thickness));
  auto blended = c.image(h, w), dummy = c.image(h, w, 1);
  auto ks = static_cast<uint32_t>(std::sqrt(se.count));
  c.dispatch("oe_blend", grid(img),
             {{"img", img},
              {"w_raw", weight.raw},
              {"wmin", weight.lo},
              {"wmax", weight.hi},
              {"se_erode", se},
              {"se_dilate", se},
              {"dst", blended},
              {"w_out", dummy},
              {"ks_erode", ks},
              {"ks_dilate", ks},
              {"height", h},
              {"width", w},
              {"norm_mode", weight.norm}});
  se = constant(c, "se" + std::to_string(std::max(1u, o.thickness - 1)));
  ks = static_cast<uint32_t>(std::sqrt(se.count));
  for (uint32_t i = 0; i < 4; ++i) {
    auto next = c.image(h, w);
    c.dispatch("morph", grid(img),
               {{"src", blended},
                {"dst", next},
                {"se", se},
                {"ks", ks},
                {"height", h},
                {"width", w},
                {"mode", i == 1 || i == 2 ? 1 : 0},
                {"do_clamp", i == 2 ? 1 : 0}});
    blended = next;
  }
  return blended;
}
Array interpolate(Context& c, const Array& src, uint32_t h, uint32_t w,
                  uint32_t mode) {
  auto out = c.image(h, w);
  c.dispatch(
      "interpolate", grid(out),
      {{"src", src},
       {"dst", out},
       {"mode", mode},
       {"in_h", src.height},
       {"in_w", src.width},
       {"out_h", h},
       {"out_w", w},
       {"scale_h", static_cast<float>(src.height) / static_cast<float>(h)},
       {"scale_w", static_cast<float>(src.width) / static_cast<float>(w)}});
  return out;
}
Array lanczos(Context& c, Array src, uint32_t h, uint32_t w) {
  for (uint32_t axis = 0; axis < 2; ++axis) {
    uint32_t n = axis ? src.width : src.height, m = axis ? w : h;
    if (n == m) {
      continue;
    }
    auto offsets = c.empty(m + 1),
         indices = c.empty(static_cast<uint64_t>(m) * 7),
         weights = c.empty(static_cast<uint64_t>(m) * 7);
    auto* off = static_cast<uint32_t*>(offsets.data);
    auto* ind = static_cast<uint32_t*>(indices.data);
    auto* taps = static_cast<float*>(weights.data);
    uint32_t used = 0;
    float scale = static_cast<float>(m) / static_cast<float>(n);
    for (uint32_t i = 0; i < m; ++i) {
      c.check();
      off[i] = used;
      float coord = static_cast<float>(i) / scale;
      float ww[7]{};
      int ii[7]{};
      uint32_t ct = 0;
      float total = 0;
      for (int x = std::max(0, static_cast<int>(std::floor(coord)) - 3);
           x < std::min(static_cast<int>(n),
                        static_cast<int>(std::floor(coord)) + 4);
           ++x) {
        float d = static_cast<float>(x) - coord, v = 0;
        if (std::abs(d) < 1e-7f) {
          v = 1;
        } else if (std::abs(d) < 3) {
          float p = 3.1415927410125732421875f * d;
          v = (std::sin(p) * std::sin(p / 3)) / (p * p / 3);
        }
        ww[ct] = v;
        ii[ct++] = x;
        total += v;
      }
      for (uint32_t j = 0; j < ct; ++j) {
        float v = ww[j] / (total + 1e-7f);
        if (std::abs(v) > 1e-7f) {
          ind[used] = ii[j];
          taps[used++] = v;
        }
      }
    }
    off[m] = used;
    auto out = c.image(axis ? src.height : h, axis ? w : src.width);
    c.dispatch("csr_pass", grid(out),
               {{"src", src},
                {"dst", out},
                {"offsets", offsets},
                {"indices", indices},
                {"weights", weights},
                {"axis", axis},
                {"in_h", src.height},
                {"in_w", src.width},
                {"out_h", out.height},
                {"out_w", out.width},
                {"do_clamp", axis == 1 || src.width == w ? 1 : 0}});
    src = out;
  }
  return src;
}
Array downscale(Context& c, const Array& img, const Options& o) {
  uint32_t p = o.pixel_size, h = img.height / p, w = img.width / p, n = h * w;
  auto out = c.image(h, w);
  if (o.mode == "contrast") {
    if (p == 2 || p == 3 || p == 4 || p == 5 || p == 6 || p == 8) {
      c.dispatch(("contrast_rgb_p" + std::to_string(p)).c_str(), {w, h, 1},
                 {{"lab_img", img},
                  {"dst", out},
                  {"height", img.height},
                  {"width", img.width},
                  {"out_h", h},
                  {"out_w", w},
                  {"up", 0}});
    } else {
      auto l = lab(c, img, false);
      c.dispatch("contrast_downscale", {w, h, 1},
                 {{"lab_img", l},
                  {"dst", out},
                  {"height", img.height},
                  {"width", img.width},
                  {"p", p},
                  {"out_h", h},
                  {"out_w", w}});
    }
  } else if (o.mode == "k_centroid") {
    auto cent = c.empty(static_cast<uint64_t>(n) * 8), item = c.empty(n),
         diff = c.empty(4);
    Arguments args{{"img", img},         {"cent", cent}, {"height", img.height},
                   {"width", img.width}, {"p", p},       {"out_h", h},
                   {"out_w", w}};
    c.dispatch("kc_init", {w, h, 1}, args);
    for (uint32_t i = 0; i < 4; ++i) {
      auto a = args;
      a.items.insert({{"item_diff", item}, {"diff", diff}, {"it", i}});
      c.dispatch("kc_iter", {w, h, 1}, a);
      uint32_t chunks = (n + 1023) / 1024;
      auto part = c.empty(chunks);
      c.dispatch("diff_partial", {chunks, 1, 1},
                 {{"item_diff", item},
                  {"part", part},
                  {"diff", diff},
                  {"it", i},
                  {"items", n},
                  {"chunk", 1024},
                  {"chunks", chunks}});
      c.dispatch(
          "diff_final", {1, 1, 1},
          {{"part", part}, {"diff", diff}, {"it", i}, {"chunks", chunks}});
    }
    args.items.emplace("dst", out);
    c.dispatch("kc_final", {w, h, 1}, args);
  } else if (o.mode == "lanczos") {
    return lanczos(c, img, h, w);
  } else {
    const std::map<std::string, uint32_t> modes{{"nearest", 0},
                                                {"nearest-exact", 1},
                                                {"bilinear", 2},
                                                {"bicubic", 3},
                                                {"area", 4}};
    return interpolate(c, img, h, w, modes.at(o.mode));
  }
  return out;
}
Array quant_weights(Context& c, const Array& full, uint32_t h, uint32_t w) {
  auto out = c.image(h, w, 1);
  // Upstream derives this shape scalar in Python binary64, then casts once.
  const float gamma = static_cast<float>(
      std::sqrt(static_cast<double>(static_cast<uint64_t>(h) * w)) / 512.0);
  c.dispatch("quant_weights", {w, h, 1},
             {{"w_full", full},
              {"w_out", out},
              {"in_h", full.height},
              {"in_w", full.width},
              {"out_h", h},
              {"out_w", w},
              {"scale_h", static_cast<float>(full.height) / h},
              {"scale_w", static_cast<float>(full.width) / w},
              {"gamma", gamma},
              {"gamma_mode",
               gamma == 0.5f ? 1 : (gamma == 1 ? 2 : (gamma == 2 ? 3 : 0))}});
  return out;
}
Array repeat_table(Context& c, const Array& weights, uint32_t n) {
  float extra = static_cast<float>(3 * n);
  auto lw = c.empty(n);
  c.dispatch("rp_log", {n, 1, 1}, {{"w", weights}, {"lw", lw}, {"total", n}});
  auto mx = minmax(c, lw, 1, n).second;
  auto e = c.empty(n);
  c.dispatch("rp_shift_exp", {n, 1, 1},
             {{"lw", lw}, {"lmax", mx}, {"e", e}, {"count", n}, {"total", n}});
  auto ss = sum(c, e, 1, n), rep = c.empty(n), rem = c.empty(n);
  c.dispatch("rp_counts", {n, 1, 1},
             {{"lw", lw},
              {"lmax", mx},
              {"sumexp", ss},
              {"repeat", rep},
              {"rem", rem},
              {"fl_out", e},
              {"count", n},
              {"total", n},
              {"extra", extra}});
  auto fl = sum(c, e, 1, n), state = c.empty(4);
  c.dispatch("rp_select_init", {1, 1, 1},
             {{"fl_sum", fl},
              {"state", state},
              {"count", n},
              {"batch", 1},
              {"extra", extra}});
  uint32_t chunks = (n + 127) / 128;
  auto hist = c.empty(static_cast<uint64_t>(chunks) * 16);
  for (int shift = 28; shift >= 0; shift -= 4) {
    c.dispatch("rp_hist", {chunks, 1, 1},
               {{"rem", rem},
                {"state", state},
                {"hist", hist},
                {"shift", shift},
                {"count", n},
                {"batch", 1},
                {"chunk", 128},
                {"chunks", chunks}});
    c.dispatch("rp_select_step", {1, 1, 1},
               {{"hist", hist},
                {"state", state},
                {"shift", shift},
                {"batch", 1},
                {"chunks", chunks}});
  }
  auto gt = c.empty(chunks), eq = c.empty(chunks);
  c.dispatch("rp_tie_count", {chunks, 1, 1},
             {{"rem", rem},
              {"state", state},
              {"chunk_gt", gt},
              {"chunk_eq", eq},
              {"count", n},
              {"batch", 1},
              {"chunk", 128},
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
              {"repeat", rep},
              {"count", n},
              {"batch", 1},
              {"chunk", 128},
              {"chunks", chunks}});
  return rep;
}
Array quantize(Context& c, const Array& img, const Array& weights,
               const Options& o) {
  uint32_t n = img.height * img.width, k = o.num_colors,
           chunks = (n + 255) / 256;
  auto mm = minmax(c, img, 3, n);
  auto interp = constant(c, "interp" + std::to_string(k)),
       cent = c.empty(static_cast<uint64_t>(k) * 3);
  c.dispatch("km_init", {k, 1, 1},
             {{"interp", interp},
              {"ch_min", mm.first},
              {"ch_max", mm.second},
              {"cent", cent},
              {"K", k},
              {"batch", 1}});
  bool repeat = o.quant_mode == "repeat-kmeans",
       weighted = o.quant_mode == "weighted-kmeans";
  auto reps = repeat ? repeat_table(c, weights, n) : c.empty(1);
  auto labels = c.empty(n), control = c.empty(32), item = c.empty(k),
       part = c.empty(static_cast<uint64_t>(chunks) * k * 4, 8);
  uint32_t iterations = 2 * static_cast<uint32_t>(std::sqrt(k));
  for (uint32_t it = 0; it < iterations; ++it) {
    c.dispatch("km_assign", {n, 1, 1},
               {{"pix", img},
                {"weight", weights.data ? weights : cent},
                {"cent", cent},
                {"labels", labels},
                {"item_diff", item},
                {"run", control},
                {"it", it},
                {"K", k},
                {"count", n},
                {"batch", 1},
                {"weighted", weighted ? 1 : 0}});
    c.dispatch("km_partial", {chunks * k, 1, 1},
               {{"pix", img},
                {"repeat", reps},
                {"labels", labels},
                {"part", part},
                {"run", control},
                {"it", it},
                {"K", k},
                {"count", n},
                {"batch", 1},
                {"chunk", 256},
                {"chunks", chunks},
                {"use_repeat", repeat ? 1 : 0}});
    c.dispatch("km_update", {k, 1, 1},
               {{"part", part},
                {"cent", cent},
                {"item_diff", item},
                {"run", control},
                {"it", it},
                {"K", k},
                {"batch", 1},
                {"chunks", chunks}});
  }
  auto quant = c.image(img.height, img.width);
  c.dispatch("km_final", {n, 1, 1},
             {{"pix", img},
              {"cent", cent},
              {"labels", labels},
              {"quant", quant},
              {"K", k},
              {"count", n},
              {"batch", 1}});
  if (o.dither_mode == "ordered") {
    auto out = c.image(img.height, img.width), bayer = constant(c, "bayer");
    c.dispatch("dither_ordered", {img.width, img.height, 1},
               {{"img", img},
                {"pal", cent},
                {"bayer", bayer},
                {"dst", out},
                {"K", k},
                {"height", img.height},
                {"width", img.width}});
    quant = out;
  } else if (o.dither_mode == "error_diffusion") {
    auto work = c.image(img.height, img.width),
         err = c.empty(static_cast<uint64_t>(9) * img.width);
    std::memcpy(work.data, img.data, img.count * 4);
    for (uint32_t y = 0; y + 2 < img.height; y += 2) {
      c.dispatch("ed_errors", {img.width, 3, 1},
                 {{"img", work},
                  {"pal", cent},
                  {"err", err},
                  {"y", y},
                  {"K", k},
                  {"height", img.height},
                  {"width", img.width}});
      c.dispatch("ed_apply", {img.width, 2, 1},
                 {{"img", work},
                  {"err", err},
                  {"y", y},
                  {"height", img.height},
                  {"width", img.width}});
    }
    c.dispatch("palette_map", {img.width, img.height, 1},
               {{"img", work},
                {"pal", cent},
                {"dst", quant},
                {"K", k},
                {"height", img.height},
                {"width", img.width}});
  }
  return match(c, quant, img, o);
}
}  // namespace
Outputs run(Context& c, Array img, const Options& o, uint32_t selected) {
  uint32_t p = o.pixel_size, h = (img.height + p - 1) / p * p,
           w = (img.width + p - 1) / p * p;
  if (h != img.height || w != img.width) {
    auto padded = c.image(h, w);
    c.dispatch("pad_replicate", grid(padded),
               {{"src", img},
                {"dst", padded},
                {"in_h", img.height},
                {"in_w", img.width},
                {"out_h", h},
                {"out_w", w},
                {"top", (h - img.height) / 2},
                {"left", (w - img.width) / 2}});
    img = padded;
  }
  Array expanded = img, normalized;
  if (o.thickness || selected == 2 ||
      (o.do_quant && o.quant_mode != "kmeans")) {
    auto ww = weight(c, img, o);
    if (selected == 2 || (o.do_quant && o.quant_mode != "kmeans")) {
      normalized = normalize(c, ww);
    }
    if (selected == 2) {
      return {{}, {}, normalized};
    }
    if (o.thickness) {
      expanded = outline(c, img, ww, o);
    }
  }
  if (o.sharpen_mode != "none") {
    auto next = c.image(h, w), taps = constant(c, o.sharpen_mode);
    c.dispatch("sharpen", grid(img),
               {{"src", expanded},
                {"dst", next},
                {"taps", taps},
                {"height", h},
                {"width", w},
                {"mode", o.sharpen_mode == "unsharp" ? 0 : 1},
                {"amount", o.sharpen_factor},
                {"threshold", 0.1f}});
    expanded = next;
  }
  if (o.do_color_match) {
    expanded = match(c, expanded, img, o);
  }
  img = {};
  if (selected == 1) {
    return {{}, expanded, {}};
  }
  auto down = downscale(c, expanded, o);
  if (o.do_quant) {
    Array weights;
    if (o.quant_mode != "kmeans") {
      weights = quant_weights(c, normalized, h / p, w / p);
    }
    down = quantize(c, down, weights, o);
  }
  if (!o.no_post_upscale) {
    auto out = c.image(h, w);
    c.dispatch("upscale_nearest_exact", grid(out),
               {{"src", down},
                {"dst", out},
                {"in_h", down.height},
                {"in_w", down.width},
                {"out_h", h},
                {"out_w", w},
                {"inv_scale", 1.0f / p}});
    down = out;
  }
  return {down, {}, {}};
}
}  // namespace px
