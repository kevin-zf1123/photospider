#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/path_bound_width.hpp"
#include "common/registry.hpp"
#include "photospider/ops/generation.hpp"

namespace ps::plugin_internal::generation {
namespace {
struct WidthEvaluation final : Program {
  bool normalize, query_parts;
  std::uint64_t queries, lengths, stops = 0;
  ElementType type;
  WidthEvaluation(const std::vector<OperationMetadata>& in, const Parameters& p,
                  bool parts)
      : normalize(text(p, "domain", "subpath_normalized_arclength") !=
                  "subpath_arclength_px"),
        query_parts(parts),
        type(dtype(p)) {
    const auto domain = text(p, "domain", "subpath_normalized_arclength");
    require(domain == "pathset_normalized_arclength" ||
                domain == "subpath_normalized_arclength" ||
                domain == "subpath_arclength_px",
            "unknown width source domain");
    const auto q = tensor(in[0]).sample_shape();
    const auto l = tensor(in[1]).sample_shape();
    require(q.size() == 1 && l.size() == 1, "width queries require rank one");
    queries = q[0];
    lengths = l[0];
    shape(in[0], {queries});
    shape(in[1], {lengths});
    require(lengths == 1 || lengths == queries, "width length shape mismatch");
    if (!parts) {
      const auto k = tensor(in[2]).sample_shape();
      require(k.size() == 1 && k[0] >= 2, "width profile requires K>=2");
      stops = k[0];
      shape(in[2], {stops});
      shape(in[3], {stops});
    } else {
      require(tensor(in[0]).descriptor.element_type == ElementType::Float64 &&
                  tensor(in[1]).descriptor.element_type == ElementType::Float64,
              "width PCHIP query stage requires Float64 source coordinates");
      type = ElementType::Float64;
    }
    add_output(this, tensor_schema(type, {queries}));
    if (parts)
      add_output(this, tensor_schema(type, {queries}));
    scratch_bytes = sizeof(Math);
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    std::vector<ReadRegion> result{
        {0, 0, region},
        {1, 0, lengths == 1 ? Region({{0, 1}}) : region}};
    if (!query_parts) {
      result.push_back({2, 0, Region({{0, stops}})});
      result.push_back({3, 0, Region({{0, stops}})});
    }
    return result;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader r{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* storage = new (invocation.scratch[0].allocation) Math(work);
    struct Destroy {
      Math* p;
      ~Destroy() { p->~Math(); }
    } destroy{storage};
    auto& m = *storage;
    auto& e = m.exact;
    const auto& out = invocation.outputs[0];
    const auto port = kernel.writes[0].first;
    samples(out, [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      e.reset();
      const auto length = m.input(r, 1, 0, {lengths == 1 ? 0 : at[0]});
      const auto distance = m.input(r, 0, 0, at);
      if (query_parts) {
        // NUM division is an explicit subsequent node. Both operands are
        // constructed here so its ordinary IEEE division never evaluates 0/0.
        const auto value =
            port == 1
                ? (normalize && !Exact::zero(length) ? length : e.integer(1))
                : (normalize && Exact::zero(length) ? e.integer(0) : distance);
        store(out, at, m.rounded(value, false, true));
        return;
      }
      const auto u = normalize ? (Exact::zero(length) ? e.integer(0)
                                                      : m.div(distance, length))
                               : distance;
      std::uint64_t lo = 0, hi = stops;
      while (lo < hi) {
        work.poll();
        const auto mid = lo + (hi - lo) / 2;
        if (e.order(m.input(r, 2, 0, {mid}), u) < 0)
          lo = mid + 1;
        else
          hi = mid;
      }
      const auto x1 = m.input(r, 2, 0, {lo});
      if (e.order(x1, u) == 0) {
        store(out, at,
              m.rounded(m.input(r, 3, 0, {lo}), type == ElementType::Float32,
                        true));
      } else {
        const auto x0 = m.input(r, 2, 0, {lo - 1});
        const auto t = m.div(m.sub(u, x0), m.sub(x1, x0));
        const auto value =
            m.lerp(m.input(r, 3, 0, {lo - 1}), m.input(r, 3, 0, {lo}), t);
        store(out, at, m.rounded(value, type == ElementType::Float32));
      }
      check(e.status());
    });
  }
};

struct WidthBinding final : Program {
  std::uint64_t profiles, bindings, stops;
  DenseLayout layout{SchemaTemplate{}};
  WidthBinding(const std::vector<OperationMetadata>& in, const Parameters& p) {
    path_info(*in[0].result_schema);
    const auto a = tensor(in[1]).sample_shape();
    const auto b = tensor(in[2]).sample_shape();
    const auto c = tensor(in[3]).sample_shape();
    require(a.size() == 2 && a[1] == 3 && b.size() == 2 && b[1] == 3 &&
                c.size() == 1,
            "width binding directory shape");
    require(tensor(in[1]).descriptor.element_type == ElementType::Int64 &&
                tensor(in[2]).descriptor.element_type == ElementType::Int64 &&
                tensor(in[3]).descriptor.element_type == ElementType::Float64 &&
                tensor(in[4]).descriptor.element_type == ElementType::Float64,
            "width binding directory types");
    shape(in[4], c);
    profiles = integer(p, "profile_count", a[0]);
    bindings = integer(p, "binding_count", b[0]);
    stops = integer(p, "stop_count", c[0]);
    require(profiles <= a[0] && bindings <= b[0] && stops <= c[0],
            "width binding count exceeds static source capacity");
    add_output(this,
               take(ps::generation::path_width_schema(a[0], b[0], c[0], 1)));
    layout = DenseLayout(*outputs[0].result_schema);
    scratch_bytes = layout.bytes;
    complete = true;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader r{kernel, invocation};
    const Work work{invocation, work_limit};
    Dense out{layout,
              static_cast<std::uint8_t*>(invocation.scratch[0].allocation),
              work};
    out.clear();
    for (std::uint64_t row = 0; row < profiles; ++row) {
      work.poll();
      for (unsigned axis = 0; axis < 3; ++axis)
        out.set(0, row * 4 + axis + 1, r.bits(1, 0, {row, axis}));
    }
    for (std::uint64_t row = 0; row < bindings; ++row) {
      work.poll();
      for (unsigned axis = 0; axis < 3; ++axis)
        out.set(1, row * 3 + axis, r.bits(2, 0, {row, axis}));
    }
    for (std::uint64_t row = 0; row < stops; ++row) {
      work.poll();
      out.set(2, row, r.bits(3, 0, {row}));
      out.set(3, row, r.bits(4, 0, {row}));
    }
    write_id(out, 4, 0, source_id(r, 0));
    out.set(5, 0, bindings);
    out.set(5, 1, profiles);
    out.set(5, 2, stops);
    out.set(5, 3, 1);
    out.publish(kernel, invocation, 0);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_path_width(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    for (unsigned profile = 0; profile < 3; ++profile) {
      const std::string suffix = profile == 0   ? "strict"
                                 : profile == 1 ? "accelerated_apple_silicon"
                                                : "accelerated_x86_64";
      auto op =
          operation("path.width_profile_linear_v1_" + suffix, 4,
                    {{"domain", OperationParameterType::String, false},
                     {"dtype", OperationParameterType::String, false}},
                    {{"values", tensor_schema(ElementType::Float64, {1})}},
                    [profile](const auto& in, const auto& p) {
                      check(numeric_ops::sequence_profile_available(
                          static_cast<numeric_ops::SequenceProfile>(profile)));
                      return std::make_shared<WidthEvaluation>(in, p, false);
                    });
      check(registry->register_operation(std::move(op)));
    }
    auto query =
        operation("path.width_query_operands_v1_strict", 2,
                  {{"domain", OperationParameterType::String, false}},
                  {{"numerator", tensor_schema(ElementType::Float64, {1})},
                   {"denominator", tensor_schema(ElementType::Float64, {1})}},
                  [](const auto& in, const auto& p) {
                    return std::make_shared<WidthEvaluation>(in, p, true);
                  });
    check(registry->register_operation(std::move(query)));
    auto binding = operation(
        "path.linear_width_binding_v1_strict", 5,
        {{"profile_count", OperationParameterType::Int64, false},
         {"binding_count", OperationParameterType::Int64, false},
         {"stop_count", OperationParameterType::Int64, false}},
        {{"width", take(ps::generation::path_width_schema(1, 1, 2, 1))}},
        [](const auto& in, const auto& p) {
          return std::make_shared<WidthBinding>(in, p);
        },
        false, false);
    any_path_input(&binding, 0);
    check(registry->register_operation(std::move(binding)));
    for (bool mapped : {false, true}) {
      const unsigned fixed = mapped ? 3 : 2;
      auto values = operation(
          mapped ? "path.bound_linear_width_mapped_values_v1_strict"
                 : "path.bound_linear_width_values_v1_strict",
          fixed,
          {{"max_integration_panels", OperationParameterType::Int64, false},
           {"max_refinements", OperationParameterType::Int64, false}},
          {{"values", tensor_schema(ElementType::Float64, {1})}},
          [mapped](const auto& in, const auto& p) {
            return std::make_shared<BoundWidthValues>(in, p, mapped);
          },
          true);
      values.traits.input_count = fixed;
      values.traits.input_schema.resize(fixed + 1);
      any_path_input(&values, 0);
      values.traits.input_schema[1].result_schema_id =
          "photospider.ops.path_width";
      values.traits.input_schema[1].result_schema_version = 1;
      if (mapped) {
        values.traits.input_schema[2].result_schema_id =
            "photospider.ops.path_source_map";
        values.traits.input_schema[2].result_schema_version = 1;
      }
      any_path_input(&values, fixed);
      values.traits.repeated_maximum = 1024 - fixed;
      check(registry->register_operation(std::move(values)));
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
