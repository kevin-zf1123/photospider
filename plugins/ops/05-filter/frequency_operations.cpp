#include <algorithm>
#include <charconv>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "05-filter/frequency.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::frequency {
namespace {
std::vector<std::uint64_t> list(const std::string& s, bool empty = false) {
  if (s.empty() && empty)
    return {};
  std::vector<std::uint64_t> result;
  std::size_t pos = 0;
  do {
    const auto end = s.find(',', pos);
    const auto stop = end == std::string::npos ? s.size() : end;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(s.data() + pos, s.data() + stop, value);
    parameter(pos < stop && (stop - pos == 1 || s[pos] != '0') &&
                  s[pos] >= '0' && s[pos] <= '9' && parsed.ec == std::errc{} &&
                  parsed.ptr == s.data() + stop,
              "frequency canonical list");
    result.push_back(value);
    if (end == std::string::npos)
      break;
    pos = end + 1;
  } while (true);
  return result;
}
std::vector<unsigned> axis_list(const Parameters& p, const char* key,
                                unsigned rank) {
  std::vector<unsigned> result;
  for (auto a : list(text(p, key), true)) {
    parameter(a < rank && (result.empty() || a > result.back()),
              "frequency sorted unique axes");
    result.push_back(static_cast<unsigned>(a));
  }
  return result;
}
void axes_of(const Parameters& p, unsigned rank, unsigned* ya, unsigned* xa) {
  const auto y = integer(p, "y_axis"), x = integer(p, "x_axis");
  parameter(rank >= 2 && y >= 0 && x >= 0 && y != x &&
                static_cast<unsigned>(y) < rank &&
                static_cast<unsigned>(x) < rank,
            "frequency spatial axes");
  *ya = static_cast<unsigned>(y);
  *xa = static_cast<unsigned>(x);
}
SchemaTemplate like(const OperationMetadata& in,
                    const std::vector<std::uint64_t>& shape, ElementType type) {
  auto s = *in.result_schema;
  s.id = "photospider.tensor";
  s.version = 1;
  auto& t = s.tensors[0];
  t.key = "samples";
  t.descriptor.element_type = type;
  for (unsigned a = 0; a < shape.size(); ++a) {
    if (a < t.batch_axes.size())
      t.batch_axes[a] = shape[a];
    else
      t.descriptor.shape[a - t.batch_axes.size()] = shape[a];
  }
  check(s.validate());
  return s;
}
}  // namespace
Grid::Grid(const Parameters& p)
    : shape(list(text(p, "original_shape"))),
      packing(text(p, "packing")),
      order(text(p, "index_order")),
      norm(text(p, "norm")),
      unit(text(p, "unit")),
      identity(text(p, "grid_id")),
      dy(real(p, "spacing_y")),
      dx(real(p, "spacing_x")),
      oy(real(p, "origin_y")),
      ox(real(p, "origin_x")) {
  parameter(integer(p, "schema_revision") == 1,
            "FrequencyGrid schema revision");
  size_product(shape);
  axes_of(p, static_cast<unsigned>(shape.size()), &ya, &xa);
  parameter(packing == "full" || packing == "r2c_x", "frequency packing");
  parameter(order == "unshifted" || order == "shifted",
            "frequency index order");
  parameter(norm == "backward" || norm == "ortho", "frequency normalization");
  parameter(!unit.empty() && !identity.empty(), "frequency unit/grid identity");
}
std::vector<std::uint64_t> Grid::stored_shape() const {
  auto result = shape;
  if (packing == "r2c_x")
    result[xa] = result[xa] / 2 + 1;
  return result;
}
Frequency::Frequency(Kind k, Profile prof,
                     const std::vector<OperationMetadata>& in,
                     const Parameters& p)
    : kind(k), profile(prof) {
  check(numeric_ops::sequence_profile_available(prof));
  if (kind == Kind::Window) {
    const auto h = integer(p, "height"), w = integer(p, "width");
    parameter(h > 0 && w > 0, "frequency window extents");
    dims = {static_cast<std::uint64_t>(h), static_cast<std::uint64_t>(w)};
    size_product(dims);
    mode = text(p, "kind");
    sampling = text(p, "sampling");
    alpha = real(p, "alpha");
    parameter(mode == "rectangular" || mode == "hann" || mode == "tukey",
              "window kind");
    parameter(sampling == "symmetric" || sampling == "periodic",
              "window sampling");
    parameter((mode == "tukey") == static_cast<bool>(p.count("alpha")),
              "window alpha presence");
    add_output(this, tensor_schema(type, dims));
    collective = true;
    return;
  }
  if (kind == Kind::Statistics) {
    const auto& s = tensor(in.at(0), true);
    require(s.sample_shape().size() == 2 && s.descriptor.element_type == type,
            "window statistics rank-2 Float64");
    dims = s.sample_shape();
    for (unsigned i = 0; i < 3; ++i)
      add_output(this, tensor_schema(type, {1}));
    collective = true;
    return;
  }
  if (kind == Kind::Apply) {
    const auto& s = tensor(in.at(0), true);
    dims = s.sample_shape();
    type = s.descriptor.element_type;
    require(floating(type), "frequency input dtype");
    axes_of(p, static_cast<unsigned>(dims.size()), &grid.ya, &grid.xa);
    broadcast =
        axis_list(p, "broadcast_axes", static_cast<unsigned>(dims.size()));
  } else {
    grid = Grid(p);
    dims = grid.stored_shape();
    if (kind == Kind::Radial || kind == Kind::Band || kind == Kind::Notch) {
      require(grid.shape.size() == 2 && grid.ya == 0 && grid.xa == 1 &&
                  grid.packing == "full" && grid.order == "unshifted",
              "frequency response full unshifted rank-2 grid");
      if (kind != Kind::Notch) {
        mode = text(p, "kind");
        pass = text(p, "pass");
        parameter(
            mode == "ideal" || mode == "gaussian" || mode == "butterworth",
            "response kind");
        parameter(
            (mode == "butterworth") == static_cast<bool>(p.count("order")),
            "response order presence");
        order = integer(p, "order", 1);
        if (kind == Kind::Radial) {
          low = high = real(p, "cutoff");
          parameter(pass == "low" || pass == "high", "radial pass");
        } else {
          low = real(p, "cutoff_low");
          high = real(p, "cutoff_high");
          parameter(pass == "band" || pass == "reject", "band pass");
        }
      } else {
        sy = real(p, "sigma_y");
        sx = real(p, "sigma_x");
        depth = real(p, "depth");
        // Static centers use exact IEEE uint64 bit encodings in canonical
        // lists.
        const auto ys = list(text(p, "centers_y_bits"));
        const auto xs = list(text(p, "centers_x_bits"));
        parameter(ys.size() == xs.size(), "notch center list lengths");
        for (unsigned i = 0; i < ys.size(); ++i)
          centers.emplace_back(number(ys[i]), number(xs[i]));
        std::sort(centers.begin(), centers.end());
      }
      add_output(this, tensor_schema(type, dims));
      add_output(this, tensor_schema(type, dims));
      return;
    }
    const auto& s = tensor(in.at(0), true);
    type = s.descriptor.element_type;
    require(floating(type), "frequency input dtype");
    if (kind == Kind::Fft || kind == Kind::Ifft || kind == Kind::Rfft ||
        kind == Kind::Irfft) {
      require(grid.order == "unshifted" &&
                  grid.packing == ((kind == Kind::Rfft || kind == Kind::Irfft)
                                       ? "r2c_x"
                                       : "full"),
              "transform grid structure");
      require(s.sample_shape() == (kind == Kind::Irfft ? dims : grid.shape),
              "transform original shape");
      if (kind == Kind::Irfft)
        dims = grid.shape;
      collective = true;
      const auto h = grid.shape[grid.ya], w = grid.shape[grid.xa];
      if (grid.norm == "ortho") {
        auto remaining = h * w;
        for (std::uint64_t prime = 2; prime <= remaining / prime; ++prime) {
          unsigned exponent = 0;
          while (remaining % prime == 0) {
            remaining /= prime;
            ++exponent;
          }
          for (unsigned j = 0; j < exponent / 2; ++j)
            norm_squarefactor *= prime;
          if (exponent & 1)
            norm_squarefree *= prime;
        }
        norm_squarefree *= remaining;
        norm_discriminant =
            norm_squarefree % 4 == 1 ? norm_squarefree : 4 * norm_squarefree;
      }
    } else {
      require(s.sample_shape() == dims, "frequency stored shape");
      if (kind == Kind::Shift || kind == Kind::Unshift) {
        require(
            grid.packing == "full" &&
                grid.order == (kind == Kind::Shift ? "unshifted" : "shifted"),
            "shift grid structure");
        axes = axis_list(p, "axes", static_cast<unsigned>(dims.size()));
        parameter(!axes.empty(), "shift axes required");
      }
      if (kind == Kind::Project)
        require(grid.packing == "full" && grid.order == "unshifted",
                "projection grid structure");
      if (kind == Kind::Multiply) {
        broadcast =
            axis_list(p, "broadcast_axes", static_cast<unsigned>(dims.size()));
        parameter(text(p, "response_unit") == "dimensionless",
                  "response unit tag");
      }
    }
  }
  for (unsigned i = 1; i < in.size(); ++i) {
    const auto& s = tensor(in[i], true);
    auto shape = s.sample_shape();
    const bool coefficient =
        kind == Kind::Apply || (kind == Kind::Multiply && i >= 2);
    require(
        s.descriptor.element_type ==
                (kind == Kind::Apply ? ElementType::Float64 : type) ||
            (coefficient && s.descriptor.element_type == ElementType::Float64),
        "frequency component dtype");
    if (kind == Kind::Multiply && i == 3)
      require(s.descriptor.element_type ==
                  tensor(in[2], true).descriptor.element_type,
              "response component dtype mismatch");
    require(shape.size() == dims.size(), "frequency component rank");
    for (auto a : broadcast) {
      parameter(a != grid.ya && a != grid.xa,
                "frequency axes cannot broadcast");
      if (coefficient) {
        require(shape[a] == 1, "broadcast extent must be one");
        shape[a] = dims[a];
      }
    }
    const auto expected =
        transform() ? tensor(in[0], true).sample_shape() : dims;
    require(shape == expected, "frequency component shape");
  }
  const auto s = like(in[0], dims, type);
  add_output(this, s);
  if (kind != Kind::Apply && kind != Kind::Irfft)
    add_output(this, s);
}
Region Frequency::plane_region(const Region& q) const {
  auto d = q.dimensions();
  d[grid.ya] = {0, grid.shape[grid.ya]};
  d[grid.xa] = {0, grid.shape[grid.xa]};
  return Region(std::move(d));
}
std::vector<ReadRegion> Frequency::support(unsigned port,
                                           const Region& q) const {
  if (kind == Kind::Window || kind == Kind::Radial || kind == Kind::Band ||
      kind == Kind::Notch)
    return {};
  if (kind == Kind::Statistics)
    return {{0, 0, Region::whole(dims)}};
  if (transform()) {
    auto d = selected.dimensions();
    const auto inshape = tensor(inputs[0], true).sample_shape();
    d[grid.ya] = {0, inshape[grid.ya]};
    d[grid.xa] = {0, inshape[grid.xa]};
    std::vector<ReadRegion> result{{0, 0, Region(d)}};
    if (!real_input())
      result.push_back({1, 0, Region(d)});
    return result;
  }
  if (kind == Kind::Shift || kind == Kind::Unshift) {
    std::vector<std::vector<RegionDimension>> boxes{q.dimensions()};
    for (auto a : axes) {
      std::vector<std::vector<RegionDimension>> next;
      for (auto box : boxes) {
        const auto n = dims[a],
                   shift = kind == Kind::Shift ? (n + 1) / 2 : n / 2;
        const auto start = (box[a].offset + shift) % n, length = box[a].extent;
        box[a] = {start, std::min(length, n - start)};
        next.push_back(box);
        if (length > n - start) {
          box[a] = {0, length - (n - start)};
          next.push_back(box);
        }
      }
      boxes = std::move(next);
    }
    std::vector<ReadRegion> result;
    for (auto& b : boxes)
      result.push_back({port, 0, Region(std::move(b))});
    return result;
  }
  if (kind == Kind::Project) {
    if (port == 1) {
      bool structural_zero = true;
      for (auto a : {grid.ya, grid.xa}) {
        const auto interval = q.dimensions()[a];
        if (interval.extent > 2) {
          structural_zero = false;
          break;
        }
        for (std::uint64_t j = 0; j < interval.extent; ++j) {
          const auto k = interval.offset + j;
          structural_zero &= k == 0 || (dims[a] % 2 == 0 && k == dims[a] / 2);
        }
      }
      if (structural_zero)
        return {};
    }
    auto result = std::vector<ReadRegion>{{port, 0, q}};
    // Negation on the two frequency axes splits only at the zero index.
    std::vector<std::vector<RegionDimension>> boxes{q.dimensions()};
    for (auto a : {grid.ya, grid.xa}) {
      std::vector<std::vector<RegionDimension>> next;
      for (auto b : boxes) {
        const auto start = b[a].offset, len = b[a].extent;
        if (start == 0) {
          auto z = b;
          z[a] = {0, 1};
          next.push_back(z);
        }
        if (len > (start == 0 ? 1U : 0U)) {
          b[a] = {dims[a] - (start + len - 1), len - (start == 0 ? 1U : 0U)};
          next.push_back(b);
        }
      }
      boxes = std::move(next);
    }
    for (auto& b : boxes)
      result.push_back({port, 0, Region(std::move(b))});
    return result;
  }
  std::vector<ReadRegion> result;
  for (unsigned i = 0; i < inputs.size(); ++i) {
    auto b = q.dimensions();
    if ((kind == Kind::Apply && i == 1) || (kind == Kind::Multiply && i >= 2))
      for (auto a : broadcast)
        b[a] = {0, 1};
    result.push_back({i, 0, Region(std::move(b))});
  }
  return result;
}
std::uint64_t Frequency::task_scratch(unsigned port,
                                      const Region& region) const {
  unsigned __int128 n = sizeof(Math) + 128;
  if (kind == Kind::Window)
    n += (static_cast<unsigned __int128>(dims[0]) + dims[1]) * 8;
  if (transform()) {
    const auto h = grid.shape[grid.ya], w = grid.shape[grid.xa];
    const auto width = transform_width();
    // Match run_transform: plane, roots, axis roots, line, real roots, blue.
    n += (static_cast<unsigned __int128>(h) * width + 2 * std::max(h, w) + h +
          width + (width != w ? width + 1 : 0)) *
         2 * sizeof(Ball);
    const auto blue_size = std::max(bluestein_size(h), bluestein_size(width));
    if (blue_size)
      n += (static_cast<unsigned __int128>(blue_size) * 3 + std::max(h, w)) *
           2 * sizeof(Ball);
  }
  if (port == 0 &&
      (((kind == Kind::Radial || kind == Kind::Band) && mode == "gaussian") ||
       (kind == Kind::Notch && depth != 0))) {
    const auto terms = kind == Kind::Notch
                           ? static_cast<unsigned __int128>(centers.size()) * 2
                       : kind == Kind::Band ? 2
                                            : 1;
    const auto axes =
        static_cast<unsigned __int128>(region.dimensions()[0].extent) +
        region.dimensions()[1].extent;
    n += terms * (2 * axes * sizeof(Ball) + 1);
  }
  return bytes(n);
}
void Frequency::run(const Kernel& k, const ps_kernel_invocation_v8& inv) const {
  if (transform())
    run_transform(k, inv);
  else
    run_local(k, inv);
}
void Frequency::run_v9(const Kernel& k,
                       const ps_kernel_invocation_v9& inv) const {
  if (transform())
    run_transform(k, inv.views, inv.work);
  else
    run_local(k, inv.views, inv.work);
}
OperationDefinition make_frequency(const std::string& key, Kind k,
                                   Profile profile) {
  std::vector<Parameter> p;
  unsigned count = k == Kind::Window || k == Kind::Radial || k == Kind::Band ||
                           k == Kind::Notch
                       ? 0
                   : k == Kind::Rfft || k == Kind::Statistics ? 1
                   : k == Kind::Multiply                      ? 4
                                                              : 2;
  if (k != Kind::Window && k != Kind::Statistics) {
    generation::append(&p, {{"y_axis", Type::Int64}, {"x_axis", Type::Int64}});
    if (k != Kind::Apply)
      generation::append(&p, {{"schema_revision", Type::Int64},
                              {"original_shape", Type::String},
                              {"packing", Type::String},
                              {"index_order", Type::String},
                              {"norm", Type::String},
                              {"unit", Type::String},
                              {"grid_id", Type::String},
                              {"spacing_y", Type::Float64},
                              {"spacing_x", Type::Float64},
                              {"origin_y", Type::Float64},
                              {"origin_x", Type::Float64}});
  }
  if (k == Kind::Shift || k == Kind::Unshift)
    p.push_back({"axes", Type::String});
  if (k == Kind::Apply || k == Kind::Multiply)
    p.push_back({"broadcast_axes", Type::String});
  if (k == Kind::Multiply)
    p.push_back({"response_unit", Type::String});
  if (k == Kind::Window)
    generation::append(&p, {{"height", Type::Int64},
                            {"width", Type::Int64},
                            {"kind", Type::String},
                            {"sampling", Type::String},
                            {"alpha", Type::Float64, false}});
  if (k == Kind::Radial || k == Kind::Band) {
    generation::append(&p, {{"kind", Type::String},
                            {"pass", Type::String},
                            {"order", Type::Int64, false}});
    if (k == Kind::Radial)
      p.push_back({"cutoff", Type::Float64});
    else
      generation::append(
          &p, {{"cutoff_low", Type::Float64}, {"cutoff_high", Type::Float64}});
  }
  if (k == Kind::Notch)
    generation::append(&p, {{"centers_y_bits", Type::String},
                            {"centers_x_bits", Type::String},
                            {"sigma_y", Type::Float64},
                            {"sigma_x", Type::Float64},
                            {"depth", Type::Float64}});
  const auto s = tensor_schema(ElementType::Float64, {1});
  std::vector<std::pair<std::string, SchemaTemplate>> out;
  if (k == Kind::Statistics)
    out = {{"coherent_gain", s}, {"mean_square", s}, {"enbw_bins", s}};
  else if (k == Kind::Window)
    out = {{"window", s}};
  else if (k == Kind::Apply || k == Kind::Irfft)
    out = {{"output", s}};
  else if (k == Kind::Rfft || k == Kind::Radial || k == Kind::Band ||
           k == Kind::Notch)
    out = {{"real", s}, {"imag", s}};
  else
    out = {{"out_real", s}, {"out_imag", s}};
  auto def = generation::operation(
      key, count, p, out, [=](const auto& in, const auto& args) {
        return std::make_shared<Frequency>(k, profile, in, args);
      });
  def = generation::budgeted_operation(std::move(def));
  return k <= Kind::Irfft ? transform_tasks(std::move(def)) : def;
}
}  // namespace ps::plugin_internal::frequency

