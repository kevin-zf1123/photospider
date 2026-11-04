// Copyright 2026 Photospider Authors
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "07-grade/collective.hpp"
#include "07-grade/histogram_math.hpp"
#include "07-grade/statistical_composition.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::histogram {
using namespace statistics;  // NOLINT(build/namespaces)
using P = OperationParameterType;
inline std::vector<OperationParameterSpec> parameters(unsigned kind) {
  std::vector<OperationParameterSpec> p;
  if (kind == 0 || kind == 3 || kind >= 5) {
    p = metering_parameters(kind != 3 && kind != 6);
  } else if (kind == 1) {
    p = metering_parameters(false);
  }
  if (kind != 1) {
    p.push_back({"edge", P::String, false});
  }
  if (kind == 0 || kind == 3 || kind >= 5) {
    p.push_back({"B", P::Int64, false});
  }
  if (kind == 3 || kind == 4 || kind == 6) {
    p.push_back({"cell_width", P::Int64, false});
    p.push_back({"cell_height", P::Int64, false});
    if (kind != 4) {
      p.push_back({"clip_factor", P::Float64, false});
    }
  }
  return p;
}
struct Configuration {
  std::uint64_t h, w, bins, cw, ch, tx, ty;
  bool clamp, weighted;
  double clip;
  Metering meter;
  Configuration(const OperationMetadata& input, const Parameters& p)
      : h(tensor(input).sample_shape().at(0)),
        w(tensor(input).sample_shape().at(1)),
        bins(integer(p, "B", 256)),
        cw(integer(p, "cell_width", 64)),
        ch(integer(p, "cell_height", 64)),
        tx(0),
        ty(0),
        clamp(text(p, "edge", "clamp") == "clamp"),
        weighted(text(p, "selection_mode", "binary") == "weighted"),
        clip(real(p, "clip_factor", 2)),
        meter(h, w, p) {
    const auto& t = tensor(input);
    require(t.sample_shape().size() == 2 && floating(t.descriptor.element_type),
            "histogram feature Float32/64[H,W]");
    require(bins >= 2 && bins <= 65536 && cw && ch && cw <= INT64_MAX &&
                ch <= INT64_MAX,
            "histogram bin/cell extent");
    require(clamp || text(p, "edge") == "reject", "histogram edge");
    tx = (w - 1) / cw + 1;
    ty = (h - 1) / ch + 1;
  }
};
/** Binary counts, weighted masses, global maps and local maps use explicit
 * typed Results. Collective histogram generation shares one scan across
 * requested count/mass/count-vector outputs without image-sized allocation.
 */
