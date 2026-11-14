#include <algorithm>
#include <array>
#include <limits>

#include "photospider/plugin/operation_module_api.h"

namespace {
using Wide = __int128;
struct Interval {
  uint64_t first, length;
};
struct Support {
  std::array<Interval, 2> spans{};
  unsigned count = 0;
};
enum Border { Constant, Clamp, Wrap, ReflectHalf, ReflectWhole, Truncate };
ps_kernel_status_v8 invalid() {
  ps_kernel_status_v8 s{};
  s.code = PS_KERNEL_INVALID_ARGUMENT_V8;
  return s;
}
Support axis(Wide low, Wide high, uint64_t extent, uint64_t border) {
  Support out;
  auto add = [&](Wide start, Wide stop) {
    if (start <= stop)
      out.spans[out.count++] = {static_cast<uint64_t>(start),
                                static_cast<uint64_t>(stop - start + 1)};
  };
  const Wide end = Wide(extent) - 1;
  if (border == Constant || border == Truncate) {
    add(std::max(Wide(0), low), std::min(end, high));
  } else if (border == Clamp || extent == 1) {
    add(std::clamp(low, Wide(0), end), std::clamp(high, Wide(0), end));
  } else {
    const Wide step = border == ReflectWhole ? end : Wide(extent);
    const Wide period = border == Wrap ? step : 2 * step;
    const auto fold = [&](Wide v) {
      auto t = v % period;
      if (t < 0)
        t += period;
      return t <= end ? t : period - t - (border == ReflectHalf);
    };
    if (high - low + 1 >= period) {
      add(0, end);
    } else if (border == Wrap) {
      auto start = fold(low), stop = start + high - low;
      add(start, std::min(end, stop));
      if (stop > end)
        add(0, stop - extent);
    } else {
      auto first = std::min(fold(low), fold(high)),
           last = std::max(fold(low), fold(high));
      auto block = low / step - (low % step < 0);
      for (auto edge = (block + 1) * step; edge <= high; edge += step) {
        first = std::min({first, fold(edge), fold(edge - 1)});
        last = std::max({last, fold(edge), fold(edge - 1)});
      }
      add(first, last);
    }
  }
  return out;
}
ps_kernel_status_v8 halo(void*, const ps_mapping_query_v1* q,
                         const ps_mapping_sink_v1* sink) {
  if (!q || q->struct_size != sizeof(*q) || !sink ||
      sink->struct_size != sizeof(*sink) || q->source_count != 1 ||
      q->config_count != 7 || !q->config || !q->sources)
    return invalid();
  const auto* cfg = q->config;
  const auto& grid = q->sources[0].grid;
  if (grid.rank != q->region.rank || cfg[0] >= grid.rank ||
      cfg[1] >= grid.rank || cfg[0] == cfg[1] || cfg[6] > Truncate)
    return invalid();
  Support axes[2];
  for (unsigned i = 0; i < 2; ++i) {
    const auto a = cfg[i];
    if (!grid.shape[a] || !q->region.extent[a])
      return invalid();
    const Wide lo = Wide(q->region.origin[a]) - cfg[2 + 2 * i];
    const Wide hi =
        Wide(q->region.origin[a]) + q->region.extent[a] - 1 + cfg[3 + 2 * i];
    axes[i] = axis(lo, hi, grid.shape[a], cfg[6]);
  }
  std::array<ps_mapping_region_v1, 4> all{};
  unsigned n = 0;
  for (unsigned y = 0; y < axes[0].count; ++y)
    for (unsigned x = 0; x < axes[1].count; ++x) {
      auto& r = all[n++];
      r = q->region;
      r.origin[cfg[0]] = axes[0].spans[y].first;
      r.extent[cfg[0]] = axes[0].spans[y].length;
      r.origin[cfg[1]] = axes[1].spans[x].first;
      r.extent[cfg[1]] = axes[1].spans[x].length;
    }
  return sink->regions(sink->context, 0, n ? all.data() : nullptr, n);
}
ps_kernel_status_v8 plane(void*, const ps_mapping_query_v1* q,
                          const ps_mapping_sink_v1* sink) {
  if (!q || q->struct_size != sizeof(*q) || !sink ||
      sink->struct_size != sizeof(*sink) || !q->config ||
      q->config_count != 2 || q->config[0] == q->config[1])
    return invalid();
  for (unsigned i = 0; i < q->source_count; ++i) {
    auto region = q->region;
    const auto& grid = q->sources[i].grid;
    if (grid.rank != region.rank || q->config[0] >= grid.rank ||
        q->config[1] >= grid.rank)
      return invalid();
    for (unsigned a = 0; a < 2; ++a) {
      auto k = q->config[a];
      region.origin[k] = 0;
      region.extent[k] = grid.shape[k];
    }
    auto status = sink->regions(sink->context, i, &region, 1);
    if (status.code)
      return status;
  }
  return {};
}
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr ps_mapper_v1 mappers[] = {{sizeof(ps_mapper_v1),
                                     1,
                                     {"halo", 4},
                                     {"geometry", 8},
                                     1,
                                     1,
                                     nullptr,
                                     halo},
                                    {sizeof(ps_mapper_v1),
                                     1,
                                     {"plane", 5},
                                     {"geometry", 8},
                                     1,
                                     1,
                                     nullptr,
                                     plane}};
// NOLINTEND
}  // namespace
const ps_mapper_v1* geometry_mapper_definitions() {
  return mappers;
}
