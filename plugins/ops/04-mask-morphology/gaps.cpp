#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "04-mask-morphology/lattice.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::mask {
namespace {
struct BridgeGaps final : Program {
  std::uint64_t height, width, maximum_gap;
  ElementType type;
  BridgeGaps(const std::vector<OperationMetadata>& in, const Parameters& p) {
    const auto& s = image(in[0], 2);
    height = s.sample_shape()[0];
    width = s.sample_shape()[1];
    type = s.descriptor.element_type;
    const auto gap = integer(p, "maximum_gap");
    parameter(gap >= 0, "negative axis-gap support");
    maximum_gap = static_cast<std::uint64_t>(gap);
    add_output(this, spatial_schema(type, {height, width}));
  }
  std::vector<ReadRegion> support(unsigned, const Region& q) const override {
    if (!maximum_gap)
      return {{0, 0, q}};
    const auto reach = maximum_gap + 1;
    return {{0, 0, expanded_region(q, height, width, 0, reach)},
            {0, 0, expanded_region(q, height, width, reach, 0)}};
  }
  std::uint64_t task_scratch(unsigned, const Region& q) const override {
    if (!maximum_gap)
      return 0;
    const auto& d = q.dimensions();
    return bytes(static_cast<unsigned __int128>(d[1].extent) *
                 (sizeof(std::uint64_t) + d[0].extent));
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    Visits visits{work};
    const Reader reader{kernel, inv};
    const auto& out = inv.outputs[0];
    if (!maximum_gap) {
      lattice_output(out, work, [&](auto y, auto x, bool) {
        return reader.raw(0, 0, {y, x});
      });
      return;
    }
    const auto y0 = out.layout->origin[0], x0 = out.layout->origin[1];
    const auto rows = out.layout->shape[0], columns = out.layout->shape[1];
    const auto y1 = y0 + rows, x1 = x0 + columns;
    auto* previous = static_cast<std::uint64_t*>(inv.scratch[0].allocation);
    auto* result = reinterpret_cast<std::uint8_t*>(previous + columns);
    for (std::uint64_t i = 0; i < columns; ++i) {
      visits.step();
      previous[i] = UINT64_MAX;
    }
    for (std::uint64_t i = 0; i < rows * columns; ++i) {
      visits.step();
      result[i] = 0;
    }
    const bool narrow = type == ElementType::Float32;
    const auto read_row = [&](std::uint64_t y, std::uint64_t begin,
                              std::uint64_t end, auto function) {
      for (auto x = begin; x < end;) {
        const Coordinate at{y, x};
        const auto& view = reader.view(0, 0, at);
        const auto n =
            std::min({UINT64_C(256), end - x,
                      view.layout->origin[1] + view.layout->shape[1] - x});
        work.poll(n);
        const auto* data = static_cast<const std::uint8_t*>(view.allocation) +
                           offset(*view.layout, view.byte_offset, at);
        for (std::uint64_t i = 0; i < n; ++i)
          if (narrow ? load<float>(data, view.layout->strides[1], i) != 0
                     : load<double>(data, view.layout->strides[1], i) != 0)
            function(x + i);
        x += n;
      }
    };
    const auto q = invocation_region(*out.layout);
    const auto horizontal =
        expanded_region(q, height, width, 0, maximum_gap + 1).dimensions();
    for (auto y = y0; y < y1; ++y) {
      auto left = UINT64_MAX;
      read_row(y, horizontal[1].offset,
               horizontal[1].offset + horizontal[1].extent, [&](auto x) {
                 if (x >= x0 && x < x1)
                   result[(y - y0) * columns + x - x0] = 1;
                 if (left != UINT64_MAX && x - left - 1 <= maximum_gap) {
                   for (auto xx = std::max(x0, left + 1); xx < std::min(x1, x);
                        ++xx) {
                     visits.step();
                     result[(y - y0) * columns + xx - x0] = 1;
                   }
                 }
                 left = x;
               });
    }
    // Both directions inspect the original fixed views. Newly bridged
    // horizontal runs never become vertical endpoints (or vice versa).
    const auto vertical =
        expanded_region(q, height, width, maximum_gap + 1, 0).dimensions();
    for (auto y = vertical[0].offset;
         y < vertical[0].offset + vertical[0].extent; ++y)
      read_row(y, x0, x1, [&](auto x) {
        auto& top = previous[x - x0];
        if (top != UINT64_MAX && y - top - 1 <= maximum_gap) {
          for (auto yy = std::max(y0, top + 1); yy < std::min(y1, y); ++yy) {
            visits.step();
            result[(yy - y0) * columns + x - x0] = 1;
          }
        }
        top = y;
      });
    lattice_output(out, work, [&](auto y, auto x, bool output_narrow) {
      return convert(result[(y - y0) * columns + x - x0], output_narrow);
    });
    visits.flush();
  }
};
// This reusable preparation stage is shared by the two authoring templates.
// Complete-input support is retained even for diagnostic-only exports.
struct PrepareGapFill final : Program {
  std::uint64_t height, width;
  PrepareGapFill(const std::vector<OperationMetadata>& in) {
    const auto& original = image(in[0], 2);
    same(in[0], in[1]);
    const auto& seeds = image(in[2], 2);
    require(original.sample_shape() == seeds.sample_shape(),
            "gap-fill seed shape mismatch");
    height = seeds.sample_shape()[0];
    width = seeds.sample_shape()[1];
    collective = true;
    scratch_bytes = size_product({height, width});
    for (unsigned i = 0; i < 4; ++i)
      add_output(
          this, spatial_schema(seeds.descriptor.element_type, {height, width}));
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    const Reader reader{kernel, inv};
    auto* state = static_cast<std::uint8_t*>(inv.scratch[0].allocation);
    for (unsigned port = 0; port < 3; ++port) {
      const bool narrow =
          kernel.groups[kernel.input_base[port]].type == ElementType::Float32;
      component_read_rows(
          reader, port, 0, height, width, work,
          [&](const void* data, std::ptrdiff_t stride, auto y, auto x, auto n) {
            for (std::uint64_t i = 0; i < n; ++i) {
              const bool value = narrow ? load<float>(data, stride, i) != 0
                                        : load<double>(data, stride, i) != 0;
              auto& sample = state[y * width + x + i];
              if (!port)
                sample = value;
              else
                sample |= static_cast<std::uint8_t>(value << port);
            }
          });
    }
    for (unsigned j = 0; j < inv.output_count; ++j) {
      const auto port = kernel.writes[j].first;
      lattice_output(inv.outputs[j], work, [&](auto y, auto x, bool narrow) {
        const auto sample = state[y * width + x];
        const bool original = (sample & 1U) != 0;
        const bool temporary = (sample & 3U) != 0;
        const bool seed = (sample & 4U) != 0;
        const bool filtered = seed && !original;
        const bool value = port == 0   ? filtered
                           : port == 1 ? original || (temporary && !filtered)
                           : port == 2 ? seed && original
                                       : temporary && filtered;
        return convert(value, narrow);
      });
    }
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_gaps(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    check(registry->register_operation(generation::operation(
        "mask.bridge_axis_gaps_strict", 1, {{"maximum_gap", Type::Int64}},
        {{"temporary", spatial_schema(ElementType::Float64, {1, 1})}},
        [](const auto& in, const auto& p) {
          return std::make_shared<BridgeGaps>(in, p);
        })));
    std::vector<std::pair<std::string, SchemaTemplate>> outputs;
    for (const auto* name :
         {"seeds", "barrier", "ignored_seeds", "reopened_barrier"})
      outputs.push_back({name, spatial_schema(ElementType::Float64, {1, 1})});
    check(registry->register_operation(
        generation::operation("mask.prepare_gap_fill_strict", 3, {}, outputs,
                              [](const auto& in, const auto&) {
                                return std::make_shared<PrepareGapFill>(in);
                              })));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