struct Generate final : Program {
  unsigned kind;
  Configuration c;
  bool need_feature = true;
  std::uint64_t requested_views = 0;
  struct CellCursor {
    std::uint64_t y, x, output;
  };
  std::vector<Region> cells;
  void configure(const std::vector<std::pair<unsigned, Region>>& regions) {
    need_feature = kind != 0;
    requested_views = regions.size();
    cells.clear();
    for (const auto& r : regions) {
      if (kind == 0) {
        need_feature |= r.first == 0 && !empty(r.second);
      } else if (!empty(r.second)) {
        const auto& y = r.second.dimensions()[0];
        const auto& x = r.second.dimensions()[1];
        const auto y0 = y.offset * c.ch, x0 = x.offset * c.cw;
        cells.emplace_back(
            Region({{y0, std::min(c.h, (y.offset + y.extent) * c.ch) - y0},
                    {x0, std::min(c.w, (x.offset + x.extent) * c.cw) - x0}}));
      }
    }
  }
  Generate(unsigned member, const std::vector<OperationMetadata>& in,
           const Parameters& p)
      : kind(member), c(in.at(0), p) {
    c.meter.selection(in.at(1));
    if (kind == 0) {
      collective = true;
      add_output(this, tensor_schema(c.weighted ? ElementType::Float64
                                                : ElementType::Int64,
                                     {c.bins}));
      add_output(this, tensor_schema(ElementType::Int64, {1}));
      add_output(this, tensor_schema(ElementType::Float64, {1}));
    } else {
      add_output(this,
                 tensor_schema(ElementType::Float64, {c.ty, c.tx, c.bins}));
      add_output(this, tensor_schema(ElementType::UInt8, {c.ty, c.tx}));
      // One collective generator caches all logical cells once for the complete
      // operator. Standalone cell requests use the same deterministic grid.
      collective = true;
    }
    scratch_bytes = c.bins * 2 * sizeof(double);
  }
  std::uint64_t task_scratch(unsigned, const Region&) const override {
    return kind == 0
               ? scratch_bytes
               : mask::bytes(static_cast<unsigned __int128>(scratch_bytes) +
                             static_cast<unsigned __int128>(requested_views) *
                                 sizeof(CellCursor));
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    std::vector<ReadRegion> out;
    const auto add = [&](const Region& r) {
      if (empty(r)) {
        return;
      }
      out.push_back({1, 0, r});
      if (need_feature) {
        out.push_back({0, 0, r});
      }
    };
    if (kind == 0) {
      add(c.meter.region());
    } else {
      for (const auto& r : cells) {
        add(r);
      }
    }
    return out;
  }
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{k, inv};
    auto* counts = static_cast<double*>(inv.scratch[0].allocation);
    auto* map = counts + c.bins;
    bool need_counts = kind != 0;
    for (const auto& w : k.writes) {
      need_counts |= w.first == 0;
    }
    const auto scan = [&](std::uint64_t y0, std::uint64_t y1, std::uint64_t x0,
                          std::uint64_t x1) {
      work.poll((y1 - y0) * (x1 - x0) * (need_counts ? 6 : 2) + c.bins * 12);
      std::fill(counts, counts + c.bins, 0);
      std::uint64_t count = 0;
      double mass = 0;
      Coordinate at{};
      at[0] = y0;
      at[1] = x0;
      const unsigned run = c.w == 1 && x0 == 0 && x1 == 1 ? 0 : 1;
      const auto begin = run ? x0 : y0, end = run ? x1 : y1;
      for (std::uint64_t row = 0; row < (run ? y1 - y0 : 1); ++row) {
        if (run) {
          at[0] = y0 + row;
        }
        for (at[run] = begin; at[run] < end;) {
          cancel(inv);
          auto n = std::min<std::uint64_t>(4096, end - at[run]);
          const auto s = span(reader, 1, at, run, &n);
          Span x;
          if (need_counts) {
            x = span(reader, 0, at, run, &n);
          }
          const bool narrow =
              inputs[0].result_schema->tensors[0].descriptor.element_type ==
              ElementType::Float32;
          std::pair<std::uint64_t, double> totals;
          if (narrow) {
            if (c.weighted) {
              totals = histogram_span<float, double>(
                  x.data, x.stride, s.data, s.stride, n, counts, c.bins,
                  c.clamp, need_counts);
            } else {
              totals = histogram_span<float, std::uint8_t>(
                  x.data, x.stride, s.data, s.stride, n, counts, c.bins,
                  c.clamp, need_counts);
            }
          } else {
            if (c.weighted) {
              totals = histogram_span<double, double>(
                  x.data, x.stride, s.data, s.stride, n, counts, c.bins,
                  c.clamp, need_counts);
            } else {
              totals = histogram_span<double, std::uint8_t>(
                  x.data, x.stride, s.data, s.stride, n, counts, c.bins,
                  c.clamp, need_counts);
            }
          }
          count += totals.first;
          mass += totals.second;
          at[run] += n;
        }
      }
      return std::pair<std::uint64_t, double>{count, mass};
    };
    if (kind == 0) {
      const auto totals = scan(c.meter.y0, c.meter.y1, c.meter.x0, c.meter.x1);
      for (unsigned o = 0; o < inv.output_count; ++o) {
        const auto port = k.writes[o].first;
        samples(inv.outputs[o], [&](const auto& at, auto) {
          const auto v =
              port == 0
                  ? (c.weighted ? bits(counts[at[0]])
                                : static_cast<std::uint64_t>(counts[at[0]]))
              : port == 1 ? totals.first
                          : bits(totals.second);
          store(inv.outputs[o], at, v);
        });
      }
    } else {
      // A charged invocation-local merge heap contains one cursor per output
      // rectangle, never one object per cell. Only requested cells are visited;
      // duplicate map-bin/flag requests share exactly one statistical scan.
      auto* heap = reinterpret_cast<CellCursor*>(map + c.bins);
      std::uint64_t size = 0, levels = 1;
      for (auto n = inv.output_count; n > 1; n = (n + 1) / 2) {
        ++levels;
      }
      work.poll(inv.output_count * levels);
      const auto later = [](const CellCursor& a, const CellCursor& b) {
        return a.y > b.y || (a.y == b.y && a.x > b.x);
      };
      for (unsigned o = 0; o < inv.output_count; ++o) {
        const auto& l = *inv.outputs[o].layout;
        if (l.shape[0] && l.shape[1]) {
          heap[size++] = {l.origin[0], l.origin[1], o};
        }
      }
      std::make_heap(heap, heap + size, later);
      while (size) {
        cancel(inv);
        const auto y = heap[0].y, x = heap[0].x;
        scan(y * c.ch, std::min(c.h, (y + 1) * c.ch), x * c.cw,
             std::min(c.w, (x + 1) * c.cw));
        const bool identity =
            mapping(counts, map, c.bins, true, c.weighted, c.clip);
        while (size && heap[0].y == y && heap[0].x == x) {
          work.poll(32 * levels);
          std::pop_heap(heap, heap + size, later);
          auto cursor = heap[--size];
          const auto o = cursor.output;
          const auto& out = inv.outputs[o];
          const auto& l = *out.layout;
          Coordinate at{};
          at[0] = y;
          at[1] = x;
          if (k.writes[o].first == 1) {
            store(out, at, identity);
          } else {
            for (at[2] = l.origin[2]; at[2] < l.origin[2] + l.shape[2];
                 ++at[2]) {
              store(out, at, bits(map[at[2]]));
            }
          }
          if (++cursor.x == l.origin[1] + l.shape[1]) {
            cursor.x = l.origin[1];
            ++cursor.y;
          }
          if (cursor.y < l.origin[0] + l.shape[0]) {
            heap[size++] = cursor;
            std::push_heap(heap, heap + size, later);
          }
        }
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    compute(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    compute(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
struct Cdf final : Program {
  std::uint64_t bins;
  bool weighted;
  Cdf(const std::vector<OperationMetadata>& in, const Parameters& p) {
    const auto& t = tensor(in.at(0));
    const auto m = text(p, "selection_mode", "binary");
    require(m == "binary" || m == "weighted", "CDF selection_mode");
    weighted = m == "weighted";
    require(t.sample_shape().size() == 1 &&
                t.descriptor.element_type ==
                    (weighted ? ElementType::Float64 : ElementType::Int64),
            "CDF counts dtype/rank");
    bins = t.sample_shape()[0];
    require(bins >= 2 && bins <= 65536, "CDF bins");
    auto s = tensor_schema(ElementType::Float64, {bins});
    s.tensors[0].atomic_trailing_axes = 1;
    add_output(this, std::move(s));
    add_output(this, tensor_schema(ElementType::UInt8, {1}));
    scratch_bytes = bins * 2 * 8;
    collective = true;
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    return {{0, 0, whole({bins})}};
  }
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    work.poll(bins * 12);
    const mask::Reader r{k, inv};
    auto* counts = static_cast<double*>(inv.scratch[0].allocation);
    auto* map = counts + bins;
    Coordinate at{};
    for (at[0] = 0; at[0] < bins; ++at[0]) {
      if (!(at[0] & 4095U)) {
        cancel(inv);
      }
      counts[at[0]] =
          weighted ? r.value(0, 0, at) : static_cast<double>(r.raw(0, 0, at));
    }
    const bool identity = mapping(counts, map, bins, false, weighted, 0);
    for (unsigned o = 0; o < inv.output_count; ++o) {
      samples(inv.outputs[o], [&](const auto& a, auto) {
        store(inv.outputs[o], a,
              k.writes[o].first == 1 ? identity : bits(map[a[0]]));
      });
    }
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    compute(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    compute(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
struct Apply final : Program {
  bool local;
  Configuration c;
  ElementType type;
  Apply(bool clahe, const std::vector<OperationMetadata>& in,
        const Parameters& p)
      : local(clahe),
        c(in.at(0), p),
        type(tensor(in[0]).descriptor.element_type) {
    const auto& maps = tensor(in.at(1));
    require(maps.descriptor.element_type == ElementType::Float64 &&
                maps.sample_shape().size() == (local ? 3 : 1),
            "histogram maps dtype/rank");
    c.bins = maps.sample_shape().back();
    require(c.bins >= 2 && c.bins <= 65536, "histogram map bins");
    typed(in[1], ElementType::Float64,
          local ? std::vector<std::uint64_t>{c.ty, c.tx, c.bins}
                : std::vector<std::uint64_t>{c.bins});
    typed(in.at(2), ElementType::UInt8,
          local ? std::vector<std::uint64_t>{c.ty, c.tx}
                : std::vector<std::uint64_t>{1});
    add_output(this, output_like(in[0]));
    if (!local) {
      scratch_bytes = c.bins * 8;
#if defined(__x86_64__)
      if (type == ElementType::Float32 && c.bins <= kPreparedMapCapacity) {
        scratch_bytes += c.bins * sizeof(float);
      }
#endif
    }
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r)) {
      return {};
    }
    if (!local) {
      return {{0, 0, r}, {1, 0, whole({c.bins})}, {2, 0, whole({1})}};
    }
    const auto y = r.dimensions()[0], x = r.dimensions()[1];
    const auto ya = bracket(y.offset, c.ch, c.h),
               yb = bracket(y.offset + y.extent - 1, c.ch, c.h);
    const auto xa = bracket(x.offset, c.cw, c.w),
               xb = bracket(x.offset + x.extent - 1, c.cw, c.w);
    return {{0, 0, r},
            {1, 0,
             Region({{ya.lo, yb.hi - ya.lo + 1},
                     {xa.lo, xb.hi - xa.lo + 1},
                     {0, c.bins}})},
            {2, 0,
             Region({{ya.lo, yb.hi - ya.lo + 1}, {xa.lo, xb.hi - xa.lo + 1}})}};
  }
  template <class T>
  bool column(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& out) const {
    const auto& l = *out.layout;
    if (!local || c.w != 1 || l.shape[1] != 1 || l.strides[0] != sizeof(T)) {
      return false;
    }
    for (unsigned pass = 0; pass < 2; ++pass) {
      Coordinate at{}, q{};
      for (at[0] = l.origin[0]; at[0] < l.origin[0] + l.shape[0];) {
        cancel(inv);
        auto n =
            std::min<std::uint64_t>(2048, l.origin[0] + l.shape[0] - at[0]);
        const auto x = span(reader, 0, at, 0, &n);
        if (x.stride != sizeof(T)) {
          return false;
        }
        const auto by = bracket(at[0], c.ch, c.h);
        if (by.lo == by.hi && by.lo == 0 && c.ty > 1) {
          const auto end =
              static_cast<std::uint64_t>(std::ceil(center(0, c.ch, c.h) - .5));
          n = std::min(n, std::max<std::uint64_t>(1, end - at[0]));
        }
        if (by.lo != by.hi) {
          const auto end = static_cast<std::uint64_t>(
              std::ceil(center(by.hi, c.ch, c.h) - .5));
          n = std::min(n, std::max<std::uint64_t>(1, end - at[0]));
        }
        LocalMap maps[4];
        const std::uint64_t cells[]{by.lo, by.hi};
        for (unsigned iy = 0; iy < 2; ++iy) {
          if (iy == 1 && by.lo == by.hi) {
            continue;
          }
          q[0] = cells[iy];
          auto& m = maps[iy];
          m.identity = reader.raw(2, 0, q) != 0;
          if (m.identity) {
            continue;
          }
          const auto& v = reader.view(1, 0, q);
          if (v.layout->origin[2] || v.layout->shape[2] != c.bins) {
            return false;
          }
          m.data = static_cast<const unsigned char*>(v.allocation) +
                   offset(*v.layout, v.byte_offset, q);
          m.stride = v.layout->strides[2];
        }
        if (pass) {
          auto* dest = static_cast<unsigned char*>(out.allocation) +
                       offset(l, out.byte_offset, at);
          const double step =
              by.lo == by.hi
                  ? 0.0
                  : 1.0 / (center(by.hi, c.ch, c.h) - center(by.lo, c.ch, c.h));
          local_apply_span<T>(x.data, x.stride, dest, l.strides[0], n, maps,
                              c.bins, c.clamp, 0.0, by.fraction, step);
        }
        at[0] += n;
      }
    }
    return true;
  }
  void compute(const Kernel& k, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader r{k, inv};
#if defined(__x86_64__)
    bool global_ready = false, global_identity = false;
    const float* prepared = nullptr;
    // Saturation is sufficient for the small-table amortization guard and
    // avoids summing overlapping output extents into an unbounded counter.
    std::uint64_t prepare_samples = 0;
    if (!local) {
      for (unsigned o = 0; o < inv.output_count; ++o) {
        const auto& l = *inv.outputs[o].layout;
        prepare_samples = std::min<std::uint64_t>(
            4 * kPreparedMapCapacity,
            prepare_samples + std::min<std::uint64_t>(4 * kPreparedMapCapacity,
                                                      l.shape[0] * l.shape[1]));
      }
    }
#endif
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      const auto& l = *out.layout;
      if (!l.shape[0] || !l.shape[1]) {
        continue;
      }
      work.poll(l.shape[0] * l.shape[1] * (local ? 48 : 8));
      if (type == ElementType::Float32 ? column<float>(r, inv, out)
                                       : column<double>(r, inv, out)) {
        continue;
      }
      Coordinate at{}, q{};
      auto* map =
          local ? nullptr : static_cast<double*>(inv.scratch[0].allocation);
#if defined(__x86_64__)
      if (!local && !global_ready && l.shape[0] && l.shape[1]) {
        global_identity = r.raw(2, 0, q) != 0;
        if (!global_identity) {
          work.poll(c.bins);
          for (q[0] = 0; q[0] < c.bins; ++q[0]) {
            if (!(q[0] & 4095U)) {
              cancel(inv);
            }
            map[q[0]] = r.value(1, 0, q);
          }
#if defined(__x86_64__)
          if (type == ElementType::Float32 && c.bins <= kPreparedMapCapacity) {
            auto* workspace = reinterpret_cast<float*>(map + c.bins);
            if (can_prepare_apply_map(workspace, c.bins, prepare_samples,
                                      false)) {
              work.poll(c.bins);
              prepared = prepare_apply_map(map, workspace, c.bins,
                                           prepare_samples, false);
            }
          }
#endif
        }
        global_ready = true;
      }
      const bool identity = !local && global_identity;
#else
      const bool identity =
          !local && l.shape[0] && l.shape[1] && r.raw(2, 0, q) != 0;
      if (!local && !identity) {
        work.poll(c.bins);
        for (q[0] = 0; q[0] < c.bins; ++q[0]) {
          if (!(q[0] & 4095U)) {
            cancel(inv);
          }
          map[q[0]] = r.value(1, 0, q);
        }
      }
#endif
      if (!local && c.w == 1 && l.shape[1] == 1) {
        for (at[0] = l.origin[0]; at[0] < l.origin[0] + l.shape[0];) {
          cancel(inv);
          auto n =
              std::min<std::uint64_t>(2048, l.origin[0] + l.shape[0] - at[0]);
          const auto x = span(r, 0, at, 0, &n);
          auto* dest = static_cast<unsigned char*>(out.allocation) +
                       offset(l, out.byte_offset, at);
          if (type == ElementType::Float32) {
#if defined(__x86_64__)
            apply_prepared_span(x.data, x.stride, dest, l.strides[0], n, map,
                                c.bins, identity, c.clamp, prepared);
#else
            apply_span<float>(x.data, x.stride, dest, l.strides[0], n, map,
                              c.bins, identity, c.clamp);
#endif
          } else {
            apply_span<double>(x.data, x.stride, dest, l.strides[0], n, map,
                               c.bins, identity, c.clamp);
          }
          at[0] += n;
        }
        continue;
      }
      for (at[0] = l.origin[0]; at[0] < l.origin[0] + l.shape[0]; ++at[0]) {
        const auto by = bracket(at[0], c.ch, c.h);
        for (at[1] = l.origin[1]; at[1] < l.origin[1] + l.shape[1];) {
          cancel(inv);
          auto n =
              std::min<std::uint64_t>(2048, l.origin[1] + l.shape[1] - at[1]);
          const auto x = span(r, 0, at, 1, &n);
          if (!local) {
            auto* destination = static_cast<unsigned char*>(out.allocation) +
                                offset(l, out.byte_offset, at);
            if (type == ElementType::Float32) {
#if defined(__x86_64__)
              apply_prepared_span(x.data, x.stride, destination, l.strides[1],
                                  n, map, c.bins, identity, c.clamp, prepared);
#else
              apply_span<float>(x.data, x.stride, destination, l.strides[1], n,
                                map, c.bins, identity, c.clamp);
#endif
            } else {
              apply_span<double>(x.data, x.stride, destination, l.strides[1], n,
                                 map, c.bins, identity, c.clamp);
            }
            at[1] += n;
            continue;
          }
          const auto bx = bracket(at[1], c.cw, c.w);
          if (bx.hi == bx.lo && bx.lo == 0 && c.tx > 1) {
            const auto end = static_cast<std::uint64_t>(
                std::ceil(center(0, c.cw, c.w) - .5));
            n = std::min(n, std::max<std::uint64_t>(1, end - at[1]));
          }
          if (bx.hi != bx.lo) {
            const auto end = static_cast<std::uint64_t>(
                std::ceil(center(bx.hi, c.cw, c.w) - .5));
            n = std::min(n, std::max<std::uint64_t>(1, end - at[1]));
          }
          LocalMap maps[4];
          bool fast = true;
          const std::uint64_t ys[]{by.lo, by.hi}, xs[]{bx.lo, bx.hi};
          for (unsigned iy = 0; iy < 2; ++iy) {
            for (unsigned ix = 0; ix < 2; ++ix) {
              auto& m = maps[2 * iy + ix];
              if ((iy == 1 && by.fraction == 0) ||
                  (ix == 1 && bx.lo == bx.hi)) {
                continue;
              }
              q = {};
              q[0] = ys[iy];
              q[1] = xs[ix];
              m.identity = r.raw(2, 0, q) != 0;
              if (m.identity) {
                continue;
              }
              const auto& v = r.view(1, 0, q);
              if (v.layout->origin[2] || v.layout->shape[2] != c.bins) {
                fast = false;
                continue;
              }
              m.data = static_cast<const unsigned char*>(v.allocation) +
                       offset(*v.layout, v.byte_offset, q);
              m.stride = v.layout->strides[2];
            }
          }
          if (fast) {
            auto* destination = static_cast<unsigned char*>(out.allocation) +
                                offset(l, out.byte_offset, at);
            const double step =
                bx.lo == bx.hi
                    ? 0
                    : 1 / (center(bx.hi, c.cw, c.w) - center(bx.lo, c.cw, c.w));
            if (type == ElementType::Float32) {
              local_apply_span<float>(x.data, x.stride, destination,
                                      l.strides[1], n, maps, c.bins, c.clamp,
                                      by.fraction, bx.fraction, step);
            } else {
              local_apply_span<double>(x.data, x.stride, destination,
                                       l.strides[1], n, maps, c.bins, c.clamp,
                                       by.fraction, bx.fraction, step);
            }
            at[1] += n;
            continue;
          }
          for (std::uint64_t j = 0; j < n; ++j) {
            const double original = x.value(j), u = domain(original, c.clamp);
            double value = u;
            bool copy = identity;
            if (!local && !identity) {
              q = {};
              q[0] = bin(u, c.bins);
              value = r.value(1, 0, q);
            }
            if (local) {
              const auto bx = bracket(at[1] + j, c.cw, c.w);
              value = 0;
              copy = true;
              const std::uint64_t ys[]{by.lo, by.hi}, xs[]{bx.lo, bx.hi};
              const double wy[]{1 - by.fraction, by.fraction},
                  wx[]{1 - bx.fraction, bx.fraction};
              for (unsigned iy = 0; iy < 2; ++iy) {
                for (unsigned ix = 0; ix < 2; ++ix) {
                  const double weight = wy[iy] * wx[ix];
                  if (weight == 0) {
                    continue;
                  }
                  q = {};
                  q[0] = ys[iy];
                  q[1] = xs[ix];
                  const bool flag = r.raw(2, 0, q) != 0;
                  copy &= flag;
                  q[2] = bin(u, c.bins);
                  value += weight * (flag ? u : r.value(1, 0, q));
                }
              }
              value = copy ? u : std::clamp(value, 0.0, 1.0);
            }
            save(out, at, 1, j,
                 copy && u == original
                     ? x.raw(j)
                     : convert(value, type == ElementType::Float32));
          }
          at[1] += n;
        }
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    compute(k, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k,
              const ps_kernel_invocation_v9& i) const override {
    compute(k, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
OperationDefinition primitive(unsigned kind) {
  static constexpr const char* names[]{
      "grade.histogram", "grade.histogram_cdf_map", "grade.histogram_apply",
      "grade.clahe_maps", "grade.clahe_apply"};
  std::vector<std::pair<std::string, SchemaTemplate>> outputs;
  if (kind == 0) {
    outputs = {{"counts", tensor_schema(ElementType::Int64, {2})},
               {"count", tensor_schema(ElementType::Int64, {1})},
               {"mass", tensor_schema(ElementType::Float64, {1})}};
  } else if (kind == 1 || kind == 3) {
    outputs = {
        {kind == 1 ? "map" : "maps", tensor_schema(ElementType::Float64, {2})},
        {"identity", tensor_schema(ElementType::UInt8, {1})}};
  } else {
    outputs = {{"values", tensor_schema(ElementType::Float64, {1})}};
  }
  auto result = budgeted_operation(generation::operation(
      names[kind],
      kind == 1                ? 1
      : kind == 2 || kind == 4 ? 3
                               : 2,
      parameters(kind), outputs,
      [kind](const auto& in, const auto& p) -> std::shared_ptr<Program> {
        if (kind == 0 || kind == 3) {
          return std::make_shared<Generate>(kind, in, p);
        }
        if (kind == 1) {
          return std::make_shared<Cdf>(in, p);
        }
        return std::make_shared<Apply>(kind == 4, in, p);
      }));
  return kind == 0 || kind == 3
             ? collective_operation<Generate>(std::move(result))
             : result;
}
OperationDefinition complete(bool local) {
  auto proto = primitive(local ? 3 : 0);
  proto.key = local ? "grade.clahe" : "grade.histogram_equalize";
  proto.traits.outputs.resize(1);
  proto.traits.outputs[0].key = "values";
  return composition::operation(
      std::move(proto), [local](const auto& in, const auto& p) {
        composition::Graph g;
        const auto counts =
            g.add(in, primitive(local ? 3 : 0), {{-1, 0}, {-1, 1}}, p);
        int map = counts;
        if (!local) {
          Parameters cp;
          if (p.count("selection_mode")) {
            cp["selection_mode"] = p.at("selection_mode");
          }
          map = g.add(in, primitive(1), {{counts, 0}}, cp);
        }
        Parameters ap;
        for (const auto* key : {"edge", "cell_width", "cell_height"}) {
          if (p.count(key)) {
            ap[key] = p.at(key);
          }
        }
        const auto apply = g.add(in, primitive(local ? 4 : 2),
                                 {{-1, 0}, {map, 0}, {map, 1}}, ap);
        g.result = {apply, 0};
        return g;
      });
}
}  // namespace ps::plugin_internal::grade::histogram
namespace ps::plugin_internal {
Status register_grade_histogram(OperationRegistry* registry) {
  for (unsigned k = 0; k < 5; ++k) {
    const auto s = registry->register_operation(grade::histogram::primitive(k));
    if (!s.ok()) {
      return s;
    }
  }
  for (bool local : {false, true}) {
    const auto s =
        registry->register_operation(grade::histogram::complete(local));
    if (!s.ok()) {
      return s;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
