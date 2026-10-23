#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/fast_math.hpp"
#include "03-generation/fractal_math.hpp"
#include "03-generation/scalar_math.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
struct TemporalScratch final {
  ScalarMath<384> wide;
  ScalarMath<80> compact;
  explicit TemporalScratch(const Work& work) : wide(work), compact(work) {}
};
struct TemporalNoise final : Program {
  bool perlin, accelerated;
  ElementType type;
  unsigned rank;
  std::uint64_t seed, stream;
  std::array<std::int64_t, 2> period;
  TemporalNoise(bool p3, bool fast, const std::vector<OperationMetadata>& in,
                const Parameters& p)
      : perlin(p3),
        accelerated(fast),
        type(dtype(p)),
        seed(integer(p, "seed")),
        stream(integer(p, "stream")),
        period{integer(p, "period_x"), integer(p, "period_y")} {
    const auto& field = tensor(in[0]);
    auto dimensions = field.sample_shape();
    require(floating(field.descriptor.element_type) && dimensions.size() >= 2 &&
                dimensions.back() == 2,
            "temporal noise requires coordinate pairs");
    rank = dimensions.size() - 1;
    dimensions.pop_back();
    shape(in[1], {perlin ? UINT64_C(1) : UINT64_C(2)});
    shape(in[2], {1});
    if (perlin)
      shape(in[3], {1});
    add_output(this, tensor_schema(type, dimensions));
    scratch_bytes = sizeof(TemporalScratch);
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    auto dimensions = region.dimensions();
    dimensions.push_back({0, 2});
    std::vector<ReadRegion> result{{0, 0, Region(std::move(dimensions))}};
    for (unsigned port = 1; port < inputs.size(); ++port)
      result.push_back(
          {port, 0, Region::whole(tensor(inputs[port]).sample_shape())});
    return result;
  }
  template <unsigned Words>
  std::uint64_t exact_value(ScalarMath<Words>& m,
                            const std::array<double, 5>& v) const {
    using E = typename ScalarMath<Words>::E;
    m.exact.reset();
    std::array<typename E::Rational, 3> q;
    q[0] = m.exact.binary(bits(v[0]));
    q[1] = m.exact.binary(bits(v[1]));
    const auto a = m.exact.binary(bits(v[2]));
    const auto b = m.exact.binary(bits(v[3]));
    const auto c = m.exact.binary(bits(v[4]));
    if (perlin) {
      q[2] = m.add(m.mul(a, b), c);
    } else {
      q[0] = m.sub(q[0], m.mul(a, c));
      q[1] = m.sub(q[1], m.mul(b, c));
    }
    return m.rounded(fractal_basis(m.exact, q, perlin, seed, stream, 0, period),
                     type == ElementType::Float32);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Work work{invocation, work_limit};
    const Reader reader{kernel, invocation};
    auto* scratch =
        new (invocation.scratch[0].allocation) TemporalScratch(work);
    struct Destroy {
      TemporalScratch* p;
      ~Destroy() { p->~TemporalScratch(); }
    } destroy{scratch};
    std::array<double, 5> values{};
    values[2] = reader.number(1, 0, {0});
    values[3] =
        reader.number(perlin ? 2 : 1, 0, {perlin ? UINT64_C(0) : UINT64_C(1)});
    values[4] = reader.number(perlin ? 3 : 2, 0, {0});
    const auto& out = invocation.outputs[0];
    samples(out, [&](const Coordinate& at, std::uint64_t) {
      work.poll();
      auto coordinate = at;
      for (unsigned axis = 0; axis < 2; ++axis) {
        coordinate[rank] = axis;
        values[axis] = reader.number(0, 0, coordinate);
      }
#ifndef PHOTOSPIDER_NOI_EXACT_ONLY
      std::array<Fast, 3> q{Fast::point(values[0]), Fast::point(values[1]), {}};
      const auto a = Fast::point(values[2]), b = Fast::point(values[3]);
      const auto c = Fast::point(values[4]);
      if (perlin) {
        q[2] = a * b + c;
      } else {
        q[0] = q[0] - a * c;
        q[1] = q[1] - b * c;
      }
      if (type == ElementType::Float32 || accelerated) {
        const auto enclosure =
            fast_basis(q, perlin, seed, stream, 0, period, work);
        if (enclosure) {
          const auto result = fast_result(
              *enclosure, type == ElementType::Float32, accelerated);
          if (result) {
            store(out, at, *result);
            return;
          }
        }
      }
#endif
      bool compact = true;
      for (const auto v : values) {
        const auto parts = numeric_ops::BinaryParts::decode(bits(v), false);
        compact &= !parts.significand ||
                   (parts.exponent >= -100 && parts.exponent <= 100);
      }
      store(out, at,
            compact ? exact_value(scratch->compact, values)
                    : exact_value(scratch->wide, values));
    });
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_temporal_noise(OperationRegistry* registry) {
  using generation::check;
  using generation::operation;
  using generation::TemporalNoise;
  using generation::tensor_schema;
  try {
    const char* suffix[] = {"strict", "accelerated_apple_silicon",
                            "accelerated_x86_64"};
    for (bool perlin : {false, true}) {
      std::vector<OperationParameterSpec> parameters{
          {"dtype", OperationParameterType::String, false}};
      if (!perlin) {
        parameters.push_back({"seed", OperationParameterType::Int64});
        parameters.push_back({"stream", OperationParameterType::Int64});
        parameters.push_back(
            {"period_x", OperationParameterType::Int64, false});
        parameters.push_back(
            {"period_y", OperationParameterType::Int64, false});
      }
      for (unsigned profile = 0; profile < 3; ++profile)
        check(registry->register_operation(operation(
            std::string("noise.") +
                (perlin ? "temporal_perlin3d" : "advected_gradient2d") +
                "_v1_" + suffix[profile],
            perlin ? 4 : 3, parameters,
            {{"values", tensor_schema(ElementType::Float64, {1})}},
            [perlin, profile](const auto& in, const auto& p) {
              check(numeric_ops::sequence_profile_available(
                  static_cast<numeric_ops::SequenceProfile>(profile)));
              return std::make_shared<TemporalNoise>(perlin, profile != 0, in,
                                                     p);
            })));
    }
    return Status::success();
  } catch (const Status& status) {
    return status;
  }
}
}  // namespace ps::plugin_internal
