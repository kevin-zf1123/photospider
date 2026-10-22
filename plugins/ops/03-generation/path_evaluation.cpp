#include <memory>
#include <string>
#include <vector>

#include "03-generation/path_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
enum class EvaluationKind { Bezier, Hermite, Arc, Spline, Core, Primitives };
struct Evaluation final : Program {
  EvaluationKind kind;
  ElementType type;
  unsigned degree = 3, index_port = 1, parameter_port = 2;
  bool full_turn = false;
  explicit Evaluation(EvaluationKind selected,
                      const std::vector<OperationMetadata>& inputs,
                      const Parameters& p)
      : kind(selected), type(dtype(p)) {
    if (kind == EvaluationKind::Spline) {
      index_port = 4;
      parameter_port = 5;
    }
    const auto& indices = tensor(inputs[index_port]);
    const auto queries = indices.sample_shape();
    require(indices.descriptor.element_type == ElementType::Int64 &&
                queries.size() == 1,
            "path queries require Int64 segment_indices[N]");
    shape(inputs[parameter_port], queries);
    if (kind == EvaluationKind::Bezier || kind == EvaluationKind::Hermite) {
      const auto& field = tensor(inputs[0]);
      const auto dimensions = field.sample_shape();
      const auto requested_degree =
          kind == EvaluationKind::Hermite ? 3 : integer(p, "degree", 3);
      require(requested_degree >= 1 && requested_degree <= 3,
              "Bezier degree must be 1..3");
      degree = static_cast<unsigned>(requested_degree);
      require(dimensions.size() == 3 && dimensions[1] == degree + 1 &&
                  dimensions[2] == 2 && floating(field.descriptor.element_type),
              "path controls must be [S,degree+1,2]");
    } else if (kind == EvaluationKind::Arc) {
      const auto& field = tensor(inputs[0]);
      const auto dimensions = field.sample_shape();
      require(dimensions.size() == 2 && dimensions[1] == 8 &&
                  floating(field.descriptor.element_type),
              "arc payload must be [S,8]");
      const auto mode = text(p, "arc_kind", "arc_sweep");
      require(mode == "arc_sweep" || mode == "full_turn",
              "arc kind must be arc_sweep or full_turn");
      full_turn = mode == "full_turn";
    } else if (kind == EvaluationKind::Spline) {
      const auto& records = tensor(inputs[0]);
      require(records.sample_shape().size() == 2 &&
                  records.sample_shape()[1] == 5 &&
                  records.descriptor.element_type == ElementType::Int64,
              "spline records require Int64[S,5]");
      const auto& controls = tensor(inputs[1]);
      require(controls.sample_shape().size() == 2 &&
                  controls.sample_shape()[1] == 2 &&
                  floating(controls.descriptor.element_type),
              "spline controls require Float32/64[U,2]");
      require(tensor(inputs[2]).sample_shape().size() == 1 &&
                  floating(tensor(inputs[2]).descriptor.element_type),
              "spline knots require a floating vector");
      shape(inputs[3], {controls.sample_shape()[0]});
    } else {
      const auto info = path_info(*inputs[0].result_schema);
      require(info.primitive == (kind == EvaluationKind::Primitives),
              "PathSet evaluation authority mismatch");
    }
    for (unsigned i = 0; i < 3; ++i) {
      add_output(this, tensor_schema(type, {queries[0], 2}, false, true));
    }
    add_output(this, tensor_schema(ElementType::UInt8, queries));
    scratch_bytes = sizeof(EvaluationScratch);
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    std::vector<ReadRegion> result;
    for (unsigned port = 0; port < inputs.size(); ++port) {
      if (port == index_port || port == parameter_port) {
        result.push_back({port, 0, Region({region.dimensions()[0]})});
      } else {
        for (unsigned field = 0;
             field < inputs[port].result_schema->tensors.size(); ++field) {
          result.push_back(
              {port, field,
               Region::whole(
                   inputs[port].result_schema->tensors[field].sample_shape())});
        }
      }
    }
    return result;
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    auto* arithmetic =
        new (invocation.scratch[0].allocation) EvaluationScratch(work);
    struct Destroy {
      EvaluationScratch* p;
      ~Destroy() { p->~EvaluationScratch(); }
    } destroy{arithmetic};
    auto& m = arithmetic->math;
    CurveMath evaluate(reader, *arithmetic);
    const auto output = kernel.writes[0].first;
    const auto& view = invocation.outputs[0];
    if ((kind == EvaluationKind::Core || kind == EvaluationKind::Primitives) &&
        reader.index(0, kind == EvaluationKind::Core ? 5 : 12, {0}) == 0) {
      throw Status{ErrorCode::OperationFailed,
                   "empty PathSet has no evaluable segment",
                   DomainErrorCode{UINT64_C(0x47454e0002)}};
    }
    for (std::uint64_t row = view.layout->origin[0];
         row < view.layout->origin[0] + view.layout->shape[0]; ++row) {
      work.poll();
      m.exact.reset();
      const auto selected =
          static_cast<std::uint64_t>(reader.index(index_port, 0, {row}));
      const auto t = m.input(reader, parameter_port, 0, {row});
      Curve curve;
      if (kind == EvaluationKind::Core) {
        curve = core_curve(reader, m, selected);
      } else if (kind == EvaluationKind::Primitives) {
        curve = primitive_curve(reader, selected);
      } else {
        curve.row = selected;
        curve.degree = degree;
        curve.kind = kind == EvaluationKind::Bezier    ? CurveKind::Bezier
                     : kind == EvaluationKind::Hermite ? CurveKind::Hermite
                     : kind == EvaluationKind::Spline  ? CurveKind::Spline
                     : full_turn                       ? CurveKind::FullTurn
                                                       : CurveKind::Arc;
      }
      CurveValue value;
      if (evaluate.rational(curve, t, output != 0, &value)) {
        const bool valid = output == 0 || !Exact::zero(value.derivative[0]) ||
                           !Exact::zero(value.derivative[1]);
        if (output == 3) {
          store(view, {row}, valid);
        } else {
          const bool narrow = type == ElementType::Float32;
          const auto norm = output == 2 ? m.add(m.sq(value.derivative[0]),
                                                m.sq(value.derivative[1]))
                                        : m.exact.integer(0);
          for (unsigned axis = view.layout->origin[1];
               axis < view.layout->origin[1] + view.layout->shape[1]; ++axis) {
            const auto result =
                output == 0   ? m.rounded(value.position[axis], narrow,
                                          value.copy_position[axis])
                : output == 1 ? m.rounded(value.derivative[axis], narrow,
                                          value.copy_derivative[axis])
                : valid       ? m.tangent(value.derivative[axis], norm, narrow)
                              : 0;
            store(view, {row, axis}, result);
          }
        }
      } else if (output == 3) {
        store(view, {row}, evaluate.sample(curve, t, 3, 0, false));
      } else {
        for (unsigned axis = view.layout->origin[1];
             axis < view.layout->origin[1] + view.layout->shape[1]; ++axis) {
          store(view, {row, axis},
                evaluate.sample(curve, t, output, axis,
                                type == ElementType::Float32));
        }
      }
      check(m.exact.status());
    }
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_path_evaluation(OperationRegistry* registry) {
  using generation::check;
  using generation::Evaluation;
  using generation::EvaluationKind;
  using generation::operation;
  using generation::tensor_schema;
  try {
    struct Entry {
      const char* name;
      EvaluationKind kind;
      unsigned inputs;
    };
    const Entry entries[] = {
        {"evaluate_bezier_path", EvaluationKind::Bezier, 3},
        {"evaluate_hermite_path", EvaluationKind::Hermite, 3},
        {"evaluate_elliptic_arc", EvaluationKind::Arc, 3},
        {"evaluate_bspline_path", EvaluationKind::Spline, 6},
        {"evaluate_core_pathset", EvaluationKind::Core, 3},
        {"evaluate_primitive_pathset", EvaluationKind::Primitives, 3}};
    const std::vector<std::pair<std::string, SchemaTemplate>> outputs{
        {"position", tensor_schema(ElementType::Float64, {1, 2}, false, true)},
        {"derivative",
         tensor_schema(ElementType::Float64, {1, 2}, false, true)},
        {"tangent", tensor_schema(ElementType::Float64, {1, 2}, false, true)},
        {"tangent_valid", tensor_schema(ElementType::UInt8, {1})}};
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    for (const auto& entry : entries) {
      std::vector<OperationParameterSpec> parameters{
          {"dtype", OperationParameterType::String, false}};
      if (entry.kind == EvaluationKind::Bezier) {
        parameters.push_back({"degree", OperationParameterType::Int64});
      }
      if (entry.kind == EvaluationKind::Arc) {
        parameters.push_back(
            {"arc_kind", OperationParameterType::String, false});
      }
      for (unsigned profile = 0; profile < 3; ++profile) {
        check(registry->register_operation(operation(
            std::string("path.") + entry.name + "_v1_" + suffix[profile],
            entry.inputs, parameters, outputs,
            [kind = entry.kind, profile](const auto& inputs, const auto& p) {
              check(numeric_ops::sequence_profile_available(
                  static_cast<numeric_ops::SequenceProfile>(profile)));
              return std::make_shared<Evaluation>(kind, inputs, p);
            })));
      }
    }
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
