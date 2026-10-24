#include <memory>
#include <string>
#include <utility>

#include "05-filter/common.hpp"

namespace ps::plugin_internal::filter {
std::uint64_t gaussian_coefficient(generation::Math& m, double sigma,
                                   std::uint64_t index) {
  if (!index) {
    return bits(1);
  }
  const auto s = m.value(sigma);
  auto exponent = m.div(m.sq(m.exact.integer(index)), m.scale(m.sq(s), 2));
  exponent.negative = true;
  // exp(-x)<2^-1075 for x>=746, so direct RN64 is positive zero.
  if (m.exact.order(m.exact.absolute(exponent), m.exact.integer(746)) >= 0) {
    check(m.exact.status());
    return 0;
  }
  return m.certify(false, [&](generation::Interval result) {
    generation::Directed::Frame frame(m.d);
    auto argument = m.d.interval();
    m.enclose(argument, exponent);
    m.functions.exponential(result, argument);
  });
}
namespace {
struct Coefficients final : Program {
  double sx, sy;
  std::uint64_t rx, ry;
  Coefficients(Profile profile, const Parameters& p)
      : sx(real(p, "sigma_x")),
        sy(real(p, "sigma_y")),
        rx(integer(p, "radius_x")),
        ry(integer(p, "radius_y")) {
    check(numeric_ops::sequence_profile_available(profile));
    parameter(rx <= (UINT64_C(1) << 39) - 1 && ry <= (UINT64_C(1) << 39) - 1,
              "Gaussian coefficient extent overflow");
    parameter((sx != 0 || !rx) && (sy != 0 || !ry),
              "zero sigma requires zero radius");
    parameter(integer(p, "x_axis") == 0 && integer(p, "y_axis") == 0,
              "Gaussian coefficient vector output axes must be zero");
    add_output(this, tensor_schema(ElementType::Float64, {2 * rx + 1}));
    add_output(this, tensor_schema(ElementType::Float64, {2 * ry + 1}));
    scratch_bytes = sizeof(generation::Math);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& inv) const override {
    const Work work{inv, work_limit};
    const Fuel fuel{work};
    generation::MathArena arena(inv.scratch[0].allocation, work);
    arena->exact.bind(fuel);
    arena->d.consume = core_internal::WorkConsumer(fuel);
    const bool x = kernel.writes[0].first == 0;
    const auto radius = x ? rx : ry;
    const auto& output = inv.outputs[0];
    samples(output, [&](const Coordinate& at, std::uint64_t) {
      fuel.poll();
      const auto j = at[0] > radius ? at[0] - radius : radius - at[0];
      std::uint64_t raw;
      if (at[0] > radius && radius - j >= output.layout->origin[0]) {
        Coordinate mirror{radius - j};
        std::memcpy(
            &raw,
            static_cast<const std::uint8_t*>(output.allocation) +
                generation::offset(*output.layout, output.byte_offset, mirror),
            8);
      } else {
        raw = gaussian_coefficient(*arena, x ? sx : sy, j);
      }
      store(output, at, raw);
    });
    fuel.flush();
  }
};
}  // namespace
OperationDefinition make_coefficients(const std::string& key, Profile profile) {
  return generation::operation(
      key, 0,
      {{"sigma_x", Type::Float64},
       {"sigma_y", Type::Float64},
       {"radius_x", Type::Int64},
       {"radius_y", Type::Int64},
       {"x_axis", Type::Int64},
       {"y_axis", Type::Int64}},
      {{"kernel_x", tensor_schema(ElementType::Float64, {1})},
       {"kernel_y", tensor_schema(ElementType::Float64, {1})}},
      [profile](const auto&, const auto& p) {
        return std::make_shared<Coefficients>(profile, p);
      });
}
}  // namespace ps::plugin_internal::filter
