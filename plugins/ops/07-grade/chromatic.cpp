// Copyright 2026 Photospider Authors
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "07-grade/chromatic_math.hpp"
#include "07-grade/statistical_common.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::grade::chromatic {
using namespace statistics;  // NOLINT(build/namespaces)

/** GRD21--23. Immutable preparation contains only selectors and color matrices.
 * Runtime controls and fields are always read from this invocation's views.
 * Per-component spans preserve independent bypass/lightness dependencies.
 */
struct Chromatic final : Program {
  unsigned kind, axis, k = 0;
  ElementType type;
  bool normalized = true;
  std::vector<std::uint64_t> dims, selected;
  feature::Edit edit;
  Chromatic(unsigned member, const std::vector<OperationMetadata>& in,
            const Parameters& p)
      : kind(member) {
    const auto& t = tensor(in.at(0), true);
    dims = t.sample_shape();
    type = t.descriptor.element_type;
    const auto s =
        select(in[0], p, kind == 0 || kind == 4, kind == 5, kind == 2);
    axis = s.axis;
    selected = s.channels;
    if (kind == 0 || kind == 4) {
      try {
        rgb(&edit, s, p);
      } catch (const basis_ops::CheckedFailure& f) {
        throw f.status;
      } catch (const basis_ops::GeometryError&) {
        throw Status{ErrorCode::InvalidArgument, "invalid grading RGB basis"};
      }
    }
    if (kind <= 2) {
      typed(in.at(1), ElementType::Float64, {kind == 2 ? 2U : 1U});
    }
    if (kind == 3 || kind == 4 || kind == 6) {
      auto expected = dims;
      if (kind == 6) {
        const auto& w = tensor(in.at(1), true);
        require(w.sample_shape().size() == dims.size(),
                "selection weight rank");
        k = w.sample_shape()[axis];
        require(k >= 1 && k <= 256, "selection weight count");
        normalized = text(p, "overlap", "normalize") == "normalize";
        require(normalized || text(p, "overlap") == "sum", "overlap");
        expected[axis] = k;
        require(w.descriptor.element_type == ElementType::Float64 &&
                    w.sample_shape() == expected,
                "selection weight shape/dtype");
        typed(in.at(2), ElementType::Float64, {k, 3});
      } else {
        expected[axis] = 3;
        const auto& z = tensor(in.at(1), true);
        require(
            floating(z.descriptor.element_type) && z.sample_shape() == expected,
            "zone field shape/dtype");
        typed(in.at(2), ElementType::Float64,
              kind == 3 ? std::vector<std::uint64_t>{3, 2}
                        : std::vector<std::uint64_t>{3, 3, 2});
        if (kind == 3) {
          typed(in.at(3), ElementType::Float64, {3});
        }
      }
    }
    if (kind == 5 || kind == 7) {
      typed(in.at(1), ElementType::Float64, {kind == 5 ? 11U : 6U});
    }
    auto out = output_like(in[0]);
    if (mask_output()) {
      out.tensors[0].descriptor.element_type = ElementType::Float64;
      out.tensors[0].descriptor.shape[axis - t.batch_axes.size()] = 1;
    }
    check(out.validate());
    add_output(this, std::move(out));
  }
  bool mask_output() const { return kind == 5 || kind == 7; }
  int component(std::uint64_t c) const {
    const auto i = std::find(selected.begin(), selected.end(), c);
    return i == selected.end() ? -1 : static_cast<int>(i - selected.begin());
  }
  bool active(int c) const {
    return c >= 0 && !((kind >= 1 && kind <= 3) && c == 0);
  }
  std::vector<ReadRegion> support(unsigned, const Region& r) const override {
    if (empty(r)) {
      return {};
    }
    std::vector<ReadRegion> out;
    if (!mask_output()) {
      out.push_back({0, 0, r});
    }
    const auto d = r.dimensions()[axis];
    // Only selected components introduce arithmetic dependencies. A spatial
    // tile can retain a component axis much larger than a 32-bit counter.
    for (unsigned c = 0; c < (mask_output() ? 1U : selected.size()); ++c) {
      if (!mask_output() && (!active(c) || selected[c] < d.offset ||
                             selected[c] - d.offset >= d.extent)) {
        continue;
      }
      if (mask_output() || kind == 0 || kind == 2) {
        for (unsigned t = (kind == 2 ? 1 : 0); t < 3; ++t) {
          auto rd = r.dimensions();
          rd[axis] = {selected[t], 1};
          out.push_back({0, 0, Region(std::move(rd))});
        }
      }
      if (kind <= 2 || mask_output()) {
        out.push_back({1, 0,
                       Region({{0, kind == 2   ? 2U
                                   : kind == 5 ? 11U
                                   : kind == 7 ? 6U
                                               : 1U}})});
      } else {
        auto rd = r.dimensions();
        rd[axis] = {0, kind == 6 ? k : 3};
        out.push_back({1, 0, Region(std::move(rd))});
        if (kind == 3) {
          out.push_back(
              {2, 0, Region({{0, 3}, {static_cast<unsigned>(c - 1), 1}})});
          out.push_back({3, 0, Region({{0, 3}})});
        } else if (kind == 4) {
          out.push_back(
              {2, 0, Region({{0, 3}, {static_cast<unsigned>(c), 1}, {0, 2}})});
        } else {
          out.push_back(
              {2, 0, Region({{0, k}, {static_cast<unsigned>(c), 1}})});
        }
      }
    }
    return out;
  }
  template <class T, class Z, unsigned C>
  bool packed(const mask::Reader& reader, const ps_kernel_invocation_v8& inv,
              const ps_kernel_mutable_view_v8& out) const {
    const auto& l = *out.layout;
    if (!axis || axis + 1 != l.rank ||
        (selected.size() != 3 || selected[0] != 0 || selected[1] != 1 ||
         selected[2] != 2) ||
        dims[axis] != C || l.origin[axis] ||
        l.shape[axis] != (mask_output() ? 1U : C) ||
        l.strides[axis] != (mask_output() ? 8 : sizeof(T)) ||
        l.strides[axis - 1] != (mask_output() ? 8 : C * sizeof(T))) {
      return false;
    }
    double controls[768];
    Coordinate q{};
    bool copy = false;
    if (kind <= 2 || mask_output()) {
      const unsigned n = kind == 2 ? 2 : kind == 5 ? 11 : kind == 7 ? 6 : 1;
      for (unsigned i = 0; i < n; ++i) {
        q[0] = i;
        controls[i] = reader.value(1, 0, q);
      }
      copy = kind <= 1 ? controls[0] == 1 : kind == 2 && controls[0] == 0;
    } else {
      copy = true;
      for (unsigned z = 0; z < (kind == 6 ? k : 3); ++z) {
        double strength = 1;
        if (kind == 3) {
          q = {};
          q[0] = z;
          strength = reader.value(3, 0, q);
        }
        for (unsigned c = 0; c < (kind == 3 ? 2U : 3U); ++c) {
          q = {};
          q[0] = z;
          q[1] = c;
          if (kind == 4) {
            controls[6 * z + 2 * c] = reader.value(2, 0, q);
            q[2] = 1;
            controls[6 * z + 2 * c + 1] = reader.value(2, 0, q);
            copy &= controls[6 * z + 2 * c] == 0 &&
                    controls[6 * z + 2 * c + 1] == 0;
          } else {
            const unsigned index = (kind == 3 ? 2 : 3) * z + c;
            controls[index] =
                strength == 0 ? 0 : strength * reader.value(2, 0, q);
            copy &= controls[index] == 0;
          }
        }
      }
    }
    PreparedSaturation saturation;
    if constexpr (std::is_same_v<T, double>) {
      if (kind == 0 && !copy) {
        saturation.prepare(controls[0], edit.brightness.forward[1].data());
      }
    }
    const unsigned run = axis >= 2 && dims[axis - 1] == 1 &&
                                 l.shape[axis - 1] == 1 &&
                                 l.strides[axis - 2] == l.strides[axis - 1]
                             ? axis - 2
                             : axis - 1;
    std::uint64_t rows = 1;
    for (unsigned a = 0; a < run; ++a) {
      rows *= l.shape[a];
    }
    for (unsigned pass = 0; pass < 2; ++pass) {
      Coordinate at{};
      for (unsigned a = 0; a < l.rank; ++a) {
        at[a] = l.origin[a];
      }
      for (std::uint64_t row = 0; row < rows; ++row) {
        for (at[run] = l.origin[run]; at[run] < l.origin[run] + l.shape[run];) {
          cancel(inv);
          auto n = std::min<std::uint64_t>(
              4096, l.origin[run] + l.shape[run] - at[run]);
          const auto x = span(reader, 0, at, run, &n);
          const auto& v = reader.view(0, 0, at);
          if (v.layout->origin[axis] || v.layout->shape[axis] != C ||
              v.layout->strides[axis] != sizeof(T) ||
              x.stride != C * sizeof(T) ||
              reinterpret_cast<std::uintptr_t>(x.data) % alignof(T)) {
            return false;
          }
          Span z;
          if (!copy && (kind == 3 || kind == 4 || kind == 6)) {
            z = span(reader, 1, at, run, &n);
            const auto& f = reader.view(1, 0, at);
            const auto count = kind == 6 ? k : 3;
            if (f.layout->origin[axis] || f.layout->shape[axis] != count ||
                f.layout->strides[axis] != sizeof(Z) ||
                z.stride != count * sizeof(Z) ||
                reinterpret_cast<std::uintptr_t>(z.data) % alignof(Z)) {
              return false;
            }
          }
          auto* destination = static_cast<unsigned char*>(out.allocation) +
                              offset(l, out.byte_offset, at);
          if (reinterpret_cast<std::uintptr_t>(destination) %
              (mask_output() ? alignof(double) : alignof(T))) {
            return false;
          }
          if (pass) {
            if (copy) {
              std::memcpy(destination, x.data, n * C * sizeof(T));
            } else {
              const auto* input = reinterpret_cast<const T*>(x.data);
              const auto* weights = reinterpret_cast<const Z*>(z.data);
              auto* output = reinterpret_cast<T*>(destination);
              auto* mask = reinterpret_cast<double*>(destination);
              const auto* lum = edit.brightness.forward[1].data();
              switch (kind) {
                case 0:
                  packed_span<0, T, Z, C>(input, weights, output, mask, n,
                                          controls, lum, k, normalized,
                                          &saturation);
                  break;
                case 1:
                  packed_span<1, T, Z, C>(input, weights, output, mask, n,
                                          controls, lum, k, normalized);
                  break;
                case 2:
                  packed_span<2, T, Z, C>(input, weights, output, mask, n,
                                          controls, lum, k, normalized);
                  break;
                case 3:
                  packed_span<3, T, Z, C>(input, weights, output, mask, n,
                                          controls, lum, k, normalized);
                  break;
                case 4:
                  packed_span<4, T, Z, C>(input, weights, output, mask, n,
                                          controls, lum, k, normalized);
                  break;
                case 5:
                  packed_span<5, T, Z, C>(input, weights, output, mask, n,
                                          controls, lum, k, normalized);
                  break;
                case 6:
                  packed_span<6, T, Z, C>(input, weights, output, mask, n,
                                          controls, lum, k, normalized);
                  break;
                case 7:
                  packed_span<7, T, Z, C>(input, weights, output, mask, n,
                                          controls, lum, k, normalized);
                  break;
              }
            }
          }
          at[run] += n;
        }
        for (unsigned a = run; a-- > 0;) {
          if (++at[a] < l.origin[a] + l.shape[a]) {
            break;
          }
          at[a] = l.origin[a];
        }
      }
    }
    return true;
  }
  template <class T>
  bool packed_dispatch(const mask::Reader& reader,
                       const ps_kernel_invocation_v8& inv,
                       const ps_kernel_mutable_view_v8& out) const {
    const bool narrow =
        (kind == 3 || kind == 4) &&
        inputs[1].result_schema->tensors[0].descriptor.element_type ==
            ElementType::Float32;
    if (dims[axis] == 3) {
      return narrow ? packed<T, float, 3>(reader, inv, out)
                    : packed<T, double, 3>(reader, inv, out);
    }
    if (dims[axis] == 4) {
      return narrow ? packed<T, float, 4>(reader, inv, out)
                    : packed<T, double, 4>(reader, inv, out);
    }
    return false;
  }
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      const auto& l = *out.layout;
      std::uint64_t count = 1;
      for (unsigned a = 0; a < l.rank; ++a) {
        count *= l.shape[a];
      }
      if (!count) {
        continue;
      }
      work.poll(count * (kind == 6 ? 8 * k + 8 : 80));
      if (type == ElementType::Float32
              ? packed_dispatch<float>(reader, inv, out)
              : packed_dispatch<double>(reader, inv, out)) {
        continue;
      }
      // Iterate requested components independently. The fixed arrays contain
      // only descriptor-bounded controls, never image-sized hidden allocation.
      for (std::uint64_t channel = l.origin[axis];
           channel < l.origin[axis] + l.shape[axis]; ++channel) {
        const int c = mask_output() ? 0 : component(channel);
        const bool calculate = mask_output() || active(c);
        double controls[768];
        bool copies = !calculate;
        Coordinate q{};
        if (calculate) {
          if (kind <= 2 || mask_output()) {
            const unsigned n = kind == 2   ? 2
                               : kind == 5 ? 11
                               : kind == 7 ? 6
                                           : 1;
            for (unsigned j = 0; j < n; ++j) {
              q[0] = j;
              controls[j] = reader.value(1, 0, q);
            }
            copies =
                kind <= 1 ? controls[0] == 1 : kind == 2 && controls[0] == 0;
          } else {
            copies = true;
            for (unsigned j = 0; j < (kind == 6 ? k : 3); ++j) {
              q = {};
              q[0] = j;
              q[1] = kind == 3 ? c - 1 : c;
              if (kind == 3) {
                Coordinate s{};
                s[0] = j;
                const double strength = reader.value(3, 0, s);
                controls[j] =
                    strength == 0 ? 0 : strength * reader.value(2, 0, q);
                copies &= controls[j] == 0;
              } else if (kind == 4) {
                controls[2 * j] = reader.value(2, 0, q);
                q[2] = 1;
                controls[2 * j + 1] = reader.value(2, 0, q);
                copies &= controls[2 * j] == 0 && controls[2 * j + 1] == 0;
              } else {
                controls[j] = reader.value(2, 0, q);
                copies &= controls[j] == 0;
              }
            }
          }
        }
        ColorSelection selection;
        PreparedSaturation saturation;
        if (kind == 0 && type == ElementType::Float64 && !copies) {
          saturation.prepare(controls[0], edit.brightness.forward[1].data());
        }
        if (kind == 5) {
          selection.prepare(controls);
        }
        const unsigned run =
            axis + 1 == l.rank && l.rank > 1 ? l.rank - 2 : l.rank - 1;
        Coordinate at{};
        std::uint64_t rows = 1;
        for (unsigned a = 0; a < l.rank; ++a) {
          at[a] = l.origin[a];
          if (a != axis && a != run) {
            rows *= l.shape[a];
          }
        }
        at[axis] = channel;
        for (std::uint64_t row = 0; row < rows; ++row) {
          const auto end =
              run == axis ? channel + 1 : l.origin[run] + l.shape[run];
          for (at[run] = run == axis ? channel : l.origin[run];
               at[run] < end;) {
            cancel(inv);
            auto n = std::min<std::uint64_t>(2048, end - at[run]);
            Span source, tuple[3], weights[256];
            if (!mask_output()) {
              source = span(reader, 0, at, run, &n);
            }
            if (!copies && (mask_output() || kind == 0 || kind == 2)) {
              for (unsigned t = kind == 2 ? 1 : 0; t < 3; ++t) {
                q = at;
                q[axis] = selected[t];
                tuple[t] = span(reader, 0, q, run, &n);
              }
            }
            if (!copies && (kind == 3 || kind == 4 || kind == 6)) {
              for (unsigned j = 0; j < (kind == 6 ? k : 3); ++j) {
                q = at;
                q[axis] = j;
                weights[j] = span(reader, 1, q, run, &n);
              }
            }
            for (std::uint64_t j = 0; j < n; ++j) {
              if (copies) {
                save(out, at, run, j, source.raw(j));
                continue;
              }
              const double x = mask_output() ? 0 : source.value(j);
              double y = x;
              bool copy = false;
              if (kind == 0) {
                const fast_color::Triple t{tuple[0].value(j), tuple[1].value(j),
                                           tuple[2].value(j)};
                copy = t[0] == t[1] && t[1] == t[2];
                double lum =
                    saturation.active
                        ? saturation.offset(t[0], t[1], t[2])
                        : fast_color::dot(edit.brightness.forward[1], t);
                const bool prepared_pixel =
                    saturation.active &&
                    (saturation.safe_offset || std::isfinite(lum));
                if (saturation.active && !prepared_pixel) {
                  const auto& w = edit.brightness.forward[1];
                  lum = w[0] * t[0] + w[1] * t[1] + w[2] * t[2];
                }
                y = prepared_pixel ? std::fma(controls[0], x, lum)
                                   : lum + controls[0] * (x - lum);
              } else if (kind == 1) {
                y = x * controls[0];
              } else if (kind == 2) {
                const double chroma =
                    std::hypot(tuple[1].value(j), tuple[2].value(j));
                copy = chroma >= controls[1];
                const double t = 1 - std::min(1.0, chroma / controls[1]);
                y = x * (1 + controls[0] * t * t);
              } else if (kind == 3 || kind == 4 || kind == 6) {
                double sum = 0, delta = 0, ev = 0;
                for (unsigned z = 0; z < (kind == 6 ? k : 3); ++z) {
                  const double w = weights[z].value(j);
                  sum += w;
                  delta += w * controls[kind == 4 ? 2 * z + 1 : z];
                  if (kind == 4) {
                    ev += w * controls[2 * z];
                  }
                }
                const double den =
                    kind == 6 ? (normalized ? std::max(1.0, sum) : 1) : sum;
                if (kind == 4) {
                  copy = ev == 0 && delta == 0;
                  y = x * std::exp2(ev / den) + delta / den;
                } else {
                  copy = delta == 0;
                  y = x + delta / den;
                }
              } else {
                const double light = tuple[0].value(j);
                if (kind == 5) {
                  y = selection(light, tuple[1].value(j), tuple[2].value(j));
                } else {
                  const double a = tuple[1].value(j), b = tuple[2].value(j);
                  y = neutral_chroma_weight(a, b, std::hypot(a, b), controls[0],
                                            controls[1]) *
                      plateau(light, controls + 2);
                }
              }
              save(out, at, run, j,
                   copy ? source.raw(j)
                        : convert(y, !mask_output() &&
                                         type == ElementType::Float32));
            }
            at[run] += n;
          }
          for (unsigned a = l.rank; a-- > 0;) {
            if (a == axis || a == run) {
              continue;
            }
            if (++at[a] < l.origin[a] + l.shape[a]) {
              break;
            }
            at[a] = l.origin[a];
          }
        }
      }
    }
    cancel(inv);
  }
  void run(const Kernel& k0, const ps_kernel_invocation_v8& i) const override {
    compute(k0, i, Work{i, work_limit});
  }
  void run_v9(const Kernel& k0,
              const ps_kernel_invocation_v9& i) const override {
    compute(k0, i.views, Work{i.views, work_limit, 0, i.work});
  }
};
OperationDefinition operation(unsigned kind) {
  using P = OperationParameterType;
  static constexpr const char* names[]{"grade.saturation_linear",
                                       "grade.saturation_opponent",
                                       "grade.vibrance_oklab",
                                       "grade.split_tone_opponent",
                                       "grade.color_wheels_rgb",
                                       "grade.select_color_region",
                                       "grade.selective_opponent_adjust",
                                       "grade.select_neutral_region"};
  auto p = selectors();
  if (kind == 6) {
    p.push_back({"overlap", P::String, false});
  }
  return budgeted_operation(generation::operation(
      names[kind],
      kind == 3                ? 4
      : kind == 4 || kind == 6 ? 3
                               : 2,
      std::move(p),
      {{kind == 5 || kind == 7 ? "mask" : "values",
        tensor_schema(ElementType::Float64, {1})}},
      [kind](const auto& in, const auto& params) {
        return std::make_shared<Chromatic>(kind, in, params);
      }));
}
}  // namespace ps::plugin_internal::grade::chromatic
namespace ps::plugin_internal {
Status register_grade_chromatic(OperationRegistry* registry) {
  for (unsigned k = 0; k < 8; ++k) {
    const auto s = registry->register_operation(grade::chromatic::operation(k));
    if (!s.ok()) {
      return s;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
