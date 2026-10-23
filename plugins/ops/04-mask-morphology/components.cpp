#include "04-mask-morphology/components.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/registry.hpp"
#include "photospider/ops/mask.hpp"

namespace ps::plugin_internal::mask {
namespace {
enum class ComponentKind { Compact, MinPixel, Count, Areas, Bboxes, Bundle };
using ComponentRow = std::array<std::uint64_t, 7>;
std::uint64_t component_hash(std::uint64_t value) {
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}
struct Components final : Program {
  ComponentKind kind;
  std::uint64_t height, width, count, maximum_count = 0, index_capacity = 2;
  bool diagonal = false;
  Components(ComponentKind k, const std::vector<OperationMetadata>& in,
             const Parameters& p)
      : kind(k) {
    const auto& source = labeling() ? image(in[0], 2) : label_image(in[0]);
    const auto dims = source.sample_shape();
    height = dims[0];
    width = dims[1];
    count = size_product(dims);
    if (labeling()) {
      diagonal = component_diagonal(p, "connectivity");
      const auto maximum = integer(p, "maximum_count");
      parameter(maximum >= 0 && static_cast<std::uint64_t>(maximum) <= count,
                "component maximum_count capacity");
      maximum_count = static_cast<std::uint64_t>(maximum);
    }
    collective = true;
    complete = table();
    if (kind == ComponentKind::Areas)
      add_output(this, take(ps::mask::component_area_index_schema(count)));
    else if (kind == ComponentKind::Bboxes)
      add_output(this, take(ps::mask::component_bbox_index_schema(count)));
    else if (kind == ComponentKind::Bundle)
      add_output(this, take(ps::mask::component_bundle_schema(height, width)));
    else if (kind == ComponentKind::Count)
      add_output(this, tensor_schema(ElementType::Int64, {1}));
    else
      add_output(this, spatial_schema(ElementType::Int64, dims));
    if (labeling()) {
      scratch_bytes =
          bytes(static_cast<unsigned __int128>(count) * 16 +
                (kind == ComponentKind::Bundle
                     ? static_cast<unsigned __int128>(maximum_count) *
                           sizeof(ComponentRow)
                     : 0));
    } else {
      while (index_capacity < count * 2)
        index_capacity *= 2;
      scratch_bytes =
          bytes(static_cast<unsigned __int128>(index_capacity) * 8 +
                static_cast<unsigned __int128>(count) * row_columns() * 8);
    }
  }
  bool labeling() const {
    return kind == ComponentKind::Compact || kind == ComponentKind::MinPixel ||
           kind == ComponentKind::Bundle;
  }
  bool table() const {
    return kind == ComponentKind::Areas || kind == ComponentKind::Bboxes ||
           kind == ComponentKind::Bundle;
  }
  unsigned row_columns() const {
    return kind == ComponentKind::Count    ? 1
           : kind == ComponentKind::Areas  ? 2
           : kind == ComponentKind::Bboxes ? 5
                                           : 7;
  }
  template <unsigned Columns>
  std::uint64_t measure(const Reader& reader, std::uint64_t* index,
                        std::uint64_t* storage, Visits* visits) const {
    using Row = std::array<std::uint64_t, Columns>;
    auto* rows = reinterpret_cast<Row*>(storage);
    std::uint64_t groups = 0, capacity = 2, previous = 0, previous_row = 0;
    const auto clear_index = [&] {
      for (std::uint64_t i = 0; i < capacity;) {
        const auto length = std::min(UINT64_C(256), capacity - i);
        visits->work.poll(length);
        std::memset(index + i, 0, length * 8);
        i += length;
      }
    };
    const auto find = [&](std::uint64_t id) {
      auto slot = component_hash(id) & (capacity - 1);
      while (index[slot] && rows[index[slot] - 1][0] != id) {
        slot = (slot + 1) & (capacity - 1);
        visits->step();
      }
      visits->step();
      return slot;
    };
    clear_index();
    component_read_rows(
        reader, 0, 0, height, width, visits->work,
        [&](const void* data, std::ptrdiff_t stride, std::uint64_t y,
            std::uint64_t x, std::uint64_t length) {
          for (std::uint64_t j = 0; j < length; ++j) {
            const auto id = load<std::uint64_t>(data, stride, j);
            if (!id)
              continue;
            if (id != previous) {
              auto slot = find(id);
              if (!index[slot]) {
                // Grow inside admitted storage; aggregate rows let us rebuild
                // in place without retaining another index or rescanning input.
                if (groups == capacity / 2) {
                  capacity *= 2;
                  clear_index();
                  for (std::uint64_t r = 0; r < groups; ++r)
                    index[find(rows[r][0])] = r + 1;
                  slot = find(id);
                }
                rows[groups] = {};
                rows[groups][0] = id;
                if constexpr (Columns == 5) {
                  rows[groups][1] = height;
                  rows[groups][2] = width;
                }
                index[slot] = ++groups;
              }
              previous = id;
              previous_row = index[slot] - 1;
            }
            auto& row = rows[previous_row];
            if constexpr (Columns == 2) {
              ++row[1];
            } else if constexpr (Columns == 5) {
              row[1] = std::min(row[1], y);
              row[2] = std::min(row[2], x + j);
              row[3] = std::max(row[3], y + 1);
              row[4] = std::max(row[4], x + j + 1);
            }
          }
        });
    if constexpr (Columns > 1) {
      std::sort(rows, rows + groups, [&](const Row& a, const Row& b) {
        visits->step();
        return a[0] < b[0];
      });
    }
    return groups;
  }
  void write_table(const ps_kernel_mutable_view_v8& out,
                   const std::uint64_t* rows, std::uint64_t groups,
                   const Work& work) const {
    const auto y0 = out.layout->origin[0], x0 = out.layout->origin[1];
    const auto end = y0 + out.layout->shape[0];
    const auto columns = out.layout->shape[1];
    auto y = y0;
    for (; y < std::min(end, groups); ++y) {
      work.poll(columns);
      auto* data = static_cast<std::uint8_t*>(out.allocation) +
                   offset(*out.layout, out.byte_offset, {y, x0});
      for (std::uint64_t c = 0; c < columns; ++c) {
        save<std::uint64_t>(data, out.layout->strides[1], c,
                            rows[y * row_columns() + x0 + c]);
      }
    }
    if (y == end)
      return;
    if (out.layout->strides[1] == 8 &&
        out.layout->strides[0] == static_cast<std::ptrdiff_t>(columns * 8)) {
      auto* data = static_cast<std::uint8_t*>(out.allocation) +
                   offset(*out.layout, out.byte_offset, {y, x0});
      const auto padding = (end - y) * columns;
      for (std::uint64_t first = 0; first < padding;) {
        const auto n = std::min(UINT64_C(256), padding - first);
        work.poll(n);
        std::memset(data + first * 8, 0, n * 8);
        first += n;
      }
    } else {
      for (; y < end; ++y) {
        work.poll(columns);
        auto* data = static_cast<std::uint8_t*>(out.allocation) +
                     offset(*out.layout, out.byte_offset, {y, x0});
        for (std::uint64_t c = 0; c < columns; ++c)
          save<std::uint64_t>(data, out.layout->strides[1], c, 0);
      }
    }
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    Visits visits{work};
    const Reader reader{kernel, inv};
    auto* labels = static_cast<std::uint64_t*>(inv.scratch[0].allocation);
    auto* queue = labels + count;
    auto* rows = labeling() ? queue + count : labels + index_capacity;
    std::uint64_t groups;
    if (labeling()) {
      component_input(reader, labels, height, width, true, &visits);
      groups =
          component_scan(labels, queue, height, width, diagonal,
                         kind == ComponentKind::Compact, maximum_count, &visits,
                         [&](std::uint64_t row, std::uint64_t id,
                             const ComponentBounds& b, const std::uint64_t*) {
                           if (kind == ComponentKind::Bundle)
                             reinterpret_cast<ComponentRow*>(rows)[row] = {
                                 id, b.area, b.first, b.y0, b.x0, b.y1, b.x1};
                         });
    } else {
      groups = kind == ComponentKind::Count
                   ? measure<1>(reader, labels, rows, &visits)
               : kind == ComponentKind::Areas
                   ? measure<2>(reader, labels, rows, &visits)
                   : measure<5>(reader, labels, rows, &visits);
    }
    for (unsigned j = 0; j < inv.output_count; ++j) {
      const auto field = kernel.writes[j].second;
      const auto& out = inv.outputs[j];
      if (kind == ComponentKind::Count ||
          (table() && field == (kind == ComponentKind::Bundle ? 2U : 1U))) {
        work.poll();
        store(out, {0}, groups);
      } else if (!table() || (kind == ComponentKind::Bundle && field == 0)) {
        component_write_image<std::uint64_t>(
            out, width, work,
            [&](std::uint64_t pixel) { return labels[pixel]; });
      } else {
        write_table(out, rows, groups, work);
      }
    }
    visits.flush();
  }
};
struct FilterArea final : Program {
  std::uint64_t height, width, count, minimum_area;
  ElementType type;
  FilterArea(const std::vector<OperationMetadata>& in, const Parameters& p) {
    const auto dims = label_image(in[0]).sample_shape();
    height = dims[0];
    width = dims[1];
    count = size_product(dims);
    require(in[1].result_schema != nullptr, "missing component area schema");
    const auto expected = take(ps::mask::component_area_index_schema(count));
    require(in[1].result_schema->same_schema(expected),
            "component area schema mismatch");
    minimum_area = static_cast<std::uint64_t>(integer(p, "minimum_area"));
    type = output_dtype(p);
    collective = true;
    scratch_bytes = bytes(static_cast<unsigned __int128>(count) * 9);
    add_output(this, spatial_schema(type, dims));
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    Visits visits{work};
    const Reader reader{kernel, inv};
    const auto groups = reader.raw(1, 1, {0});
    auto* ids = static_cast<std::uint64_t*>(inv.scratch[0].allocation);
    auto* selected = reinterpret_cast<std::uint8_t*>(ids + count);
    // The complete, sorted effective prefix is a caller premise. This builds
    // the lookup, without remeasuring labels or examining unused padding.
    for (std::uint64_t i = 0; i < groups; ++i) {
      ids[i] = reader.raw(1, 0, {i, 0});
      selected[i] = reader.raw(1, 0, {i, 1}) >= minimum_area;
      visits.step();
    }
    std::uint64_t previous = 0;
    bool previous_selected = false;
    const auto emit = [&](auto value_type) {
      using T = decltype(value_type);
      for (unsigned j = 0; j < inv.output_count; ++j) {
        unary_rows<T>(
            reader, inv.outputs[j], work,
            [&](const void* input, std::ptrdiff_t input_stride, void* output,
                std::ptrdiff_t output_stride, std::uint64_t length) {
              for (std::uint64_t i = 0; i < length; ++i) {
                const auto label = load<std::uint64_t>(input, input_stride, i);
                bool keep = false;
                if (label) {
                  if (label != previous) {
                    std::uint64_t first = 0, last = groups;
                    while (first < last) {
                      const auto mid = first + (last - first) / 2;
                      if (ids[mid] < label)
                        first = mid + 1;
                      else
                        last = mid;
                      visits.step();
                    }
                    previous = label;
                    previous_selected = selected[first] != 0;
                  }
                  keep = previous_selected;
                }
                save<T>(output, output_stride, i, static_cast<T>(keep));
              }
            });
      }
    };
    if (type == ElementType::Float32)
      emit(float{});
    else
      emit(double{});
    visits.flush();
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_components(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  try {
    for (auto entry :
         {std::make_pair("label_compact", ComponentKind::Compact),
          std::make_pair("label_min_pixel", ComponentKind::MinPixel),
          std::make_pair("component_count", ComponentKind::Count),
          std::make_pair("component_areas", ComponentKind::Areas),
          std::make_pair("component_bboxes", ComponentKind::Bboxes),
          std::make_pair("component_bundle", ComponentKind::Bundle)}) {
      std::vector<Parameter> parameters;
      const auto kind = entry.second;
      if (kind == ComponentKind::Compact || kind == ComponentKind::MinPixel ||
          kind == ComponentKind::Bundle) {
        parameters = {{"connectivity", Type::Int64},
                      {"maximum_count", Type::Int64}};
      }
      SchemaTemplate schema;
      std::string output;
      if (kind == ComponentKind::Areas) {
        schema = take(ps::mask::component_area_index_schema(1));
        output = "area_index";
      } else if (kind == ComponentKind::Bboxes) {
        schema = take(ps::mask::component_bbox_index_schema(1));
        output = "bbox_index";
      } else if (kind == ComponentKind::Bundle) {
        schema = take(ps::mask::component_bundle_schema(1, 1));
        output = "components";
      } else if (kind == ComponentKind::Count) {
        schema = tensor_schema(ElementType::Int64, {1});
        output = "count";
      } else {
        schema = spatial_schema(ElementType::Int64, {1, 1});
        output = "labels";
      }
      check(registry->register_operation(generation::operation(
          std::string("mask.") + entry.first + "_strict", 1,
          std::move(parameters), {{output, schema}},
          [kind](const auto& in, const auto& p) {
            return std::make_shared<Components>(kind, in, p);
          })));
    }
    auto filter = generation::operation(
        "mask.filter_area_index_strict", 2,
        {{"output_dtype", Type::String}, {"minimum_area", Type::Int64}},
        {{"mask", spatial_schema(ElementType::Float64, {1, 1})}},
        [](const auto& in, const auto& p) {
          return std::make_shared<FilterArea>(in, p);
        });
    filter.traits.input_schema[1].element_type_mask = 0;
    filter.traits.input_schema[1].result_schema_id =
        "photospider.ops.mask.component_areas";
    filter.traits.input_schema[1].result_schema_version = 1;
    check(registry->register_operation(std::move(filter)));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
