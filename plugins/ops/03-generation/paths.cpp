#include "03-generation/paths.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/path_reverse.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
enum class PathKind { Core, Primitives, Concat, Split, Reverse, Star };
struct Paths final : Program {
  PathKind kind;
  PathInfo info;
  std::uint64_t groups = 1, points = 3, order_offset = 0, counts_offset = 0,
                cursor_offset = 0;
  std::unique_ptr<DenseLayout> storage;
  std::unique_ptr<PrimitiveReverseLayout> reversal;
  explicit Paths(PathKind selected) : kind(selected) { complete = true; }
  void finish() {
    storage = std::make_unique<DenseLayout>(*outputs[0].result_schema);
    if (kind == PathKind::Core || kind == PathKind::Primitives) {
      return;
    }
    scratch_bytes = sizeof(Math) + storage->bytes;
    if (kind == PathKind::Reverse && info.primitive) {
      reversal = std::make_unique<PrimitiveReverseLayout>(scratch_bytes, info);
      scratch_bytes = reversal->bytes;
    }
    const auto subpaths = info.capacities[info.primitive ? 1 : 2];
    if (kind == PathKind::Split) {
      order_offset = scratch_bytes;
      scratch_bytes += subpaths * 8;
      counts_offset = scratch_bytes;
      scratch_bytes += (groups + 1) * 8;
      cursor_offset = scratch_bytes;
      scratch_bytes += groups * 8;
    }
  }
  void constructor(const Kernel& kernel,
                   const ps_kernel_invocation_v8& invocation) const {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    for (unsigned i = 0; i < kernel.writes.size(); ++i) {
      const auto field = kernel.writes[i].second;
      samples(invocation.outputs[i], [&](const Coordinate& at, std::uint64_t) {
        work.poll();
        store(invocation.outputs[i], at, reader.bits(field, 0, at));
      });
    }
  }
  void core_subpath(const Reader& reader, unsigned port, std::uint64_t subpath,
                    const Dense& out,
                    std::array<std::uint64_t, 8>* used) const {
    const auto first =
        static_cast<std::uint64_t>(reader.index(port, 3, {subpath}));
    const auto last =
        static_cast<std::uint64_t>(reader.index(port, 3, {subpath + 1}));
    const auto cb = static_cast<std::uint64_t>(reader.index(port, 1, {first}));
    const auto ce = static_cast<std::uint64_t>(reader.index(port, 1, {last}));
    const auto v = (*used)[0], c = (*used)[1], s = (*used)[2];
    operational_capacity(last - first <= info.capacities[0] - v &&
                         ce - cb <= info.capacities[1] - c);
    out.set(3, s, v);
    out.set(4, s, reader.bits(port, 4, {subpath}));
    for (auto j = first; j < last; ++j) {
      out.work.poll();
      out.set(0, v + j - first, reader.bits(port, 0, {j}));
      out.set(1, v + j - first, c + reader.index(port, 1, {j}) - cb);
    }
    copy_rows(reader, port, 2, out, cb, ce, c, 2);
    (*used)[0] += last - first;
    (*used)[1] += ce - cb;
    ++(*used)[2];
  }
  void primitive_pools(const Reader& reader, unsigned port, const Dense& out,
                       const std::array<std::uint64_t, 8>& used,
                       const std::array<std::uint64_t, 8>& count) const {
    copy_rows(reader, port, 4, out, 0, count[2], used[2]);
    copy_rows(reader, port, 5, out, 0, count[2], used[2], 8);
    copy_rows(reader, port, 6, out, 0, count[3], used[3], 8);
    copy_rows(reader, port, 7, out, 0, count[4], used[4], 8);
    copy_rows(reader, port, 9, out, 0, count[6], used[6], 2);
    copy_rows(reader, port, 10, out, 0, count[7], used[7]);
    copy_rows(reader, port, 11, out, 0, count[6], used[6]);
    for (std::uint64_t row = 0; row < count[5]; ++row) {
      out.work.poll(5);
      for (unsigned field = 0; field < 5; ++field) {
        auto value = reader.index(port, 8, {row, field});
        if (field == 1) {
          value += used[6];
        }
        if (field == 3) {
          value += used[7];
        }
        out.set(8, (used[5] + row) * 5 + field,
                static_cast<std::uint64_t>(value));
      }
    }
  }
  void primitive_subpath(const Reader& reader, unsigned port,
                         std::uint64_t subpath, const Dense& out,
                         const std::array<std::uint64_t, 8>& base,
                         std::array<std::uint64_t, 8>* used,
                         bool reverse) const {
    const auto first =
        static_cast<std::uint64_t>(reader.index(port, 1, {subpath}));
    const auto last =
        static_cast<std::uint64_t>(reader.index(port, 1, {subpath + 1}));
    const auto r = (*used)[0], s = (*used)[1];
    out.set(1, s, r);
    out.set(2, s, reader.bits(port, 2, {subpath}));
    copy_rows(reader, port, 3, out, subpath, subpath + 1, s, 2);
    for (auto j = first; j < last; ++j) {
      out.work.poll();
      const auto input_row = reverse ? last - 1 - (j - first) : j;
      const auto type = reader.index(port, 0, {input_row, 0});
      auto payload =
          static_cast<std::uint64_t>(reader.index(port, 0, {input_row, 1}));
      if (type == 1) {
        payload += base[2];
      } else if (type == 2 || type == 3) {
        payload += base[3];
      } else if (type == 4) {
        payload += base[4];
      } else if (type == 5) {
        payload += base[5];
      } else if (type == 6) {
        payload = s;
      }
      out.set(0, 2 * (r + j - first), type);
      out.set(0, 2 * (r + j - first) + 1, payload);
    }
    (*used)[0] += last - first;
    ++(*used)[1];
  }
  void finalize(const Dense& out,
                const std::array<std::uint64_t, 8>& used) const {
    if (info.primitive) {
      for (auto s = used[1]; s <= info.capacities[1]; ++s) {
        out.work.poll();
        out.set(1, s, used[0]);
      }
      for (unsigned i = 0; i < 8; ++i) {
        out.set(12, i, used[i]);
      }
    } else {
      for (auto v = used[0]; v <= info.capacities[0]; ++v) {
        out.work.poll();
        out.set(1, v, used[1]);
      }
      for (auto s = used[2]; s <= info.capacities[2]; ++s) {
        out.work.poll();
        out.set(3, s, used[0]);
      }
      for (unsigned i = 0; i < 3; ++i) {
        out.set(5, i, used[i]);
      }
    }
  }
  void reverse_core(const Reader& reader, const Dense& out,
                    std::array<std::uint64_t, 8>* used) const {
    const auto count = path_counts(reader, 0, false);
    const auto vertex = [&](std::uint64_t control, unsigned axis) {
      return reader.bits(0, 2, {control, axis});
    };
    const auto append = [&](unsigned verb,
                            const std::array<std::uint64_t, 6>& controls,
                            unsigned n) {
      operational_capacity((*used)[0] < info.capacities[0] &&
                           n <= info.capacities[1] - (*used)[1]);
      out.set(0, (*used)[0], verb);
      out.set(1, (*used)[0], (*used)[1]);
      for (unsigned i = 0; i < 2 * n; ++i) {
        out.set(2, 2 * (*used)[1] + i, controls[i]);
      }
      ++(*used)[0];
      (*used)[1] += n;
    };
    for (std::uint64_t s = 0; s < count[2]; ++s) {
      out.work.poll();
      const auto begin = static_cast<std::uint64_t>(reader.index(0, 3, {s}));
      const auto end = static_cast<std::uint64_t>(reader.index(0, 3, {s + 1}));
      const bool closed = reader.bits(0, 4, {s}) != 0;
      const auto first_control =
          static_cast<std::uint64_t>(reader.index(0, 1, {begin}));
      const auto final_control =
          static_cast<std::uint64_t>(reader.index(0, 1, {end})) - 1;
      out.set(3, s, (*used)[0]);
      out.set(4, s, closed);
      const auto start = closed ? first_control : final_control;
      append(1, {vertex(start, 0), vertex(start, 1)}, 1);
      const auto limit = closed ? end - 1 : end;
      bool fold_first_line = false;
      if (closed && limit > begin + 1) {
        append(2, {vertex(final_control, 0), vertex(final_control, 1)}, 1);
        fold_first_line = reader.bits(0, 0, {begin + 1}) == 2;
      }
      for (auto row = limit; row-- > begin + 1 + (fold_first_line ? 1U : 0U);) {
        out.work.poll();
        const auto verb = static_cast<unsigned>(reader.bits(0, 0, {row}));
        const auto cb = static_cast<std::uint64_t>(reader.index(0, 1, {row}));
        const auto ce =
            static_cast<std::uint64_t>(reader.index(0, 1, {row + 1}));
        const auto degree = static_cast<unsigned>(ce - cb);
        std::array<std::uint64_t, 6> controls{};
        for (unsigned j = 0; j + 1 < degree; ++j) {
          for (unsigned a = 0; a < 2; ++a) {
            controls[2 * j + a] = vertex(ce - 2 - j, a);
          }
        }
        for (unsigned a = 0; a < 2; ++a) {
          controls[2 * (degree - 1) + a] = vertex(cb - 1, a);
        }
        append(verb, controls, degree);
      }
      if (closed) {
        append(5, {}, 0);
      }
      ++(*used)[2];
    }
  }
  void reverse_payloads(const Reader& reader, const Dense& out,
                        const std::array<std::uint64_t, 8>& count) const {
    // Shared rows are always reconstructed from the immutable source. Repeated
    // references therefore repeat the same write instead of reversing twice.
    for (std::uint64_t row = 0; row < count[0]; ++row) {
      out.work.poll();
      const auto kind = reader.index(0, 0, {row, 0});
      const auto payload =
          static_cast<std::uint64_t>(reader.index(0, 0, {row, 1}));
      if (kind == 1) {
        const auto degree =
            static_cast<unsigned>(reader.index(0, 4, {payload}));
        for (unsigned j = 0; j <= degree; ++j) {
          for (unsigned axis = 0; axis < 2; ++axis) {
            out.set(5, 8 * payload + 2 * j + axis,
                    reader.bits(0, 5, {payload, degree - j, axis}));
          }
        }
      } else if (kind == 4) {
        for (unsigned j = 0; j < 4; ++j) {
          for (unsigned axis = 0; axis < 2; ++axis) {
            const auto source = j < 2 ? 1 - j : 5 - j;
            auto value = reader.bits(0, 7, {payload, source, axis});
            if (j >= 2) {
              value ^= UINT64_C(1) << 63;
            }
            out.set(7, 8 * payload + 2 * j + axis, value);
          }
        }
      }
    }
  }
  void star(const Reader& reader, const Dense& out, Math& m,
            std::array<std::uint64_t, 8>* used) const {
    const auto rotation = m.input(reader, 2, 0, {0});
    std::array<Rational, 2> center{m.input(reader, 0, 0, {0}),
                                   m.input(reader, 0, 0, {1})};
    for (std::uint64_t vertex = 0; vertex < 2 * points; ++vertex) {
      out.work.poll();
      m.exact.reset();
      const auto radius = m.input(reader, 1, 0, {vertex % 2 ? 0U : 1U});
      out.set(0, vertex, vertex ? 2 : 1);
      out.set(1, vertex, vertex);
      for (unsigned axis = 0; axis < 2; ++axis) {
        std::uint64_t result;
        if (Exact::zero(rotation) && (2 * vertex) % points == 0) {
          const unsigned quadrant =
              static_cast<unsigned>((2 * vertex / points) & 3);
          const int cosine[] = {1, 0, -1, 0}, sine[] = {0, 1, 0, -1};
          result = m.rounded(
              m.add(center[axis],
                    m.scale(radius, axis ? sine[quadrant] : cosine[quadrant])),
              false);
        } else {
          result = m.certify(false, [&](Interval value) {
            auto angle = m.d.interval(), factor = m.d.interval(),
                 sine = m.d.interval(), cosine = m.d.interval();
            m.d.constant(angle, true);
            m.enclose(factor, m.exact.fraction(vertex, points));
            m.d.multiply(angle, angle, factor);
            m.enclose(factor, rotation);
            m.d.add(angle, angle, factor);
            m.sincos(sine, cosine, angle);
            m.enclose(factor, radius);
            m.d.multiply(value, axis ? sine : cosine, factor);
            m.enclose(factor, center[axis]);
            m.d.add(value, value, factor);
          });
        }
        out.set(2, 2 * vertex + axis, result);
      }
    }
    out.set(0, 2 * points, 5);
    out.set(1, 2 * points, 2 * points);
    out.set(4, 0, 1);
    *used = {2 * points + 1, 2 * points, 1};
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    if (kind == PathKind::Core || kind == PathKind::Primitives) {
      constructor(kernel, invocation);
      return;
    }
    const Work work{invocation, work_limit};
    const Reader reader{kernel, invocation};
    auto* scratch =
        static_cast<std::uint8_t*>(invocation.scratch[0].allocation);
    MathArena arena(scratch, work);
    Dense out{*storage, scratch + sizeof(Math), work};
    out.clear();
    std::array<std::uint64_t, 8> used{};
    if (kind == PathKind::Star) {
      star(reader, out, *arena, &used);
    } else if (kind == PathKind::Reverse && !info.primitive) {
      reverse_core(reader, out, &used);
    } else if (kind == PathKind::Split) {
      const auto count = path_counts(reader, 0, info.primitive);
      const auto ns = count[info.primitive ? 1 : 2];
      auto* order = reinterpret_cast<std::uint64_t*>(scratch + order_offset);
      auto* offsets = reinterpret_cast<std::uint64_t*>(scratch + counts_offset);
      auto* cursor = reinterpret_cast<std::uint64_t*>(scratch + cursor_offset);
      for (std::uint64_t g = 0; g <= groups; ++g) {
        work.poll();
        offsets[g] = 0;
      }
      for (std::uint64_t s = 0; s < ns; ++s) {
        work.poll();
        ++offsets[reader.index(1, 0, {s}) + 1];
      }
      for (std::uint64_t g = 0; g < groups; ++g) {
        work.poll();
        offsets[g + 1] += offsets[g];
        cursor[g] = offsets[g];
      }
      for (std::uint64_t s = 0; s < ns; ++s) {
        work.poll();
        order[cursor[reader.index(1, 0, {s})]++] = s;
      }
      if (info.primitive) {
        primitive_pools(reader, 0, out, {}, count);
        for (unsigned i = 2; i < 8; ++i) {
          used[i] = count[i];
        }
      }
      for (std::uint64_t s = 0; s < ns; ++s) {
        if (info.primitive) {
          primitive_subpath(reader, 0, order[s], out, {}, &used, false);
        } else {
          core_subpath(reader, 0, order[s], out, &used);
        }
      }
      const auto object_id =
          invocation.input_object_ids
              [kernel.groups[kernel.input_base[0]].slots[0].second];
      for (unsigned i = 0; i < kernel.writes.size(); ++i) {
        if (kernel.writes[i].first != 1) {
          continue;
        }
        const auto field = kernel.writes[i].second;
        samples(invocation.outputs[i],
                [&](const Coordinate& at, std::uint64_t) {
                  work.poll();
                  store(invocation.outputs[i], at,
                        field == 0   ? offsets[at[0]]
                        : field == 1 ? ns
                                     : (object_id >> (8 * at[0])) & 255);
                });
      }
    } else {
      const unsigned ports = kind == PathKind::Concat ? inputs.size() : 1;
      for (unsigned port = 0; port < ports; ++port) {
        const auto count = path_counts(reader, port, info.primitive);
        const auto base = used;
        if (info.primitive) {
          primitive_pools(reader, port, out, base, count);
        }
        const auto ns = count[info.primitive ? 1 : 2];
        for (std::uint64_t s = 0; s < ns; ++s) {
          if (info.primitive) {
            primitive_subpath(reader, port, s, out, base, &used,
                              kind == PathKind::Reverse);
          } else {
            core_subpath(reader, port, s, out, &used);
          }
        }
        if (info.primitive) {
          for (unsigned i = 2; i < 8; ++i) {
            used[i] += count[i];
          }
        }
        if (kind == PathKind::Reverse) {
          reverse_payloads(reader, out, count);
          reverse_spline_pools(
              reader, out, *arena, count, info.capacities,
              reinterpret_cast<ReverseKnotState*>(scratch + reversal->states),
              scratch + reversal->knots, &used);
          reverse_arc_pool(
              reader, out, *arena, count, info.capacities[3],
              reinterpret_cast<std::uint64_t*>(scratch + reversal->arcs),
              reinterpret_cast<std::uint64_t*>(scratch + reversal->arc_hash),
              &used);
        }
      }
    }
    finalize(out, used);
    out.publish(kernel, invocation);
  }
};
std::shared_ptr<Program> make_paths(
    PathKind kind, const std::vector<OperationMetadata>& inputs,
    const Parameters& p) {
  auto state = std::make_shared<Paths>(kind);
  if (kind == PathKind::Core || kind == PathKind::Primitives) {
    state->info.primitive = kind == PathKind::Primitives;
    const auto row = [&](unsigned field) {
      const auto shape = tensor(inputs[field]).sample_shape();
      return shape[0];
    };
    if (!state->info.primitive) {
      state->info.capacities = {row(0), row(2), row(4)};
    } else {
      state->info.capacities = {row(0), row(2), row(4), row(6),
                                row(7), row(8), row(9), row(10)};
    }
    auto output = path_schema(state->info);
    for (unsigned i = 0; i < output.tensors.size(); ++i) {
      const auto& field = tensor(inputs[i]);
      require(field.descriptor.element_type ==
                      output.tensors[i].descriptor.element_type &&
                  field.sample_shape() == output.tensors[i].sample_shape(),
              "PathSet constructor field shape/type mismatch");
    }
    add_output(state.get(), std::move(output));
  } else if (kind == PathKind::Star) {
    state->points = integer(p, "points", 5);
    require(state->points >= 3 && state->points <= 4096,
            "star point count capacity");
    shape(inputs[0], {2});
    shape(inputs[1], {2});
    shape(inputs[2], {1});
    state->info.capacities = {2 * state->points + 1, 2 * state->points, 1};
    add_output(state.get(), path_schema(state->info));
  } else {
    state->info = path_info(*inputs[0].result_schema);
    const auto policy = text(p, "attribute_policy", "reject_unmappable");
    require(policy == "reject_unmappable" || policy == "drop_unmappable",
            "path attribute policy");
    if (kind == PathKind::Concat) {
      for (unsigned port = 1; port < inputs.size(); ++port) {
        const auto next = path_info(*inputs[port].result_schema);
        require(next.primitive == state->info.primitive,
                "mixed path authorities require explicit conversion");
        for (unsigned i = 0; i < (next.primitive ? 8U : 3U); ++i) {
          state->info.capacities[i] =
              checked_add(state->info.capacities[i], next.capacities[i]);
        }
      }
    }
    if (kind == PathKind::Reverse && !state->info.primitive) {
      // Retaining the original M anchor can turn the old Z into an explicit L.
      state->info.capacities[0] =
          checked_add(state->info.capacities[0], state->info.capacities[2]);
      state->info.capacities[1] =
          checked_add(state->info.capacities[1], state->info.capacities[2]);
    }
    add_output(state.get(), path_schema(state->info));
    if (kind == PathKind::Split) {
      require(tensor(inputs[1]).descriptor.element_type == ElementType::Int64 &&
                  tensor(inputs[1]).sample_shape().size() == 1,
              "path split groups require an Int64 vector");
      state->groups = integer(p, "group_count");
      add_output(state.get(),
                 take(ps::generation::path_partition_schema(state->groups)));
    }
  }
  state->finish();
  return state;
}
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_paths(OperationRegistry* registry) {
  using generation::check;
  using generation::make_paths;
  using generation::operation;
  using generation::PathKind;
  using generation::take;
  try {
    struct Entry {
      const char* key;
      PathKind kind;
      unsigned inputs;
    };
    const Entry entries[] = {
        {"path.make_core_path_v1_strict", PathKind::Core, 6},
        {"path.make_primitive_path_v1_strict", PathKind::Primitives, 13},
        {"path.concat_paths_v1_strict", PathKind::Concat, 1},
        {"path.split_subpaths_v1_strict", PathKind::Split, 2},
        {"path.reverse_paths_v1_strict", PathKind::Reverse, 1},
        {"generation.star_path_v1_strict", PathKind::Star, 3}};
    for (const auto& entry : entries) {
      std::vector<OperationParameterSpec> parameters;
      if (entry.kind == PathKind::Concat || entry.kind == PathKind::Split ||
          entry.kind == PathKind::Reverse) {
        parameters.push_back(
            {"attribute_policy", OperationParameterType::String, false});
      }
      if (entry.kind == PathKind::Split) {
        parameters.push_back({"group_count", OperationParameterType::Int64});
      }
      if (entry.kind == PathKind::Star) {
        parameters.push_back({"points", OperationParameterType::Int64, false});
      }
      std::vector<std::pair<std::string, SchemaTemplate>> outputs{
          {"path", entry.kind == PathKind::Primitives
                       ? take(ps::generation::primitive_path_schema({}))
                       : take(ps::generation::core_path_schema({}))}};
      if (entry.kind == PathKind::Split) {
        outputs.push_back(
            {"partition", take(ps::generation::path_partition_schema(1))});
      }
      auto definition = operation(
          entry.key, entry.inputs, parameters, outputs,
          [kind = entry.kind](const auto& inputs, const auto& p) {
            return make_paths(kind, inputs, p);
          },
          entry.kind == PathKind::Concat, entry.kind != PathKind::Split);
      if (entry.kind == PathKind::Concat || entry.kind == PathKind::Split ||
          entry.kind == PathKind::Reverse) {
        // The static input directory chooses one of the two registered
        // authorities. The output predicate permits either counts member.
        auto& input_constraint = definition.traits.input_schema[0];
        input_constraint.tensor_key = "counts";
        input_constraint.element_type_mask = 0;
        input_constraint.element_type =
            static_cast<std::uint32_t>(ElementType::Int64);
        auto& constraint = definition.traits.outputs[0].output_schema;
        constraint.result_schema_id.clear();
        constraint.result_schema_version = 0;
        constraint.tensor_key = "counts";
        constraint.element_type =
            static_cast<std::uint32_t>(ElementType::Int64);
      }
      check(registry->register_operation(std::move(definition)));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
