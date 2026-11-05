// Copyright 2026 Photospider contributors
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "07-grade/artistic_common.hpp"
#include "07-grade/collective.hpp"
#include "07-grade/dither_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::artistic {
struct Directory {
  struct Rectangle {
    std::uint64_t y0, y1, x0, x1, slot;
  };
  struct Band {
    std::uint64_t y0, y1, begin, end;
  };
  std::vector<Rectangle> rectangles;
  std::vector<Band> bands;
  void prepare() {
    std::sort(rectangles.begin(), rectangles.end(),
              [](const auto& a, const auto& b) {
                return a.y0 < b.y0 || (a.y0 == b.y0 && a.x0 < b.x0);
              });
    bands.clear();
    for (std::uint64_t j = 0; j < rectangles.size(); ++j) {
      const auto& r = rectangles[j];
      if (bands.empty() || bands.back().y0 != r.y0)
        bands.push_back({r.y0, r.y1, j, j + 1});
      else
        bands.back().end = j + 1;
    }
  }
  std::int64_t locate(std::uint64_t y, std::uint64_t x) const {
    auto b = std::upper_bound(bands.begin(), bands.end(), y,
                              [](auto y, const auto& b) { return y < b.y0; });
    if (b == bands.begin() || y >= (--b)->y1)
      return -1;
    auto r = std::upper_bound(rectangles.begin() + b->begin,
                              rectangles.begin() + b->end, x,
                              [](auto x, const auto& r) { return x < r.x0; });
    if (r == rectangles.begin() + b->begin || x >= (--r)->x1)
      return -1;
    return r->slot;
  }
};
struct Dither final : Program {
  Image image;
  unsigned kind, levels = 2, order = 2, palette_size = 0;
  std::uint64_t map_h = 0, map_w = 0;
  std::int64_t origin_x = 0, origin_y = 0, pattern_x = 0, pattern_y = 0;
  bool clamp = true, serpentine = true, narrow_noise = false;
  bool noise_float32 = false;
  std::vector<std::int64_t> phases;
  std::vector<std::uint64_t> phase_offsets;
  std::vector<double> bayer;
  fast_color::Matrix to_lms{};
  std::vector<Region> requests;
  std::vector<Directory> directories;
  std::vector<unsigned> needed;
  bool arithmetic = false;
  Dither(unsigned member, const std::vector<OperationMetadata>& in,
         const Parameters& p)
      : image(in.at(0), p,
              member >= 4 ? std::vector<std::string>{"red", "green", "blue"}
                          : std::vector<std::string>{},
              true),
        kind(member) {
    origin_x = integer(p, "origin_x");
    origin_y = integer(p, "origin_y");
    if (kind < 4) {
      const auto l = integer(p, "levels", 2);
      require(l >= 2 && l <= 65536, "dither levels");
      levels = l;
      clamp = clamping(p);
    }
    if (kind == 0) {
      const auto value = integer(p, "order", 2);
      require(value >= 1 && value <= 8, "Bayer order");
      order = value;
      map_h = map_w = 1U << order;
      bayer.resize(map_h * map_w);
      for (unsigned y = 0; y < map_h; ++y)
        for (unsigned x = 0; x < map_w; ++x)
          bayer[y * map_w + x] =
              (dither::bayer_rank(x, y, order) + .5) / (map_h * map_w);
    }
    if (kind == 0 || kind == 3 || kind == 4) {
      const auto origin = integers(p, "origin", 2, "0,0");
      pattern_x = origin[0];
      pattern_y = origin[1];
      if (kind < 4)
        phases = integers(p, "channel_phase", image.selected.size() * 2);
    }
    if (kind == 3 || kind == 4) {
      const auto& t = tensor(in.at(kind == 3 ? 1 : 2));
      const auto shape = t.sample_shape();
      require(t.descriptor.element_type == ElementType::Float64 &&
                  shape.size() == 2 && shape[0] <= 4096 && shape[1] <= 4096,
              "dither thresholds require Float64[My,Mx], My/Mx <=4096");
      map_h = shape[0];
      map_w = shape[1];
    }
    if (kind == 0 || kind == 3 || kind == 4) {
      phase_offsets.resize(2 * (kind == 4 ? 1 : image.selected.size()));
      for (unsigned c = 0; c < phase_offsets.size() / 2; ++c) {
        phase_offsets[2 * c] =
            dither::wrap(static_cast<__int128>(origin_x) - pattern_x +
                             (kind == 4 ? 0 : phases[2 * c]),
                         map_w);
        phase_offsets[2 * c + 1] =
            dither::wrap(static_cast<__int128>(origin_y) - pattern_y +
                             (kind == 4 ? 0 : phases[2 * c + 1]),
                         map_h);
      }
    }
    if (kind == 1 || kind == 5) {
      const auto& noise = tensor(in.at(kind == 1 ? 1 : 2));
      require(floating(noise.descriptor.element_type) &&
                  noise.sample_shape() ==
                      std::vector<std::uint64_t>{
                          image.dims[image.yaxis], image.dims[image.xaxis],
                          kind == 1 ? image.selected.size() : 1},
              "dither noise dtype/shape");
      noise_float32 = noise.descriptor.element_type == ElementType::Float32;
      narrow_noise =
          kind == 1 && image.type == ElementType::Float32 && noise_float32;
    }
    if (kind >= 4) {
      const auto& table = tensor(in.at(1));
      const auto shape = table.sample_shape();
      require(shape.size() == 2 && shape[1] == 3 && shape[0] <= 256 &&
                  table.descriptor.element_type == image.type,
              "palette requires matching T[K,3], 1<=K<=256");
      palette_size = shape[0];
      try {
        feature::Edit conversion;
        statistics::rgb(&conversion, image.selection, p);
        to_lms = conversion.brightness.rgb_to_lms;
      } catch (const basis_ops::CheckedFailure& e) {
        throw e.status;
      } catch (const basis_ops::GeometryError&) {
        throw Status{ErrorCode::InvalidArgument, "dither RGB basis"};
      }
    }
    if (kind == 2 || kind == 6) {
      enum_parameter(p, "scan", "serpentine", "raster");
      serpentine = text(p, "scan", "serpentine") == "serpentine";
    }
    // Palette preparation is invocation-local and shared by all output tiles.
    // Error diffusion likewise performs one canonical scan per invocation.
    collective = kind == 2 || kind >= 4;
    add_output(this, output_like(in[0]));
  }
  void configure(const std::vector<std::pair<unsigned, Region>>& r) {
    requests.clear();
    directories.assign(image.selected.size(), {});
    needed.clear();
    arithmetic = false;
    for (unsigned j = 0; j < r.size(); ++j) {
      if (empty(r[j].second))
        continue;
      requests.push_back(r[j].second);
      const auto& d = r[j].second.dimensions();
      for (unsigned c = 0; c < image.selected.size(); ++c)
        if (image.selected[c] >= d[image.axis].offset &&
            image.selected[c] - d[image.axis].offset < d[image.axis].extent) {
          directories[c].rectangles.push_back(
              {d[image.yaxis].offset,
               d[image.yaxis].offset + d[image.yaxis].extent,
               d[image.xaxis].offset,
               d[image.xaxis].offset + d[image.xaxis].extent, j});
        }
    }
    for (unsigned c = 0; c < directories.size(); ++c) {
      directories[c].prepare();
      if (!directories[c].rectangles.empty())
        needed.push_back(c);
    }
    arithmetic = !needed.empty();
    scratch_bytes = 0;
    if (arithmetic && (kind == 2 || kind == 6))
      scratch_bytes = mask::bytes(static_cast<unsigned __int128>(2) *
                                  (image.dims[image.xaxis] + 2) *
                                  (kind == 6 ? 3 : 1) * sizeof(double));
    if (arithmetic && (kind == 4 || kind == 5))
      scratch_bytes =
          mask::bytes(static_cast<unsigned __int128>(palette_size) *
                      (palette_size - 1) / 2 * sizeof(dither::Segment));
  }
  void add_aux(std::vector<ReadRegion>* out, const Region& r) const {
    const auto& d = r.dimensions();
    if (kind == 1) {
      for (unsigned c = 0; c < image.selected.size(); ++c)
        if (image.selected[c] >= d[image.axis].offset &&
            image.selected[c] - d[image.axis].offset < d[image.axis].extent)
          out->push_back(
              {1, 0, Region({d[image.yaxis], d[image.xaxis], {c, 1}})});
    }
    if (kind == 3 || kind == 4)
      out->push_back({kind == 3 ? 1U : 2U, 0, whole({map_h, map_w})});
    if (kind == 5)
      out->push_back({2, 0, Region({d[image.yaxis], d[image.xaxis], {0, 1}})});
    if (kind >= 4)
      out->push_back({1, 0, whole({palette_size, 3})});
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (!collective) {
      auto out = image.support(r, false);
      if (image.active(r))
        add_aux(&out, r);
      return out;
    }
    std::vector<ReadRegion> out;
    for (const auto& q : requests) {
      auto local = image.support(q, kind >= 4);
      out.insert(out.end(), local.begin(), local.end());
      if (kind == 5 && image.active(q)) {
        const auto& d = q.dimensions();
        out.push_back({2, 0, Region({d[image.yaxis], d[image.xaxis], {0, 1}})});
      }
    }
    // Shared tables are dependencies of the evaluation, not of each retained
    // tile. The runtime charges tile enumeration before deduplicating reads.
    if (arithmetic && kind >= 4)
      out.push_back({1, 0, whole({palette_size, 3})});
    if (arithmetic && kind == 4)
      out.push_back({2, 0, whole({map_h, map_w})});
    if (arithmetic && (kind == 2 || kind == 6)) {
      const auto add = [&](unsigned c) {
        auto d = whole(image.dims).dimensions();
        d[image.axis] = {image.selected[c], 1};
        out.push_back({0, 0, Region(std::move(d))});
      };
      if (kind == 6) {
        for (unsigned c = 0; c < 3; ++c)
          add(c);
      } else {
        for (auto c : needed)
          add(c);
      }
    }
    return out;
  }
  double uniform(double value, double threshold) const {
    return narrow_noise ? static_cast<double>(dither::quantize_float(
                              static_cast<float>(value),
                              static_cast<float>(threshold), levels, clamp))
                        : dither::quantize(value, threshold, levels, clamp);
  }
  double threshold(const mask::Reader& reader, std::uint64_t y, std::uint64_t x,
                   unsigned c, const double* table) const {
    if (kind == 1 || kind == 5)
      return reader.value(kind == 1 ? 1 : 2, 0,
                          Coordinate{y, x, kind == 1 ? c : 0});
    // Logical coordinates are bounded by 2^40; reduced static phases are at
    // most 4095, so these additions cannot overflow uint64_t.
    const auto xx = (x + phase_offsets[2 * c]) % map_w;
    const auto yy = (y + phase_offsets[2 * c + 1]) % map_h;
    return table ? table[yy * map_w + xx]
                 : reader.value(kind == 3 ? 1 : 2, 0, Coordinate{yy, xx});
  }
  Coordinate at(std::uint64_t y, std::uint64_t x, unsigned c) const {
    Coordinate q{};
    q[image.yaxis] = y;
    q[image.xaxis] = x;
    q[image.axis] = image.selected[c];
    return q;
  }
  template <class T>
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{k, inv};
    std::array<dither::Triple, 256> lab{};
    std::array<std::array<std::uint64_t, 3>, 256> raw_palette{};
    dither::Palette palette{lab.data(), palette_size, nullptr, 0};
    if (kind >= 4 && arithmetic) {
      work.poll(static_cast<std::uint64_t>(palette_size) * 256 +
                static_cast<std::uint64_t>(palette_size) * palette_size * 32);
      for (unsigned j = 0; j < palette_size; ++j) {
        dither::Triple rgb{};
        for (unsigned c = 0; c < 3; ++c) {
          raw_palette[j][c] = reader.raw(1, 0, Coordinate{j, c});
          rgb[c] = reader.value(1, 0, Coordinate{j, c});
        }
        lab[j] = dither::oklab(rgb, to_lms);
      }
      if (kind != 6 && palette_size > 1) {
        palette.segments =
            static_cast<dither::Segment*>(inv.scratch[0].allocation);
        palette.prepare();
      }
    }
    const auto h = image.dims[image.yaxis], w = image.dims[image.xaxis];
    if ((kind == 2 || kind == 6) && arithmetic) {
      const auto amount =
          static_cast<unsigned __int128>(h) * w *
          (kind == 2 ? needed.size() * 48 : 256 + 16 * palette_size);
      work.poll(mask::bytes(amount));
      auto* rows = static_cast<double*>(inv.scratch[0].allocation);
      if (kind == 2) {
        for (auto c : needed) {
          std::int64_t cached = -1;
          dither::diffuse<1>(
              h, w, origin_y, serpentine, rows,
              [&](auto y, auto x) {
                const double v = reader.value(0, 0, at(y, x, c));
                return std::array<double, 1>{clamp ? std::clamp(v, 0.0, 1.0)
                                                   : v};
              },
              [&](const auto& value) {
                const double q =
                    std::clamp(dither::ties_even(value[0] * (levels - 1)), 0.0,
                               static_cast<double>(levels - 1));
                const double reconstruction =
                    q / static_cast<double>(levels - 1);
                return std::make_pair(reconstruction,
                                      std::array<double, 1>{reconstruction});
              },
              [&](auto y, auto x, double value) {
                auto q = at(y, x, c);
                if (cached < 0 ||
                    q[image.yaxis] <
                        inv.outputs[cached].layout->origin[image.yaxis] ||
                    q[image.yaxis] -
                            inv.outputs[cached].layout->origin[image.yaxis] >=
                        inv.outputs[cached].layout->shape[image.yaxis] ||
                    q[image.xaxis] <
                        inv.outputs[cached].layout->origin[image.xaxis] ||
                    q[image.xaxis] -
                            inv.outputs[cached].layout->origin[image.xaxis] >=
                        inv.outputs[cached].layout->shape[image.xaxis])
                  cached = directories[c].locate(y, x);
                if (cached >= 0)
                  store(inv.outputs[cached], q, convert(value, sizeof(T) == 4));
              },
              [&] { cancel(inv); });
        }
      } else {
        std::array<std::int64_t, 3> cached{-1, -1, -1};
        dither::diffuse<3>(
            h, w, origin_y, serpentine, rows,
            [&](auto y, auto x) {
              dither::Triple rgb{};
              for (unsigned c = 0; c < 3; ++c)
                rgb[c] = reader.value(0, 0, at(y, x, c));
              return dither::oklab(rgb, to_lms);
            },
            [&](const auto& value) {
              const auto index = palette.nearest(value);
              return std::make_pair(index, lab[index]);
            },
            [&](auto y, auto x, unsigned index) {
              for (auto c : needed) {
                const auto q = at(y, x, c);
                auto& slot = cached[c];
                if (slot < 0 ||
                    q[image.yaxis] <
                        inv.outputs[slot].layout->origin[image.yaxis] ||
                    q[image.yaxis] -
                            inv.outputs[slot].layout->origin[image.yaxis] >=
                        inv.outputs[slot].layout->shape[image.yaxis] ||
                    q[image.xaxis] <
                        inv.outputs[slot].layout->origin[image.xaxis] ||
                    q[image.xaxis] -
                            inv.outputs[slot].layout->origin[image.xaxis] >=
                        inv.outputs[slot].layout->shape[image.xaxis])
                  slot = directories[c].locate(y, x);
                if (slot >= 0)
                  store(inv.outputs[slot], q, raw_palette[index][c]);
              }
            },
            [&] { cancel(inv); });
      }
    }
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      const bool active = image.active(region(*out.layout));
      work.poll(
          mask::bytes(static_cast<unsigned __int128>(count(*out.layout)) *
                      (kind >= 4 ? 256 + 24 * palette.segment_count : 32)));
      const double* table = kind == 0 ? bayer.data() : nullptr;
      if (active && (kind == 3 || (kind == 4 && palette_size > 1))) {
        const auto& view = reader.view(kind == 3 ? 1 : 2, 0, {});
        const auto& l = *view.layout;
        if (!l.origin[0] && !l.origin[1] && l.shape[0] == map_h &&
            l.shape[1] == map_w && l.strides[0] == map_w * 8 &&
            l.strides[1] == 8 && aligned(address<double>(view, {})))
          table = address<double>(view, {});
      }
      const auto slow = [&](Coordinate q, std::uint64_t j) {
        if ((j & 127U) == 0)
          cancel(inv);
        unsigned chosen = 0;
        if (active && (kind == 4 || kind == 5) && palette_size > 1) {
          dither::Triple rgb{};
          for (unsigned c = 0; c < 3; ++c) {
            auto src = q;
            src[image.axis] = image.selected[c];
            rgb[c] = reader.value(0, 0, src);
          }
          chosen = palette.segment(
              dither::oklab(rgb, to_lms),
              threshold(reader, q[image.yaxis], q[image.xaxis], 0, table));
        }
        for (q[image.axis] = out.layout->origin[image.axis];
             q[image.axis] <
             out.layout->origin[image.axis] + out.layout->shape[image.axis];
             ++q[image.axis]) {
          const auto c = image.component(q[image.axis]);
          if (c < 0)
            store(out, q, reader.raw(0, 0, q));
          else if (kind == 2 || kind == 6)
            continue;
          else if (kind >= 4)
            store(out, q, raw_palette[chosen][c]);
          else
            store(out, q,
                  convert(uniform(reader.value(0, 0, q),
                                  threshold(reader, q[image.yaxis],
                                            q[image.xaxis], c, table)),
                          sizeof(T) == 4));
        }
      };
      if (kind == 2 || kind == 6) {
        pixels(out, image.axis, slow);
        continue;
      }
      const bool vertical = out.layout->shape[image.xaxis] == 1;
      image_runs<T>(
          image, reader, inv, out,
          [&](const T* src, T* dst, std::uint64_t n, const Coordinate& q) {
            const auto channels = image.dims[image.axis];
            bool canonical = kind < 4 && image.selected.size() == channels;
            for (unsigned c = 0; canonical && c < image.selected.size(); ++c)
              canonical &= image.selected[c] == c;
            if (canonical) {
              const auto cycle = (vertical ? map_h : map_w) * channels;
              if ((kind == 0 || kind == 3) && table && cycle <= 256) {
                dither::periodic_uniform_span(
                    src, dst, n * channels, levels, clamp, cycle,
                    [&](std::size_t j) {
                      const auto pixel = j / channels;
                      return threshold(reader,
                                       q[image.yaxis] + (vertical ? pixel : 0),
                                       q[image.xaxis] + (vertical ? 0 : pixel),
                                       j % channels, table);
                    });
                return true;
              }
              if (kind == 1) {
                const Coordinate at{q[image.yaxis], q[image.xaxis], 0};
                const auto& noise = reader.view(1, 0, at);
                const unsigned run = vertical ? 0 : 1;
                const auto bytes =
                    noise_float32 ? sizeof(float) : sizeof(double);
                const auto& layout = *noise.layout;
                if (layout.origin[2] == 0 && layout.shape[2] == channels &&
                    layout.strides[2] == static_cast<std::int64_t>(bytes) &&
                    layout.strides[run] ==
                        static_cast<std::int64_t>(channels * bytes) &&
                    layout.origin[run] + layout.shape[run] - at[run] >= n) {
                  const auto execute = [&](const auto* values) {
                    dither::uniform_span(
                        src, dst, n * channels, levels, clamp, narrow_noise,
                        [&](std::size_t j) { return values[j]; });
                  };
                  if (noise_float32 && aligned(address<float>(noise, at))) {
                    execute(address<float>(noise, at));
                    return true;
                  }
                  if (!noise_float32 && aligned(address<double>(noise, at))) {
                    execute(address<double>(noise, at));
                    return true;
                  }
                }
              }
              dither::uniform_span(
                  src, dst, n * channels, levels, clamp, narrow_noise,
                  [&](std::size_t j) {
                    const auto pixel = j / channels;
                    return threshold(
                        reader, q[image.yaxis] + (vertical ? pixel : 0),
                        q[image.xaxis] + (vertical ? 0 : pixel),
                        static_cast<unsigned>(j % channels), table);
                  });
              return true;
            }
            std::memcpy(dst, src, n * channels * sizeof(T));
            if (kind >= 4) {
              dither::palette_points(
                  n, palette, to_lms,
                  [&](std::size_t j) {
                    dither::Triple rgb{};
                    for (unsigned c = 0; c < 3; ++c)
                      rgb[c] = src[j * channels + image.selected[c]];
                    return rgb;
                  },
                  [&](std::size_t j) {
                    return threshold(
                        reader, q[image.yaxis] + (vertical ? j : 0),
                        q[image.xaxis] + (vertical ? 0 : j), 0, table);
                  },
                  [&](std::size_t j, unsigned index) {
                    for (unsigned c = 0; c < 3; ++c)
                      std::memcpy(dst + j * channels + image.selected[c],
                                  &raw_palette[index][c], sizeof(T));
                  });
              return true;
            }
            for (std::uint64_t j = 0; j < n; ++j) {
              const auto y = q[image.yaxis] + (vertical ? j : 0);
              const auto x = q[image.xaxis] + (vertical ? 0 : j);
              for (unsigned c = 0; c < image.selected.size(); ++c) {
                const auto at = j * channels + image.selected[c];
                dst[at] = static_cast<T>(
                    uniform(src[at], threshold(reader, y, x, c, table)));
              }
            }
            return true;
          },
          slow, vertical);
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    if (image.type == ElementType::Float32)
      compute<float>(k, i, Work{i, work_limit});
    else
      compute<double>(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    if (image.type == ElementType::Float32)
      compute<float>(k, i.views, Work{i.views, work_limit, 0, i.work});
    else
      compute<double>(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
OperationDefinition dither_operation(unsigned kind) {
  constexpr const char* names[]{"grade.dither_bayer",
                                "grade.dither_noise",
                                "grade.dither_floyd_steinberg",
                                "grade.dither_ordered_map",
                                "grade.dither_palette_ordered",
                                "grade.dither_palette_noise",
                                "grade.dither_palette_floyd_steinberg"};
  constexpr unsigned ports[]{1, 2, 1, 2, 3, 3, 2};
  auto p = selectors();
  append(&p, {{"origin_x", P::Int64, false}, {"origin_y", P::Int64, false}});
  if (kind < 4)
    append(&p, {{"levels", P::Int64, false}, {"edge", P::String, false}});
  if (kind == 0)
    p.push_back({"order", P::Int64, false});
  if (kind == 0 || kind == 3 || kind == 4)
    p.push_back({"origin", P::String, false});
  if (kind == 0 || kind == 3)
    p.push_back({"channel_phase", P::String, false});
  if (kind == 2 || kind == 6)
    p.push_back({"scan", P::String, false});
  auto definition = budgeted_operation(generation::operation(
      names[kind], ports[kind], std::move(p),
      {{"values", tensor_schema(ElementType::Float64, {1})}},
      [kind](const auto& in, const auto& p) {
        return std::make_shared<Dither>(kind, in, p);
      }));
  return kind == 2 || kind >= 4
             ? statistics::collective_operation<Dither>(std::move(definition))
             : definition;
}
}  // namespace ps::plugin_internal::grade::artistic
namespace ps::plugin_internal {
Status register_grade_dither(OperationRegistry* registry) {
  for (unsigned k = 0; k < 7; ++k) {
    const auto status =
        registry->register_operation(grade::artistic::dither_operation(k));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
