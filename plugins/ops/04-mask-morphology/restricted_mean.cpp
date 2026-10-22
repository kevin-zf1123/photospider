#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "04-mask-morphology/exact_sum.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
struct Restricted final : Program {
  Footprint footprint;
  ElementType type;
  std::uint64_t height, width;
  bool empty_error;
  Restricted(const std::vector<OperationMetadata>& in, const Parameters& p)
      : footprint(p), empty_error(text(p, "empty_policy") == "error") {
    parameter(empty_error || text(p, "empty_policy") == "zero",
              "mask empty_policy");
    const auto& s = image(in[0], 3);
    const auto& m = image(in[1], 2);
    type = s.descriptor.element_type;
    height = s.descriptor.shape[1];
    width = s.descriptor.shape[2];
    require(m.descriptor.shape == std::vector<std::uint64_t>{height, width} &&
                m.descriptor.element_type == type,
            "restricted mean mask schema");
    add_output(this, spatial_schema(type, s.descriptor.shape));
    add_output(this, spatial_schema(type, {height, width}));
  }
  std::vector<ReadRegion> support(unsigned port,
                                  const Region& q) const override {
    const auto s = footprint.support(q, height, width);
    std::vector<ReadRegion> result{{1, 0, s}};
    if (port == 0) {
      const auto c = q.dimensions()[0];
      result.push_back(
          {0, 0, Region({c, s.dimensions()[0], s.dimensions()[1]})});
    }
    return result;
  }
  std::uint64_t task_scratch(unsigned, const Region& q) const override {
    const auto support_region = footprint.support(q, height, width);
    const auto columns =
        footprint.kind == "square" ? support_region.dimensions()[1].extent : 0;
    if (type == ElementType::Float32)
      return bytes(sizeof(numeric_ops::ExactRatioWorkspace<12>) +
                   static_cast<unsigned __int128>(columns + 1) *
                       sizeof(WeightedSum<12>) +
                   64);
    return bytes(sizeof(numeric_ops::ExactRatioWorkspace<68>) +
                 static_cast<unsigned __int128>(columns + 1) *
                     sizeof(WeightedSum<68>) +
                 64);
  }
  template <unsigned Words>
  void calculate(const Kernel& k, const ps_kernel_invocation_v8& inv) const {
    using Sum = WeightedSum<Words>;
    using Ratio = numeric_ops::ExactRatioWorkspace<Words>;
    const bool narrow = type == ElementType::Float32;
    const bool values = k.writes[0].first == 0;
    const Reader reader{k, inv};
    const auto& out = inv.outputs[0];
    const unsigned ya = values ? 1 : 0, xa = ya + 1;
    const auto& l = *out.layout;
    const auto sy = l.origin[ya], sx = l.origin[xa];
    const auto ey = sy + l.shape[ya], ex = sx + l.shape[xa];
    const auto c0 = values ? l.origin[0] : 0;
    const auto cn = values ? l.shape[0] : 1;
    const auto radius = static_cast<std::uint64_t>(footprint.rx);
    const auto left = sx > radius ? sx - radius : 0;
    const auto columns = std::min(width, ex + radius) - left;
    auto* ratio = new (inv.scratch[0].allocation)
        Ratio(numeric_ops::SequenceProfile::Strict);
    auto* sums = reinterpret_cast<Sum*>(ratio + 1);
    Work work{inv, work_limit};
    const auto insert = [&](Sum* s, std::uint64_t c, std::uint64_t y,
                            std::uint64_t x, bool subtract) {
      const auto w = reader.raw(1, 0, {y, x});
      const auto v = values ? reader.raw(0, 0, {c, y, x}) : 0;
      work.poll(1);
      s->sample(v, w, narrow, subtract);
    };
    const auto emit = [&](const Sum& s, std::uint64_t c, std::uint64_t y,
                          std::uint64_t x) {
      std::uint64_t v;
      if (!values) {
        v = convert(!s.empty(), narrow);
      } else if (s.empty()) {
        if (empty_error)
          throw Status{ErrorCode::OperationFailed,
                       "zero restricted denominator",
                       DomainErrorCode{UINT64_C(0x4d41534b0001)}};
        v = 0;
      } else {
        v = s.finish(*ratio, narrow, work);
      }
      store(out, values ? Coordinate{c, y, x} : Coordinate{y, x}, v);
    };
    for (auto c = c0; c < c0 + cn; ++c) {
      if (footprint.kind == "square") {
        for (std::uint64_t j = 0; j < columns; ++j) {
          new (sums + j) Sum{};
          const auto low = sy > static_cast<std::uint64_t>(footprint.ry)
                               ? sy - footprint.ry
                               : 0;
          const auto high = std::min(height, sy + footprint.ry + 1);
          for (auto y = low; y < high; ++y)
            insert(sums + j, c, y, left + j, false);
        }
        for (auto y = sy; y < ey; ++y) {
          if (y != sy) {
            for (std::uint64_t j = 0; j < columns; ++j) {
              if (y > static_cast<std::uint64_t>(footprint.ry))
                insert(sums + j, c, y - footprint.ry - 1, left + j, true);
              if (y + footprint.ry < height)
                insert(sums + j, c, y + footprint.ry, left + j, false);
            }
          }
          Sum total;
          auto low = sx > static_cast<std::uint64_t>(footprint.rx)
                         ? sx - footprint.rx
                         : 0;
          auto high = std::min(width, sx + footprint.rx + 1);
          for (auto x = low; x < high; ++x) {
            total.merge(sums[x - left], false);
            work.poll(1);
          }
          for (auto x = sx; x < ex; ++x) {
            if (x != sx) {
              if (x > static_cast<std::uint64_t>(footprint.rx))
                total.merge(sums[x - footprint.rx - 1 - left], true);
              if (x + footprint.rx < width)
                total.merge(sums[x + footprint.rx - left], false);
              work.poll(1);
            }
            emit(total, c, y, x);
          }
        }
        continue;
      }
      for (auto y = sy; y < ey; ++y)
        for (auto x = sx; x < ex; ++x) {
          Sum total;
          footprint.each([&](std::int64_t dy, std::int64_t dx) {
            const auto yy = static_cast<std::int64_t>(y) + dy;
            const auto xx = static_cast<std::int64_t>(x) + dx;
            work.poll(1);
            if (yy >= 0 && xx >= 0 && static_cast<std::uint64_t>(yy) < height &&
                static_cast<std::uint64_t>(xx) < width)
              insert(&total, c, yy, xx, false);
          });
          emit(total, c, y, x);
        }
    }
    ratio->~Ratio();
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& i) const override {
    if (type == ElementType::Float32)
      calculate<12>(k, i);
    else
      calculate<68>(k, i);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_restricted(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  auto p = footprint_parameters();
  p.push_back({"empty_policy", Type::String});
  return registry->register_operation(definition(
      "restricted_mean", 2, std::move(p),
      [](const auto& in, const auto& p) {
        return std::make_shared<Restricted>(in, p);
      },
      true));
}
}  // namespace ps::plugin_internal
