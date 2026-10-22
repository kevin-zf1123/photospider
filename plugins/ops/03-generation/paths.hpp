#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "03-generation/math.hpp"
#include "photospider/ops/generation.hpp"

namespace ps::plugin_internal::generation {
struct PathInfo final {
  bool primitive = false;
  std::array<std::uint64_t, 8> capacities{};
};
inline PathInfo path_info(const SchemaTemplate& schema) {
  check(schema.validate());
  PathInfo info;
  require(schema.version == 1, "unsupported PathSet schema version");
  if (schema.id == "photospider.ops.path_core") {
    require(schema.tensors.size() == 6, "Core PathSet field count");
    const auto& fields = schema.tensors;
    for (const auto& f : fields) {
      require(!f.sample_shape().empty(), "PathSet field rank");
    }
    info.capacities = {fields[0].sample_shape()[0], fields[2].sample_shape()[0],
                       fields[4].sample_shape()[0]};
    const auto expected = take(ps::generation::core_path_schema(
        {info.capacities[0], info.capacities[1], info.capacities[2]}));
    require(schema.same_schema(expected),
            "Core PathSet schema directory mismatch");
  } else {
    require(schema.id == "photospider.ops.path_primitives" &&
                schema.tensors.size() == 13,
            "operation requires Core or Primitives PathSet");
    for (const auto& f : schema.tensors) {
      require(!f.sample_shape().empty(), "PathSet field rank");
    }
    info.primitive = true;
    const unsigned rows[] = {0, 2, 4, 6, 7, 8, 9, 10};
    for (unsigned i = 0; i < 8; ++i) {
      info.capacities[i] = schema.tensors[rows[i]].sample_shape()[0];
    }
    const auto& c = info.capacities;
    const auto expected = take(ps::generation::primitive_path_schema(
        {c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]}));
    require(schema.same_schema(expected),
            "Primitives PathSet schema directory mismatch");
  }
  return info;
}
inline SchemaTemplate path_schema(const PathInfo& info) {
  const auto& c = info.capacities;
  return info.primitive
             ? take(ps::generation::primitive_path_schema(
                   {c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]}))
             : take(ps::generation::core_path_schema({c[0], c[1], c[2]}));
}
inline std::array<std::uint64_t, 8> path_counts(const Reader& reader,
                                                unsigned port, bool primitive) {
  std::array<std::uint64_t, 8> counts{};
  for (unsigned i = 0; i < (primitive ? 8U : 3U); ++i) {
    counts[i] =
        static_cast<std::uint64_t>(reader.index(port, primitive ? 12 : 5, {i}));
  }
  return counts;
}
struct DenseLayout final {
  std::vector<std::uint64_t> offsets, counts;
  std::vector<unsigned> widths;
  std::uint64_t bytes = 0;
  explicit DenseLayout(const SchemaTemplate& schema) {
    for (const auto& field : schema.tensors) {
      bytes = (bytes + 7) & ~UINT64_C(7);
      offsets.push_back(bytes);
      const auto count = size_product(field.sample_shape());
      counts.push_back(count);
      widths.push_back(width(field.descriptor.element_type));
      bytes += count * widths.back();
    }
    bytes = (bytes + 7) & ~UINT64_C(7);
  }
};
struct Dense final {
  const DenseLayout& layout;
  std::uint8_t* data;
  const Work& work;
  void clear() const {
    for (std::uint64_t offset = 0; offset < layout.bytes;) {
      const auto count = std::min(UINT64_C(65536), layout.bytes - offset);
      work.poll(count);
      std::memset(data + offset, 0, count);
      offset += count;
    }
  }
  void set(unsigned member, std::uint64_t index, std::uint64_t bits) const {
    std::memcpy(data + layout.offsets[member] + index * layout.widths[member],
                &bits, layout.widths[member]);
  }
  std::uint64_t get(unsigned member, std::uint64_t index) const {
    std::uint64_t value = 0;
    std::memcpy(&value,
                data + layout.offsets[member] + index * layout.widths[member],
                layout.widths[member]);
    return value;
  }
  void publish(const Kernel& kernel, const ps_kernel_invocation_v8& invocation,
               unsigned port = 0) const {
    for (unsigned i = 0; i < kernel.writes.size(); ++i) {
      if (kernel.writes[i].first != port) {
        continue;
      }
      const auto field = kernel.writes[i].second;
      const auto& shape = kernel.program->outputs[port]
                              .result_schema->tensors[field]
                              .sample_shape();
      samples(invocation.outputs[i], [&](const Coordinate& at, std::uint64_t) {
        work.poll();
        std::uint64_t index = 0;
        for (unsigned axis = 0; axis < shape.size(); ++axis) {
          index = index * shape[axis] + at[axis];
        }
        store(invocation.outputs[i], at, get(field, index));
      });
    }
  }
};
inline std::uint64_t linear_read(const Reader& reader, unsigned port,
                                 unsigned member, std::uint64_t index) {
  const auto& shape = reader.kernel.program->inputs[port]
                          .result_schema->tensors[member]
                          .sample_shape();
  Coordinate at{};
  for (unsigned axis = shape.size(); axis-- > 0;) {
    at[axis] = index % shape[axis];
    index /= shape[axis];
  }
  return reader.bits(port, member, at);
}
inline void copy_rows(const Reader& reader, unsigned port, unsigned field,
                      const Dense& out, std::uint64_t begin, std::uint64_t end,
                      std::uint64_t destination, std::uint64_t columns = 1) {
  for (auto row = begin; row < end; ++row) {
    out.work.poll(columns);
    for (std::uint64_t j = 0; j < columns; ++j) {
      out.set(field, (destination + row - begin) * columns + j,
              linear_read(reader, port, field, row * columns + j));
    }
  }
}
inline void operational_capacity(bool admitted) {
  if (!admitted) {
    throw Status{ErrorCode::ResourceExhausted,
                 "PathSet output capacity exhausted",
                 FailureReason::CapacityLimit};
  }
}
}  // namespace ps::plugin_internal::generation
