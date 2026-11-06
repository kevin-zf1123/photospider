// Copyright 2026 Photospider Authors
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "09-composite/keying_common.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::keying {
template <Kind K>
struct Spatial final : Program {
  ElementType type;
  Color color;
  std::vector<std::uint64_t> dims;
  std::uint64_t h = 0, w = 0, radius = 0;
  Spatial(const std::vector<OperationMetadata>& in, const Parameters& p) {
    const auto& first = tensor(in[0]);
    type = first.descriptor.element_type;
    dims = first.sample_shape();
    if constexpr (K == Kind::Nearest || K == Kind::Extend) {
      require(dims.size() == 3 && floating(type),
              "KEY spatial image rank/dtype");
      color = Color(in[0], p, false, K == Kind::Extend, true);
      auto plane = dims;
      plane.erase(plane.begin() + color.axis);
      h = plane[0];
      w = plane[1];
      if constexpr (K == Kind::Nearest) {
        require(color.count == 3, "KEY plate requires exactly RGB");
        check_shape(in[1], plane, ElementType::UInt8);
      } else {
        check_shape(in[1], {1}, type);
      }
    } else {
      require(dims.size() == 2 && (K == Kind::Fill ? type == ElementType::UInt8
                                                   : floating(type)),
              "KEY matte rank/dtype");
      h = dims[0];
      w = dims[1];
      if constexpr (K == Kind::Guided) {
        check_shape(in[1], dims, type);
        check_shape(in[2], {1}, type);
      }
    }
    if constexpr (K == Kind::Erode || K == Kind::Dilate || K == Kind::Guided ||
                  K == Kind::Extend) {
      radius = static_cast<std::uint64_t>(integer(p, "radius"));
    }
    collective = true;
    add_output(this, output_schema(in[0]));
    if constexpr (K == Kind::Nearest) {
      add_output(this, plane_schema(type, h, w));
    }
    const unsigned __int128 n = static_cast<unsigned __int128>(h) * w;
    const unsigned __int128 elements = size_product(dims);
    if constexpr (K == Kind::Erode || K == Kind::Dilate) {
      scratch_bytes = mask::bytes(3 * n * generation::width(type) +
                                  std::max(h, w) * sizeof(std::uint64_t) + 128);
    }
    if constexpr (K == Kind::Guided) {
      scratch_bytes = mask::bytes(3 * n * generation::width(type) +
                                  (8 * n + 4 * w) * sizeof(double) + 128);
    }
    if constexpr (K == Kind::Fill) {
      scratch_bytes = mask::bytes(n + 2 * n * sizeof(std::uint64_t) + 128);
    }
    if constexpr (K == Kind::Nearest || K == Kind::Extend) {
      scratch_bytes = mask::bytes(2 * elements * generation::width(type) +
                                  n * (sizeof(std::int64_t) + 1) +
                                  3 * w * sizeof(std::int64_t) + h + 128);
    }
  }
  std::vector<ReadRegion> support(unsigned output,
                                  const Region&) const override {
    if constexpr (K == Kind::Nearest) {
      if (output == 1) {
        return {{1, 0, Region::whole({h, w})}};
      }
      return {{0, 0, Region::whole(dims)}, {1, 0, Region::whole({h, w})}};
    } else {
      std::vector<ReadRegion> result{{0, 0, Region::whole(dims)}};
      if constexpr (K == Kind::Guided) {
        if (radius) {
          result.push_back({1, 0, Region::whole(dims)});
          result.push_back({2, 0, Region::whole({1})});
        }
      }
      if constexpr (K == Kind::Extend) {
        if (radius) {
          result.push_back({1, 0, Region::whole({1})});
        }
      }
      return result;
    }
  }
  std::vector<ReadRegion> collective_support(
      const std::vector<unsigned>& ports) const override {
    if constexpr (K == Kind::Nearest) {
      return support(
          std::find(ports.begin(), ports.end(), 0) == ports.end() ? 1 : 0, {});
    }
    return support(0, {});
  }
  std::uint64_t index(std::uint64_t y, std::uint64_t x, std::uint64_t c) const {
    const auto at = insert_axis(Coordinate{y, x}, color.axis, c);
    return (at[0] * dims[1] + at[1]) * dims[2] + at[2];
  }
  template <class T>
  static void copy_rgb_alpha(T* destination, const T* donor, const T* target) {
#if defined(__aarch64__)
    if constexpr (sizeof(T) == 4) {
      const auto value = vld1q_lane_f32(target + 3, vld1q_f32(donor), 3);
      vst1q_f32(destination, value);
    } else {
      const auto rg = vld1q_f64(donor);
      const auto ba = vld1q_lane_f64(target + 3, vld1q_f64(donor + 2), 1);
      vst1q_f64(destination, rg);
      vst1q_f64(destination + 2, ba);
    }
#else
    std::memcpy(destination, donor, 3 * sizeof(T));
    std::memcpy(destination + 3, target + 3, sizeof(T));
#endif
  }
  template <class T>
  static void packed_alpha_flags(const T* input, std::uint8_t* flags,
                                 std::uint64_t count, T threshold) {
    std::uint64_t i = 0;
#if defined(__aarch64__)
    if constexpr (sizeof(T) == 4) {
      const auto limit = vdupq_n_f32(threshold);
      for (; i + 8 <= count; i += 8) {
        const auto a = vcgeq_f32(vld4q_f32(input + 4 * i).val[3], limit);
        const auto b = vcgeq_f32(vld4q_f32(input + 4 * (i + 4)).val[3], limit);
        const auto lanes = vcombine_u16(vmovn_u32(a), vmovn_u32(b));
        vst1_u8(flags + i, vmovn_u16(vshrq_n_u16(lanes, 15)));
      }
    } else {
      const auto limit = vdupq_n_f64(threshold);
      for (; i + 8 <= count; i += 8) {
        const auto a =
            vmovn_u64(vcgeq_f64(vld4q_f64(input + 4 * i).val[3], limit));
        const auto b =
            vmovn_u64(vcgeq_f64(vld4q_f64(input + 4 * (i + 2)).val[3], limit));
        const auto c =
            vmovn_u64(vcgeq_f64(vld4q_f64(input + 4 * (i + 4)).val[3], limit));
        const auto d =
            vmovn_u64(vcgeq_f64(vld4q_f64(input + 4 * (i + 6)).val[3], limit));
        const auto lanes = vcombine_u16(vmovn_u32(vcombine_u32(a, b)),
                                        vmovn_u32(vcombine_u32(c, d)));
        vst1_u8(flags + i, vmovn_u16(vshrq_n_u16(lanes, 15)));
      }
    }
#endif
    for (; i < count; ++i) {
      flags[i] = input[4 * i + 3] >= threshold;
    }
  }
  // If each nonempty row has the same donor columns, the donor set is
  // exactly rows x columns. Both one-dimensional argmins then separate.
  template <class T>
  bool extend_product_band(const T* source, T* output,
                           const std::uint8_t* flags, std::int32_t* scratch,
                           std::uint64_t origin, std::uint64_t limit,
                           std::uint64_t first, std::uint64_t stop,
                           const Work& work) const {
    auto* near_x = scratch;
    auto* near_y = scratch + w;
    const std::uint8_t* columns = nullptr;
    for (auto y = origin; y < limit; ++y) {
      const auto* row = flags + y * w;
      bool any = false;
      for (std::uint64_t x = 0; x < w && !any; x += 4096) {
        const auto count = std::min<std::uint64_t>(4096, w - x);
        work.poll(count);
        any = std::memchr(row + x, 1, count) != nullptr;
      }
      near_y[y - origin] = any ? static_cast<std::int32_t>(y - origin) : -1;
      if (!any)
        continue;
      if (!columns) {
        columns = row;
        continue;
      }
      for (std::uint64_t x = 0; x < w; x += 4096) {
        const auto count = std::min<std::uint64_t>(4096, w - x);
        work.poll(count);
        if (std::memcmp(columns + x, row + x, count))
          return false;
      }
    }
    // Uniform all-zero and all-one bands are handled before this call.
    if (!columns)
      return false;
    for (std::uint64_t x = 0; x < w; ++x) {
      if ((x & 4095) == 0)
        work.poll(std::min<std::uint64_t>(4096, w - x));
      near_x[x] = columns[x] ? static_cast<std::int32_t>(x) : -1;
    }
    const auto fill_nearest = [&](std::int32_t* axis, std::uint64_t count) {
      std::int32_t left = -1, right = -1;
      for (std::uint64_t i = 0; i < count; ++i) {
        if ((i & 4095) == 0)
          work.poll(std::min<std::uint64_t>(4096, count - i));
        if (axis[i] >= 0)
          left = axis[i];
        else
          axis[i] = left;
      }
      for (auto i = count; i-- > 0;) {
        if ((i & 4095) == 4095 || i == count - 1)
          work.poll(std::min<std::uint64_t>((i & 4095) + 1, i + 1));
        const auto x = static_cast<std::int32_t>(i);
        if (axis[i] == x)
          right = x;
        else if (right >= 0 && (axis[i] < 0 || right - x < x - axis[i]))
          axis[i] = right;
      }
    };
    fill_nearest(near_x, w);
    fill_nearest(near_y, limit - origin);
    for (auto y = first; y < stop; ++y) {
      const auto row = origin + static_cast<std::uint64_t>(near_y[y - origin]);
      const auto dy = row > y ? row - y : y - row;
      const auto* target = source + 4 * y * w;
      auto* dest = output + 4 * y * w;
      if (dy > radius) {
        work.poll(4 * w);
        copy_elements(dest, target, 4 * w,
                      [&](auto amount) { work.poll(amount); });
        continue;
      }
      const auto available = radius * radius - dy * dy;
      for (std::uint64_t block = 0; block < w; block += 4096) {
        const auto end = std::min(w, block + 4096);
        work.poll((end - block) * 12);
        for (auto x = block; x < end; ++x) {
          const auto q = static_cast<std::uint64_t>(near_x[x]);
          const auto dx = q > x ? q - x : x - q;
          const auto i = dx * dx <= available ? row * w + q : y * w + x;
          copy_rgb_alpha(dest + 4 * x, source + 4 * i, target + 4 * x);
        }
      }
    }
    return true;
  }
  struct DiskOffset {
    int dy, dx;
    unsigned squared;
  };
  // Visit the rest of the radius-eight disk in exact (distance, y, x) order.
  static constexpr auto outer_disk = [] {
    std::array<DiskOffset, 17 * 17> result{};
    std::size_t count = 0;
    for (unsigned d = 9; d <= 64; ++d)
      for (int y = -8; y <= 8; ++y)
        for (int x = -8; x <= 8; ++x)
          if (static_cast<unsigned>(y * y + x * x) == d)
            result[count++] = {y, x, d};
    result[count] = {0, 0, 65};
    return result;
  }();
  // Every donor in a 5x5 square is closer than every site outside it
  // (maximum squared distance 8, minimum outside 9). Encode the exact
  // (distance, dy, dx) order in a byte. Empty squares use the full disk.
  template <class T>
  bool extend_dense_band(const T* source, T* output, const std::uint8_t* flags,
                         std::uint8_t* scratch, std::uint64_t first,
                         std::uint64_t stop, const Work& work) const {
    constexpr std::uint8_t missing = 255;
    const auto top = first > 2 ? first - 2 : 0;
    const auto bottom = std::min(h, stop + 2);
    auto* scores = scratch + (bottom - top) * w;
    for (auto y = top; y < bottom; ++y) {
      const auto* row = flags + y * w;
      auto* horizontal = scratch + (y - top) * w;
      const auto border = [&](std::uint64_t x) {
        auto score = missing;
        for (auto q = x > 2 ? x - 2 : 0; q < std::min(w, x + 3); ++q) {
          const int dx = static_cast<int>(q - x + 2) - 2;
          if (row[q]) {
            score = std::min(score,
                             static_cast<std::uint8_t>(25 * dx * dx + dx + 2));
          }
        }
        horizontal[x] = score;
      };
      work.poll(5 * w);
      border(0);
      border(1);
      for (std::uint64_t block = 2; block < w - 2; block += 4096) {
        work.poll(0);
        const auto end = std::min(w - 2, block + 4096);
        auto x = block;
#if defined(__aarch64__)
        for (; x + 16 <= end; x += 16) {
          auto value = vdupq_n_u8(missing);
          const auto consider = [&](int dx, std::uint8_t rank) {
            value = vbslq_u8(vceqzq_u8(vld1q_u8(row + x + dx)), value,
                             vdupq_n_u8(rank));
          };
          consider(2, 104);
          consider(-2, 100);
          consider(1, 28);
          consider(-1, 26);
          consider(0, 2);
          vst1q_u8(horizontal + x, value);
        }
#endif
        for (; x < end; ++x) {
          horizontal[x] = row[x]       ? 2
                          : row[x - 1] ? 26
                          : row[x + 1] ? 28
                          : row[x - 2] ? 100
                          : row[x + 2] ? 104
                                       : missing;
        }
      }
      border(w - 2);
      border(w - 1);
    }
    // The table maps an encoded winner to its offset within the source image.
    std::int64_t offsets[256]{};
    for (int dy = -2; dy <= 2; ++dy) {
      for (int dx = -2; dx <= 2; ++dx) {
        if (static_cast<unsigned>(dy * dy + dx * dx) > radius * radius)
          continue;
        offsets[25 * (dy * dy + dx * dx) + (dy + 2) * 5 + dx + 2] =
            dy * static_cast<std::int64_t>(w) + dx;
      }
    }
    std::uint64_t misses = 0;
    for (auto y = first; y < stop; ++y) {
      work.poll(5 * w);
      const auto lo = y > 2 ? y - 2 : 0, hi = std::min(h, y + 3);
      for (std::uint64_t block = 0; block < w; block += 4096) {
        work.poll(0);
        const auto end = std::min(w, block + 4096);
        std::fill(scores + block, scores + end, missing);
        for (auto q = lo; q < hi; ++q) {
          const int dy = static_cast<int>(q - y + 2) - 2;
          const int bias = 25 * dy * dy + (dy + 2) * 5;
          const auto* row = scratch + (q - top) * w;
          for (auto x = block; x < end; ++x) {
            scores[x] = static_cast<std::uint8_t>(
                std::min<int>(scores[x], row[x] + bias));
          }
        }
        work.poll(12 * (end - block));
        for (auto x = block; x < end; ++x) {
          const auto i = y * w + x, donor = i + offsets[scores[x]];
          copy_rgb_alpha(output + 4 * i, source + 4 * donor, source + 4 * i);
        }
        if (radius >= 3) {
          const auto* next = scores + block;
          while ((next = static_cast<const std::uint8_t*>(
                      std::memchr(next, missing, scores + end - next)))) {
            const auto x = static_cast<std::uint64_t>(next++ - scores);
            if (++misses > (stop - first) * w / 64)
              return false;
            work.poll((2 * radius + 1) * (2 * radius + 1));
            for (const auto offset : outer_disk) {
              if (offset.squared > radius * radius)
                break;
              const auto yy = y + offset.dy, xx = x + offset.dx;
              if (yy < h && xx < w && flags[yy * w + xx]) {
                const auto i = y * w + x;
                copy_rgb_alpha(output + 4 * i, source + 4 * (yy * w + xx),
                               source + 4 * i);
                break;
              }
            }
          }
        }
      }
    }
    return true;
  }
  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    if constexpr (K == Kind::Extend) {
      bool active = false;
      for (unsigned o = 0; o < inv.output_count; ++o) {
        const auto& layout = *inv.outputs[o].layout;
        for (auto c : color.channels) {
          active |= c >= layout.origin[color.axis] &&
                    c - layout.origin[color.axis] < layout.shape[color.axis];
        }
      }
      if (!active) {
        for (unsigned o = 0; o < inv.output_count; ++o) {
          const auto& out = inv.outputs[o];
          std::uint64_t count = 1;
          for (unsigned a = 0; a < out.layout->rank; ++a) {
            count *= out.layout->shape[a];
          }
          work.poll(count);
          samples(out, [&](const Coordinate& at, std::uint64_t i) {
            if ((i & 4095) == 0) {
              cancel(inv);
            }
            store(out, at, reader.raw(0, 0, at));
          });
        }
        return;
      }
    }
    Arena arena(inv);
    const auto n = h * w;
    const auto total = size_product(dims);
    const auto poll = [&](std::uint64_t amount) { work.poll(amount); };
    if constexpr (K == Kind::Fill) {
      auto* state = arena.take<std::uint8_t>(n);
      auto* queue = arena.take<std::uint64_t>(2 * n);
      const auto* source = gather<std::uint8_t>(reader, 0, dims, state, work);
      if (source != state) {
        work.poll(n);
        copy_elements(state, source, n, poll);
      }
      work.poll(n * 64);
      std::uint64_t pending = 0;
      fill_holes(state, queue, h, w, [&](std::uint64_t amount) {
        pending += amount;
        if (!amount || pending >= 4096) {
          cancel(inv);
          pending = 0;
        }
      });
      for (unsigned o = 0; o < inv.output_count; ++o) {
        scatter(state, dims, inv.outputs[o], work);
      }
    } else if constexpr (K == Kind::Nearest || K == Kind::Extend) {
      auto* image = arena.take<T>(total);
      auto* output = arena.take<T>(total);
      for (unsigned o = 0; o < inv.output_count; ++o) {
        if (inv.outputs[o].port == 0) {
          if (auto* direct = direct_output<T>(inv.outputs[o], dims)) {
            output = direct;
          }
        }
      }
      auto* sites = arena.take<std::int64_t>(n);
      auto* envelope = arena.take<std::int64_t>(w);
      auto* starts = arena.take<std::int64_t>(2 * w);
      auto* known = arena.take<std::uint8_t>(n);
      auto* certificates = arena.take<std::uint8_t>(h);
      fill_elements(certificates, h, std::uint8_t(0), poll);
      bool plate = false;
      for (unsigned o = 0; o < inv.output_count; ++o) {
        plate |= inv.outputs[o].port == 0;
      }
      const T* source = nullptr;
      const std::uint8_t* flags = known;
      // Small radii need only a halo around each band. Keep its distance
      // transform and source pixels hot through RGB publication.
      constexpr std::uint64_t kBandRows = 64;
      const bool banded = K == Kind::Extend && color.packed &&
                          color.count == 4 && radius > 0 && radius <= 8 &&
                          h > kBandRows && w >= 64 && n <= INT32_MAX;
      if constexpr (K == Kind::Nearest) {
        flags = gather<std::uint8_t>(reader, 1, {h, w}, known, work);
      } else {
        source = gather<T>(reader, 0, dims, image, work);
        if (!radius) {
          for (unsigned o = 0; o < inv.output_count; ++o) {
            scatter(source, dims, inv.outputs[o], work);
          }
          return;
        }
        const T threshold = load<T>(reader, 1, {});
        if (color.packed && !banded) {
          const T* const rgba_source = source;
          const auto count = n, channels = color.count, alpha = color.alpha;
          for (std::uint64_t block = 0; block < count; block += 4096) {
            const auto end = std::min(count, block + 4096);
            work.poll(end - block);
            if (channels == 4 && alpha == 3) {
              packed_alpha_flags(rgba_source + 4 * block, known + block,
                                 end - block, threshold);
            } else {
              for (auto i = block; i < end; ++i) {
                known[i] = rgba_source[channels * i + alpha] >= threshold;
              }
            }
          }
        } else if (!color.packed) {
          for (std::uint64_t y = 0; y < h; ++y) {
            work.poll(w);
            for (std::uint64_t block = 0; block < w; block += 4096) {
              cancel(inv);
              for (auto x = block; x < std::min(w, block + 4096); ++x) {
                known[y * w + x] =
                    source[index(y, x, color.alpha)] >= threshold;
              }
            }
          }
        }
      }
      if (plate) {
        const auto publish = [&](const auto* donor_sites, bool found,
                                 std::uint64_t origin, std::uint64_t first,
                                 std::uint64_t stop) {
          if constexpr (K == Kind::Nearest) {
            if (!found) {
              fail("NoSolution: KEY nearest plate has no donor");
            }
            source = gather<T>(reader, 0, dims, image, work);
          }
          bool packed_done = false;
          if constexpr (K == Kind::Nearest) {
            if (color.packed) {
              for (std::uint64_t block = 0; block < n; block += 4096) {
                work.poll(std::min<std::uint64_t>(4096, n - block) * 4);
                auto i = block;
                const auto end = std::min(n, block + 4096);
#if defined(__aarch64__)
                if constexpr (sizeof(T) == 8) {
                  for (; i + 2 <= end; i += 2) {
                    const auto d0 = static_cast<std::uint64_t>(donor_sites[i]);
                    const auto d1 =
                        static_cast<std::uint64_t>(donor_sites[i + 1]);
                    const auto rg = vld1q_f64(source + 3 * d0);
                    const auto br = vld1q_lane_f64(
                        source + 3 * d1,
                        vld1q_lane_f64(source + 3 * d0 + 2, vdupq_n_f64(0), 0),
                        1);
                    const auto gb = vld1q_f64(source + 3 * d1 + 1);
                    vst1q_f64(output + 3 * i, rg);
                    vst1q_f64(output + 3 * i + 2, br);
                    vst1q_f64(output + 3 * i + 4, gb);
                  }
                }
#endif
                for (; i < end; ++i) {
                  const auto donor = static_cast<std::uint64_t>(donor_sites[i]);
                  std::memcpy(output + 3 * i, source + 3 * donor,
                              3 * sizeof(T));
                }
              }
              packed_done = true;
            }
          }
          if constexpr (K == Kind::Extend) {
            if (color.packed && color.count == 4) {
              const auto width = w, count = n, r = radius;
              const T* const rgba_source = source + 4 * origin * width;
              T* const rgba_output = output + 4 * origin * width;
              const double reciprocal = 1.0 / static_cast<double>(width);
              for (auto y = first - origin; y < stop - origin; ++y) {
                const auto base = y * width;
                const auto* sites_row = donor_sites + base;
                const auto* target_row = rgba_source + 4 * base;
                auto* output_row = rgba_output + 4 * base;
                for (std::uint64_t block = 0; block < width; block += 4096) {
                  const auto end = std::min(width, block + 4096);
                  work.poll((end - block) * 12);
                  if (found && certificates[y + origin] &&
                      r >= certificates[y + origin]) {
                    auto x = block;
#if defined(__aarch64__)
                    if constexpr (sizeof(T) == 8) {
                      for (; x + 4 <= end; x += 4) {
                        const auto d0 =
                            static_cast<std::uint64_t>(sites_row[x]);
                        const auto d1 =
                            static_cast<std::uint64_t>(sites_row[x + 1]);
                        const auto d2 =
                            static_cast<std::uint64_t>(sites_row[x + 2]);
                        const auto d3 =
                            static_cast<std::uint64_t>(sites_row[x + 3]);
                        const auto rg0 = vld1q_f64(rgba_source + 4 * d0);
                        const auto rg1 = vld1q_f64(rgba_source + 4 * d1);
                        const auto rg2 = vld1q_f64(rgba_source + 4 * d2);
                        const auto rg3 = vld1q_f64(rgba_source + 4 * d3);
                        const auto ba0 = vld1q_lane_f64(
                            target_row + 4 * x + 3,
                            vld1q_f64(rgba_source + 4 * d0 + 2), 1);
                        const auto ba1 = vld1q_lane_f64(
                            target_row + 4 * x + 7,
                            vld1q_f64(rgba_source + 4 * d1 + 2), 1);
                        const auto ba2 = vld1q_lane_f64(
                            target_row + 4 * x + 11,
                            vld1q_f64(rgba_source + 4 * d2 + 2), 1);
                        const auto ba3 = vld1q_lane_f64(
                            target_row + 4 * x + 15,
                            vld1q_f64(rgba_source + 4 * d3 + 2), 1);
                        vst1q_f64(output_row + 4 * x, rg0);
                        vst1q_f64(output_row + 4 * x + 2, ba0);
                        vst1q_f64(output_row + 4 * x + 4, rg1);
                        vst1q_f64(output_row + 4 * x + 6, ba1);
                        vst1q_f64(output_row + 4 * x + 8, rg2);
                        vst1q_f64(output_row + 4 * x + 10, ba2);
                        vst1q_f64(output_row + 4 * x + 12, rg3);
                        vst1q_f64(output_row + 4 * x + 14, ba3);
                      }
                    }
#endif
                    for (; x < end; ++x) {
                      const auto donor =
                          static_cast<std::uint64_t>(sites_row[x]);
                      copy_rgb_alpha(output_row + 4 * x,
                                     rgba_source + 4 * donor,
                                     target_row + 4 * x);
                    }
                    continue;
                  }
                  for (auto x = block; x < end; ++x) {
                    const auto i = base + x;
                    auto chosen = i;
                    if (found && !flags[origin * width + i]) {
                      const auto donor =
                          static_cast<std::uint64_t>(sites_row[x]);
                      std::uint64_t dy;
                      if (count <= (UINT64_C(1) << 52)) {
                        // The quotient estimate is within one integer here;
                        // exact products/remainders repair the rounding
                        // boundary.
                        dy = static_cast<std::uint64_t>(
                            static_cast<std::int64_t>(
                                static_cast<double>(donor) * reciprocal));
                        if (dy * width > donor) {
                          --dy;
                        } else if (donor - dy * width >= width) {
                          ++dy;
                        }
                      } else {
                        dy = donor / width;
                      }
                      const auto dx = donor - dy * width;
                      const auto yy = y > dy ? y - dy : dy - y;
                      const auto xx = x > dx ? x - dx : dx - x;
                      if (yy <= r && xx <= r && yy * yy + xx * xx <= r * r) {
                        chosen = donor;
                      }
                    }
                    copy_rgb_alpha(output_row + 4 * x, rgba_source + 4 * chosen,
                                   target_row + 4 * x);
                  }
                }
              }
              packed_done = true;
            } else {
              work.poll(total);
              copy_elements(output, source, total, poll);
            }
          }
          if (!packed_done) {
            for (std::uint64_t y = 0; y < h; ++y) {
              work.poll(w * 8);
              for (std::uint64_t x = 0; x < w; ++x) {
                if ((x & 4095) == 0) {
                  cancel(inv);
                }
                const auto donor =
                    found ? static_cast<std::int64_t>(donor_sites[y * w + x])
                          : std::int64_t{-1};
                if (donor < 0) {
                  continue;
                }
                if constexpr (K == Kind::Nearest) {
                  if (color.packed) {
                    std::memcpy(output + 3 * (y * w + x), source + 3 * donor,
                                3 * sizeof(T));
                    continue;
                  }
                }
                const auto dy = static_cast<std::uint64_t>(donor) / w;
                const auto dx = static_cast<std::uint64_t>(donor) % w;
                if constexpr (K == Kind::Extend) {
                  const __int128 yy = __int128(y) - dy, xx = __int128(x) - dx;
                  if (flags[y * w + x] ||
                      yy * yy + xx * xx > __int128(radius) * radius) {
                    continue;
                  }
                }
                if (color.packed) {
                  std::memcpy(output + color.count * (y * w + x),
                              source + color.count * donor, 3 * sizeof(T));
                } else {
                  for (auto c : color.channels) {
                    std::memcpy(output + index(y, x, c),
                                source + index(dy, dx, c), sizeof(T));
                  }
                }
              }
            }
          }
        };
        if (banded) {
          const T threshold = load<T>(reader, 1, {});
          auto* compact = reinterpret_cast<std::int32_t*>(sites);
          std::uint64_t loaded = 0;
          for (std::uint64_t first = 0; first < h; first += kBandRows) {
            const auto stop = std::min(h, first + kBandRows);
            const auto origin = first > radius ? first - radius : 0;
            const auto limit = std::min(h, stop + radius);
            while (loaded < limit * w) {
              const auto count =
                  std::min<std::uint64_t>(4096, limit * w - loaded);
              work.poll(count);
              packed_alpha_flags(source + 4 * loaded, known + loaded, count,
                                 threshold);
              loaded += count;
            }
            const auto* local = flags + origin * w;
            const auto size = (limit - origin) * w;
            // Uniform bands (including both halos) are exact identity copies.
            bool uniform = true;
            for (std::uint64_t i = 0; i < size && uniform; i += 4096) {
              const auto count = std::min<std::uint64_t>(4096, size - i);
              work.poll(count);
              uniform = std::memchr(local + i, 1 - local[0], count) == nullptr;
            }
            if (uniform) {
              const auto begin = first * w * 4, count = (stop - first) * w * 4;
              work.poll(count);
              copy_elements(output + begin, source + begin, count, poll);
              continue;
            }
            if (extend_product_band(source, output, flags, compact, origin,
                                    limit, first, stop, work)) {
              continue;
            }
            if (radius >= 2 &&
                extend_dense_band(source, output, flags,
                                  reinterpret_cast<std::uint8_t*>(sites), first,
                                  stop, work)) {
              continue;
            }
            fill_elements(certificates + origin, limit - origin,
                          std::uint8_t(0), poll);
            nearest_typed<std::int64_t>(
                local, compact, reinterpret_cast<std::int32_t*>(envelope),
                reinterpret_cast<std::int32_t*>(starts), limit - origin, w,
                poll, certificates + origin);
            publish(compact, true, origin, first, stop);
          }
        } else if (n <= INT32_MAX) {
          auto* compact = reinterpret_cast<std::int32_t*>(sites);
          const bool found = nearest_typed<std::int64_t>(
              flags, compact, reinterpret_cast<std::int32_t*>(envelope),
              reinterpret_cast<std::int32_t*>(starts), h, w, poll,
              certificates);
          publish(compact, found, 0, 0, h);
        } else {
          const bool found = nearest_typed<__int128>(
              flags, sites, envelope, starts, h, w, poll, certificates);
          publish(sites, found, 0, 0, h);
        }
      }
      for (unsigned o = 0; o < inv.output_count; ++o) {
        if (inv.outputs[o].port == 0) {
          if (direct_output<T>(inv.outputs[o], dims) != output) {
            scatter(output, dims, inv.outputs[o], work);
          }
        } else {
          if (auto* target = direct_output<T>(inv.outputs[o], {h, w})) {
            for (std::uint64_t block = 0; block < n; block += 4096) {
              work.poll(std::min<std::uint64_t>(4096, n - block));
              for (auto i = block; i < std::min(n, block + 4096); ++i) {
                target[i] = T(flags[i]);
              }
            }
            continue;
          }
          samples(inv.outputs[o], [&](const Coordinate& at, std::uint64_t i) {
            if ((i & 4095) == 0) {
              work.poll(std::min<std::uint64_t>(4096, n - i));
            }
            save(inv.outputs[o], at, T(flags[at[0] * w + at[1]]));
          });
        }
      }
    } else {
      auto* input = arena.take<T>(n);
      auto* output = arena.take<T>(n);
      for (unsigned o = 0; o < inv.output_count; ++o) {
        if (auto* direct = direct_output<T>(inv.outputs[o], dims)) {
          output = direct;
        }
      }
      const auto* source = gather<T>(reader, 0, dims, input, work);
      if constexpr (K == Kind::Guided) {
        if (!radius) {
          for (unsigned o = 0; o < inv.output_count; ++o) {
            scatter(source, dims, inv.outputs[o], work);
          }
          return;
        }
        auto* guide_buffer = arena.take<T>(n);
        const auto* guide = gather<T>(reader, 1, dims, guide_buffer, work);
        auto* scratch = arena.take<double>(8 * n + 4 * w);
        guided(source, guide, output, scratch, h, w, radius,
               static_cast<double>(load<T>(reader, 2, {})), poll);
      } else {
        auto* temporary = arena.take<T>(n);
        auto* queue = arena.take<std::uint64_t>(std::max(h, w));
        morphology<K == Kind::Dilate>(source, output, temporary, queue, h, w,
                                      radius, poll);
      }
      for (unsigned o = 0; o < inv.output_count; ++o) {
        if (direct_output<T>(inv.outputs[o], dims) != output) {
          scatter(output, dims, inv.outputs[o], work);
        }
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    dispatch(k, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    dispatch(k, inv.views, Work{inv.views, work_limit, 0, inv.work});
  }
  void dispatch(const Kernel& k, const ps_kernel_invocation_v8& inv,
                const Work& work) const {
    if (type == ElementType::Float32) {
      compute<float>(k, inv, work);
    } else {
      compute<double>(k, inv, work);
    }
  }
};

struct Median final : Program {
  ElementType type;
  Color color;
  std::vector<std::uint64_t> dims, plane;
  std::uint64_t frames;
  struct Sample {
    double value;
    std::uint64_t frame, raw;
  };
  Median(const std::vector<OperationMetadata>& in, const Parameters& p) {
    const auto& a = tensor(in[0]);
    type = a.descriptor.element_type;
    dims = a.sample_shape();
    require(dims.size() == 4 && floating(type),
            "KEY temporal stack rank/dtype");
    color = Color(in[0], p, false, false, true);
    require(color.count == 3 && color.axis != 0,
            "KEY temporal stack requires leading frame axis and RGB");
    frames = dims[0];
    plane = dims;
    plane.erase(plane.begin() + color.axis);
    check_shape(in[1], plane, ElementType::UInt8);
    auto out_dims = dims;
    out_dims.erase(out_dims.begin());
    auto output = tensor_schema(type, out_dims);
    output.tensors[0].layout.spatial = true;
    output.tensors[0].layout.channel_axis = color.axis - 1;
    output.tensors[0].layout.height_axis = color.axis == 1 ? 1 : 0;
    output.tensors[0].layout.width_axis = color.axis == 3 ? 1 : 2;
    add_output(this, std::move(output));
    add_output(this, plane_schema(type, plane[1], plane[2]));
    collective = color.packed && frames <= 16;
    const unsigned __int128 pixels =
        static_cast<unsigned __int128>(plane[1]) * plane[2];
    scratch_bytes = mask::bytes(
        static_cast<unsigned __int128>(frames) * sizeof(Sample) + 128 +
        (collective
             ? pixels * ((3 * frames + 4) * generation::width(type) + frames)
             : 0));
  }
  std::vector<ReadRegion> support(unsigned output,
                                  const Region& r) const override {
    if (collective) {
      std::vector<ReadRegion> reads{{1, 0, Region::whole(plane)}};
      if (output == 0) {
        reads.push_back({0, 0, Region::whole(dims)});
      }
      return reads;
    }
    if (empty(r)) {
      return {};
    }
    auto d = r.dimensions();
    std::vector<ReadRegion> result;
    if (!output) {
      d.insert(d.begin(), {0, frames});
      result.push_back({0, 0, Region(d)});
      d.erase(d.begin() + color.axis);
    } else {
      d.insert(d.begin(), {0, frames});
    }
    result.push_back({1, 0, Region(std::move(d))});
    return result;
  }
  std::vector<ReadRegion> collective_support(
      const std::vector<unsigned>& ports) const override {
    return support(
        std::find(ports.begin(), ports.end(), 0) == ports.end() ? 1 : 0, {});
  }
  template <class T>
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{k, inv};
    Arena arena(inv);
    auto* samples_buffer = arena.take<Sample>(frames);
    if (collective) {
      const auto pixels = plane[1] * plane[2];
      auto* image_buffer = arena.take<T>(3 * frames * pixels);
      auto* flag_buffer = arena.take<std::uint8_t>(frames * pixels);
      auto* plate = arena.take<T>(3 * pixels);
      auto* confidence = arena.take<T>(pixels);
      bool wants_plate = false;
      for (unsigned o = 0; o < inv.output_count; ++o) {
        wants_plate |= inv.outputs[o].port == 0;
        auto out_dims = inv.outputs[o].port == 0 ? dims : plane;
        out_dims.erase(out_dims.begin());
        if (auto* direct = direct_output<T>(inv.outputs[o], out_dims)) {
          if (inv.outputs[o].port == 0) {
            plate = direct;
          } else {
            confidence = direct;
          }
        }
      }
      const auto* flags =
          gather<std::uint8_t>(reader, 1, plane, flag_buffer, work);
      const auto* image = wants_plate
                              ? gather<T>(reader, 0, dims, image_buffer, work)
                              : nullptr;
      const bool complete = temporal(
          image, flags, wants_plate ? plate : nullptr, confidence, frames,
          pixels, reinterpret_cast<TemporalSample<T>*>(samples_buffer),
          [&](std::uint64_t amount) { work.poll(amount); }, true);
      for (unsigned o = 0; o < inv.output_count; ++o) {
        if (!complete && inv.outputs[o].port == 0) {
          const auto& l = *inv.outputs[o].layout;
          for (auto y = l.origin[0]; y < l.origin[0] + l.shape[0]; ++y) {
            work.poll(l.shape[1]);
            for (auto x = l.origin[1]; x < l.origin[1] + l.shape[1]; ++x) {
              if (x && (x & 4095) == 0) {
                cancel(inv);
              }
              if (confidence[y * plane[2] + x] == T(0)) {
                fail("NoSolution: KEY temporal plate has no valid sample");
              }
            }
          }
        }
        auto out_dims = inv.outputs[o].port == 0 ? dims : plane;
        out_dims.erase(out_dims.begin());
        const auto* values = inv.outputs[o].port == 0 ? plate : confidence;
        if (direct_output<T>(inv.outputs[o], out_dims) != values) {
          scatter(values, out_dims, inv.outputs[o], work);
        }
      }
      cancel(inv);
      return;
    }
    if (color.packed && inv.output_count == 1) {
      const auto& out = inv.outputs[0];
      auto out_dims = out.port == 0 ? dims : plane;
      out_dims.erase(out_dims.begin());
      const auto* flags = direct_input<std::uint8_t>(reader, 1, plane);
      auto* target = direct_output<T>(out, out_dims);
      const auto* image =
          out.port == 0 ? direct_input<T>(reader, 0, dims) : nullptr;
      if (flags && target && (out.port == 1 || image)) {
        const bool ok = temporal(
            image, flags, out.port == 0 ? target : nullptr,
            out.port == 1 ? target : nullptr, frames, plane[1] * plane[2],
            reinterpret_cast<TemporalSample<T>*>(samples_buffer),
            [&](std::uint64_t amount) { work.poll(amount); });
        if (!ok) {
          fail("NoSolution: KEY temporal plate has no valid sample");
        }
        cancel(inv);
        return;
      }
    }
    std::uint64_t comparisons = 0;
    const auto less = [&](const Sample& a, const Sample& b) {
      if ((++comparisons & 4095) == 0) {
        cancel(inv);
      }
      return a.value < b.value || (a.value == b.value && a.frame < b.frame);
    };
    for (unsigned oi = 0; oi < inv.output_count; ++oi) {
      const auto& out = inv.outputs[oi];
      samples(out, [&](const Coordinate& at, std::uint64_t) {
        work.poll(frames * 8 + 1);
        auto spatial = out.port == 0 ? remove_axis(at, color.axis - 1) : at;
        std::uint64_t count = 0;
        for (std::uint64_t f = 0; f < frames; ++f) {
          if ((f & 4095) == 0) {
            cancel(inv);
          }
          if (!reader.raw(1, 0, {f, spatial[0], spatial[1]})) {
            continue;
          }
          if (out.port == 0) {
            const auto source = insert_axis(at, 0, f);
            const auto raw = reader.raw(0, 0, source);
            T value;
            std::memcpy(&value, &raw, sizeof(T));
            samples_buffer[count] = {static_cast<double>(value), f, raw};
          }
          ++count;
        }
        if (out.port == 1) {
          save(out, at,
               static_cast<T>(static_cast<double>(count) /
                              static_cast<double>(frames)));
        } else {
          if (!count) {
            fail("NoSolution: KEY temporal plate has no valid sample");
          }
          if (count == 1) {
            store(out, at, samples_buffer[0].raw);
            return;
          }
          const auto middle = count / 2;
          std::nth_element(samples_buffer, samples_buffer + middle,
                           samples_buffer + count, less);
          if (count & 1) {
            store(out, at, samples_buffer[middle].raw);
          } else {
            const auto lower =
                std::max_element(samples_buffer, samples_buffer + middle, less);
            // Halving before addition avoids overflow of a finite midpoint.
            const double lo = lower->value, hi = samples_buffer[middle].value;
            save(out, at, midpoint(static_cast<T>(lo), static_cast<T>(hi)));
          }
        }
      });
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    dispatch(k, inv, Work{inv, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& inv) const override {
    dispatch(k, inv.views, Work{inv.views, work_limit, 0, inv.work});
  }
  void dispatch(const Kernel& k, const ps_kernel_invocation_v8& inv,
                const Work& work) const {
    if (type == ElementType::Float32) {
      compute<float>(k, inv, work);
    } else {
      compute<double>(k, inv, work);
    }
  }
};

template <Kind K>
OperationDefinition spatial_operation(const char* key) {
  using P = OperationParameterType;
  auto p = K == Kind::Nearest || K == Kind::Extend
               ? color_parameters()
               : std::vector<OperationParameterSpec>{};
  if constexpr (K == Kind::Extend) {
    p.push_back({"alpha_index", P::Int64, false});
  }
  if constexpr (K == Kind::Erode || K == Kind::Dilate || K == Kind::Guided ||
                K == Kind::Extend) {
    p.push_back({"radius", P::Int64, true});
  }
  const char* name = K == Kind::Nearest  ? "plate"
                     : K == Kind::Extend ? "image"
                     : K == Kind::Fill   ? "mask"
                                         : "matte";
  std::vector<std::pair<std::string, SchemaTemplate>> out{
      {name,
       tensor_schema(
           K == Kind::Fill ? ElementType::UInt8 : ElementType::Float64, {1})}};
  if constexpr (K == Kind::Nearest) {
    out.emplace_back("confidence", tensor_schema(ElementType::Float64, {1}));
  }
  return budgeted_operation(generation::operation(
      key,
      K == Kind::Guided                           ? 3
      : (K == Kind::Nearest || K == Kind::Extend) ? 2
                                                  : 1,
      std::move(p), out, [](const auto& in, const auto& p) {
        return std::make_shared<Spatial<K>>(in, p);
      }));
}
}  // namespace ps::plugin_internal::keying

namespace ps::plugin_internal {
Status register_keying_spatial(OperationRegistry* registry) {
  using namespace keying;  // NOLINT(build/namespaces)
  for (auto op :
       {spatial_operation<Kind::Nearest>("keying.clean_plate_nearest"),
        spatial_operation<Kind::Erode>("keying.matte_erode"),
        spatial_operation<Kind::Dilate>("keying.matte_dilate"),
        spatial_operation<Kind::Fill>("keying.binary_fill_holes"),
        spatial_operation<Kind::Guided>("keying.matte_guided_gray"),
        spatial_operation<Kind::Extend>("keying.edge_extend_nearest"),
        budgeted_operation(generation::operation(
            "keying.clean_plate_temporal_median", 2, color_parameters(),
            {{"plate", tensor_schema(ElementType::Float64, {1})},
             {"confidence", tensor_schema(ElementType::Float64, {1})}},
            [](const auto& in, const auto& p) {
              return std::make_shared<Median>(in, p);
            }))}) {
    auto status = registry->register_operation(std::move(op));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
