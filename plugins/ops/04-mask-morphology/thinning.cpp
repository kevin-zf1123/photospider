#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "04-mask-morphology/lattice.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
enum class ThinKind { Topological, Distance, ZhangSuen, GuoHall };
constexpr int kNeighborY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
constexpr int kNeighborX[8] = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr int magnitude(int x) {
  return x < 0 ? -x : x;
}
constexpr unsigned ring_components(unsigned mask, bool diagonal,
                                   bool incident) {
  unsigned seen = 0, count = 0;
  for (unsigned i = 0; i < 8; ++i) {
    if (!(mask & (1U << i)) || (seen & (1U << i)))
      continue;
    unsigned component = 1U << i, old = 0;
    while (old != component) {
      old = component;
      for (unsigned a = 0; a < 8; ++a)
        if (component & (1U << a)) {
          for (unsigned b = 0; b < 8; ++b) {
            const int dy = magnitude(kNeighborY[a] - kNeighborY[b]);
            const int dx = magnitude(kNeighborX[a] - kNeighborX[b]);
            if ((mask & (1U << b)) &&
                (diagonal ? std::max(dy, dx) == 1 : dy + dx == 1))
              component |= 1U << b;
          }
        }
    }
    seen |= component;
    if (!incident || (component & 0x55U))
      ++count;
  }
  return count;
}
constexpr bool delete_point(unsigned mask, ThinKind kind, unsigned phase) {
  bool b[8]{};
  unsigned count = 0, transitions = 0;
  for (unsigned i = 0; i < 8; ++i) {
    b[i] = (mask & (1U << i)) != 0;
    count += b[i];
  }
  if (kind == ThinKind::Topological)
    return count > 1 && ring_components(mask, true, false) == 1 &&
           ring_components(mask ^ 255U, false, true) == 1;
  if (kind == ThinKind::ZhangSuen) {
    for (unsigned i = 0; i < 8; ++i)
      transitions += !b[i] && b[(i + 1) % 8];
    return count >= 2 && count <= 6 && transitions == 1 &&
           (phase ? !(b[0] && b[2] && b[6]) && !(b[0] && b[4] && b[6])
                  : !(b[0] && b[2] && b[4]) && !(b[2] && b[4] && b[6]));
  }
  unsigned crossings = 0, n1 = 0, n2 = 0;
  for (unsigned i = 0; i < 8; i += 2) {
    crossings += !b[i] && (b[(i + 1) % 8] || b[(i + 2) % 8]);
    n1 += b[(i + 7) % 8] || b[i];
    n2 += b[i] || b[(i + 1) % 8];
  }
  const auto n = std::min(n1, n2);
  const bool gate =
      phase ? b[2] && (b[0] || b[1] || !b[3]) : b[6] && (b[4] || b[5] || !b[7]);
  return crossings == 1 && n >= 2 && n <= 3 && !gate;
}
constexpr std::array<std::uint8_t, 256> deletion_table(ThinKind kind,
                                                       unsigned phase) {
  std::array<std::uint8_t, 256> result{};
  for (unsigned mask = 0; mask < 256; ++mask)
    result[mask] = delete_point(mask, kind, phase);
  return result;
}
// In the planar (8,4) pair, a deletion preserves both global component
// counts exactly iff T8(foreground)=1 and T4(background incident to the
// center)=1. The latter counts only components touching cardinal neighbors;
// corner-only background components are unaffected by the deletion.
const auto kSimple = deletion_table(ThinKind::Topological, 0);
constexpr auto kZhang0 = deletion_table(ThinKind::ZhangSuen, 0);
constexpr auto kZhang1 = deletion_table(ThinKind::ZhangSuen, 1);
constexpr auto kGuo0 = deletion_table(ThinKind::GuoHall, 0);
constexpr auto kGuo1 = deletion_table(ThinKind::GuoHall, 1);

