#include <algorithm>
#include <cstring>
#include <string>

#include "gpu_runtime.hpp"  // NOLINT(build/include_subdir)

namespace px {
namespace {
uint64_t add(uint64_t a, uint64_t b) {
  if (a > UINT64_MAX - b) {
    throw Failure(4, "PixelOE work addition overflow");
  }
  return a + b;
}
uint64_t mul(uint64_t a, uint64_t b) {
  if (b && a > UINT64_MAX / b) {
    throw Failure(4, "PixelOE work product overflow");
  }
  return a * b;
}
uint64_t ceil_div(uint64_t a, uint64_t b) {
  if (!b) {
    throw Failure(6, "PixelOE work divisor is zero");
  }
  return a / b + (a % b != 0);
}
uint64_t radix(uint64_t n) {
  return add(256, mul(64, add(mul(15, n), 256)));
}
uint64_t registers(uint64_t n) {
  return add(256, mul(32, add(mul(33, n), 64)));
}
}  // namespace
uint64_t gpu_work_bound(const GpuKernel& kernel, std::array<uint32_t, 3> grid,
                        const Arguments& args) {
  const std::string name(kernel.name);
  const auto u = [&](const char* key) { return args.get(key).u; };
  const auto square = [&](uint64_t n) { return mul(n, n); };
  const auto span = [&](uint64_t radius) { return add(mul(2, radius), 1); };
  const auto fixed = [&](const char* prefix) -> unsigned {
    const auto length = std::strlen(prefix);
    return name.compare(0, length, prefix) == 0
               ? static_cast<unsigned>(std::stoul(name.substr(length)))
               : 0;
  };
  uint64_t c = 0;
  if (name == "copy_words") {
    c = 8;
  } else if (name == "luminance" || name == "lab_image") {
    c = 768;
  } else if (name == "match_apply") {
    c = 1536;
  } else if (name == "blur2d") {
    c = add(256, mul(64, square(span(u("radius")))));
  } else if (name == "blur_rows" || name == "blur_cols" || name == "lr_rows" ||
             name == "lr_cols") {
    c = add(256, mul(64, span(u("radius"))));
  } else if (const auto r = fixed("blur2d_sym_r")) {
    c = add(256,
            mul(64, add(add(2 * r + 4, mul(r + 1, add(2 * r + 4, 5 * (r + 1)))),
                        4)));
  } else if (name.compare(0, 9, "lr_rows_r") == 0 ||
             name.compare(0, 9, "lr_cols_r") == 0) {
    const auto r = std::stoul(name.substr(9));
    c = add(256, mul(64, add(add(8 + 2 * r, mul(9, span(r))), 8)));
  } else if (name == "lattice_median" || name == "sliding_median") {
    c = add(256, radix(square(u("ksize"))));
  } else if (name == "lattice_minmax") {
    c = add(256, mul(64, square(u("ksize"))));
  } else if (const auto h = fixed("lattice_stats_h")) {
    c = add(add(256, mul(96, square(2 * h))), registers(square(2 * h)));
  } else if (name == "sliding_minmax_rows" || name == "sliding_minmax_cols") {
    c = add(256, mul(64, u("ksize")));
  } else if (name == "weight_raw") {
    const auto fold = [&](const char* size, const char* h, const char* w) {
      const auto count = ceil_div(u(size), u("stride"));
      return mul(std::min(u(h), count), std::min(u(w), count));
    };
    c = add(768,
            mul(64, add(fold("med_ksize", "med_lat_h", "med_lat_w"),
                        mul(2, fold("mm_ksize", "mm_lat_h", "mm_lat_w")))));
  } else if (name == "weight_dense") {
    c = 768;
  } else if (name == "weight_gate" || name == "quant_weights" ||
             name == "rp_counts") {
    c = 512;
  } else if (name == "normalize_map" || name == "km_init" || name == "rp_log" ||
             name == "rp_shift_exp" || name == "rp_select_init" ||
             name == "pad_replicate" || name == "upscale_nearest_exact") {
    c = 256;
  } else if (name == "morph") {
    c = add(256, mul(64, square(u("ks"))));
  } else if (name == "oe_blend") {
    c = add(512, mul(64, add(square(u("ks_erode")), square(u("ks_dilate")))));
  } else if (name == "contrast_downscale") {
    c = add(add(1024, mul(3, radix(square(u("p"))))), mul(64, square(u("p"))));
  } else if (const auto p = fixed("contrast_rgb_p")) {
    c = add(
        add(1024, mul(768, square(p))),
        add(mul(3, registers(square(p))), mul(64, u("up") ? square(p) : 1)));
  } else if (name == "kc_init") {
    c = add(256, mul(64, square(u("p"))));
  } else if (name == "kc_iter" || name == "kc_iter_max") {
    c = add(1024, mul(256, square(u("p"))));
  } else if (name == "kc_final") {
    c = add(512, mul(128, square(u("p"))));
  } else if (name == "seg_minmax_level" || name == "seg_sum_level" ||
             name == "diff_partial") {
    c = add(256, mul(32, u("chunk")));
  } else if (name == "lab_moments_partial") {
    c = add(256, mul(768, u("chunk")));
  } else if (name == "moments_merge") {
    c = add(256, mul(128, u("chunk")));
  } else if (name == "diff_final") {
    c = add(256, mul(32, u("chunks")));
  } else if (name == "km_assign") {
    c = add(256, add(mul(32, mul(u("batch"), u("K"))), mul(128, u("K"))));
  } else if (name == "km_partial") {
    c = add(512, mul(128, u("chunk")));
  } else if (name == "km_update") {
    c = add(512, mul(128, u("chunks")));
  } else if (name == "km_final" || name == "ed_errors" ||
             name == "palette_map") {
    c = add(256, mul(128, u("K")));
  } else if (name == "dither_ordered") {
    c = add(512, mul(128, u("K")));
  } else if (name == "rp_hist") {
    c = add(256, mul(64, add(u("chunk"), 32)));
  } else if (name == "rp_select_step") {
    c = add(256, mul(1024, add(u("chunks"), 1)));
  } else if (name == "rp_tie_count" || name == "rp_mark") {
    c = add(256, mul(64, u("chunk")));
  } else if (name == "rp_tie_scan") {
    c = add(256, mul(64, u("chunks")));
  } else if (name == "ed_apply") {
    c = 256 + 64 * 18;
  } else if (name == "sharpen") {
    c = 512 + 64 * 9;
  } else if (name == "csr_pass") {
    c = 256 + 64 * 7;  // Host Lanczos rows have <=7 entries.
  } else if (name == "interpolate") {
    switch (u("mode")) {
      case 0:
      case 1:
        c = 256;
        break;
      case 2:
        c = 512;
        break;
      case 3:
        c = 2048;
        break;
      case 4:
        c = add(
            256,
            mul(64, mul(std::min(u("in_h"),
                                 add(ceil_div(u("in_h"), u("out_h")), 1)),
                        std::min(u("in_w"),
                                 add(ceil_div(u("in_w"), u("out_w")), 1)))));
        break;
      default:
        throw Failure(6, "unknown interpolation work profile");
    }
  } else {
    throw Failure(6, "unknown native work profile: " + name);
  }
  uint64_t threads = 1;
  for (unsigned axis = 0; axis < 3; ++axis) {
    threads = mul(threads, mul(ceil_div(grid[axis], kernel.group[axis]),
                               kernel.group[axis]));
  }
  // Model version 1: scalar source operations; transcendental calls cost 64.
  // Driver compilation and hardware instruction expansion are measured
  // separately.
  const auto a = uint64_t{args.items.size()},
             r = uint64_t{kernel.parameter_count};
  const auto l = uint64_t{64};  // Frozen entry and parameter names are shorter.
  const auto host = add(
      4096, add(mul(4, kernel.source_size),
                mul(64, add(add(mul(gpu_kernels().size(), l + 1),
                                mul(mul(a, r), l + 1)),
                            add(square(a),
                                add(31, ceil_div(kernel.constant_size, 4)))))));
  return add(host, mul(threads, c));
}
}  // namespace px