namespace ps::plugin_internal {
Status register_frequency(OperationRegistry* registry) {
  using namespace frequency;  // NOLINT(build/namespaces)
  using Kind = frequency::Kind;
  try {
    for (auto profile :
         {Profile::Strict, Profile::AppleSilicon, Profile::X86Avx2}) {
      const std::string suffix = profile == Profile::Strict ? "_strict"
                                 : profile == Profile::AppleSilicon
                                     ? "_accelerated_apple_silicon"
                                     : "_accelerated_x86_64";
      for (auto entry : {std::make_pair("fft2", Kind::Fft),
                         {"ifft2", Kind::Ifft},
                         {"rfft2", Kind::Rfft},
                         {"irfft2", Kind::Irfft},
                         {"project_hermitian", Kind::Project},
                         {"fftshift", Kind::Shift},
                         {"ifftshift", Kind::Unshift},
                         {"window2d", Kind::Window},
                         {"apply_window", Kind::Apply},
                         {"window_statistics", Kind::Statistics},
                         {"radial_frequency_response", Kind::Radial},
                         {"band_frequency_response", Kind::Band},
                         {"paired_notch_response", Kind::Notch},
                         {"frequency_multiply", Kind::Multiply}}) {
        if ((entry.second == Kind::Shift || entry.second == Kind::Unshift) &&
            profile != Profile::Strict)
          continue;
        check(registry->register_operation(
            make_frequency("frequency." + std::string(entry.first) + suffix,
                           entry.second, profile)));
      }
    }
    return Status::success();
  } catch (const Status& s) {
    return s;
  }
}
}  // namespace ps::plugin_internal