struct Thinning final : Program {
  ThinKind kind;
  std::uint64_t height, width, count, padded, distance_workspace = 0;
  ElementType type, radius_type = ElementType::Float64;
  std::optional<Metric> metric;
  Thinning(ThinKind k, const std::vector<OperationMetadata>& in,
           const Parameters& p)
      : kind(k) {
    const auto& s = image(in[0], 2);
    height = s.sample_shape()[0];
    width = s.sample_shape()[1];
    count = size_product({height, width});
    padded = bytes(static_cast<unsigned __int128>(height + 2) * (width + 2));
    type = s.descriptor.element_type;
    collective = true;
    add_output(this, spatial_schema(type, {height, width}));
    if (kind == ThinKind::Distance) {
      auto parameters = p;
      parameters["metric"] = std::string("l2");
      metric.emplace(parameters);
      distance_workspace =
          bytes(metric->isotropic
                    ? static_cast<unsigned __int128>(height + 2) *
                          sizeof(DistanceParabola)
                    : static_cast<unsigned __int128>(count) * sizeof(SiteNode));
      radius_type = output_dtype(p);
      add_output(this, spatial_schema(radius_type, {height, width}));
    }
    const auto distance_bytes =
        kind == ThinKind::Distance
            ? sizeof(Exact) + distance_workspace +
                  static_cast<unsigned __int128>(count) * sizeof(Site)
            : 0;
    scratch_bytes = bytes(
        distance_bytes + padded +
        static_cast<unsigned __int128>(count) * sizeof(std::uint64_t) * 2 + 64);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    Visits visits{work};
    const Reader reader{kernel, inv};
    auto* memory = static_cast<std::uint8_t*>(inv.scratch[0].allocation);
    std::optional<ExactArena> arena;
    void* distance_memory = nullptr;
    Site* distances = nullptr;
    if (metric) {
      arena.emplace(memory, work);
      memory += sizeof(Exact);
      distance_memory = memory;
      memory += distance_workspace;
      distances = reinterpret_cast<Site*>(memory);
      memory += count * sizeof(Site);
    }
    auto* order = reinterpret_cast<std::uint64_t*>(memory);
    auto* deleted = order + count;
    auto* state = reinterpret_cast<std::uint8_t*>(deleted + count);
    const auto stride = width + 2;
    lattice_input(reader, 0, state, height, width, work);
    const bool synchronous =
        kind == ThinKind::ZhangSuen || kind == ThinKind::GuoHall;
    std::uint64_t candidates = 0;
    for (std::uint64_t y = 0; y < height; ++y)
      for (std::uint64_t x = 0; x < width; ++x) {
        visits.step();
        const auto at = (y + 1) * stride + x + 1;
        if (state[at] &&
            (!synchronous || lattice_neighbors(state, at, stride) != 255)) {
          order[candidates++] = y * width + x;
          if (synchronous)
            state[at] = 3;
        }
      }
    if (metric) {
      auto& exact = *arena->p;
      if (metric->isotropic) {
        lattice_l2_distances(state, height, width, distances,
                             static_cast<DistanceParabola*>(distance_memory),
                             &visits);
        std::sort(order, order + candidates, [&](auto a, auto b) {
          visits.step();
          const auto aa = lattice_l2_key(distances[a]);
          const auto bb = lattice_l2_key(distances[b]);
          return aa < bb || (aa == bb && a < b);
        });
      } else {
        auto* nodes = static_cast<SiteNode*>(distance_memory);
        const auto sites =
            background_tree(state, height, width, nodes, &visits);
        SiteTree tree{nodes, sites, *metric, exact, visits};
        tree.build(0, sites);
        for (std::uint64_t i = 0; i < candidates; ++i) {
          const auto p = order[i];
          const Site center{static_cast<std::int64_t>(p / width),
                            static_cast<std::int64_t>(p % width)};
          distances[p] = nearest_background(tree, center, height, width);
        }
        std::sort(order, order + candidates, [&](auto a, auto b) {
          visits.step();
          const auto cmp =
              metric->compare(exact, {}, distances[a], distances[b]);
          return cmp < 0 || (!cmp && a < b);
        });
      }
    }
    // Store padded offsets once and compact deleted candidates without
    // changing the original visitation order. A/B mutate immediately; E/F
    // accumulate a phase's deletions and publish them simultaneously.
    for (std::uint64_t i = 0; i < candidates; ++i) {
      visits.step();
      const auto p = order[i];
      order[i] = (p / width + 1) * stride + p % width + 1;
    }
    bool changed;
    do {
      work.poll();
      changed = false;
      for (unsigned phase = 0; phase < (synchronous ? 2U : 1U); ++phase) {
        work.poll();
        const auto& table = !synchronous ? kSimple
                            : kind == ThinKind::ZhangSuen
                                ? (phase ? kZhang1 : kZhang0)
                                : (phase ? kGuo1 : kGuo0);
        std::uint64_t deletions = 0, surviving = 0;
        for (std::uint64_t i = 0; i < candidates; ++i) {
          visits.step();
          const auto at = order[i];
          if (!table[lattice_neighbors(state, at, stride)]) {
            order[surviving++] = at;
            continue;
          }
          changed = true;
          if (synchronous)
            deleted[deletions++] = at;
          else
            state[at] = 0;
        }
        candidates = surviving;
        for (std::uint64_t i = 0; i < deletions; ++i) {
          state[deleted[i]] = 0;
          visits.step();
        }
        if (synchronous) {
          // A fully surrounded pixel cannot pass either phase table. It
          // becomes eligible only after a neighbor is deleted. Membership
          // uses the high bit; all predicates still see the binary low bit.
          for (std::uint64_t i = 0; i < deletions; ++i) {
            const auto at = deleted[i];
            for (unsigned j = 0; j < 8; ++j) {
              visits.step();
              const auto neighbor = static_cast<std::uint64_t>(
                  static_cast<std::int64_t>(at) +
                  kNeighborY[j] * static_cast<std::int64_t>(stride) +
                  kNeighborX[j]);
              if (state[neighbor] == 1) {
                state[neighbor] = 3;
                order[candidates++] = neighbor;
              }
            }
          }
        }
      }
    } while (changed);
    for (unsigned j = 0; j < inv.output_count; ++j) {
      const bool radius = kernel.writes[j].first == 1;
      lattice_output(inv.outputs[j], work,
                     [&](auto y, auto x, bool narrow) -> std::uint64_t {
                       if (!state[(y + 1) * stride + x + 1])
                         return 0;
                       if (radius)
                         return metric->rounded_distance(
                             *arena->p, {}, distances[y * width + x], false,
                             narrow);
                       return convert(1, narrow);
                     });
    }
    visits.flush();
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_thinning(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    for (auto entry :
         {std::make_pair("thin_topological", ThinKind::Topological),
          std::make_pair("thin_distance_ordered", ThinKind::Distance),
          std::make_pair("thin_zhang_suen", ThinKind::ZhangSuen),
          std::make_pair("thin_guo_hall", ThinKind::GuoHall)}) {
      std::vector<Parameter> parameters;
      std::vector<std::pair<std::string, SchemaTemplate>> outputs{
          {"skeleton", spatial_schema(ElementType::Float64, {1, 1})}};
      if (entry.second == ThinKind::Distance) {
        parameters = {{"sy", Type::Float64},
                      {"sx", Type::Float64},
                      {"output_dtype", Type::String}};
        outputs.push_back(
            {"radius", spatial_schema(ElementType::Float64, {1, 1})});
      }
      check(registry->register_operation(generation::operation(
          std::string("mask.") + entry.first + "_strict", 1,
          std::move(parameters), outputs,
          [kind = entry.second](const auto& in, const auto& p) {
            return std::make_shared<Thinning>(kind, in, p);
          })));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
