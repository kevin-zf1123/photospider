// Copyright 2026 Photospider Authors
#include <algorithm>
#include <array>
#include <cmath>
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
struct Point final : Program {
  static constexpr bool kLab = K == Kind::Lab76 || K == Kind::LabChroma;
  static constexpr bool kSpill = K == Kind::Green || K == Kind::Directional;
  static constexpr bool kColorOut = kSpill || K == Kind::Foreground;
  ElementType type;
  Color color;
  std::vector<std::uint64_t> dims, spatial;
  bool invert = false;

  Point(const std::vector<OperationMetadata>& in, const Parameters& p) {
    const auto& first = tensor(in[0]);
    type = first.descriptor.element_type;
    require(floating(type), "KEY requires Float32/64");
    dims = first.sample_shape();
    if constexpr (K == Kind::Range) {
      require(dims.size() == 2, "KEY range requires [H,W]");
      for (unsigned port = 1; port < 4; ++port) {
        check_shape(in[port], {1}, type);
      }
      if (p.count("invert")) {
        invert = std::get<bool>(p.at("invert"));
      }
      add_output(this, output_schema(in[0]));
    } else {
      color = Color(in[0], p, kLab, false, !kLab);
      spatial = dims;
      spatial.erase(spatial.begin() + color.axis);
      if constexpr (!kSpill) {
        require(dims.size() == 3 && color.count == 3,
                "KEY color-only input requires logical [H,W,3]");
      }
      if constexpr (kLab) {
        check_shape(in[1], {3}, type);
        check_shape(in[2], {1}, type);
        check_shape(in[3], {1}, type);
      } else if constexpr (K == Kind::Difference) {
        check_shape(in[1], dims, type);
        (void)Color(in[1], p, false, false, true);
        check_shape(in[2], {1}, type);
        check_shape(in[3], {1}, type);
      } else if constexpr (K == Kind::Foreground) {
        check_shape(in[1], spatial, type);
        for (unsigned port : {2U, 3U}) {
          check_shape(in[port], dims, type);
          (void)Color(in[port], p, false, false, true);
        }
        check_shape(in[4], {1}, type);
      } else {
        auto mask = dims;
        mask[color.axis] = 1;
        check_shape(in[K == Kind::Green ? 1 : 2], mask, type);
        check_shape(in[K == Kind::Green ? 2 : 3], {1}, type);
        if constexpr (K == Kind::Directional) {
          check_shape(in[1], {3}, type);
        }
      }
      if constexpr (kColorOut) {
        add_output(this, output_schema(in[0]));
      } else {
        add_output(this, plane_schema(type, spatial[0], spatial[1]));
      }
    }
  }

  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    if (empty(q)) {
      return {};
    }
    std::vector<ReadRegion> result;
    const auto scalar = [&](unsigned p, unsigned first = 0, unsigned n = 1) {
      result.push_back({p, 0, Region({{first, n}})});
    };
    if constexpr (K == Kind::Range) {
      result.push_back({0, 0, q});
      scalar(1);
      scalar(2);
      scalar(3);
    } else {
      auto d = q.dimensions();
      if constexpr (!kColorOut) {
        d.insert(d.begin() + color.axis, {0, color.count});
      }
      const auto add = [&](unsigned port, std::uint64_t c) {
        auto r = d;
        r[color.axis] = {c, 1};
        result.push_back({port, 0, Region(std::move(r))});
      };
      if constexpr (kLab || K == Kind::Difference) {
        for (unsigned c = K == Kind::LabChroma ? 1 : 0; c < 3; ++c) {
          add(0, color.channels[c]);
          if constexpr (K == Kind::Difference) {
            add(1, color.channels[c]);
          }
        }
        if constexpr (kLab) {
          scalar(1, K == Kind::LabChroma ? 1 : 0, K == Kind::LabChroma ? 2 : 3);
        }
        scalar(2);
        scalar(3);
      } else if constexpr (K == Kind::Foreground) {
        for (unsigned p : {0U, 2U, 3U}) {
          result.push_back({p, 0, q});
        }
        auto plane = d;
        plane.erase(plane.begin() + color.axis);
        result.push_back({1, 0, Region(std::move(plane))});
        scalar(4);
      } else {
        result.push_back({0, 0, q});
        bool active = false;
        const auto c = d[color.axis];
        for (unsigned j = K == Kind::Green ? 1 : 0;
             j < (K == Kind::Green ? 2U : 3U); ++j) {
          active |= color.channels[j] >= c.offset &&
                    color.channels[j] - c.offset < c.extent;
        }
        if (active) {
          for (auto channel : color.channels) {
            add(0, channel);
          }
          auto mask = d;
          mask[color.axis] = {0, 1};
          result.push_back(
              {K == Kind::Green ? 1U : 2U, 0, Region(std::move(mask))});
          scalar(K == Kind::Green ? 2 : 3);
          if constexpr (K == Kind::Directional) {
            scalar(1, 0, 3);
          }
        }
      }
    }
    return result;
  }

  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    T controls[5]{};
    if constexpr (K == Kind::Range) {
      for (unsigned p = 1; p < 4; ++p) {
        controls[p - 1] = load<T>(reader, p, {});
      }
    } else if constexpr (kLab || K == Kind::Difference) {
      controls[0] = load<T>(reader, 2, {});
      controls[1] = load<T>(reader, 3, {});
      if constexpr (kLab) {
        for (unsigned c = K == Kind::LabChroma ? 1 : 0; c < 3; ++c) {
          controls[2 + c] = load<T>(reader, 1, {c});
        }
      }
    }
    std::array<double, 3> direction{};
    bool direction_ready = false;
    const auto prepare_direction = [&] {
      if (direction_ready) {
        return;
      }
      double spill[3];
      for (unsigned c = 0; c < 3; ++c) {
        spill[c] = load<T>(reader, 1, {c});
      }
      const double y =
          (color.weights[0] * spill[0] + color.weights[1] * spill[1]) +
          color.weights[2] * spill[2];
      for (unsigned c = 0; c < 3; ++c) {
        direction[c] = spill[c] - y;
      }
      const double norm = std::hypot(direction[0], direction[1], direction[2]);
      for (auto& d : direction) {
        d /= norm;
      }
      direction_ready = true;
    };
    for (unsigned oi = 0; oi < inv.output_count; ++oi) {
      const auto& out = inv.outputs[oi];
      const auto& l = *out.layout;
      const unsigned component = kColorOut ? color.axis : l.rank;
      // A rank-one color tuple has no axis along which to batch pixels.
      const bool single_tuple = kColorOut && l.rank == 1;
      const unsigned xaxis =
          l.rank > 1 && l.rank - 1 == component ? l.rank - 2 : l.rank - 1;
      Coordinate at{};
      std::uint64_t pixels = 1;
      for (unsigned a = 0; a < l.rank; ++a) {
        at[a] = l.origin[a];
        if (a != component) {
          pixels *= l.shape[a];
        }
      }
      if (!pixels) {
        continue;
      }
      bool modifies = true;
      if constexpr (kSpill) {
        modifies = false;
        for (unsigned c = K == Kind::Green ? 1 : 0;
             c < (K == Kind::Green ? 2U : 3U); ++c) {
          modifies |=
              color.channels[c] >= l.origin[component] &&
              color.channels[c] - l.origin[component] < l.shape[component];
        }
        if (modifies) {
          controls[0] = load<T>(reader, K == Kind::Green ? 2 : 3, {});
        }
      }
      for (std::uint64_t done = 0; done < pixels;) {
        cancel(inv);
        auto n = single_tuple
                     ? UINT64_C(1)
                     : std::min<std::uint64_t>(
                           4096, l.origin[xaxis] + l.shape[xaxis] - at[xaxis]);
        const auto source_at =
            K == Kind::Range || kColorOut ? at : insert_axis(at, color.axis);
        const unsigned input_x = K == Kind::Range || kColorOut
                                     ? xaxis
                                     : xaxis + (xaxis >= color.axis);
        const unsigned output_width = kColorOut ? color.count : 1;
        auto* destination = static_cast<unsigned char*>(out.allocation) +
                            offset(l, out.byte_offset, at);
        bool fast =
            !single_tuple && (K == Kind::Range || color.packed) &&
            l.strides[xaxis] ==
                static_cast<std::int64_t>(output_width * sizeof(T)) &&
            reinterpret_cast<std::uintptr_t>(destination) % alignof(T) == 0;
        if constexpr (kColorOut) {
          fast &= !l.origin[component] && l.shape[component] == color.count &&
                  l.strides[component] == sizeof(T);
        }
        const T* ptr[4]{};
        const T* scalar_controls = controls;
        const auto image_pointer = [&](unsigned port) -> const T* {
          auto pos = source_at;
          if constexpr (K != Kind::Range) {
            pos[color.axis] = K == Kind::LabChroma ? color.channels[1] : 0;
            const auto& v = reader.view(port, 0, pos);
            if (v.layout->origin[color.axis] != 0 ||
                v.layout->shape[color.axis] != color.count ||
                v.layout->strides[color.axis] != sizeof(T)) {
              return nullptr;
            }
            pos[color.axis] = 0;
          }
          return pointer<T>(reader, port, pos, input_x,
                            K == Kind::Range ? 1 : color.count, &n);
        };
        if (fast) {
          ptr[0] = image_pointer(0);
          fast &= ptr[0] != nullptr;
          if constexpr (K == Kind::Difference) {
            fast &= (ptr[1] = image_pointer(1)) != nullptr;
          }
          if constexpr (K == Kind::Foreground) {
            auto a = remove_axis(at, color.axis);
            fast &=
                (ptr[1] = pointer<T>(reader, 1, a, xaxis - (xaxis > color.axis),
                                     1, &n)) != nullptr;
            fast &= (ptr[2] = image_pointer(2)) != nullptr;
            fast &= (ptr[3] = image_pointer(3)) != nullptr;
            const auto& v = reader.view(4, 0, {});
            const auto* p =
                static_cast<const unsigned char*>(v.allocation) + v.byte_offset;
            fast &= reinterpret_cast<std::uintptr_t>(p) % alignof(T) == 0;
            scalar_controls = reinterpret_cast<const T*>(p);
          }
          if constexpr (kSpill) {
            if (controls[0] != T(0)) {
              auto a = at;
              a[color.axis] = 0;
              fast &= (ptr[1] = pointer<T>(reader, K == Kind::Green ? 1 : 2, a,
                                           xaxis, 1, &n)) != nullptr;
              if constexpr (K == Kind::Directional) {
                if (fast) {
                  for (std::uint64_t j = 0; j < n; ++j) {
                    if (controls[0] * ptr[1][j] != T(0)) {
                      prepare_direction();
                      break;
                    }
                  }
                }
              }
            }
          }
        }
        if (fast) {
          work.poll(n * 64);
          if constexpr (kSpill) {
            if (output_width == 3) {
              point_span<K, T, 3>(ptr, reinterpret_cast<T*>(destination), n,
                                  scalar_controls, 3, direction, color.weights,
                                  invert);
            } else if (output_width == 4) {
              point_span<K, T, 4>(ptr, reinterpret_cast<T*>(destination), n,
                                  scalar_controls, 4, direction, color.weights,
                                  invert);
            } else {
              point_span<K>(ptr, reinterpret_cast<T*>(destination), n,
                            scalar_controls, output_width, direction,
                            color.weights, invert);
            }
          } else {
            point_span<K>(ptr, reinterpret_cast<T*>(destination), n,
                          scalar_controls, output_width, direction,
                          color.weights, invert);
          }
        } else {
          n = 1;
          work.poll(64);
          if constexpr (kSpill) {
            auto position = at;
            T factor = 0;
            if (modifies && controls[0] != T(0)) {
              position[color.axis] = 0;
              factor = controls[0] *
                       load<T>(reader, K == Kind::Green ? 1 : 2, position);
            }
            double correction[3]{};
            if (factor != T(0)) {
              T rgb[3];
              for (unsigned c = 0; c < 3; ++c) {
                position[color.axis] = color.channels[c];
                rgb[c] = load<T>(reader, 0, position);
              }
              if constexpr (K == Kind::Green) {
                correction[1] =
                    factor * std::max(T(0), rgb[1] - std::max(rgb[0], rgb[2]));
              } else {
                prepare_direction();
                const double y =
                    (color.weights[0] * rgb[0] + color.weights[1] * rgb[1]) +
                    color.weights[2] * rgb[2];
                const double e = std::max(0.0, (direction[0] * (rgb[0] - y) +
                                                direction[1] * (rgb[1] - y)) +
                                                   direction[2] * (rgb[2] - y));
                for (unsigned c = 0; c < 3; ++c) {
                  correction[c] =
                      static_cast<double>(factor) * e * direction[c];
                }
              }
            }
            for (auto c = l.origin[component];
                 c < l.origin[component] + l.shape[component]; ++c) {
              position = at;
              position[component] = c;
              const auto raw = reader.raw(0, 0, position);
              const auto found =
                  std::find(color.channels.begin(), color.channels.end(), c);
              if (found == color.channels.end() ||
                  correction[found - color.channels.begin()] == 0) {
                store(out, position, raw);
              } else {
                T value;
                std::memcpy(&value, &raw, sizeof(T));
                save(out, position,
                     static_cast<T>(
                         value - correction[found - color.channels.begin()]));
              }
            }
          } else if constexpr (K == Kind::Foreground) {
            const T a = load<T>(reader, 1, remove_axis(at, color.axis));
            T lambda = 0;
            if (a != T(0) && a != T(1)) {
              lambda = load<T>(reader, 4, {});
            }
            for (auto c = l.origin[component];
                 c < l.origin[component] + l.shape[component]; ++c) {
              auto pos = at;
              pos[component] = c;
              if (a == T(0) || a == T(1)) {
                store(out, pos, reader.raw(a == T(0) ? 3 : 0, 0, pos));
              } else {
                const T value = (a * (load<T>(reader, 0, pos) -
                                      (T(1) - a) * load<T>(reader, 2, pos)) +
                                 lambda * load<T>(reader, 3, pos)) /
                                (a * a + lambda);
                save(out, pos, value);
              }
            }
          } else {
            T a[3]{}, b[3]{}, value;
            if constexpr (K == Kind::Range) {
              a[0] = load<T>(reader, 0, at);
            } else {
              for (unsigned c = K == Kind::LabChroma ? 1 : 0; c < 3; ++c) {
                auto pos = source_at;
                pos[color.axis] = color.channels[c];
                a[c] = load<T>(reader, 0, pos);
                if constexpr (K == Kind::Difference) {
                  b[c] = load<T>(reader, 1, pos);
                }
              }
            }
            const T* inputs[4]{a, b};
            point_span<K>(inputs, &value, 1, controls, 1, direction,
                          color.weights, invert);
            save(out, at, value);
          }
        }
        done += n;
        advance(&at, l, component, n);
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

template <Kind K>
OperationDefinition point_operation(const char* key, unsigned inputs,
                                    const char* output) {
  using P = OperationParameterType;
  auto p = K == Kind::Range
               ? std::vector<OperationParameterSpec>{{"invert", P::Bool, false}}
               : color_parameters(Point<K>::kLab);
  return budgeted_operation(generation::operation(
      key, inputs, std::move(p),
      {{output, tensor_schema(ElementType::Float64, {1})}},
      [](const auto& in, const auto& p) {
        return std::make_shared<Point<K>>(in, p);
      }));
}
}  // namespace ps::plugin_internal::keying

namespace ps::plugin_internal {
Status register_keying_point(OperationRegistry* registry) {
  using keying::Kind;
  using keying::point_operation;
  for (auto op :
       {point_operation<Kind::Range>("keying.range_select", 4, "matte"),
        point_operation<Kind::Lab76>("keying.lab76_chroma_key", 4, "matte"),
        point_operation<Kind::LabChroma>("keying.lab_chroma_key", 4, "matte"),
        point_operation<Kind::Difference>("keying.difference_rgb_l2", 4,
                                          "matte"),
        point_operation<Kind::Foreground>("keying.foreground_regularized", 5,
                                          "foreground"),
        point_operation<Kind::Green>("keying.despill_green_excess", 3,
                                     "foreground"),
        point_operation<Kind::Directional>("keying.despill_directional", 4,
                                           "foreground")}) {
    auto status = registry->register_operation(std::move(op));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
