#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "04-mask-morphology/lattice.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
enum class FloodKind { Fixed, Neighbor, Barrier };
struct Flood final : Program {
  FloodKind kind;
  std::uint64_t height, width, count, components = 0;
  unsigned seed_port, barrier_port;
  bool diagonal;
  double tolerance = 0;
  ElementType type;
  Flood(FloodKind k, const std::vector<OperationMetadata>& in,
        const Parameters& p)
      : kind(k) {
    seed_port = kind == FloodKind::Barrier ? 0 : 1;
    barrier_port = seed_port + 1;
    const auto& seeds = image(in[seed_port], 2);
    const auto& barrier = image(in[barrier_port], 2);
    require(seeds.sample_shape() == barrier.sample_shape(),
            "flood seed/barrier shape mismatch");
    height = seeds.sample_shape()[0];
    width = seeds.sample_shape()[1];
    count = size_product({height, width});
    type = seeds.descriptor.element_type;
    const auto connectivity = integer(p, "connectivity");
    parameter(connectivity == 4 || connectivity == 8, "flood connectivity");
    diagonal = connectivity == 8;
    if (kind != FloodKind::Barrier) {
      const auto& input = image(in[0], 3);
      components = input.sample_shape()[0];
      require(
          input.sample_shape()[1] == height && input.sample_shape()[2] == width,
          "flood image shape mismatch");
      tolerance = real(p, "tolerance");
    }
    collective = true;
    scratch_bytes =
        bytes(sizeof(Exact) +
              static_cast<unsigned __int128>(count) *
                  (sizeof(std::uint64_t) * (kind == FloodKind::Fixed ? 3 : 2) +
                   1 + components * sizeof(double)) +
              64);
    add_output(this, spatial_schema(type, {height, width}));
  }
  bool similar(const double* data, std::uint64_t a, std::uint64_t b, Exact& e,
               Visits& visits) const {
    for (std::uint64_t c = 0; c < components; ++c) {
      visits.step();
      const auto x = data[c * count + a], y = data[c * count + b];
      const auto delta = std::abs(x - y);
      if (delta < tolerance) {
        continue;
      }
      if (delta > tolerance) {
        return false;
      }
      if (x == y) {
        continue;
      }
      // Equality of a rounded subtraction is the only ambiguous comparison
      // against a representable tolerance. Resolve it in the exact field.
      const auto difference =
          e.absolute(e.subtract(e.binary(bits(x)), e.binary(bits(y))));
      const auto order = e.order(difference, e.binary(bits(tolerance)));
      check(e.status());
      if (order > 0) {
        return false;
      }
    }
    return true;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    Visits visits{work};
    const Reader reader{kernel, inv};
    ExactArena arena(inv.scratch[0].allocation, work);
    auto& exact = *arena.p;
    auto* queue = reinterpret_cast<std::uint64_t*>(arena.p + 1);
    auto* seeds = queue + count;
    auto* seen = seeds + count;
    auto* data = reinterpret_cast<double*>(
        seen + (kind == FloodKind::Fixed ? count : 0));
    auto* flags = reinterpret_cast<std::uint8_t*>(data + count * components);
    std::uint64_t seed_count = 0;
    const auto read_rows = [&](unsigned port, std::uint64_t channel,
                               auto receive) {
      const bool narrow =
          kernel.groups[kernel.input_base[port]].type == ElementType::Float32;
      const bool planar = port == 0 && components;
      const unsigned axis = planar ? 2 : 1;
      for (std::uint64_t y = 0; y < height; ++y)
        for (std::uint64_t x = 0; x < width;) {
          const Coordinate at =
              planar ? Coordinate{channel, y, x} : Coordinate{y, x};
          const auto& view = reader.view(port, 0, at);
          const auto n = std::min(
              {UINT64_C(256), width - x,
               view.layout->origin[axis] + view.layout->shape[axis] - x});
          work.poll(n);
          const auto* source =
              static_cast<const std::uint8_t*>(view.allocation) +
              offset(*view.layout, view.byte_offset, at);
          const auto step = view.layout->strides[axis];
          for (std::uint64_t i = 0; i < n; ++i)
            receive(y * width + x + i,
                    narrow ? static_cast<double>(load<float>(source, step, i))
                           : load<double>(source, step, i));
          x += n;
        }
    };
    read_rows(barrier_port, 0, [&](auto index, auto value) {
      flags[index] = value != 0;
      if (kind == FloodKind::Fixed)
        seen[index] = 0;
    });
    read_rows(seed_port, 0, [&](auto index, auto value) {
      if (value != 0)
        seeds[seed_count++] = index;
    });
    if (seed_count && components) {
      for (std::uint64_t c = 0; c < components; ++c)
        read_rows(0, c, [&](auto index, auto value) {
          data[c * count + index] = value;
        });
    }
    const auto reference_order = [&](std::uint64_t a, std::uint64_t b) {
      for (std::uint64_t c = 0; c < components; ++c) {
        visits.step();
        const auto x = data[c * count + a], y = data[c * count + b];
        if (x < y)
          return -1;
        if (x > y)
          return 1;
      }
      return 0;
    };
    if (kind == FloodKind::Fixed) {
      std::sort(seeds, seeds + seed_count, [&](auto a, auto b) {
        const auto order = reference_order(a, b);
        return order < 0 || (!order && a < b);
      });
    }
    for (std::uint64_t pass = 0; pass < seed_count;) {
      work.poll();
      const auto stamp = pass + 1;
      std::uint64_t head = 0, tail = 0;
      auto end = seed_count;
      if (kind == FloodKind::Fixed) {
        end = pass + 1;
        while (end < seed_count && !reference_order(seeds[pass], seeds[end]))
          ++end;
        // Identical reference vectors define the same admissible graph.
        // One multi-source traversal computes the union of their floods.
        for (auto i = pass; i < end; ++i) {
          visits.step();
          const auto s = seeds[i];
          queue[tail++] = s;
          seen[s] = stamp;
          flags[s] |= 2;
        }
      } else {
        for (std::uint64_t j = 0; j < seed_count; ++j) {
          const auto s = seeds[j];
          queue[tail++] = s;
          flags[s] |= 2;
          visits.step();
        }
      }
      while (head < tail) {
        const auto from = queue[head++];
        const auto y = from / width, x = from % width;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            if ((!dy && !dx) || (!diagonal && dy && dx)) {
              continue;
            }
            visits.step();
            if ((dy < 0 && y == 0) || (dy > 0 && y + 1 == height) ||
                (dx < 0 && x == 0) || (dx > 0 && x + 1 == width)) {
              continue;
            }
            const auto to = (dy < 0   ? from - width
                             : dy > 0 ? from + width
                                      : from) +
                            (dx < 0 ? UINT64_MAX : static_cast<unsigned>(dx));
            if (flags[to] & 1) {
              continue;
            }
            if (kind == FloodKind::Fixed) {
              if (seen[to] == stamp) {
                continue;
              }
              seen[to] = stamp;
              if (!similar(data, seeds[pass], to, exact, visits)) {
                continue;
              }
            } else {
              if (flags[to] & 2) {
                continue;
              }
              if (kind == FloodKind::Neighbor &&
                  !similar(data, from, to, exact, visits)) {
                continue;
              }
            }
            flags[to] |= 2;
            queue[tail++] = to;
          }
        }
      }
      pass = end;
    }
    for (unsigned j = 0; j < inv.output_count; ++j)
      lattice_output(inv.outputs[j], work, [&](auto y, auto x, bool narrow) {
        return convert((flags[y * width + x] & 2) != 0, narrow);
      });
    visits.flush();
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_flood(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    for (auto entry : {std::make_pair("flood_fixed", FloodKind::Fixed),
                       std::make_pair("flood_neighbor", FloodKind::Neighbor),
                       std::make_pair("flood_barrier", FloodKind::Barrier)}) {
      std::vector<Parameter> parameters{{"connectivity", Type::Int64}};
      if (entry.second != FloodKind::Barrier) {
        parameters.push_back({"tolerance", Type::Float64});
      }
      check(registry->register_operation(
          definition(entry.first, entry.second == FloodKind::Barrier ? 2 : 3,
                     std::move(parameters),
                     [kind = entry.second](const auto& in, const auto& p) {
                       return std::make_shared<Flood>(kind, in, p);
                     })));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
