#include <memory>
#include <string>
#include <vector>

#include "03-generation/math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
// Sign of a+b*sqrt(c), including exact cancellation at a tangency.
int radical_sign(Math& m, const Rational& a, const Rational& b,
                 const Rational& c) {
  const auto zero = m.exact.integer(0);
  const int sa = m.exact.order(a, zero), sb = m.exact.order(b, zero);
  if (!sb || Exact::zero(c))
    return sa;
  if (!sa || sa == sb)
    return sb;
  const int cmp = m.exact.order(m.sq(a), m.mul(m.sq(b), c));
  return cmp == 0 ? 0 : cmp > 0 ? sa : sb;
}
struct TwoCircle final : Program {
  Canvas canvas;
  bool reject;
  std::uint64_t refinements;
  TwoCircle(const std::vector<OperationMetadata>& in, const Parameters& p)
      : canvas(p),
        reject(text(p, "no_solution", "valid_zero") == "reject"),
        refinements(integer(p, "max_refinements", 4096)) {
    shape(in[0], {2});
    shape(in[1], {1});
    shape(in[2], {2});
    shape(in[3], {1});
    const auto mode = text(p, "no_solution", "valid_zero");
    require(mode == "valid_zero" || mode == "reject",
            "unknown two-circle no_solution policy");
    add_output(this,
               tensor_schema(canvas.type, {canvas.height, canvas.width}, true));
    add_output(this, tensor_schema(ElementType::UInt8,
                                   {canvas.height, canvas.width}, true));
    scratch_bytes = sizeof(Math);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Reader reader{kernel, invocation};
    const Work work{invocation, work_limit};
    MathArena arena(invocation.scratch[0].allocation, work, refinements);
    auto& m = *arena;
    const auto r = m.input(reader, 1, 0, {0});
    const auto s = m.sub(m.input(reader, 3, 0, {0}), r);
    const auto cx = m.input(reader, 0, 0, {0});
    const auto cy = m.input(reader, 0, 0, {1});
    const auto dx = m.sub(m.input(reader, 2, 0, {0}), cx);
    const auto dy = m.sub(m.input(reader, 2, 0, {1}), cy);
    const auto a = m.sub(m.add(m.sq(dx), m.sq(dy)), m.sq(s));
    const auto zero = m.exact.integer(0);
    const auto& view = invocation.outputs[0];
    const bool valid_output = kernel.writes[0].first == 1;
    const bool narrow = canvas.type == ElementType::Float32;
    samples(view, [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      m.exact.reset();
      const auto qx = m.sub(m.center(canvas.origin_x, at[1]), cx);
      const auto qy = m.sub(m.center(canvas.origin_y, at[0]), cy);
      const auto b =
          m.scale(m.add(m.add(m.mul(qx, dx), m.mul(qy, dy)), m.mul(r, s)), -2);
      const auto c = m.sub(m.add(m.sq(qx), m.sq(qy)), m.sq(r));
      bool valid = false;
      Rational bias = zero, factor = zero, radicand = zero;
      if (Exact::zero(a)) {
        if (!Exact::zero(b)) {
          bias = m.div(m.scale(c, -1), b);
          valid = m.exact.order(m.add(r, m.mul(s, bias)), zero) >= 0;
        } else if (Exact::zero(c) && m.exact.order(s, zero) < 0) {
          bias = m.div(m.scale(r, -1), s);
          valid = true;
        }
      } else {
        radicand = m.sub(m.sq(b), m.scale(m.mul(a, c), 4));
        if (m.exact.order(radicand, zero) >= 0) {
          const auto denominator = m.scale(a, 2);
          bias = m.div(m.scale(b, -1), denominator);
          factor = m.div(m.exact.integer(m.exact.order(a, zero) < 0 ? -1 : 1),
                         denominator);
          const auto radius_bias = m.add(r, m.mul(s, bias));
          valid = radical_sign(m, radius_bias, m.mul(s, factor), radicand) >= 0;
          if (!valid && !Exact::zero(radicand)) {
            factor = m.scale(factor, -1);
            valid =
                radical_sign(m, radius_bias, m.mul(s, factor), radicand) >= 0;
          }
        }
      }
      check(m.exact.status());
      if (!valid && reject)
        throw Status{ErrorCode::OperationFailed,
                     "two-circle coordinate has no finite maximum",
                     DomainErrorCode{UINT64_C(0x47454e0002)}};
      store(view, at,
            valid_output ? static_cast<std::uint64_t>(valid)
            : !valid     ? 0
            : Exact::zero(factor) || Exact::zero(radicand)
                ? m.rounded(bias, narrow)
                : m.affine_sqrt(bias, factor, radicand, narrow));
    });
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_two_circle_radial(OperationRegistry* registry) {
  using namespace generation;  // NOLINT(build/namespaces)
  try {
    auto parameters = canvas_parameters();
    parameters.push_back(
        {"no_solution", OperationParameterType::String, false});
    parameters.push_back(
        {"max_refinements", OperationParameterType::Int64, false});
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    for (unsigned profile = 0; profile < 3; ++profile)
      check(registry->register_operation(operation(
          std::string("generation.two_circle_radial_coordinate_v1_") +
              suffix[profile],
          4, parameters,
          {{"t", tensor_schema(ElementType::Float64, {1, 1}, true)},
           {"valid", tensor_schema(ElementType::UInt8, {1, 1}, true)}},
          [profile](const auto& in, const auto& p) {
            check(numeric_ops::sequence_profile_available(
                static_cast<numeric_ops::SequenceProfile>(profile)));
            return std::make_shared<TwoCircle>(in, p);
          })));
    return Status::success();
  } catch (const Status& failure) {
    return failure;
  }
}
}  // namespace ps::plugin_internal
