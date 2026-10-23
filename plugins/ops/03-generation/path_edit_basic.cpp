#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/path_support.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
enum class BasicPathKind { Transform, Uniform, Chaikin };
struct BasicPath final : Program {
  BasicPathKind kind;
  PathInfo input, output;
  std::uint64_t count = 0, iterations = 0, max_vertices = 0;
  DenseLayout storage;
  BasicPath(BasicPathKind selected, const std::vector<OperationMetadata>& in,
            const Parameters& p)
      : kind(selected),
        input(path_info(*in[0].result_schema)),
        output(input),
        storage(*in[0].result_schema) {
    complete = true;
    if (kind == BasicPathKind::Transform) {
      shape(in[1], {2, 3});
      require(text(p, "width_policy", "preserve_px") == "preserve_px",
              "centerline transform width_policy must be preserve_px");
      add_output(this, *in[0].result_schema);
    } else if (kind == BasicPathKind::Uniform) {
      const auto n = integer(p, "samples_per_segment", 2);
      require(n >= 2, "sample count defines a positive fixed output capacity");
      count = static_cast<std::uint64_t>(n);
      const auto bound = size_product({input.capacities[0], count});
      max_vertices =
          integer(p, "max_count", checked_add(bound, subpath_capacity(input)));
      require(text(p, "endpoint_mode", "per_segment") == "per_segment",
              "unknown parameter sampling endpoint mode");
      add_output(this, take(ps::generation::path_samples_schema(
                           max_vertices, subpath_capacity(input))));
    } else {
      require(!input.primitive, "Chaikin requires CoreVerbs");
      const auto rounds = integer(p, "iterations", 1);
      require(rounds >= 0 && rounds <= 12,
              "Chaikin iteration count controls static backing");
      iterations = rounds;
      max_vertices = integer(p, "max_vertices", 1048576);
      require(text(p, "open_endpoint_policy", "preserve") == "preserve",
              "unknown Chaikin endpoint policy");
      if (iterations) {
        output = output_core(p, max_vertices, subpath_capacity(input));
        output.capacities[1] = max_vertices;
        output.capacities[0] = checked_add(max_vertices, output.capacities[2]);
      }
      add_output(this, path_schema(output));
    }
    storage = DenseLayout(*outputs[0].result_schema);
    scratch_bytes = sizeof(EvaluationScratch) + storage.bytes;
    if (kind == BasicPathKind::Chaikin && iterations) {
      scratch_bytes =
          checked_add(scratch_bytes, size_product({2, max_vertices, 2, 8}));
    }
  }
  void transform(const Reader& reader, const Dense& out, Math& m) const {
    copy_dense(reader, 0, out);
    std::array<Rational, 6> a;
    bool identity = true;
    for (unsigned axis = 0; axis < 2; ++axis) {
      for (unsigned j = 0; j < 3; ++j) {
        a[3 * axis + j] = m.input(reader, 1, 0, {axis, j});
        identity &= reader.number(1, 0, {axis, j}) == (j == axis ? 1.0 : 0.0);
      }
    }
    if (identity) {
      return;
    }
    const auto counts = path_counts(reader, 0, input.primitive);
    const auto apply = [&](unsigned field, std::uint64_t row,
                           std::uint64_t columns, std::uint64_t offset,
                           bool translate) {
      const auto x =
          m.exact.binary(linear_read(reader, 0, field, row * columns + offset));
      const auto y = m.exact.binary(
          linear_read(reader, 0, field, row * columns + offset + 1));
      for (unsigned axis = 0; axis < 2; ++axis) {
        m.work.poll();
        auto v = m.add(m.mul(a[3 * axis], x), m.mul(a[3 * axis + 1], y));
        if (translate) {
          v = m.add(v, a[3 * axis + 2]);
        }
        out.set(field, row * columns + offset + axis, m.rounded(v, false));
      }
    };
    if (!input.primitive) {
      for (std::uint64_t row = 0; row < counts[1]; ++row) {
        apply(2, row, 2, 0, true);
      }
      return;
    }
    for (std::uint64_t row = 0; row < counts[1]; ++row) {
      if (reader.index(0, 1, {row}) == reader.index(0, 1, {row + 1})) {
        apply(3, row, 2, 0, true);
      }
    }
    for (std::uint64_t row = 0; row < counts[2]; ++row) {
      for (std::int64_t j = 0; j <= reader.index(0, 4, {row}); ++j) {
        apply(5, row, 8, 2 * j, true);
      }
    }
    for (std::uint64_t row = 0; row < counts[3]; ++row) {
      for (unsigned j = 0; j < 3; ++j) {
        apply(6, row, 8, 2 * j, j == 0);
      }
    }
    for (std::uint64_t row = 0; row < counts[4]; ++row) {
      for (unsigned j = 0; j < 4; ++j) {
        apply(7, row, 8, 2 * j, j < 2);
      }
    }
    for (std::uint64_t row = 0; row < counts[6]; ++row) {
      apply(9, row, 2, 0, true);
    }
  }
  void uniform(const Reader& reader, const Dense& out,
               EvaluationScratch& scratch) const {
    auto& m = scratch.math;
    const PathCursor path(reader, input, m);
    CurveMath evaluation(reader, scratch);
    std::uint64_t used = 0;
    for (std::uint64_t s = 0; s < path.subpaths(); ++s) {
      out.set(3, s, used);
      if (path.first(s) == path.end(s)) {
        operational_capacity(used < max_vertices);
        out.set(0, 2 * used, path.isolated(s, 0));
        out.set(0, 2 * used + 1, path.isolated(s, 1));
        out.set(1, used++, UINT64_MAX);
        continue;
      }
      for (auto row = path.first(s); row < path.end(s); ++row) {
        const auto curve = path.curve(row);
        for (std::uint64_t j = 0; j < count; ++j) {
          m.work.poll();
          operational_capacity(used < max_vertices);
          m.exact.reset();
          const auto t = m.exact.fraction(j, count - 1);
          out.set(0, 2 * used, evaluation.sample(curve, t, 0, 0, false));
          out.set(0, 2 * used + 1, evaluation.sample(curve, t, 0, 1, false));
          out.set(1, used, row);
          out.set(2, used, m.rounded(t, false));
          ++used;
        }
      }
    }
    for (auto s = path.subpaths(); s <= subpath_capacity(input); ++s) {
      out.set(3, s, used);
    }
    out.set(4, 0, used);
    out.set(4, 1, path.subpaths());
    write_id(out, 5, 0, source_id(reader, 0));
  }
  void chaikin(const Reader& reader, const Dense& out, Math& m,
               std::uint64_t* first, std::uint64_t* second) const {
    if (!iterations) {
      copy_dense(reader, 0, out);
      return;
    }
    const PathCursor path(reader, input, m);
    CoreWriter writer(out, output);
    for (std::uint64_t s = 0; s < path.subpaths(); ++s) {
      const auto begin = reader.index(0, 1, {path.begin(s)});
      const auto end = reader.index(0, 1, {path.end(s)});
      std::uint64_t n = end - begin;
      operational_capacity(n <= max_vertices);
      for (std::uint64_t j = 0; j < n; ++j) {
        for (unsigned axis = 0; axis < 2; ++axis) {
          first[2 * j + axis] = reader.bits(0, 2, {begin + j, axis});
        }
      }
      for (std::uint64_t round = 0; round < iterations && n > 1; ++round) {
        const auto next = 2 * n;
        operational_capacity(next <= max_vertices);
        std::uint64_t cursor = 0;
        if (!path.closed(s)) {
          second[0] = first[0];
          second[1] = first[1];
          ++cursor;
        }
        for (std::uint64_t j = 0; j < n - (path.closed(s) ? 0 : 1); ++j) {
          m.work.poll();
          for (unsigned a = 0; a < 2; ++a) {
            const auto x = m.exact.binary(first[2 * j + a]);
            const auto y = m.exact.binary(first[2 * ((j + 1) % n) + a]);
            second[2 * cursor + a] = m.rounded(
                m.div(m.add(m.scale(x, 3), y), m.exact.integer(4)), false);
            second[2 * (cursor + 1) + a] = m.rounded(
                m.div(m.add(x, m.scale(y, 3)), m.exact.integer(4)), false);
          }
          cursor += 2;
        }
        if (!path.closed(s)) {
          second[2 * cursor] = first[2 * (n - 1)];
          second[2 * cursor + 1] = first[2 * (n - 1) + 1];
          ++cursor;
        }
        n = cursor;
        std::swap(first, second);
      }
      writer.begin(path.closed(s));
      for (std::uint64_t j = 0; j < n; ++j) {
        writer.point(j ? 2 : 1, first[2 * j], first[2 * j + 1]);
      }
      if (path.closed(s)) {
        writer.append(5, nullptr, 0);
      }
    }
    writer.finish();
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* bytes = static_cast<std::uint8_t*>(invocation.scratch[0].allocation);
    EvaluationArena arithmetic(bytes, work);
    Dense out{storage, bytes + sizeof(EvaluationScratch), work};
    out.clear();
    if (kind == BasicPathKind::Transform) {
      transform(reader, out, arithmetic.p->math);
    } else if (kind == BasicPathKind::Uniform) {
      uniform(reader, out, *arithmetic.p);
    } else {
      auto* buffer = reinterpret_cast<std::uint64_t*>(
          bytes + sizeof(EvaluationScratch) + storage.bytes);
      chaikin(reader, out, arithmetic.p->math, buffer,
              buffer + (iterations ? 2 * max_vertices : 0));
    }
    out.publish(kernel, invocation);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_path_basic_edit(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    struct Entry {
      const char* key;
      BasicPathKind kind;
      unsigned inputs;
    };
    for (const auto& entry :
         {Entry{"path.transform_centerline_v1_strict", BasicPathKind::Transform,
                2},
          Entry{"path.resample_uniform_t_v1_strict", BasicPathKind::Uniform, 1},
          Entry{"path.smooth_chaikin_v1_strict", BasicPathKind::Chaikin, 1}}) {
      auto parameters = path_capacity_parameters();
      if (entry.kind == BasicPathKind::Transform) {
        parameters.push_back(
            {"width_policy", OperationParameterType::String, false});
      }
      if (entry.kind == BasicPathKind::Uniform) {
        parameters.push_back(
            {"samples_per_segment", OperationParameterType::Int64, false});
        parameters.push_back(
            {"max_count", OperationParameterType::Int64, false});
        parameters.push_back(
            {"endpoint_mode", OperationParameterType::String, false});
      }
      if (entry.kind == BasicPathKind::Chaikin) {
        parameters.push_back(
            {"iterations", OperationParameterType::Int64, false});
        parameters.push_back(
            {"max_vertices", OperationParameterType::Int64, false});
        parameters.push_back(
            {"open_endpoint_policy", OperationParameterType::String, false});
      }
      auto definition = operation(
          entry.key, entry.inputs, parameters,
          {{entry.kind == BasicPathKind::Uniform ? "samples" : "path",
            entry.kind == BasicPathKind::Uniform
                ? take(ps::generation::path_samples_schema(1, 1))
                : take(ps::generation::core_path_schema({}))}},
          [kind = entry.kind](const auto& in, const auto& p) {
            return std::make_shared<BasicPath>(kind, in, p);
          },
          false, entry.kind != BasicPathKind::Uniform);
      any_path_input(&definition, 0);
      if (entry.kind == BasicPathKind::Transform) {
        any_path_output(&definition, 0);
      }
      check(registry->register_operation(std::move(definition)));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
