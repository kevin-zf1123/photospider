#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
void matrix_bits_and_batches() {
  Driver d;
  for (unsigned cin = 2; cin <= 4; ++cin)
    for (unsigned cout = 2; cout <= 4; ++cout) {
      std::vector<uint64_t> vectors(130 * cin), matrix(cin * cout), bias(cout);
      for (unsigned r = 0; r < 130; ++r)
        for (unsigned j = 0; j < cin; ++j)
          vectors[r * cin + (cin - 1 - j)] =
              float_bits((r % 17 + j + 1) * .25F);
      for (unsigned o = 0; o < cout; ++o) {
        bias[o] = float_bits((o + 1) * .25F);
        for (unsigned j = 0; j < cin; ++j)
          matrix[o * cin + j] =
              float_bits((o + j + 1) * .5F * (j % 2 ? -1 : 1));
      }
      auto x = d.source({ElementType::Float32, {65, cin}}, vectors,
                        {1 + (cin - 1) * 4,
                         {static_cast<int64_t>(65 * cin * 4),
                          static_cast<int64_t>(cin * 4), -4}},
                        {}, {2});
      auto m = d.source({ElementType::Float32, {cout, cin}}, matrix,
                        {1, {static_cast<int64_t>(cin * 4), 4}});
      auto b = d.source({ElementType::Float32, {cout}}, bias, {1, {4}});
      for (auto profile :
           {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
            CpuNumericProfile::X86Avx2}) {
        auto node = take(numeric::matrix_transform_node(
            7, WorkflowInputReference{11}, WorkflowInputReference{12},
            WorkflowInputReference{13}, profile));
        std::vector<OperationMetadata> metadata;
        for (const auto& input : {x, m, b}) {
          OperationMetadata item;
          item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
          metadata.push_back(std::move(item));
        }
        auto available =
            d.registry->resolve_traits(node.operation, metadata, {});
        if (!available.ok()) {
          require(profile != CpuNumericProfile::Strict &&
                      available.status().code == ErrorCode::BackendUnavailable,
                  "unavailable matrix profile reports BackendUnavailable");
          continue;
        }
        std::fenv_t saved;
        require(std::fegetenv(&saved) == 0 && std::fesetround(FE_UPWARD) == 0,
                "matrix floating-environment fixture setup");
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO);
        auto result = take(d.run(node.operation, {x, m, b}));
        const bool restored = std::fegetround() == FE_UPWARD &&
                              std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
        std::fesetenv(&saved);
        require(restored,
                "matrix execution restores caller rounding mode and exception "
                "flags");
        const auto& tensor = result.results.at("out").schema().tensors[0];
        require(
            tensor.descriptor.shape == std::vector<uint64_t>({2, 65, cout}) &&
                tensor.batch_axes.empty() && tensor.facets.empty(),
            "matrix Result includes batch dimensions and replaces only last "
            "extent");
        for (unsigned r = 0; r < 130; ++r)
          for (unsigned o = 0; o < cout; ++o) {
            double expected = (o + 1) * .25;
            for (unsigned j = 0; j < cin; ++j)
              expected +=
                  (r % 17 + j + 1) * .25 * (o + j + 1) * .5 * (j % 2 ? -1 : 1);
            require(read_bits(result.results.at("out"), {r / 65, r % 65, o}) ==
                        float_bits(static_cast<float>(expected)),
                    "rectangular matrix matches independent exact dyadic sum "
                    "including block tails");
          }
        for (unsigned port = 0; port < 3; ++port)
          for (uint32_t role : {1U, 4U})
            require(take(result.dependencies.potential_dirty(
                             "input" + std::to_string(port),
                             take(Footprint::all(metadata[port]
                                                     .result_schema->tensors[0]
                                                     .sample_shape())),
                             role))
                            .at("out") == take(Footprint::all({2, 65, cout})),
                    "every matrix operand retains full Data and Validation "
                    "dependence");
      }
    }
}
void matrix_numerics_and_boundaries() {
  Driver d;
  auto x = d.source(
      {ElementType::Float64, {2}},
      {double_bits(std::ldexp(1., 600)), double_bits(std::ldexp(1., 600))},
      {1, {8}});
  auto m = d.source({ElementType::Float64, {2, 2}},
                    {double_bits(std::ldexp(1., 600)),
                     double_bits(-std::ldexp(1., 600)), 0, 0},
                    {1, {16, 8}});
  auto b = d.source({ElementType::Float64, {2}},
                    {double_bits(3), double_bits(-0.)}, {1, {8}});
  auto out = take(d.run("numeric.matrix_transform_strict", {x, m, b}))
                 .results.at("out");
  require(
      read_bits(out, {0}) == double_bits(3) && read_bits(out, {1}) == 0,
      "exact products cancel before overflow and singular matrices are legal");
  auto residual = d.source({ElementType::Float32, {4}},
                           {float_bits(std::ldexp(1.F, 120)), 0x3f800000,
                            float_bits(-std::ldexp(1.F, 120)), 0},
                           {1, {4}});
  auto ones =
      d.source({ElementType::Float32, {2, 4}}, {0x3f800000}, {1, {0, 0}});
  auto zero_bias = d.source({ElementType::Float32, {2}}, {0}, {1, {0}});
  std::vector<OperationMetadata> residual_metadata;
  for (const auto& input : {residual, ones, zero_bias}) {
    OperationMetadata item;
    item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
    residual_metadata.push_back(std::move(item));
  }
  for (const auto* suffix :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    const auto key = std::string("numeric.matrix_transform") + suffix;
    auto available = d.registry->resolve_traits(key, residual_metadata, {});
    if (!available.ok()) {
      require(std::string(suffix) != "_strict" &&
                  available.status().code == ErrorCode::BackendUnavailable,
              "unavailable exact-replay profile remains explicit");
      continue;
    }
    auto exact =
        take(d.run(key, {residual, ones, zero_bias})).results.at("out");
    require(read_bits(exact, {0}) == 0x3f800000 &&
                read_bits(exact, {1}) == 0x3f800000,
            "matrix candidate cannot erase a residual lost by binary64 "
            "sequential summation");
  }
  x = d.source({ElementType::Float32, {2}}, {0xff800123, 0x7f800000}, {1, {4}});
  m = d.source({ElementType::Float32, {2, 2}}, {0x7f800456, 0, 0, 0x3f800000},
               {1, {8, 4}});
  b = d.source({ElementType::Float32, {2}}, {0x7f800789}, {1, {0}});
  for (const auto* key :
       {"numeric.matrix_transform_strict",
        "numeric.matrix_transform_accelerated_apple_silicon"}) {
    if (std::string(key).find("apple") != std::string::npos) {
#if !defined(__aarch64__) || !defined(__APPLE__)
      continue;
#endif
    }
    out = take(d.run(key, {x, m, b})).results.at("out");
    require(
        read_bits(out, {0}) == 0xffc00123 && read_bits(out, {1}) == 0xffc00123,
        "vector source NaN wins over matrix, bias and generated invalid "
        "products");
  }
  x = d.source({ElementType::Float32, {2}}, {0, 0x7f800000}, {1, {4}});
  m = d.source({ElementType::Float32, {2, 2}}, {0x7f800000, 0, 0, 0x3f800000},
               {1, {8, 4}});
  b = d.source({ElementType::Float32, {2}}, {0}, {1, {0}});
  out = take(d.run("numeric.matrix_transform_strict", {x, m, b}))
            .results.at("out");
  require(
      read_bits(out, {0}) == 0x7fc00000 && read_bits(out, {1}) == 0x7f800000,
      "matrix zero-times-infinity and infinity classification");
  x = d.source({ElementType::Float32, {2}}, {0x80000000, 0}, {1, {4}});
  m = d.source({ElementType::Float32, {2, 2}},
               {0x3f800000, 0xbf800000, 0x3f800000, 0xbf800000}, {1, {8, 4}});
  b = d.source({ElementType::Float32, {2}}, {0x80000000, 0}, {1, {4}});
  out = take(d.run("numeric.matrix_transform_strict", {x, m, b}))
            .results.at("out");
  require(read_bits(out, {0}) == 0x80000000 && read_bits(out, {1}) == 0,
          "matrix exact zero is negative only when all products and bias are "
          "negative zero");
  std::vector<OperationMetadata> metadata;
  for (const auto& input : {x, m, b}) {
    OperationMetadata item;
    item.result_schema = std::make_shared<SchemaTemplate>(input.schema());
    metadata.push_back(std::move(item));
  }
  auto bad_bias = b.schema();
  bad_bias.tensors[0].descriptor.shape = {3};
  auto invalid_metadata = metadata;
  invalid_metadata[2].result_schema =
      std::make_shared<SchemaTemplate>(bad_bias);
  auto rejected = d.registry->resolve_traits("numeric.matrix_transform_strict",
                                             invalid_metadata, {});
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "matrix rejects output-component and bias-length mismatch before "
          "execution");
  auto oversized = x.schema();
  oversized.tensors[0].descriptor.shape = {UINT64_C(1) << 40, 2};
  invalid_metadata = metadata;
  invalid_metadata[0].result_schema =
      std::make_shared<SchemaTemplate>(oversized);
  rejected = d.registry->resolve_traits("numeric.matrix_transform_strict",
                                        invalid_metadata, {});
  require(
      !rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
      "matrix input count above 2^40 is rejected without payload allocation");
  auto typed =
      d.source({ElementType::Float32, {2, 2}}, {0, 0, 0, 0x40000000},
               {1, {8, 4}}, {take(encode_semantic(coverage_semantics()))});
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto failed =
      d.run("numeric.matrix_transform_strict", {typed, m, b},
            take(Footprint::from_regions({2, 2}, {Region({{0, 1}, {0, 1}})})));
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.input_id == 11 &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "matrix validates typed samples outside projected output and "
          "releases payload");
  auto empty = take(d.run("numeric.matrix_transform_strict", {typed, m, b},
                          take(Footprint::none({2, 2}))));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty matrix skips payload validation");
  Driver limited(30000);
  auto vectors = limited.source({ElementType::Float32, {65, 2}}, {0x3f800000},
                                {1, {0, 0}});
  auto matrix =
      limited.source({ElementType::Float32, {2, 2}}, {0x3f800000}, {1, {0, 0}});
  auto bias = limited.source({ElementType::Float32, {2}}, {0}, {1, {0}});
  const auto live = limited.root.statistics().live.values;
  failed =
      limited.run("numeric.matrix_transform_strict", {vectors, matrix, bias});
  require(!failed.ok() && failed.status().reason == FailureReason::WorkLimit &&
              limited.root.statistics().live[ResourceKind::Payload] ==
                  live[static_cast<size_t>(ResourceKind::Payload)],
          "matrix WorkLimit releases unpublished payload and exact workspace");
  limited.context.reset();
  require(limited.root.statistics().live.values == live,
          "retired matrix failure releases all execution metadata");
  CancellationSource stop;
  stop.cancel();
  failed =
      d.run("numeric.matrix_transform_strict", {x, m, b}, {}, {}, stop.token());
  require(!failed.ok() && failed.status().code == ErrorCode::Cancelled,
          "matrix pre-cancellation preserves host category");
}
}  // namespace
int main() {
  try {
    matrix_bits_and_batches();
    matrix_numerics_and_boundaries();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
