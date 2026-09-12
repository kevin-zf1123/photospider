#include "photospider/data/representation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#include "data/input_validation.hpp"
#include "photospider/data/layer.hpp"

namespace ps {
namespace {
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr const char* kIds[] = {"",
                                "photospider.spectrum",
                                "photospider.bands",
                                "photospider.path_set",
                                "photospider.point_set",
                                "photospider.components",
                                "photospider.ycbcr420",
                                "photospider.brush",
                                "photospider.iterative"};
// NOLINTEND
Status invalid(const char* message = "invalid representation schema") {
  return Status{ErrorCode::TypeMismatch, message};
}
Status exhausted() {
  return Status{ErrorCode::ResourceExhausted,
                "representation budget exhausted"};
}
bool finite(double value) {
  return std::isfinite(value);
}
bool word_product(std::uint64_t a, std::uint64_t b, std::uint64_t* result) {
  if (a && b > static_cast<std::uint64_t>(INT64_MAX) / a)
    return false;
  *result = a * b;
  return true;
}
struct Writer {
  ResultFacet facet{"photospider.representation", 1, {}};
  void word(std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
      facet.payload.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  }
  void real(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, 8);
    word(bits);
  }
  void text(std::string_view value) {
    word(value.size());
    facet.payload.insert(facet.payload.end(), value.begin(), value.end());
  }
  template <class T>
  void words(const ResourceVector<T>& values) {
    word(values.size());
    for (auto value : values)
      word(value);
  }
  void reals(const ResourceVector<double>& values) {
    word(values.size());
    for (auto value : values)
      real(value);
  }
};
struct Reader {
  const std::uint8_t* data = nullptr;
  std::size_t size = 0, position = 0;
  bool valid = true;
  std::uint64_t word() {
    if (position > size || size - position < 8) {
      valid = false;
      return 0;
    }
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
      result |= static_cast<std::uint64_t>(data[position++]) << (8 * i);
    return result;
  }
  double real() {
    auto bits = word();
    double result = 0;
    std::memcpy(&result, &bits, 8);
    if (!finite(result))
      valid = false;
    return result;
  }
  std::string_view text() {
    auto count = word();
    if (!count || count > 256 || position > size || count > size - position) {
      valid = false;
      return {};
    }
    std::string_view result(reinterpret_cast<const char*>(data + position),
                            count);
    position += count;
    for (unsigned char c : result)
      if (c < 0x21 || c > 0x7e)
        valid = false;
    return result;
  }
  std::uint32_t words(std::array<std::uint64_t, 8>* values) {
    auto count = word();
    if (count > 8) {
      valid = false;
      return 0;
    }
    for (std::uint32_t i = 0; i < count; ++i)
      (*values)[i] = word();
    return static_cast<std::uint32_t>(count);
  }
  bool reals(std::array<double, 8>* values, std::uint32_t count) {
    if (word() != count) {
      valid = false;
      return false;
    }
    for (std::uint32_t i = 0; i < count; ++i)
      (*values)[i] = real();
    return valid;
  }
};
struct ParsedBand {
  std::uint64_t level = 0, mask = 0, count = 0, offset = 0;
  std::array<std::uint64_t, 8> shape{}, parent{};
  std::array<double, 8> origin{}, step{}, phase{};
  bool zero = false;
};
struct Parsed {
  std::uint32_t kind = 0, rank = 0, axes_count = 0, band_count = 0;
  std::array<std::uint64_t, 8> shape{}, axes{}, order{}, shifts{};
  std::array<double, 8> origin{}, step{};
  std::array<std::uint64_t, 16> u{};
  std::array<double, 8> f{};
  std::array<std::string_view, 5> text{};
  std::array<ParsedBand, 49> bands{};
  std::uint64_t count = 0, stored_count = 0;
};
bool permutation(const std::array<std::uint64_t, 8>& axes, std::uint32_t count,
                 std::uint32_t rank) {
  std::uint32_t seen = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (axes[i] >= rank || (seen & (1U << axes[i])))
      return false;
    seen |= 1U << axes[i];
  }
  return true;
}
bool shape_count(Parsed* p) {
  if (!p->rank || p->rank > 8)
    return false;
  p->count = 1;
  for (std::uint32_t i = 0; i < p->rank; ++i)
    if (!p->shape[i] || !word_product(p->count, p->shape[i], &p->count))
      return false;
  return true;
}
Status parse(const SchemaTemplate& schema, Parsed* p) {
  for (unsigned i = 1; i <= 8; ++i)
    if (schema.id == kIds[i])
      p->kind = i;
  if (!p->kind)
    return Status::success();
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Status{ErrorCode::OperationFailed,
                  "representation numeric environment unavailable"};
  if (schema.version != 1 ||
      schema.publication != PublishPolicy::CompleteBundle ||
      schema.metadata.size() != 1 ||
      schema.metadata[0].key != "photospider.representation" ||
      schema.metadata[0].version != 1)
    return invalid();
  const auto& bytes = schema.metadata[0].payload;
  Reader r{bytes.data(), bytes.size()};
  if (r.word() != p->kind || r.word() != 1)
    return invalid();
  if (p->kind == 1) {
    p->rank = r.words(&p->shape);
    p->axes_count = r.words(&p->axes);
    if (r.words(&p->order) != p->rank || r.words(&p->shifts) != p->rank ||
        !r.reals(&p->origin, p->rank) || !r.reals(&p->step, p->rank))
      return invalid();
    for (unsigned i = 0; i < 5; ++i)
      p->u[i] = r.word();
    p->f[0] = r.real();
    p->f[1] = r.real();
    p->text[0] = r.text();
    // packing, packed axis, sign encoding (0 negative/1 positive), norm, real
    // policy.
    if (!shape_count(p) || !p->axes_count ||
        !permutation(p->axes, p->axes_count, p->rank) ||
        !permutation(p->order, p->rank, p->rank) || p->u[0] < 1 ||
        p->u[0] > 2 || p->u[1] >= p->rank || p->u[2] > 1 || p->u[3] < 1 ||
        p->u[3] > 3 || p->u[4] < 1 || p->u[4] > 3 || p->f[0] < 0 || p->f[1] < 0)
      return invalid();
    if (p->u[4] == 2 && (p->f[0] != 0 || p->f[1] != 0))
      return invalid();
    bool packed_transformed = false;
    for (std::uint32_t i = 0; i < p->axes_count; ++i)
      packed_transformed |= p->axes[i] == p->u[1];
    p->stored_count = p->count;
    if (p->u[0] == 2) {
      if (!packed_transformed || p->u[4] == 1)
        return invalid();
      p->stored_count =
          p->count / p->shape[p->u[1]] * (p->shape[p->u[1]] / 2 + 1);
    }
    for (std::uint32_t i = 0; i < p->rank; ++i)
      if (p->step[i] <= 0 || p->shifts[i] >= p->shape[i] ||
          (p->u[0] == 2 && p->shifts[i] != 0))
        return invalid();
  } else if (p->kind == 2) {
    p->rank = r.words(&p->shape);
    p->axes_count = r.words(&p->axes);
    p->u[0] = r.word();
    p->text[0] = r.text();
    auto count = r.word();
    if (!shape_count(p) || p->rank > 2 || p->axes_count != p->rank ||
        !permutation(p->axes, p->axes_count, p->rank) || p->u[0] > 16 ||
        p->text[0] != "haar-average-difference-v1" ||
        count != 1 + ((1U << p->rank) - 1) * p->u[0] || count > 49)
      return invalid();
    p->band_count = static_cast<std::uint32_t>(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      auto& b = p->bands[i];
      b.level = r.word();
      b.mask = r.word();
      if (r.words(&b.shape) != p->rank || r.words(&b.parent) != p->rank ||
          !r.reals(&b.origin, p->rank) || !r.reals(&b.step, p->rank) ||
          !r.reals(&b.phase, p->rank))
        return invalid();
      auto zero = r.word();
      if (zero > 1 || b.level > p->u[0] || b.mask >= (1U << p->rank) ||
          (b.mask == 0 ? b.level != p->u[0] : b.level == 0))
        return invalid();
      b.zero = zero != 0;
      b.count = 1;
      for (std::uint32_t j = 0; j < i; ++j)
        if (p->bands[j].level == b.level && p->bands[j].mask == b.mask)
          return invalid();
      for (std::uint32_t axis = 0; axis < p->rank; ++axis) {
        auto parent = p->shape[axis];
        for (std::uint64_t level = 1; level < b.level; ++level)
          parent = parent / 2 + parent % 2;
        const auto shape = b.level ? parent / 2 + parent % 2 : parent;
        if (b.parent[axis] != parent || b.shape[axis] != shape ||
            b.step[axis] != std::ldexp(1.0, static_cast<int>(b.level)) ||
            b.origin[axis] != 0 || b.phase[axis] != 0 ||
            !word_product(b.count, shape, &b.count))
          return invalid();
      }
      b.offset = p->stored_count;
      if (!b.zero) {
        if (b.count > static_cast<std::uint64_t>(INT64_MAX) - p->stored_count)
          return invalid();
        p->stored_count += b.count;
      }
    }
  } else if (p->kind == 3) {
    for (unsigned i = 0; i < 4; ++i)
      p->u[i] = r.word();
    p->text[0] = r.text();
    if (p->u[0] < 1 || p->u[0] > 2 || !p->u[1] || !p->u[2] ||
        p->u[1] > INT64_MAX || p->u[2] > INT64_MAX || p->u[3] > 32)
      return invalid();
  } else if (p->kind == 4) {
    for (unsigned i = 0; i < 5; ++i)
      p->u[i] = r.word();
    p->text[0] = r.text();
    if (p->u[0] < 2 || p->u[0] > 3 || !p->u[1] || p->u[1] > 64 || p->u[2] < 1 ||
        p->u[2] > 2 || p->u[3] > INT64_MAX || p->u[4] > INT64_MAX)
      return invalid();
  } else if (p->kind == 5 || p->kind == 6) {
    p->rank = 2;
    p->shape[0] = r.word();
    p->shape[1] = r.word();
    if (!shape_count(p))
      return invalid();
    if (p->kind == 5) {
      p->u[0] = r.word();
      p->u[1] = r.word();
      if (p->u[0] > INT64_MAX || p->u[1] < 1 || p->u[1] > 2)
        return invalid();
    } else {
      for (unsigned i = 0; i < 3; ++i)
        p->text[i] = r.text();
      if (p->text[0] != "srgb-d65" || p->text[1] != "bt709" ||
          (p->text[2] != "display" && p->text[2] != "scene"))
        return invalid();
      p->stored_count = (p->shape[0] / 2 + p->shape[0] % 2) *
                        (p->shape[1] / 2 + p->shape[1] % 2);
    }
  } else if (p->kind == 7) {
    p->f[0] = r.real();
    p->u[0] = r.word();
    p->u[1] = r.word();
    p->text[0] = r.text();
    if (p->f[0] <= 0 || p->u[0] > 1048576 || p->u[1] > p->u[0] ||
        p->text[0] != "constant-spacing-1d-v1")
      return invalid();
  } else {
    p->rank = 1;
    p->shape[0] = r.word();
    p->u[0] = r.word();
    p->f[0] = r.real();
    p->u[1] = r.word();
    p->text[0] = r.text();
    p->text[1] = r.text();
    if (!shape_count(p) || p->u[0] > INT64_MAX || p->f[0] < 0 || p->u[1] > 1 ||
        (p->text[1] != "zero" && p->text[1] != "explicit"))
      return invalid();
  }
  return r.valid && r.position == r.size ? Status::success() : invalid();
}
// This one layout function is used both to emit and to check the closed schema.
// Independent tests must also check contents and malformed cross-field cases.
template <class Emit>
bool fields(const Parsed& p, Emit emit) {
  const auto fixed = ResultExtentKind::Fixed,
             dynamic = ResultExtentKind::RuntimeCount;
  if (p.kind == 1)
    return emit("samples", ElementType::Float64, fixed, p.stored_count, 2);
  if (p.kind == 2)
    return emit("samples", ElementType::Float64, fixed, p.stored_count, 1);
  if (p.kind == 3) {
    return emit("verbs", ElementType::UInt8, dynamic, 1, 1) &&
           emit("control_offsets", ElementType::Int64, dynamic, 1, 1) &&
           emit("controls", ElementType::Float64, dynamic, 1, 2) &&
           emit("subpath_offsets", ElementType::Int64, dynamic, 1, 1) &&
           emit("closed", ElementType::UInt8, dynamic, 1, 1) &&
           emit("primitives", ElementType::Int64, dynamic, 1, 4) &&
           emit("bezier", ElementType::Float64, dynamic, 1, 8) &&
           emit("arcs", ElementType::Float64, dynamic, 1, 8) &&
           emit("hermite", ElementType::Float64, dynamic, 1, 8) &&
           emit("splines", ElementType::Int64, dynamic, 1, 5) &&
           emit("knots", ElementType::Float64, dynamic, 1, 1) &&
           emit("weights", ElementType::Float64, dynamic, 1, 1) &&
           emit("attributes", ElementType::Int64, dynamic, 1, 6) &&
           emit("attribute_values", ElementType::Float64, dynamic, 1, 1) &&
           emit("arc_positions", ElementType::Float64, dynamic, 1, 1);
  }
  if (p.kind == 4)
    return emit("ids", ElementType::Int64, dynamic, 1, 1) &&
           emit("positions", ElementType::Float64, dynamic, 1, p.u[0]) &&
           emit("attributes", ElementType::Float64, dynamic, 1, p.u[1]) &&
           emit("id_to_row", ElementType::Int64, dynamic, 1, 2);
  if (p.kind == 5)
    return emit("labels", ElementType::Int64, fixed, p.count, 1) &&
           emit("components", ElementType::Int64, dynamic, 1, 3);
  if (p.kind == 6)
    return emit("y", ElementType::Float32, fixed, p.count, 1) &&
           emit("cb", ElementType::Float32, fixed, p.stored_count, 1) &&
           emit("cr", ElementType::Float32, fixed, p.stored_count, 1);
  if (p.kind == 7)
    return emit("state_ids", ElementType::Int64, fixed, 1, 10) &&
           emit("state", ElementType::Float64, fixed, 1, 9) &&
           emit("pending_ids", ElementType::Int64, dynamic, 1, 1) &&
           emit("pending_positions", ElementType::Float64, dynamic, 1, 1) &&
           emit("dabs", ElementType::Float64, dynamic, 1, 1);
  return emit("state", ElementType::Int64, fixed, 1, 3) &&
         emit("estimate", ElementType::Float64, fixed, p.count, 1) &&
         emit("diagonal", ElementType::Float64, fixed, p.count, 1) &&
         emit("rhs", ElementType::Float64, fixed, p.count, 1) &&
         emit("residual", ElementType::Float64, fixed, 1, 1);
}
Result<SchemaTemplate> finish(unsigned kind, Writer writer) {
  SchemaTemplate schema;
  schema.id = kIds[kind];
  schema.metadata.push_back(std::move(writer.facet));
  Parsed parsed;
  auto checked = parse(schema, &parsed);
  if (!checked.ok())
    return Result<SchemaTemplate>(checked);
  fields(parsed, [&](const char* key, ElementType type, ResultExtentKind rows,
                     std::uint64_t count, std::uint64_t width) {
    ResultFieldSpec field{key, type, {rows, count}, {}};
    if (width > 1)
      field.record_shape.push_back(width);
    schema.fields.push_back(std::move(field));
    return true;
  });
  for (std::uint32_t i = 0; i < parsed.rank; ++i)
    schema.domain.push_back({ResultExtentKind::Fixed, parsed.shape[i]});
  checked = schema.validate(true);
  return checked.ok() ? Result<SchemaTemplate>(std::move(schema))
                      : Result<SchemaTemplate>(checked);
}
Writer begin(unsigned kind) {
  Writer writer;
  writer.word(kind);
  writer.word(1);
  return writer;
}
}  // namespace
bool has_representation_schema(std::string_view id) noexcept {
  if (has_layer_schema(id))
    return true;
  for (unsigned i = 1; i <= 8; ++i)
    if (id == kIds[i])
      return true;
  return false;
}
Status validate_representation_schema(const SchemaTemplate& schema) {
  if (has_layer_schema(schema.id))
    return validate_layer_schema(schema);
  Parsed p;
  auto checked = parse(schema, &p);
  if (!checked.ok() || !p.kind)
    return checked;
  std::size_t index = 0;
  const bool matched =
      fields(p, [&](const char* key, ElementType type, ResultExtentKind rows,
                    std::uint64_t count, std::uint64_t width) {
        if (index >= schema.fields.size())
          return false;
        const auto& field = schema.fields[index++];
        const auto& e = field.rows;
        return field.key == key && field.element_type == type &&
               e.kind == rows && e.value == count && e.input == 0 &&
               e.axis == 0 && e.field == 0 && e.divisor == 1 && e.offset == 0 &&
               (width == 1 ? field.record_shape.empty()
                           : field.record_shape.size() == 1 &&
                                 field.record_shape[0] == width);
      });
  if (!matched || index != schema.fields.size() ||
      schema.domain.size() != p.rank)
    return invalid();
  for (std::uint32_t i = 0; i < p.rank; ++i) {
    const auto& e = schema.domain[i];
    if (e.kind != ResultExtentKind::Fixed || e.value != p.shape[i] || e.input ||
        e.axis || e.field || e.offset || e.divisor != 1)
      return invalid();
  }
  return Status::success();
}
Result<SchemaTemplate> spectrum_schema(const SpectrumSpec& s) {
  if ((s.sign != -1 && s.sign != 1) || s.original_shape.size() > 8 ||
      s.transformed_axes.size() > 8 || s.axis_order.size() > 8 ||
      s.shifts.size() > 8 || s.sample_origin.size() > 8 ||
      s.sample_step.size() > 8 || s.unit.size() > 256)
    return Result<SchemaTemplate>(invalid());
  auto w = begin(1);
  w.words(s.original_shape);
  w.words(s.transformed_axes);
  w.words(s.axis_order);
  w.words(s.shifts);
  w.reals(s.sample_origin);
  w.reals(s.sample_step);
  w.word(static_cast<unsigned>(s.packing));
  w.word(s.packed_axis);
  w.word(s.sign == 1);
  w.word(static_cast<unsigned>(s.normalization));
  w.word(static_cast<unsigned>(s.real_policy));
  w.real(s.atol);
  w.real(s.rtol);
  w.text(s.unit);
  return finish(1, std::move(w));
}
Result<SchemaTemplate> bands_schema(const BandsSpec& s) {
  if (s.original_shape.size() > 8 || s.axes.size() > 8 || s.bands.size() > 49 ||
      s.filter_snapshot.size() > 256)
    return Result<SchemaTemplate>(invalid());
  for (const auto& b : s.bands)
    if (b.shape.size() > 8 || b.parent_shape.size() > 8 ||
        b.logical_origin.size() > 8 || b.sample_step.size() > 8 ||
        b.phase.size() > 8)
      return Result<SchemaTemplate>(invalid());
  auto w = begin(2);
  w.words(s.original_shape);
  w.words(s.axes);
  w.word(s.levels);
  w.text(s.filter_snapshot);
  w.word(s.bands.size());
  for (const auto& b : s.bands) {
    w.word(b.level);
    w.word(b.mask);
    w.words(b.shape);
    w.words(b.parent_shape);
    w.reals(b.logical_origin);
    w.reals(b.sample_step);
    w.reals(b.phase);
    w.word(b.explicit_zero);
  }
  return finish(2, std::move(w));
}
Result<SchemaTemplate> path_set_schema(const PathSetSpec& s) {
  if (s.coordinate_system.size() > 256)
    return Result<SchemaTemplate>(invalid());
  auto w = begin(3);
  w.word(static_cast<unsigned>(s.authority));
  w.word(s.maximum_segments);
  w.word(s.maximum_controls);
  w.word(s.maximum_spline_degree);
  w.text(s.coordinate_system);
  return finish(3, std::move(w));
}
Result<SchemaTemplate> point_set_schema(const PointSetSpec& s) {
  if (s.coordinate_system.size() > 256)
    return Result<SchemaTemplate>(invalid());
  auto w = begin(4);
  w.word(s.dimensions);
  w.word(s.attribute_width);
  w.word(static_cast<unsigned>(s.ids));
  w.word(s.basis_count);
  w.word(s.maximum_count);
  w.text(s.coordinate_system);
  return finish(4, std::move(w));
}
Result<SchemaTemplate> components_schema(const ComponentsSpec& s) {
  auto w = begin(5);
  w.word(s.height);
  w.word(s.width);
  w.word(s.maximum_count);
  w.word(static_cast<unsigned>(s.ids));
  return finish(5, std::move(w));
}
Result<SchemaTemplate> ycbcr420_schema(const YCbCr420Spec& s) {
  if (s.primaries.size() > 256 || s.transfer.size() > 256 ||
      s.reference.size() > 256)
    return Result<SchemaTemplate>(invalid());
  auto w = begin(6);
  w.word(s.height);
  w.word(s.width);
  w.text(s.primaries);
  w.text(s.transfer);
  w.text(s.reference);
  return finish(6, std::move(w));
}
Result<SchemaTemplate> brush_schema(const BrushSpec& s) {
  if (s.algorithm.size() > 256)
    return Result<SchemaTemplate>(invalid());
  auto w = begin(7);
  w.real(s.spacing);
  w.word(s.maximum_pending);
  w.word(s.lookahead);
  w.text(s.algorithm);
  return finish(7, std::move(w));
}
Result<SchemaTemplate> iterative_schema(const IterativeSpec& s) {
  if (s.system_snapshot.size() > 256 || s.initialization.size() > 256)
    return Result<SchemaTemplate>(invalid());
  auto w = begin(8);
  w.word(s.size);
  w.word(s.maximum_iterations);
  w.real(s.target_residual);
  w.word(s.require_converged);
  w.text(s.system_snapshot);
  w.text(s.initialization);
  return finish(8, std::move(w));
}
Result<SpectrumSpec> spectrum_spec(const SchemaTemplate& schema) {
  auto checked = schema.validate(true);
  Parsed p;
  if (!checked.ok() || schema.id != kIds[1] || !parse(schema, &p).ok())
    return Result<SpectrumSpec>(invalid());
  SpectrumSpec s;
  for (std::uint32_t i = 0; i < p.rank; ++i) {
    s.original_shape.push_back(p.shape[i]);
    s.axis_order.push_back(p.order[i]);
    s.shifts.push_back(p.shifts[i]);
    s.sample_origin.push_back(p.origin[i]);
    s.sample_step.push_back(p.step[i]);
  }
  for (std::uint32_t i = 0; i < p.axes_count; ++i)
    s.transformed_axes.push_back(p.axes[i]);
  s.packing = static_cast<SpectrumPacking>(p.u[0]);
  s.packed_axis = p.u[1];
  s.sign = p.u[2] ? 1 : -1;
  s.normalization = static_cast<SpectrumNormalization>(p.u[3]);
  s.real_policy = static_cast<SpectrumRealPolicy>(p.u[4]);
  s.atol = p.f[0];
  s.rtol = p.f[1];
  s.unit.assign(p.text[0].data(), p.text[0].size());
  return Result<SpectrumSpec>(std::move(s));
}
namespace {
class Pages {
 public:
  Pages(const ResultRef& result, const ResourceBudget& root,
        std::uint64_t maximum, const CancellationToken& cancellation,
        const std::function<Status(std::uint64_t)>& work)
      : result_(result),
        root_(root),
        maximum_(maximum),
        cancellation_(cancellation),
        work_(work) {
    auto descriptor = result.descriptor();
    if (descriptor.ok())
      descriptor_ = descriptor.take_value();
    else
      status_ = descriptor.status();
  }
  std::uint64_t rows(unsigned field) const { return descriptor_.rows(field); }
  const Status& status() const { return status_; }
  bool ok() const { return status_.ok(); }
  void reject(const char* detail) {
    if (ok())
      status_ = invalid(detail);
  }
  bool charge(std::uint64_t count = 1) {
    if (!ok())
      return false;
    if (cancellation_.cancelled()) {
      status_ = Status{ErrorCode::Cancelled, {}};
      return false;
    }
    status_ = root_.consume({count});
    if (ok() && work_)
      status_ = work_(count);
    return ok();
  }
  template <class T>
  T get(unsigned field, std::uint64_t row, unsigned component = 0) {
    T value{};
    if (!charge())
      return value;
    if (field >= descriptor_.field_count() || row >= rows(field)) {
      reject("representation range mismatch");
      return value;
    }
    auto width = result_.schema().row_bytes(field);
    if (!width.ok()) {
      status_ = width.status();
      return value;
    }
    if ((component + 1ULL) * sizeof(T) > width.value() ||
        Value::element_size(result_.schema().fields[field].element_type) !=
            sizeof(T)) {
      reject("representation scalar width mismatch");
      return value;
    }
    if (!page_ || field != field_ || row < first_ || row >= first_ + count_) {
      page_.reset();
      if (maximum_ < width.value()) {
        status_ = exhausted();
        return value;
      }
      count_ = std::min(rows(field) - row, maximum_ / width.value());
      first_ = row;
      field_ = field;
      auto plan = result_.prepare_read(descriptor_, field, first_, count_);
      if (!plan.ok()) {
        status_ = plan.status();
        return value;
      }
      auto loaded = plan.value().load(maximum_, cancellation_);
      if (!loaded.ok()) {
        status_ = loaded.status();
        return value;
      }
      page_ = loaded.take_value();
    }
    std::memcpy(&value,
                page_->bytes().data() + (row - first_) * width.value() +
                    component * sizeof(T),
                sizeof(T));
    return value;
  }
  bool finite_fields() {
    for (unsigned field = 0; field < descriptor_.field_count(); ++field) {
      const auto type = result_.schema().fields[field].element_type;
      if (type != ElementType::Float32 && type != ElementType::Float64)
        continue;
      const auto width =
          result_.schema().row_bytes(field).value() / Value::element_size(type);
      for (std::uint64_t row = 0; row < rows(field) && ok(); ++row)
        for (unsigned component = 0; component < width && ok(); ++component) {
          const auto value = type == ElementType::Float64
                                 ? get<double>(field, row, component)
                                 : get<float>(field, row, component);
          if (!finite(value))
            reject("nonfinite representation sample");
        }
    }
    return ok();
  }

