// Copyright 2026 Photospider Authors
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "09-composite/keying_common.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::keying {
struct Matting final : Program {
  ElementType type;
  Color color;
  std::vector<std::uint64_t> dims;
  std::uint64_t h, w, maximum_iterations;
  double atol, rtol;
  Matting(const std::vector<OperationMetadata>& in, const Parameters& p) {
    const auto& a = tensor(in[0]);
    dims = a.sample_shape();
    type = a.descriptor.element_type;
    require(dims.size() == 3 && floating(type), "KEY matting image rank/dtype");
    color = Color(in[0], p, false, false, true);
    require(color.count == 3, "KEY matting requires RGB-only image");
    auto plane = dims;
    plane.erase(plane.begin() + color.axis);
    h = plane[0];
    w = plane[1];
    check_shape(in[1], plane, ElementType::UInt8);
    check_shape(in[2], {1}, type);
    atol = real(p, "solve_atol", 1e-12);
    rtol = real(p, "solve_rtol", 1e-8);
    maximum_iterations =
        static_cast<std::uint64_t>(integer(p, "max_iterations", 4096));
    collective = true;
    add_output(this, plane_schema(type, h, w));
    add_output(this, tensor_schema(ElementType::Float64, {1}));
    const unsigned __int128 n = static_cast<unsigned __int128>(h) * w;
    scratch_bytes =
        mask::bytes(n * (1 + 2 * sizeof(std::int64_t) +
                         3 * generation::width(type) + 31 * sizeof(double)) +
                    256 * 256 * sizeof(double) + 256);
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    return {{0, 0, Region::whole(dims)},
            {1, 0, Region::whole({h, w})},
            {2, 0, Region::whole({1})}};
  }
  std::uint64_t image_index(std::uint64_t y, std::uint64_t x,
                            unsigned c) const {
    const auto a = insert_axis(Coordinate{y, x}, color.axis, color.channels[c]);
    return (a[0] * dims[1] + a[1]) * dims[2] + a[2];
  }
  template <class T>
  void compute(const Kernel& kernel, const ps_kernel_invocation_v8& inv,
               const Work& work) const {
    const mask::Reader reader{kernel, inv};
    Arena arena(inv);
    const auto n = h * w;
    auto* label_buffer = arena.take<std::uint8_t>(n);
    const auto* labels =
        gather<std::uint8_t>(reader, 1, {h, w}, label_buffer, work);
    bool needs_solve = false;
    for (unsigned o = 0; o < inv.output_count; ++o) {
      needs_solve |= inv.outputs[o].port == 1;
    }
    for (unsigned o = 0; !needs_solve && o < inv.output_count; ++o) {
      if (inv.outputs[o].port == 1) {
        needs_solve = true;
        break;
      }
      samples(inv.outputs[o], [&](const Coordinate& at, std::uint64_t i) {
        if ((i & 4095) == 0) {
          work.poll(4096);
        }
        needs_solve |= labels[at[0] * w + at[1]] == 1;
      });
    }
    std::uint64_t unknowns = 0, anchors = 0;
    std::uint64_t min_y = h, min_x = w, max_y = 0, max_x = 0;
    auto* map = arena.take<std::int64_t>(n);
    auto* positions = arena.take<std::int64_t>(n);
    if (needs_solve) {
      for (std::uint64_t y = 0; y < h; ++y) {
        work.poll(w);
        for (std::uint64_t x = 0; x < w; ++x) {
          if ((x & 4095) == 0) {
            cancel(inv);
          }
          const auto i = y * w + x;
          if (labels[i] == 1) {
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            map[i] = static_cast<std::int64_t>(unknowns);
            positions[unknowns++] = static_cast<std::int64_t>(i);
          } else {
            map[i] = -1;
            ++anchors;
          }
        }
      }
    }
    double residual = 0;
    double* solution = nullptr;
    if (unknowns) {
      if (!anchors) {
        fail("NoSolution: KEY matting has no known anchor");
      }
      auto* image_buffer = arena.take<T>(3 * n);
      const auto* image = gather<T>(reader, 0, dims, image_buffer, work);
      const double epsilon = load<T>(reader, 2, {});
      auto* matrix = arena.take<double>(25 * unknowns);
      auto* rhs = arena.take<double>(unknowns);
      solution = arena.take<double>(unknowns);
      auto* r = arena.take<double>(unknowns);
      auto* z = arena.take<double>(unknowns);
      auto* direction = arena.take<double>(unknowns);
      auto* product = arena.take<double>(unknowns);
      work.poll(26 * unknowns);
      const auto checkpoint = [&](std::uint64_t) { cancel(inv); };
      fill_elements(matrix, 25 * unknowns, 0.0, checkpoint);
      fill_elements(rhs, unknowns, 0.0, checkpoint);
      // Only windows touching an unknown can contribute to the reduced system.
      const auto cy0 = min_y ? min_y - 1 : 0, cy1 = std::min(h, max_y + 2);
      const auto cx0 = min_x ? min_x - 1 : 0, cx1 = std::min(w, max_x + 2);
      for (std::uint64_t cy = cy0; cy < cy1; ++cy) {
        work.poll((cx1 - cx0) * 9);
        for (std::uint64_t cx = cx0; cx < cx1; ++cx) {
          if ((cx & 4095) == 0) {
            cancel(inv);
          }
          const auto y0 = cy ? cy - 1 : 0, y1 = std::min(h, cy + 2);
          const auto x0 = cx ? cx - 1 : 0, x1 = std::min(w, cx + 2);
          std::uint64_t at[9]{};
          unsigned slot[9]{};
          unsigned count = 0, active = 0;
          for (auto y = y0; y < y1; ++y) {
            for (auto x = x0; x < x1; ++x) {
              at[count] = y * w + x;
              slot[count++] = static_cast<unsigned>(5 * (y - y0) + x - x0);
              active += labels[y * w + x] == 1;
            }
          }
          if (!active) {
            continue;
          }
          work.poll(256 + active * count * 24);
          double v[9][3]{}, mean[3]{};
          for (unsigned c = 0; c < 3; ++c) {
            const double origin = image[image_index(y0, x0, c)];
            for (unsigned i = 0; i < count; ++i) {
              v[i][c] = static_cast<double>(
                            image[image_index(at[i] / w, at[i] % w, c)]) -
                        origin;
              mean[c] += v[i][c];
            }
            mean[c] /= count;
            for (unsigned i = 0; i < count; ++i) {
              v[i][c] -= mean[c];
            }
          }
          double cov[3][3]{};
          for (unsigned i = 0; i < count; ++i) {
            for (unsigned a = 0; a < 3; ++a) {
              for (unsigned b = 0; b <= a; ++b) {
                cov[a][b] += v[i][a] * v[i][b];
              }
            }
          }
          for (unsigned a = 0; a < 3; ++a) {
            cov[a][a] += epsilon;
          }
          // Cholesky whitening computes centered^T * inverse(cov) * centered
          // without explicitly forming the inverse.
          double lower[3][3]{};
          for (unsigned a = 0; a < 3; ++a) {
            for (unsigned b = 0; b <= a; ++b) {
              double value = cov[a][b];
              for (unsigned k = 0; k < b; ++k) {
                value -= lower[a][k] * lower[b][k];
              }
              if (a == b) {
                if (!(value > 0) || !std::isfinite(value)) {
                  fail("NotConverged: KEY matting covariance factorization");
                }
                lower[a][b] = std::sqrt(value);
              } else {
                lower[a][b] = value / lower[b][b];
              }
            }
          }
          for (unsigned i = 0; i < count; ++i) {
            for (unsigned a = 0; a < 3; ++a) {
              for (unsigned b = 0; b < a; ++b) {
                v[i][a] -= lower[a][b] * v[i][b];
              }
              v[i][a] /= lower[a][a];
            }
          }
          for (unsigned i = 0; i < count; ++i) {
            if (map[at[i]] < 0) {
              continue;
            }
            const auto row = static_cast<std::uint64_t>(map[at[i]]);
            for (unsigned j = 0; j < count; ++j) {
              const double dot =
                  (v[i][0] * v[j][0] + v[i][1] * v[j][1]) + v[i][2] * v[j][2];
              const double value = (i == j ? 1.0 : 0.0) - 1.0 / count - dot;
              if (map[at[j]] < 0) {
                if (labels[at[j]] == 2) {
                  rhs[row] -= value;
                }
              } else {
                matrix[25 * row + 12 - slot[i] + slot[j]] += value;
              }
            }
          }
        }
      }
      const auto multiply = [&](const double* x, double* out) {
        for (std::uint64_t i = 0; i < unknowns; ++i) {
          if ((i & 4095) == 0) {
            cancel(inv);
          }
          if ((i & 1023) == 0) {
            work.poll(std::min<std::uint64_t>(1024, unknowns - i) * 50);
          }
          const auto y = positions[i] / static_cast<std::int64_t>(w);
          const auto x0 = positions[i] % static_cast<std::int64_t>(w);
          double sum = 0;
          for (int dy = -2; dy <= 2; ++dy) {
            if (y + dy < 0 || y + dy >= static_cast<std::int64_t>(h)) {
              continue;
            }
            for (int dx = -2; dx <= 2; ++dx) {
              if (x0 + dx < 0 || x0 + dx >= static_cast<std::int64_t>(w)) {
                continue;
              }
              const auto j = map[(y + dy) * w + x0 + dx];
              if (j >= 0) {
                sum += matrix[25 * i + (dy + 2) * 5 + dx + 2] * x[j];
              }
            }
          }
          out[i] = sum;
        }
      };
      double matrix_norm = 0, rhs_norm = 0;
      for (std::uint64_t i = 0; i < unknowns; ++i) {
        if ((i & 4095) == 0) {
          cancel(inv);
        }
        double sum = 0;
        for (unsigned j = 0; j < 25; ++j) {
          sum += std::abs(matrix[25 * i + j]);
        }
        matrix_norm = std::max(matrix_norm, sum);
        rhs_norm = std::max(rhs_norm, std::abs(rhs[i]));
        solution[i] = 0.5;
      }
      const auto measure = [&] {
        multiply(solution, product);
        residual = 0;
        double xnorm = 0;
        for (std::uint64_t i = 0; i < unknowns; ++i) {
          if ((i & 4095) == 0) {
            cancel(inv);
          }
          r[i] = rhs[i] - product[i];
          residual = std::max(residual, std::abs(r[i]));
          xnorm = std::max(xnorm, std::abs(solution[i]));
          if (!std::isfinite(r[i]) || !std::isfinite(solution[i])) {
            fail("NotConverged: KEY matting nonfinite residual");
          }
        }
        const double scale = matrix_norm * xnorm + rhs_norm;
        return std::pair<double, double>{
            atol + rtol * scale,
            std::min(atol + rtol * scale, 1e-14 + 1e-12 * scale)};
      };
      bool converged = false;
      if (unknowns <= 256) {
        auto* dense = arena.take<double>(unknowns * unknowns);
        work.poll(unknowns * unknowns);
        fill_elements(dense, unknowns * unknowns, 0.0, checkpoint);
        for (std::uint64_t i = 0; i < unknowns; ++i) {
          if ((i & 4095) == 0) {
            cancel(inv);
          }
          const auto y = positions[i] / static_cast<std::int64_t>(w);
          const auto x0 = positions[i] % static_cast<std::int64_t>(w);
          for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
              if (y + dy < 0 || y + dy >= static_cast<std::int64_t>(h) ||
                  x0 + dx < 0 || x0 + dx >= static_cast<std::int64_t>(w)) {
                continue;
              }
              const auto j = map[(y + dy) * w + x0 + dx];
              if (j >= 0) {
                dense[i * unknowns + j] =
                    matrix[25 * i + (dy + 2) * 5 + dx + 2];
              }
            }
          }
        }
        bool positive = true;
        for (std::uint64_t i = 0; i < unknowns && positive; ++i) {
          work.poll((i + 1) * (i + 1));
          for (std::uint64_t j = 0; j <= i; ++j) {
            double value = dense[i * unknowns + j];
            for (std::uint64_t k = 0; k < j; ++k) {
              value -= dense[i * unknowns + k] * dense[j * unknowns + k];
            }
            if (i == j) {
              if (!(value > 0) || !std::isfinite(value)) {
                positive = false;
                break;
              }
              dense[i * unknowns + j] = std::sqrt(value);
            } else {
              dense[i * unknowns + j] = value / dense[j * unknowns + j];
            }
          }
        }
        const auto solve = [&](double* value) {
          work.poll(2 * unknowns * unknowns);
          for (std::uint64_t i = 0; i < unknowns; ++i) {
            if ((i & 4095) == 0) {
              cancel(inv);
            }
            for (std::uint64_t j = 0; j < i; ++j) {
              value[i] -= dense[i * unknowns + j] * value[j];
            }
            value[i] /= dense[i * unknowns + i];
          }
          for (std::uint64_t i = unknowns; i-- > 0;) {
            for (std::uint64_t j = i + 1; j < unknowns; ++j) {
              value[i] -= dense[j * unknowns + i] * value[j];
            }
            value[i] /= dense[i * unknowns + i];
          }
        };
        if (positive) {
          std::copy(rhs, rhs + unknowns, solution);
          solve(solution);
          for (unsigned refinement = 0; refinement < 4; ++refinement) {
            const auto tolerance = measure();
            if (residual <= tolerance.second) {
              converged = true;
              break;
            }
            std::copy(r, r + unknowns, z);
            solve(z);
            for (std::uint64_t i = 0; i < unknowns; ++i) {
              solution[i] += z[i];
            }
          }
        }
      }
      // A narrow unknown bounding box yields a narrow exact raster-order
      // band. Keep the original stencil for true residuals and PCG fallback.
      const auto bandwidth =
          std::min<std::uint64_t>(unknowns - 1, 2 * (max_x - min_x + 1) + 2);
      if (unknowns > 256 && bandwidth <= 32 &&
          unknowns * (bandwidth + 1) <= 31 * (n - unknowns) + 256 * 256) {
        const auto stride = bandwidth + 1;
        auto* factor = arena.take<double>(unknowns * stride);
        work.poll(unknowns * stride);
        fill_elements(factor, unknowns * stride, 0.0, checkpoint);
        bool positive = true;
        for (std::uint64_t i = 0; i < unknowns; ++i) {
          if ((i & 127) == 0) {
            work.poll(std::min<std::uint64_t>(128, unknowns - i) * 25);
          }
          const auto y = positions[i] / static_cast<std::int64_t>(w);
          const auto x0 = positions[i] % static_cast<std::int64_t>(w);
          for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
              if (y + dy < 0 || y + dy >= static_cast<std::int64_t>(h) ||
                  x0 + dx < 0 || x0 + dx >= static_cast<std::int64_t>(w)) {
                continue;
              }
              const auto j = map[(y + dy) * w + x0 + dx];
              if (j >= 0 && static_cast<std::uint64_t>(j) <= i) {
                const auto distance = i - static_cast<std::uint64_t>(j);
                if (distance > bandwidth) {
                  positive = false;
                } else {
                  factor[i * stride + distance] =
                      matrix[25 * i + (dy + 2) * 5 + dx + 2];
                }
              }
            }
          }
        }
        for (std::uint64_t i = 0; i < unknowns && positive; ++i) {
          const auto first = i > bandwidth ? i - bandwidth : 0;
          const auto count = i - first + 1;
          work.poll(count * count);
          for (auto j = first; j <= i; ++j) {
            double value = factor[i * stride + i - j];
            for (auto k = first; k < j; ++k) {
              value -= factor[i * stride + i - k] * factor[j * stride + j - k];
            }
            if (i == j) {
              if (!(value > 0) || !std::isfinite(value)) {
                positive = false;
                break;
              }
              factor[i * stride] = std::sqrt(value);
            } else {
              factor[i * stride + i - j] = value / factor[j * stride];
            }
          }
        }
        const double finite_limit = std::numeric_limits<double>::max() /
                                    std::max(1.0, matrix_norm + rhs_norm) *
                                    0.25;
        const auto solve_band = [&](double* value) {
          work.poll(2 * unknowns * (2 * bandwidth + 1));
          for (std::uint64_t i = 0; i < unknowns; ++i) {
            if ((i & 63) == 0) {
              cancel(inv);
            }
            const auto first = i > bandwidth ? i - bandwidth : 0;
            for (auto j = first; j < i; ++j) {
              value[i] -= factor[i * stride + i - j] * value[j];
            }
            value[i] /= factor[i * stride];
            if (!std::isfinite(value[i]) || std::abs(value[i]) > finite_limit) {
              return false;
            }
          }
          for (auto i = unknowns; i-- > 0;) {
            if ((i & 63) == 0) {
              cancel(inv);
            }
            const auto last = std::min(unknowns, i + bandwidth + 1);
            for (auto j = i + 1; j < last; ++j) {
              value[i] -= factor[j * stride + j - i] * value[j];
            }
            value[i] /= factor[i * stride];
            if (!std::isfinite(value[i]) || std::abs(value[i]) > finite_limit) {
              return false;
            }
          }
          return true;
        };
        if (positive) {
          copy_elements(solution, rhs, unknowns, checkpoint);
          positive = solve_band(solution);
          for (unsigned refinement = 0; positive && refinement < 4;
               ++refinement) {
            const auto tolerance = measure();
            if (residual <= tolerance.second) {
              converged = true;
              break;
            }
            copy_elements(z, r, unknowns, checkpoint);
            positive = solve_band(z);
            if (!positive) {
              break;
            }
            work.poll(unknowns);
            for (std::uint64_t i = 0; i < unknowns; ++i) {
              if ((i & 4095) == 0) {
                cancel(inv);
              }
              solution[i] += z[i];
              positive &= std::isfinite(solution[i]) &&
                          std::abs(solution[i]) <= finite_limit;
            }
          }
        }
        if (!positive) {
          fill_elements(solution, unknowns, 0.5, checkpoint);
        }
      }
      if (!converged) {
        auto tolerance = measure();
        converged = residual <= tolerance.second;
        double rho = 0;
        for (std::uint64_t i = 0; i < unknowns; ++i) {
          if ((i & 4095) == 0) {
            cancel(inv);
          }
          const double diagonal = matrix[25 * i + 12];
          if (!(diagonal > 0)) {
            fail("NotConverged: KEY matting nonpositive numerical diagonal");
          }
          direction[i] = z[i] = r[i] / diagonal;
          rho += r[i] * z[i];
        }
        for (std::uint64_t iteration = 0;
             !converged && iteration < maximum_iterations; ++iteration) {
          multiply(direction, product);
          double denominator = 0;
          for (std::uint64_t i = 0; i < unknowns; ++i) {
            if ((i & 4095) == 0) {
              cancel(inv);
            }
            denominator += direction[i] * product[i];
          }
          if (!(denominator > 0) || !std::isfinite(denominator)) {
            break;
          }
          const double step = rho / denominator;
          double estimate = 0;
          work.poll(unknowns * 8);
          for (std::uint64_t i = 0; i < unknowns; ++i) {
            if ((i & 4095) == 0) {
              cancel(inv);
            }
            solution[i] += step * direction[i];
            r[i] -= step * product[i];
            estimate = std::max(estimate, std::abs(r[i]));
          }
          const bool restart =
              (iteration & 31) == 31 || estimate <= tolerance.second;
          if (restart) {
            tolerance = measure();
            converged = residual <= tolerance.second;
            if (converged) {
              break;
            }
          }
          double next_rho = 0;
          for (std::uint64_t i = 0; i < unknowns; ++i) {
            if ((i & 4095) == 0) {
              cancel(inv);
            }
            z[i] = r[i] / matrix[25 * i + 12];
            next_rho += r[i] * z[i];
          }
          const double beta = restart ? 0 : next_rho / rho;
          for (std::uint64_t i = 0; i < unknowns; ++i) {
            if ((i & 4095) == 0) {
              cancel(inv);
            }
            direction[i] = z[i] + beta * direction[i];
          }
          rho = next_rho;
        }
      }
      const auto final_tolerance = measure();
      if (!(residual <= final_tolerance.first)) {
        fail("NotConverged: KEY matting residual criterion");
      }
    }
    for (unsigned o = 0; o < inv.output_count; ++o) {
      const auto& out = inv.outputs[o];
      if (out.port == 1) {
        work.poll(1);
        save(out, Coordinate{}, residual);
        continue;
      }
      const auto& l = *out.layout;
      for (auto y = l.origin[0]; y < l.origin[0] + l.shape[0]; ++y) {
        work.poll(l.shape[1] * 2);
        auto* p = static_cast<unsigned char*>(out.allocation) +
                  offset(l, out.byte_offset, Coordinate{y, l.origin[1]});
        const bool dense =
            l.strides[1] == sizeof(T) &&
            reinterpret_cast<std::uintptr_t>(p) % alignof(T) == 0;
        for (std::uint64_t x = 0; x < l.shape[1]; ++x) {
          if ((x & 4095) == 0) {
            cancel(inv);
          }
          const auto index = y * w + l.origin[1] + x;
          const T value =
              labels[index] == 1
                  ? static_cast<T>(std::clamp(solution[map[index]], 0.0, 1.0))
                  : T(labels[index] == 2);
          if (dense) {
            reinterpret_cast<T*>(p)[x] = value;
          } else {
            save(out, Coordinate{y, l.origin[1] + x}, value);
          }
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
}  // namespace ps::plugin_internal::keying

namespace ps::plugin_internal {
Status register_keying_matting(OperationRegistry* registry) {
  using namespace keying;  // NOLINT(build/namespaces)
  using P = OperationParameterType;
  auto p = color_parameters();
  p.push_back({"solve_atol", P::Float64, false});
  p.push_back({"solve_rtol", P::Float64, false});
  p.push_back({"max_iterations", P::Int64, false});
  return registry->register_operation(budgeted_operation(generation::operation(
      "keying.closed_form_matting_clipped", 3, std::move(p),
      {{"matte", tensor_schema(ElementType::Float64, {1})},
       {"raw_residual", tensor_schema(ElementType::Float64, {1})}},
      [](const auto& in, const auto& p) {
        return std::make_shared<Matting>(in, p);
      })));
}
}  // namespace ps::plugin_internal
