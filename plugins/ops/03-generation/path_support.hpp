#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <utility>
#include <vector>

#include "03-generation/path_math.hpp"

namespace ps::plugin_internal::generation {
inline std::uint64_t source_id(const Reader& r, unsigned port) {
  return r.invocation
      .input_object_ids[r.kernel
                            .groups[r.kernel.input_base[port + r.input_offset]]
                            .slots[0]
                            .second];
}
inline void write_id(const Dense& out, unsigned member, std::uint64_t row,
                     std::uint64_t id) {
  for (unsigned byte = 0; byte < 8; ++byte)
    out.set(member, row * 8 + byte, (id >> (8 * byte)) & 255);
}
struct EvaluationArena final {
  EvaluationScratch* p;
  EvaluationArena(void* storage, const Work& work)
      : p(new (storage) EvaluationScratch(work)) {}
  ~EvaluationArena() { p->~EvaluationScratch(); }
};
inline std::uint64_t subpath_capacity(const PathInfo& p) {
  return p.capacities[p.primitive ? 1 : 2];
}
struct PathCursor final {
  const Reader& reader;
  const PathInfo& info;
  Math& m;
  std::array<std::uint64_t, 8> count;
  PathCursor(const Reader& r, const PathInfo& i, Math& arithmetic)
      : reader(r),
        info(i),
        m(arithmetic),
        count(path_counts(r, 0, i.primitive)) {}
  std::uint64_t subpaths() const { return count[info.primitive ? 1 : 2]; }
  std::uint64_t begin(std::uint64_t s) const {
    return reader.index(0, info.primitive ? 1 : 3, {s});
  }
  std::uint64_t end(std::uint64_t s) const { return begin(s + 1); }
  bool closed(std::uint64_t s) const {
    return reader.bits(0, info.primitive ? 2 : 4, {s}) != 0;
  }
  Curve curve(std::uint64_t row) const {
    return info.primitive ? primitive_curve(reader, row)
                          : core_curve(reader, m, row);
  }
  std::uint64_t first(std::uint64_t s) const {
    return begin(s) + (info.primitive ? 0 : 1);
  }
  std::uint64_t isolated(std::uint64_t s, unsigned axis) const {
    return info.primitive ? reader.bits(0, 3, {s, axis})
                          : reader.bits(0, 2,
                                        {static_cast<std::uint64_t>(
                                             reader.index(0, 1, {begin(s)})),
                                         axis});
  }
};
struct CoreWriter final {
  const Dense& out;
  CoreWriter(const Dense& storage, const PathInfo& capacity)
      : out(storage), cap(capacity.capacities) {}
  std::array<std::uint64_t, 8> cap;
  std::uint64_t verbs = 0, controls = 0, subpaths = 0;
  void begin(bool closed) {
    operational_capacity(subpaths < cap[2]);
    out.set(3, subpaths, verbs);
    out.set(4, subpaths++, closed);
  }
  void append(unsigned verb, const std::uint64_t* xy, unsigned count) {
    out.work.poll(count + 1);
    operational_capacity(verbs < cap[0] && count <= cap[1] - controls);
    out.set(0, verbs, verb);
    out.set(1, verbs++, controls);
    for (unsigned j = 0; j < 2 * count; ++j)
      out.set(2, 2 * controls + j, xy[j]);
    controls += count;
  }
  void point(unsigned verb, std::uint64_t x, std::uint64_t y) {
    const std::uint64_t xy[] = {x, y};
    append(verb, xy, 1);
  }
  void finish() {
    for (auto v = verbs; v <= cap[0]; ++v)
      out.set(1, v, controls);
    for (auto s = subpaths; s <= cap[2]; ++s)
      out.set(3, s, verbs);
    out.set(5, 0, verbs);
    out.set(5, 1, controls);
    out.set(5, 2, subpaths);
  }
};
inline void copy_dense(const Reader& reader, unsigned port, const Dense& out) {
  for (unsigned member = 0; member < out.layout.counts.size(); ++member)
    for (std::uint64_t j = 0; j < out.layout.counts[member]; ++j) {
      out.work.poll();
      out.set(member, j, linear_read(reader, port, member, j));
    }
}
inline PathInfo output_core(const Parameters& p, std::uint64_t vertices = 4096,
                            std::uint64_t subpaths = 256) {
  PathInfo out;
  out.capacities = {
      static_cast<std::uint64_t>(integer(p, "max_verbs", vertices + subpaths)),
      static_cast<std::uint64_t>(integer(p, "max_controls", vertices)),
      static_cast<std::uint64_t>(integer(p, "max_subpaths", subpaths))};
  // The public factory performs fixed-extent and checked-product validation.
  path_schema(out);
  return out;
}
inline void geometry_report(const Dense& report, const Reader& reader,
                            unsigned sources, double approximation = 0,
                            double quantization = 0, double publication = 0,
                            std::int64_t topology = 0, bool dropped = false) {
  report.set(0, 0, bits(approximation));
  report.set(0, 1, bits(quantization));
  report.set(0, 2, bits(publication));
  report.set(1, 0, topology);
  report.set(2, 0, dropped);
  for (unsigned i = 0; i < sources; ++i)
    write_id(report, 3, i, source_id(reader, i));
}
inline void any_path_input(OperationDefinition* definition, unsigned port) {
  auto& input = definition->traits.input_schema[port];
  input.element_type_mask = 0;
  input.tensor_key = "counts";
  input.element_type = static_cast<std::uint32_t>(ElementType::Int64);
}
inline void any_path_output(OperationDefinition* definition, unsigned port) {
  auto& output = definition->traits.outputs[port].output_schema;
  output.result_schema_id.clear();
  output.result_schema_version = 0;
  output.tensor_key = "counts";
  output.element_type = static_cast<std::uint32_t>(ElementType::Int64);
}
inline std::vector<OperationParameterSpec> path_capacity_parameters() {
  return {{"max_verbs", OperationParameterType::Int64, false},
          {"max_controls", OperationParameterType::Int64, false},
          {"max_subpaths", OperationParameterType::Int64, false},
          {"attribute_policy", OperationParameterType::String, false}};
}
[[noreturn]] inline void invalid_quality(const char* why) {
  throw Status{ErrorCode::OperationFailed, why,
               DomainErrorCode{UINT64_C(0x47454e0003)}};
}
[[noreturn]] inline void no_solution(const char* why) {
  throw Status{ErrorCode::OperationFailed, why,
               DomainErrorCode{UINT64_C(0x47454e0002)}};
}
}  // namespace ps::plugin_internal::generation