 private:
  const ResultRef& result_;
  const ResourceBudget& root_;
  std::uint64_t maximum_;
  const CancellationToken& cancellation_;
  const std::function<Status(std::uint64_t)>& work_;
  ResultDescriptor descriptor_;
  Status status_;
  std::shared_ptr<const CpuStorage> page_;
  unsigned field_ = 0;
  std::uint64_t first_ = 0, count_ = 0;
};
// Nonnegative magnitudes retain a normal binary64 fraction and a separate
// exponent. Normalizing a defect by an unrelated huge input would quantize a
// small relative tolerance to subnormal precision; this representation avoids
// that loss and intermediate overflow. It remains a measured floating mode.
struct Magnitude {
  double fraction = 0;
  int exponent = 0;
};
Magnitude magnitude(double value, int exponent = 0) {
  if (value == 0)
    return {};
  int shift = 0;
  auto fraction = std::frexp(std::abs(value), &shift);
  return {fraction, exponent + shift};
}
bool greater(Magnitude a, Magnitude b) {
  if (a.fraction == 0)
    return false;
  if (b.fraction == 0)
    return true;
  return a.exponent != b.exponent ? a.exponent > b.exponent
                                  : a.fraction > b.fraction;
}
Magnitude sum(Magnitude a, Magnitude b) {
  if (a.fraction == 0)
    return b;
  if (b.fraction == 0)
    return a;
  const auto exponent = std::max(a.exponent, b.exponent);
  return magnitude(std::scalbn(a.fraction, a.exponent - exponent) +
                       std::scalbn(b.fraction, b.exponent - exponent),
                   exponent);
}
Magnitude product(Magnitude a, Magnitude b) {
  return magnitude(a.fraction * b.fraction, a.exponent + b.exponent);
}
Magnitude norm(Magnitude a, Magnitude b) {
  if (a.fraction == 0)
    return b;
  if (b.fraction == 0)
    return a;
  const auto exponent = std::max(a.exponent, b.exponent);
  return magnitude(std::hypot(std::scalbn(a.fraction, a.exponent - exponent),
                              std::scalbn(b.fraction, b.exponent - exponent)),
                   exponent);
}
Magnitude difference(double a, double b) {
  const auto value = a - b;
  if (finite(value))
    return magnitude(value);
  constexpr auto exponent = std::numeric_limits<double>::max_exponent - 1;
  return magnitude(std::scalbn(a, -exponent) - std::scalbn(b, -exponent),
                   exponent);
}
void spectrum_values(const Parsed& p, Pages* pages) {
  if (p.u[4] == 1)
    return;
  auto stored_shape = p.shape;
  if (p.u[0] == 2)
    stored_shape[p.u[1]] = p.shape[p.u[1]] / 2 + 1;
  for (std::uint64_t index = 0; index < p.stored_count && pages->ok();
       ++index) {
    std::array<std::uint64_t, 8> coordinate{}, reflected{};
    auto residue = index;
    for (std::uint32_t j = p.rank; j > 0; --j) {
      auto axis = p.order[j - 1];
      coordinate[axis] = residue % stored_shape[axis];
      residue /= stored_shape[axis];
      // Stored index represents logical k+shift, with modular arithmetic.
      reflected[axis] = (coordinate[axis] + p.shifts[axis]) % p.shape[axis];
    }
    for (std::uint32_t j = 0; j < p.axes_count; ++j) {
      const auto axis = p.axes[j];
      reflected[axis] = reflected[axis] ? p.shape[axis] - reflected[axis] : 0;
    }
    bool stored = true;
    std::uint64_t other = 0;
    for (std::uint32_t j = 0; j < p.rank; ++j) {
      const auto axis = p.order[j];
      reflected[axis] =
          (reflected[axis] + p.shape[axis] - p.shifts[axis]) % p.shape[axis];
      if (reflected[axis] >= stored_shape[axis])
        stored = false;
      other = other * stored_shape[axis] + reflected[axis];
    }
    // Interior omitted half is defined by conjugation. Only stored mirror pairs
    // (including self-conjugate points and complete boundary columns) constrain
    // it.
    if (!stored)
      continue;
    const std::complex<double> a(pages->get<double>(0, index, 0),
                                 pages->get<double>(0, index, 1));
    const std::complex<double> b(pages->get<double>(0, other, 0),
                                 pages->get<double>(0, other, 1));
    if (a.real() == b.real() && a.imag() == -b.imag())
      continue;
    if (p.u[4] == 2 || (p.f[0] == 0 && p.f[1] == 0)) {
      pages->reject("Spectrum exact Hermitian components differ");
      continue;
    }
    const auto real_difference = a.real() - b.real();
    const auto imaginary_difference = a.imag() + b.imag();
    if (p.f[1] == 0) {
      // Absolute tolerance must remain in the original units. Scaling it by a
      // large, equal real component could erase a decisive small imaginary
      // defect. An overflowing difference exceeds every finite absolute bound.
      if (std::hypot(real_difference, imaginary_difference) > p.f[0])
        pages->reject("Spectrum Hermitian defect exceeds named tolerance");
      continue;
    }
    const auto defect =
        norm(difference(a.real(), b.real()), difference(a.imag(), -b.imag()));
    const auto norm_a = norm(magnitude(a.real()), magnitude(a.imag()));
    const auto norm_b = norm(magnitude(b.real()), magnitude(b.imag()));
    const auto reference = greater(norm_a, norm_b) ? norm_a : norm_b;
    const auto tolerance =
        sum(magnitude(p.f[0]), product(magnitude(p.f[1]), reference));
    if (greater(defect, tolerance))
      pages->reject("Spectrum Hermitian defect exceeds named tolerance");
  }
}
void point_values(const Parsed& p, Pages* pages) {
  const auto count = pages->rows(0);
  if (count > p.u[4] || pages->rows(1) != count || pages->rows(2) != count ||
      pages->rows(3) != count) {
    pages->reject("point count or association mismatch");
    return;
  }
  std::int64_t previous = -1;
  for (std::uint64_t i = 0; i < count && pages->ok(); ++i) {
    const auto id = pages->get<std::int64_t>(3, i, 0),
               row = pages->get<std::int64_t>(3, i, 1);
    if (id <= previous || row < 0 || static_cast<std::uint64_t>(row) >= count ||
        (p.u[2] == 1 ? static_cast<std::uint64_t>(id) >= p.u[3]
                     : id != static_cast<std::int64_t>(i)) ||
        pages->get<std::int64_t>(0, row) != id)
      pages->reject("point stable ID mapping mismatch");
    previous = id;
  }
}
void component_values(const Parsed& p, Pages* pages) {
  const auto count = pages->rows(1);
  if (count > p.u[0]) {
    pages->reject("component semantic count limit");
    return;
  }
  std::int64_t previous_id = 0, previous_minimum = -1;
  for (std::uint64_t i = 0; i < count && pages->ok(); ++i) {
    const auto id = pages->get<std::int64_t>(1, i, 0),
               area = pages->get<std::int64_t>(1, i, 1),
               minimum = pages->get<std::int64_t>(1, i, 2);
    if (id <= previous_id || area <= 0 || minimum <= previous_minimum ||
        static_cast<std::uint64_t>(minimum) >= p.count ||
        (p.u[1] == 1 ? id != minimum + 1
                     : id != static_cast<std::int64_t>(i + 1))) {
      pages->reject("component ID basis mismatch");
      break;
    }
    std::uint64_t actual_area = 0, actual_minimum = p.count;
    for (std::uint64_t pixel = 0; pixel < p.count && pages->ok(); ++pixel)
      if (pages->get<std::int64_t>(0, pixel) == id) {
        ++actual_area;
        actual_minimum = std::min(actual_minimum, pixel);
      }
    if (actual_area != static_cast<std::uint64_t>(area) ||
        actual_minimum != static_cast<std::uint64_t>(minimum))
      pages->reject("component table does not match labels");
    previous_id = id;
    previous_minimum = minimum;
  }
  for (std::uint64_t pixel = 0; pixel < p.count && pages->ok(); ++pixel) {
    const auto id = pages->get<std::int64_t>(0, pixel);
    if (!id)
      continue;
    std::uint64_t first = 0, last = count;
    while (first < last && pages->ok()) {
      const auto middle = first + (last - first) / 2;
      if (pages->get<std::int64_t>(1, middle, 0) < id)
        first = middle + 1;
      else
        last = middle;
    }
    if (id < 0 || first == count || pages->get<std::int64_t>(1, first, 0) != id)
      pages->reject("label lacks associated component row");
  }
}
bool offsets(Pages* pages, unsigned field, std::uint64_t rows,
             std::uint64_t end) {
  if (pages->rows(field) != rows + 1 || pages->get<std::int64_t>(field, 0) != 0)
    return false;
  std::int64_t previous = 0;
  for (std::uint64_t i = 1; i <= rows && pages->ok(); ++i) {
    auto value = pages->get<std::int64_t>(field, i);
    if (value < previous || static_cast<std::uint64_t>(value) > end)
      return false;
    previous = value;
  }
  return pages->ok() && static_cast<std::uint64_t>(previous) == end;
}
void path_attributes(Pages* pages, std::uint64_t subpaths,
                     std::uint64_t segments, std::uint64_t controls) {
  std::uint64_t next_values = 0, next_parameters = 0;
  for (std::uint64_t i = 0; i < pages->rows(12) && pages->ok(); ++i) {
    std::array<std::int64_t, 6> a{};
    for (unsigned j = 0; j < 6; ++j)
      a[j] = pages->get<std::int64_t>(12, i, j);
    if (a[0] < 1 || a[0] > 5 || a[1] < 1 || a[1] > 2 || a[2] < 1 || a[2] > 64 ||
        a[3] < 0 || a[4] < 0 || a[5] < 0 ||
        static_cast<std::uint64_t>(a[4]) != next_values) {
      pages->reject("invalid path attribute record");
      break;
    }
    const auto count = static_cast<std::uint64_t>(a[3]);
    const std::uint64_t domain[] = {0, 1, subpaths, segments, controls};
    if (a[0] != 5 && (count != domain[a[0]] || a[5] != 0)) {
      pages->reject("path attribute owner mismatch");
      break;
    }
    std::uint64_t size = 0;
    if (!word_product(count, a[2], &size) ||
        size > pages->rows(13) - std::min(next_values, pages->rows(13))) {
      pages->reject("path attribute span mismatch");
      break;
    }
    next_values += size;
    if (a[0] == 5) {
      if (static_cast<std::uint64_t>(a[5]) != next_parameters || count < 2 ||
          count >
              pages->rows(14) - std::min(next_parameters, pages->rows(14))) {
        pages->reject("arc-length parameter span mismatch");
        break;
      }
      double previous = -1;
      for (std::uint64_t j = 0; j < count && pages->ok(); ++j) {
        auto u = pages->get<double>(14, next_parameters + j);
        if (u <= previous || u < 0 || u > 1 || (j == 0 && u != 0) ||
            (j + 1 == count && u != 1))
          pages->reject("arc-length parameters must span [0,1] in order");
        previous = u;
      }
      next_parameters += count;
    }
  }
  if (next_values != pages->rows(13) || next_parameters != pages->rows(14))
    pages->reject("unassociated path attribute payload");
}
struct Endpoints {
  std::array<double, 2> first{}, last{};
  bool known = false, full_turn = false;
};
Endpoints primitive(Pages* pages, const Parsed& p, std::uint64_t i,
                    std::uint64_t object) {
  Endpoints ends;
  const auto tag = pages->get<std::int64_t>(5, i, 0),
             row = pages->get<std::int64_t>(5, i, 1),
             member = pages->get<std::int64_t>(5, i, 2);
  const auto association = pages->get<std::uint64_t>(5, i, 3);
  const auto expected_member = tag <= 3 ? 6 : tag <= 5 ? 7 : tag == 6 ? 8 : 9;
  if (tag < 1 || tag > 7 || row < 0 || member != expected_member ||
      static_cast<std::uint64_t>(row) >= pages->rows(expected_member) ||
      association != object) {
    pages->reject("invalid PrimitiveRef tag, record or association");
    return ends;
  }
  if (tag <= 3 || tag == 6) {
    const auto last = tag == 6 ? 2U : static_cast<unsigned>(tag * 2);
    for (unsigned c = 0; c < 2; ++c) {
      ends.first[c] = pages->get<double>(expected_member, row, c);
      ends.last[c] = pages->get<double>(expected_member, row, last + c);
    }
    if (tag <= 3) {
      for (unsigned c = last + 2; c < 8; ++c)
        if (pages->get<double>(6, row, c) != 0)
          pages->reject("noncanonical Bezier padding");
    }
    ends.known = true;
    return ends;
  }
  if (tag <= 5) {
    const long double ux = pages->get<double>(7, row, 2),
                      uy = pages->get<double>(7, row, 3),
                      vx = pages->get<double>(7, row, 4),
                      vy = pages->get<double>(7, row, 5);
    const auto a = ux * vy, b = uy * vx;
    const auto low = std::nextafter(
        std::nextafter(a, -INFINITY) - std::nextafter(b, INFINITY), -INFINITY);
    const auto high = std::nextafter(
        std::nextafter(a, INFINITY) - std::nextafter(b, -INFINITY), INFINITY);
    const auto sweep = pages->get<double>(7, row, 7);
    if (!std::isfinite(low) || !std::isfinite(high) || !(low > 0 || high < 0))
      pages->reject("arc determinant zero or interval unresolved");
    if (tag == 4) {
      // A strict upper bound below the mathematical 2*pi excludes ambiguous
      // full turns. Multi-turn and degenerate curves require explicit
      // conversion.
      constexpr double turn_lower = 0x1.921fb54442d18p+2;
      if (sweep == 0 || std::abs(sweep) >= turn_lower)
        pages->reject("invalid ArcSweep range");
    } else {
      if (sweep != -1 && sweep != 1)
        pages->reject("full turn needs signed direction");
      ends.full_turn = true;
    }
    return ends;
  }
  std::array<std::int64_t, 5> s{};
  for (unsigned j = 0; j < 5; ++j)
    s[j] = pages->get<std::int64_t>(9, row, j);
  if (s[0] < 0 || static_cast<std::uint64_t>(s[0]) > p.u[3] || s[1] < 0 ||
      s[2] < s[0] + 1 || s[3] < 0 || s[4] < -1 ||
      static_cast<std::uint64_t>(s[1]) > pages->rows(2) ||
      static_cast<std::uint64_t>(s[2]) > pages->rows(2) - s[1] ||
      static_cast<std::uint64_t>(s[3]) > pages->rows(10) ||
      static_cast<std::uint64_t>(s[2]) + s[0] + 1 > pages->rows(10) - s[3] ||
      (s[4] >= 0 &&
       (static_cast<std::uint64_t>(s[4]) > pages->rows(11) ||
        static_cast<std::uint64_t>(s[2]) > pages->rows(11) - s[4]))) {
    pages->reject("invalid B-spline degree or record spans");
    return ends;
  }
  const auto lower = pages->get<double>(10, s[3] + s[0]),
             upper = pages->get<double>(10, s[3] + s[2]);
  if (!(lower < upper))
    pages->reject("empty B-spline parameter domain");
  double previous = -std::numeric_limits<double>::infinity();
  std::uint64_t multiplicity = 0;
  for (std::uint64_t j = 0;
       j < static_cast<std::uint64_t>(s[2] + s[0] + 1) && pages->ok(); ++j) {
    const auto knot = pages->get<double>(10, s[3] + j);
    if (knot < previous)
      pages->reject("B-spline knots must be nondecreasing");
    multiplicity = knot == previous ? multiplicity + 1 : 1;
    if (multiplicity > static_cast<std::uint64_t>(s[0] + 1) ||
        (knot > lower && knot < upper &&
         multiplicity > static_cast<std::uint64_t>(s[0])))
      pages->reject("discontinuous B-spline must split subpaths");
    previous = knot;
  }
  if (s[4] >= 0) {
    for (std::int64_t j = 0; j < s[2] && pages->ok(); ++j)
      if (pages->get<double>(11, s[4] + j) <= 0)
        pages->reject("rational spline weights must be positive");
  }
  bool clamped = true;
  for (std::int64_t j = 0; j <= s[0] && pages->ok(); ++j)
    clamped &= pages->get<double>(10, s[3] + j) == lower &&
               pages->get<double>(10, s[3] + s[2] + j) == upper;
  if (clamped) {
    ends.known = true;
    for (unsigned c = 0; c < 2; ++c) {
      ends.first[c] = pages->get<double>(2, s[1], c);
      ends.last[c] = pages->get<double>(2, s[1] + s[2] - 1, c);
    }
  }
  return ends;
}
void path_values(const Parsed& p, Pages* pages, std::uint64_t object) {
  const auto core = p.u[0] == 1;
  const auto segments = pages->rows(core ? 0 : 5), controls = pages->rows(2),
             subpaths = pages->rows(4);
  if (segments > p.u[1] || controls > p.u[2] || subpaths > p.u[1] ||
      !offsets(pages, 3, subpaths, segments)) {
    pages->reject("path count or subpath offsets mismatch");
    return;
  }
  if (core) {
    if (!offsets(pages, 1, segments, controls)) {
      pages->reject("control offsets mismatch");
      return;
    }
    for (unsigned field : {5U, 6U, 7U, 8U, 9U, 10U, 11U})
      if (pages->rows(field))
        pages->reject("core path has a second geometry authority");
  } else if (pages->rows(0) || !offsets(pages, 1, 0, 0)) {
    pages->reject("primitive path has a second geometry authority");
    return;
  }
  for (std::uint64_t j = 0; j < subpaths && pages->ok(); ++j) {
    const auto first = pages->get<std::int64_t>(3, j),
               last = pages->get<std::int64_t>(3, j + 1);
    const auto closed = pages->get<std::uint8_t>(4, j);
    if (closed > 1 || first == last) {
      pages->reject("empty subpath requires an explicit Move point");
      break;
    }
    Endpoints initial, previous;
    for (auto segment = first; segment < last && pages->ok(); ++segment) {
      if (core) {
        auto verb = pages->get<std::uint8_t>(0, segment);
        const auto a = pages->get<std::int64_t>(1, segment),
                   b = pages->get<std::int64_t>(1, segment + 1);
        const std::uint64_t arity[] = {0, 1, 1, 2, 3, 0};
        if (verb < 1 || verb > 5 ||
            static_cast<std::uint64_t>(b - a) != arity[verb] ||
            (segment == first ? verb != 1 : verb == 1) ||
            (verb == 5 && segment + 1 != last) ||
            (segment + 1 == last && (verb == 5) != (closed != 0)))
          pages->reject("path verb, arity or closure mismatch");
      } else {
        auto ends = primitive(pages, p, segment, object);
        if (last - first > 1 &&
            (!ends.known || (segment > first && ends.first != previous.last)))
          pages->reject(
              "primitive endpoint equality unresolved or discontinuous");
        if (segment == first)
          initial = ends;
        previous = ends;
      }
    }
    if (!core && closed && !(last - first == 1 && initial.full_turn) &&
        (!initial.known || !previous.known || initial.first != previous.last))
      pages->reject("closed primitive endpoints do not agree exactly");
  }
  path_attributes(pages, subpaths, segments, controls);
}
void brush_values(const Parsed& p, Pages* pages) {
  std::array<std::int64_t, 10> ids{};
  for (unsigned j = 0; j < ids.size(); ++j) {
    ids[j] = pages->get<std::int64_t>(0, 0, j);
    if (ids[j] < 0)
      pages->reject("negative brush state identity");
  }
  if (!pages->ok())
    return;
  // stroke,next_event,final_event,final_dab,start_canvas,current_canvas,
  // generation,seed,counter,end_of_stroke. state =
  // last_position,remaining,P,A,E.
  const auto pending = pages->rows(2);
  if (ids[9] > 1 || ids[2] > ids[1] ||
      static_cast<std::uint64_t>(ids[1] - ids[2]) != pending ||
      pending != pages->rows(3) || pending > p.u[0] || pending > p.u[1] ||
      (ids[9] && pending) ||
      static_cast<std::uint64_t>(ids[3]) != pages->rows(4) || ids[5] < ids[4] ||
      ids[6] == 0)
    pages->reject("brush pending/final prefix or generation mismatch");
  const auto remaining = pages->get<double>(1, 0, 1),
             alpha = pages->get<double>(1, 0, 5);
  if (remaining < 0 || remaining > p.f[0] || alpha < 0 || alpha > 1)
    pages->reject("invalid brush carry state");
  if (alpha == 0) {
    for (unsigned c = 2; c < 5; ++c)
      if (pages->get<double>(1, 0, c) != 0)
        pages->reject("brush coverage association mismatch");
  }
  const auto last_position = pages->get<double>(1, 0, 0);
  if (!pages->rows(4) && (ids[2] || last_position != 0 || remaining != 0))
    pages->reject("unstarted brush has inconsistent position or phase");
  double last_dab = 0;
  const auto tolerance = [&](double a, double b) {
    return (32 * std::numeric_limits<double>::epsilon()) *
               std::max({std::abs(a), std::abs(b), p.f[0]}) +
           32 * std::numeric_limits<double>::denorm_min();
  };
  for (std::uint64_t j = 0; j < pages->rows(4) && pages->ok(); ++j) {
    const auto dab = pages->get<double>(4, j);
    if (j && (!(dab > last_dab) ||
              std::abs((dab - last_dab) - p.f[0]) > tolerance(dab, last_dab)))
      pages->reject("brush finalized dabs violate ordered spacing");
    last_dab = dab;
  }
  if (pages->rows(4)) {
    const auto distance = last_position - last_dab;
    if (!finite(distance) || distance < -tolerance(last_position, last_dab) ||
        std::abs((p.f[0] - distance) - remaining) >
            tolerance(last_position, last_dab))
      pages->reject("brush final dab and carried phase disagree");
  }
  auto previous =
      ids[2] ? last_position : -std::numeric_limits<double>::infinity();
  for (std::uint64_t j = 0; j < pending && pages->ok(); ++j) {
    auto position = pages->get<double>(3, j);
    if (pages->get<std::int64_t>(2, j) !=
            ids[2] + static_cast<std::int64_t>(j) ||
        position < previous)
      pages->reject("brush pending events are not ordered");
    previous = position;
  }
}
void iterative_values(const Parsed& p, Pages* pages) {
  auto generation = pages->get<std::int64_t>(0, 0, 0),
       iterations = pages->get<std::int64_t>(0, 0, 1),
       reason = pages->get<std::int64_t>(0, 0, 2);
  if (generation <= 0 || iterations < 0 ||
      static_cast<std::uint64_t>(iterations) > p.u[0] || reason < 0 ||
      reason > 1) {
    pages->reject("invalid iterative generation or stop reason");
    return;
  }
  double measured = 0;
  for (std::uint64_t j = 0; j < p.count && pages->ok(); ++j) {
    const auto a = pages->get<double>(2, j), x = pages->get<double>(1, j),
               b = pages->get<double>(3, j);
    if (a == 0)
      pages->reject("singular diagonal system");
    if (iterations == 0 && p.text[1] == "zero" && x != 0)
      pages->reject("iteration zero contradicts zero initialization");
    volatile double product = a * x;
    const double residual = b - product;
    if (!finite(residual))
      pages->reject("nonfinite measured residual");
    measured = std::max(measured, std::abs(residual));
  }
  const auto reported = pages->get<double>(4, 0);
  if (reported != measured || (reason == 0 && measured > p.f[0]) ||
      (reason == 1 && (static_cast<std::uint64_t>(iterations) != p.u[0] ||
                       measured <= p.f[0])) ||
      (p.u[1] && measured > p.f[0]))
    pages->reject(
        "iteration has no matching converged or approximate observation");
}
}  // namespace
Status validate_representation(
    const ResultRef& result, const ResourceBudget& resources,
    std::uint64_t maximum_window, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume_work) {
  if (!result.valid() || !result.owned_by(resources))
    return invalid("foreign representation owner");
  if (has_layer_schema(result.schema().id))
    return validate_layer_result(result, resources, maximum_window,
                                 cancellation, consume_work);
  if (!has_representation_schema(result.schema().id))
    return Status::success();
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Status{ErrorCode::OperationFailed,
                  "representation numeric environment unavailable"};
  if (!maximum_window)
    return exhausted();
  auto charged = resources.consume({result.schema().canonical_size()});
  if (charged.ok() && consume_work)
    charged = consume_work(result.schema().canonical_size());
  if (!charged.ok())
    return charged;
  auto checked = result.schema().validate(true);
  if (!checked.ok())
    return checked;
  Parsed p;
  checked = parse(result.schema(), &p);
  if (!checked.ok())
    return checked;
  Pages pages(result, resources, maximum_window, cancellation, consume_work);
  if (!pages.finite_fields())
    return pages.status();
  switch (p.kind) {
    case 1:
      spectrum_values(p, &pages);
      break;
    case 3:
      path_values(p, &pages, result.object_id());
      break;
    case 4:
      point_values(p, &pages);
      break;
    case 5:
      component_values(p, &pages);
      break;
    case 6:
      for (unsigned field = 0; field < 3; ++field)
        for (std::uint64_t row = 0; row < pages.rows(field) && pages.ok();
             ++row) {
          auto value = pages.get<float>(field, row);
          if (value < (field ? -0.5f : 0.0f) || value > (field ? 0.5f : 1.0f))
            pages.reject("YCbCr sample outside named full range");
        }
      break;
    case 7:
      brush_values(p, &pages);
      break;
    case 8:
      iterative_values(p, &pages);
      break;
    default:
      break;
  }
  return pages.status();
}
Result<BrushAdvance> advance_causal_brush(
    const BrushSpec& spec, const CausalBrushState& previous,
    const BrushEvent* events, std::uint64_t count, bool end,
    const ResourceBudget& resources, const CancellationToken& cancellation) {
  using Answer = Result<BrushAdvance>;
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Answer(Status{ErrorCode::OperationFailed,
                         "brush numeric environment unavailable"});
  if (!finite(spec.spacing) || spec.spacing <= 0 || spec.lookahead != 0 ||
      spec.maximum_pending > 1048576 ||
      spec.algorithm != "constant-spacing-1d-v1" || (count && !events) ||
      previous.ended || !finite(previous.last_position) ||
      !finite(previous.remaining) || previous.remaining < 0 ||
      previous.remaining > spec.spacing ||
      (!previous.started &&
       (previous.next_event || previous.dab_count || previous.remaining != 0 ||
        previous.last_position != 0)))
    return Answer(invalid("invalid causal brush state or mode"));
  try {
    BrushAdvance result{previous,
                        ResourceVector<double>(ResourceAllocator<double>(
                            resources, ResourceAllocationKind::Payload))};
    for (std::uint64_t i = 0; i < count; ++i) {
      if (cancellation.cancelled())
        return Answer(Status{ErrorCode::Cancelled, {}});
      auto status = resources.consume({1});
      if (!status.ok())
        return Answer(status);
      if (events[i].id != result.state.next_event ||
          events[i].id == UINT64_MAX || !finite(events[i].position) ||
          (result.state.started &&
           events[i].position < result.state.last_position))
        return Answer(invalid("brush event order mismatch"));
      if (!result.state.started) {
        result.state.started = true;
        result.state.last_position = events[i].position;
        result.state.remaining = 0;
      }
      auto distance = events[i].position - result.state.last_position;
      if (!finite(distance))
        return Answer(invalid("brush distance overflow"));
      auto position = result.state.last_position;
      while (distance >= result.state.remaining) {
        if (cancellation.cancelled())
          return Answer(Status{ErrorCode::Cancelled, {}});
        status = resources.consume({1});
        if (!status.ok())
          return Answer(status);
        const auto next = position + result.state.remaining;
        if (!finite(next) || (result.state.remaining > 0 && next <= position) ||
            result.state.dab_count == UINT64_MAX)
          return Answer(invalid("brush spacing cannot advance in binary64"));
        result.dabs.push_back(next);
        ++result.state.dab_count;
        distance -= result.state.remaining;
        position = next;
        result.state.remaining = spec.spacing;
      }
      result.state.remaining -= distance;
      result.state.last_position = events[i].position;
      ++result.state.next_event;
    }
    result.state.ended = end;
    return Answer(std::move(result));
  } catch (const std::bad_alloc&) {
    return Answer(exhausted());
  }
}
namespace {
Status decode_schema(const SchemaTemplate& schema, unsigned kind, Parsed* p) {
  auto checked = schema.validate(true);
  if (!checked.ok() || schema.id != kIds[kind])
    return invalid();
  return parse(schema, p);
}
}  // namespace
Result<BandsSpec> bands_spec(const SchemaTemplate& schema) {
  Parsed p;
  auto checked = decode_schema(schema, 2, &p);
  if (!checked.ok())
    return Result<BandsSpec>(checked);
  BandsSpec result;
  result.original_shape.assign(p.shape.begin(), p.shape.begin() + p.rank);
  result.axes.assign(p.axes.begin(), p.axes.begin() + p.axes_count);
  result.levels = p.u[0];
  result.filter_snapshot.assign(p.text[0].data(), p.text[0].size());
  for (unsigned i = 0; i < p.band_count; ++i) {
    const auto& b = p.bands[i];
    BandSpec band;
    band.level = b.level;
    band.mask = b.mask;
    band.explicit_zero = b.zero;
    band.shape.assign(b.shape.begin(), b.shape.begin() + p.rank);
    band.parent_shape.assign(b.parent.begin(), b.parent.begin() + p.rank);
    band.logical_origin.assign(b.origin.begin(), b.origin.begin() + p.rank);
    band.sample_step.assign(b.step.begin(), b.step.begin() + p.rank);
    band.phase.assign(b.phase.begin(), b.phase.begin() + p.rank);
    result.bands.push_back(std::move(band));
  }
  return Result<BandsSpec>(std::move(result));
}
Result<PathSetSpec> path_set_spec(const SchemaTemplate& schema) {
  Parsed p;
  auto checked = decode_schema(schema, 3, &p);
  if (!checked.ok())
    return Result<PathSetSpec>(checked);
  PathSetSpec result;
  result.authority = static_cast<PathAuthority>(p.u[0]);
  result.maximum_segments = p.u[1];
  result.maximum_controls = p.u[2];
  result.maximum_spline_degree = p.u[3];
  result.coordinate_system.assign(p.text[0].data(), p.text[0].size());
  return Result<PathSetSpec>(std::move(result));
}
Result<PointSetSpec> point_set_spec(const SchemaTemplate& schema) {
  Parsed p;
  auto checked = decode_schema(schema, 4, &p);
  if (!checked.ok())
    return Result<PointSetSpec>(checked);
  PointSetSpec result;
  result.dimensions = p.u[0];
  result.attribute_width = p.u[1];
  result.ids = static_cast<StableIdScheme>(p.u[2]);
  result.basis_count = p.u[3];
  result.maximum_count = p.u[4];
  result.coordinate_system.assign(p.text[0].data(), p.text[0].size());
  return Result<PointSetSpec>(std::move(result));
}
Result<ComponentsSpec> components_spec(const SchemaTemplate& schema) {
  Parsed p;
  auto checked = decode_schema(schema, 5, &p);
  if (!checked.ok())
    return Result<ComponentsSpec>(checked);
  return Result<ComponentsSpec>(ComponentsSpec{
      p.shape[0], p.shape[1], p.u[0], static_cast<ComponentIdScheme>(p.u[1])});
}
Result<YCbCr420Spec> ycbcr420_spec(const SchemaTemplate& schema) {
  Parsed p;
  auto checked = decode_schema(schema, 6, &p);
  if (!checked.ok())
    return Result<YCbCr420Spec>(checked);
  YCbCr420Spec result{p.shape[0], p.shape[1]};
  result.reference.assign(p.text[2].data(), p.text[2].size());
  return Result<YCbCr420Spec>(std::move(result));
}
Result<BrushSpec> brush_spec(const SchemaTemplate& schema) {
  Parsed p;
  auto checked = decode_schema(schema, 7, &p);
  if (!checked.ok())
    return Result<BrushSpec>(checked);
  BrushSpec result;
  result.spacing = p.f[0];
  result.maximum_pending = p.u[0];
  result.lookahead = p.u[1];
  result.algorithm.assign(p.text[0].data(), p.text[0].size());
  return Result<BrushSpec>(std::move(result));
}
Result<IterativeSpec> iterative_spec(const SchemaTemplate& schema) {
  Parsed p;
  auto checked = decode_schema(schema, 8, &p);
  if (!checked.ok())
    return Result<IterativeSpec>(checked);
  IterativeSpec result;
  result.size = p.shape[0];
  result.maximum_iterations = p.u[0];
  result.target_residual = p.f[0];
  result.require_converged = p.u[1] != 0;
  result.system_snapshot.assign(p.text[0].data(), p.text[0].size());
  result.initialization.assign(p.text[1].data(), p.text[1].size());
  return Result<IterativeSpec>(std::move(result));
}
Result<BandRange> band_range(const ResultRef& result, std::uint32_t level,
                             std::uint32_t mask) {
  if (!result.valid())
    return Result<BandRange>(invalid());
  Parsed p;
  auto checked = decode_schema(result.schema(), 2, &p);
  if (!checked.ok())
    return Result<BandRange>(checked);
  auto descriptor = result.descriptor();
  if (!descriptor.ok())
    return Result<BandRange>(descriptor.status());
  for (unsigned i = 0; i < p.band_count; ++i)
    if (p.bands[i].level == level && p.bands[i].mask == mask) {
      const auto& band = p.bands[i];
      return Result<BandRange>(BandRange{band.offset, band.count, band.zero});
    }
  return Result<BandRange>(Status{ErrorCode::NotFound, "band key absent"});
}
}  // namespace ps
