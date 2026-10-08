#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "support/result_image_fixture.hpp"

namespace {
using namespace ps::test_image;  // NOLINT(build/namespaces)
void image_gaussian_contract() {
  Driver d;
  const auto shape = schema();
  auto builder = take(ResultBuilder::start(d.root, shape, "gaussian.source"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(d.root, 1, {0, 8, 0, 0})))
              .ok(),
          "Gaussian source descriptor");
  std::vector<float> pixels(240);
  for (unsigned i = 0; i < pixels.size(); ++i) {
    const auto slot = i % 60, batch = i / 60;
    pixels[i] =
        slot % 4 == 3
            ? .5F
            : static_cast<float>(
                  static_cast<int>((slot * 37 + batch * 11) % 101) - 50) /
                  7.F;
  }
  require(builder
              .publish_tensor(
                  0, Region::whole(shape.tensors[0].sample_shape()),
                  ByteView(reinterpret_cast<const uint8_t*>(pixels.data()),
                           pixels.size() * 4),
                  take(ResultRelation::cartesian(d.root, pixels.size(),
                                                 {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "Gaussian source samples");
  ExecutionBinding binding;
  binding.name = "image";
  binding.result = take(builder.seal());
  // Captured from the c552eb56 C Gaussian callback, radius 2 and sigma .9.
  const uint32_t expected[] = {
      0xc0748e68U, 0xbe1ac4d9U, 0x3de5e9f3U, 0x3f000000U, 0xbee97e75U,
      0x3f01f488U, 0xbea767b7U, 0x3f000000U, 0x3fc9673cU, 0x3e180e3eU,
      0x3d233547U, 0x3f000000U, 0x3fbcce6eU, 0xbe9b9600U, 0xbea8169dU,
      0x3f000000U, 0x400187f5U, 0xbfb8c0b3U, 0x3f6dae14U, 0x3f000000U,
      0xbfd52c35U, 0xbf76c091U, 0xbfb69522U, 0x3f000000U, 0xbee05335U,
      0xbea4c006U, 0x3e93ead2U, 0x3f000000U, 0x3d334a19U, 0x3e2a64bfU,
      0x3fad90f2U, 0x3f000000U, 0xbe93c8f1U, 0xbe306b87U, 0x3f8dd821U,
      0x3f000000U, 0xbee9d7faU, 0x3e7aaf6eU, 0x3fc33b7eU, 0x3f000000U,
      0xbddc56b7U, 0xc04cfb58U, 0xbf70e0aeU, 0x3f000000U, 0xbf918f3bU,
      0xbf0ba965U, 0x3f820f01U, 0x3f000000U, 0xbfbd36d8U, 0x3fbeeab7U,
      0x3fadb92aU, 0x3f000000U, 0xbff96effU, 0x3fb15959U, 0x3f81e347U,
      0x3f000000U, 0xbfb32d83U, 0x40291b06U, 0xbdff7ec7U, 0x3f000000U,
      0xc0291b06U, 0x3fb32d83U, 0x3fc6d359U, 0x3f000000U, 0xbfb15959U,
      0x3ff96effU, 0x3ed3e09cU, 0x3f000000U, 0xbfbeeab7U, 0x3fbd36d8U,
      0xbd71b27eU, 0x3f000000U, 0x3f0ba965U, 0x3f918f3bU, 0xbf0c6bcdU,
      0x3f000000U, 0x404cfb58U, 0x3ddc56b7U, 0xbe069444U, 0x3f000000U,
      0xbe7aaf6eU, 0x3ee9d7faU, 0xbdc60121U, 0x3f000000U, 0x3e306b87U,
      0x3e93c8f1U, 0x3ea40d34U, 0x3f000000U, 0xbe2a64bfU, 0xbd334a19U,
      0xbe2d2833U, 0x3f000000U, 0x3ea4c006U, 0x3ee05335U, 0xbf245582U,
      0x3f000000U, 0x3f76c091U, 0x3fd52c35U, 0xbfe405f4U, 0x3f000000U,
      0x3fb8c0b3U, 0xc00187f5U, 0x3f0008c8U, 0x3f000000U, 0x3e9b9600U,
      0xbfbcce6eU, 0x3fe0e116U, 0x3f000000U, 0xbe180e3eU, 0xbfc9673cU,
      0x3fa111ecU, 0x3f000000U, 0xbf01f488U, 0x3ee97e75U, 0x3f4b6610U,
      0x3f000000U, 0x3e1ac4d9U, 0x40748e68U, 0xbf97a17fU, 0x3f000000U,
      0xbff363f5U, 0x3ef0cf20U, 0x403fa40bU, 0x3f000000U, 0xbfad50c0U,
      0xbf8ee2f3U, 0x3fdc8ec6U, 0x3f000000U, 0xbf92566dU, 0x3f073d56U,
      0x3fa03751U, 0x3f000000U, 0xbf682556U, 0x400a93dcU, 0x3f42c170U,
      0x3f000000U, 0xc0305badU, 0x3fa4ac33U, 0x3fa7a234U, 0x3f000000U,
      0xbe5a6aaaU, 0x3f88983dU, 0x3f03845aU, 0x3f000000U, 0xbf8e3907U,
      0x3da0b541U, 0xbd263ca0U, 0x3f000000U, 0xbe924927U, 0x3ed4e68eU,
      0xbf050a38U, 0x3f000000U, 0x3f0a28e8U, 0x3f3f34eeU, 0xbf805ff3U,
      0x3f000000U, 0xbeb75cf7U, 0x3eafd2bbU, 0xbf95e3b5U, 0x3f000000U,
      0x400bc963U, 0xbf15365cU, 0xbedbd55fU, 0x3f000000U, 0x3eabb85eU,
      0xbe14caeaU, 0xbfd9aea7U, 0x3f000000U, 0x3f1263b2U, 0xbf066098U,
      0xc00b030fU, 0x3f000000U, 0x3f48585aU, 0xbf62ecbaU, 0xc02a6e5bU,
      0x3f000000U, 0x3faa3f63U, 0xc008c89dU, 0xc0074d9dU, 0x3f000000U,
      0xbea8fd8cU, 0xbeeb17cdU, 0xc03db2faU, 0x3f000000U, 0x3db23fb1U,
      0xbf65fd01U, 0x3ecd5cfaU, 0x3f000000U, 0xbece115dU, 0xbf0815d0U,
      0x401b8f0bU, 0x3f000000U, 0xbf601424U, 0xbf665474U, 0x401542a5U,
      0x3f000000U, 0xc000f2a2U, 0x3eb6c9e1U, 0x40386362U, 0x3f000000U,
      0x3fadd73eU, 0xbfffb9b5U, 0xbf4eeab4U, 0x3f000000U, 0x3e5fe055U,
      0xbe90a777U, 0x3ed68839U, 0x3f000000U, 0xbe8226c0U, 0x3f48d8c2U,
      0x3f66a256U, 0x3f000000U, 0xbf3e6407U, 0x3f09671eU, 0x3f11893eU,
      0x3f000000U, 0xbea73a91U, 0x3f742ddcU, 0x3ecd036fU, 0x3f000000U,
      0x40705badU, 0xbfc194e9U, 0x3f3fe2deU, 0x3f000000U, 0x3fe362d4U,
      0x3ee3a9bbU, 0xbe8f6183U, 0x3f000000U, 0x3fa803efU, 0x3f492932U,
      0xbf1efffeU, 0x3f000000U, 0x3f50698bU, 0x3ee2fad3U, 0xbf8bb824U,
      0x3f000000U, 0x400488bcU, 0xbf3238fbU, 0xbf0aed51U, 0x3f000000U,
  };
  const auto parameters =
      std::map<std::string, ParameterValue>{{"radius", int64_t{2}},
                                            {"sigma", .9}};
  auto prepared = d.prepare("image.gaussian_blur", {binding}, parameters);
  auto result = take(d.context->execute(prepared.plan, prepared.bindings))
                    .results.at("out");
  auto captured = take(result.descriptor());
  for (uint64_t i = 0; i < pixels.size(); ++i) {
    const std::vector<uint64_t> at{i / 120, i / 60 % 2, i / 20 % 3, i / 4 % 5,
                                   i % 4};
    uint32_t bits = 0;
    require(result.read_tensor(captured, 0, at, &bits, 4).ok() &&
                bits == expected[i],
            "Gaussian preserves old two-pass Float32 rounding across batches");
  }
  const auto output_shape = shape.tensors[0].sample_shape();
  const auto query = take(Footprint::from_regions(
      output_shape, {Region({{1, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 4}})}));
  auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
  auto partial = take(d.context->execute_fragments(frozen, {{"out", query}}));
  require(read(partial.results.at("out"), {1, 0, 0, 0, 0}) ==
              read(result, {1, 0, 0, 0, 0}),
          "Gaussian offset ROI matches full output");
  const auto needed = take(partial.dependencies.source_support()).at("image");
  require(needed == take(Footprint::from_regions(
                        output_shape,
                        {Region({{1, 1}, {0, 1}, {0, 3}, {0, 3}, {0, 4}})})),
          "Gaussian exact clipped halo support");
  const auto elsewhere = take(Footprint::from_regions(
      output_shape, {Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 4}})}));
  require(take(partial.dependencies.potential_dirty("image", elsewhere))
              .at("out")
              .empty(),
          "Gaussian dependency isolates frame and layer");
  for (const auto& values : std::vector<std::map<std::string, ParameterValue>>{
           {{"radius", int64_t{0}}, {"sigma", .9}},
           {{"radius", int64_t{65}}, {"sigma", .9}},
           {{"radius", int64_t{2}}, {"sigma", .09}},
           {{"radius", int64_t{2}},
            {"sigma", std::numeric_limits<double>::quiet_NaN()}}}) {
    OperationMetadata metadata;
    metadata.result_schema = std::make_shared<SchemaTemplate>(shape);
    require(
        !d.registry->resolve_traits("image.gaussian_blur", {metadata}, values)
             .ok(),
        "Gaussian parameter bounds");
  }
}
void gaussian_sparse_domain() {
  Driver d;
  auto s = schema();
  s.tensors[0].batch_axes = {UINT64_MAX, 2};
  s.tensors[0].descriptor.shape = {1, 1, 4};
  auto builder = take(ResultBuilder::start(d.root, s, "gaussian.sparse"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(d.root, 1, {})))
              .ok(),
          "sparse Gaussian descriptor");
  const float pixels[] = {.25F, -.5F, 1.F, .5F};
  const Region cell({{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 4}});
  require(builder
              .publish_tensor(
                  0, cell,
                  ByteView(reinterpret_cast<const uint8_t*>(pixels),
                           sizeof(pixels)),
                  take(ResultRelation::cartesian(d.root, UINT64_MAX, {})),
                  {true, true, true, true})
              .ok(),
          "sparse Gaussian source");
  auto prepared =
      d.prepare("image.gaussian_blur", {{"image", take(builder.seal())}},
                {{"radius", int64_t{1}}, {"sigma", .9}});
  auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
  auto query =
      take(Footprint::from_regions(s.tensors[0].sample_shape(), {cell}));
  auto result = take(d.context->execute_fragments(frozen, {{"out", query}}));
  for (uint64_t c = 0; c < 4; ++c)
    require(read(result.results.at("out"), {0, 0, 0, 0, c}) == pixels[c],
            "Gaussian bounded ROI accepts unrepresentable full cardinality");
  auto empty = take(d.context->execute_fragments(
      frozen, {{"out", take(Footprint::none(s.tensors[0].sample_shape()))}}));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Gaussian Empty sparse domain publishes no samples");
  require(take(empty.dependencies.source_observations()).empty(),
          "Gaussian Empty consumes no source payload or descriptor");
}

}  // namespace
int main() {
  try {
    image_gaussian_contract();
    gaussian_sparse_domain();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
