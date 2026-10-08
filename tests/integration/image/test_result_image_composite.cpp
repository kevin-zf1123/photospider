#include <iostream>
#include <utility>
#include <vector>

#include "support/result_image_fixture.hpp"

namespace {
using namespace ps::test_image;  // NOLINT(build/namespaces)
void mix_cancellation_residual() {
  Driver driver;
  auto make = [&](float sample, bool mask) {
    auto shape = schema(mask);
    shape.tensors[0].batch_axes[0] = shape.tensors[0].batch_axes[1] = 1;
    shape.tensors[0].descriptor.shape =
        mask ? std::vector<std::uint64_t>{1, 1}
             : std::vector<std::uint64_t>{1, 1, 4};
    auto builder = take(ResultBuilder::start(driver.root, shape, "mix.source"));
    require(builder
                .bind_descriptor_relation(take(
                    ResultRelation::cartesian(driver.root, 1, {0, 1, 0, 0})))
                .ok(),
            "mix source basis");
    const std::vector<float> samples =
        mask ? std::vector<float>{sample} : std::vector<float>{sample, 0, 0, 1};
    require(
        builder
            .publish_tensor(
                0, Region::whole(shape.tensors[0].sample_shape()),
                ByteView(reinterpret_cast<const std::uint8_t*>(samples.data()),
                         samples.size() * sizeof(float)),
                take(ResultRelation::cartesian(driver.root, samples.size(),
                                               {0, 1, 0, 0})),
                {true, true, true, true})
            .ok(),
        "mix source samples");
    ExecutionBinding binding;
    binding.name = mask ? "mask" : sample > 0 ? "a" : "b";
    binding.result = take(builder.seal());
    return binding;
  };
  const auto output =
      driver.run("image.mix", {make(std::ldexp(1.F, -30), false),
                               make(-std::ldexp(1.F, 70), false),
                               make(std::ldexp(1.F, -100), true)});
  const float sample = read(output, {0, 0, 0, 0, 0});
  std::uint32_t bits = 0;
  std::memcpy(&bits, &sample, sizeof(bits));
  require(bits == 0x80080000U,
          "mix preserves compensated cancellation residual");
}
void brush_oracle() {
  with_spatial_oracle(false, [&](Driver& d, const SchemaTemplate&,
                                 const ExecutionBinding& input,
                                 const std::vector<float>& pixels,
                                 const auto& verify) {
    constexpr std::uint64_t height = 5, width = 7;
    const std::vector<float> controls{2.5F, 1.5F, 1, .8F, .2F, .1F, .5F};
    std::vector<ExecutionBinding> bindings{input};
    for (float value : controls)
      bindings.push_back(
          {"p" + std::to_string(bindings.size()), scalar(d.root, value)});
    auto expected = pixels;
    for (std::uint64_t y = 0; y < height; ++y)
      for (std::uint64_t x = 0; x < width; ++x) {
        if (std::hypot(static_cast<double>(x) + .5 - controls[0],
                       static_cast<double>(y) + .5 - controls[1]) > controls[2])
          continue;
        for (unsigned c = 0; c < 4; ++c)
          expected[(y * width + x) * 4 + c] =
              (c == 3 ? controls[6] : controls[3 + c] * controls[6]) +
              pixels[(y * width + x) * 4 + c] * (1 - controls[6]);
      }
    auto prepared = d.prepare("image.brush_circle", bindings);
    verify(take(d.context->execute(prepared.plan, prepared.bindings))
               .results.at("out"),
           Region::whole({1, 1, height, width, 4}), expected, width);
    const Region roi({{0, 1}, {0, 1}, {0, 3}, {1, 3}, {0, 4}});
    auto tile = take(prepared.plan.tile_plan("out", roi));
    verify(take(d.context->execute(tile, prepared.bindings)).results.at("out"),
           roi, expected, width);
    require(
        !prepared.plan
             .tile_plan("out", Region({{0, 1}, {0, 1}, {0, 6}, {0, 7}, {0, 4}}))
             .ok(),
        "out of bounds tile rejected");
    require(!prepared.plan.tile_plan("out", Region({{0, 1}})).ok(),
            "tile rank rejected");
  });
}
void run() {
  Driver d;
  ExecutionBinding a, b, mask, control;
  a.name = "a";
  a.result = image(d.root);
  b.name = "b";
  b.result = image(d.root, false, 2);
  mask.name = "mask";
  mask.result = image(d.root, true);
  control.name = "control";
  control.result = scalar(d.root, 2);
  const std::vector<std::uint64_t> at{1, 1, 2, 4, 0};
  const auto x = read(a.result, at);
  require(read(d.run("image.exposure_gain", {a, control}), at) == 2 * x,
          "exposure across frames and layers");
  control.result = scalar(d.root, .25F);
  require(read(d.run("image.opacity", {a, control}), at) == x * .25F,
          "opacity");
  require(read(d.run("image.mask", {a, mask}), at) == x * .25F, "mask");
  require(read(d.run("image.source_over", {a, b}), at) == x + (x + 2) * .5F,
          "source-over");
  require(read(d.run("image.mix", {a, b, mask}), at) == x + .5F, "mix");
  auto reduced =
      d.run("image.downsample_box", {a}, {{"factor", std::int64_t{2}}});
  require(reduced.schema().tensors[0].descriptor.shape ==
              std::vector<std::uint64_t>({2, 3, 4}),
          "ceil downsample shape");
  require(read(reduced, {1, 1, 1, 2, 0}) == x, "clipped downsample edge");
  require(
      read(d.run("mask.downsample_box", {mask}, {{"factor", std::int64_t{2}}}),
           {1, 1, 1, 2}) == .25F,
      "mask downsample");
  require(read(d.run("image.split_horizontal", {a},
                     {{"split_x", std::int64_t{2}}}, "right"),
               {1, 1, 2, 2, 0}) == x,
          "selected right crop");
  std::vector<ExecutionBinding> brush{a};
  for (float number : {4.5F, 2.5F, .5F, 1.F, 0.F, 0.F, .25F}) {
    ExecutionBinding input;
    input.name = "p" + std::to_string(brush.size());
    input.result = scalar(d.root, number);
    brush.push_back(std::move(input));
  }
  require(read(d.run("image.brush_circle", brush), at) == .25F + .75F * x,
          "brush coverage");
  auto prepared = d.prepare("image.mask", {a, mask});
  auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
  auto wanted = take(Footprint::from_regions(
      schema().tensors[0].sample_shape(),
      {Region({{1, 1}, {0, 1}, {1, 1}, {1, 1}, {0, 4}}),
       Region({{0, 1}, {1, 1}, {2, 1}, {4, 1}, {0, 4}})}));
  auto result = take(d.context->execute_fragments(frozen, {{"out", wanted}}));
  auto out = result.results.at("out");
  require(read(out, {1, 0, 1, 1, 0}) == read(a.result, {1, 0, 1, 1, 0}) * .25F,
          "nonzero sparse ROI");
  float missing = 0;
  require(
      !out.read_tensor(take(out.descriptor()), 0, {1, 0, 1, 2, 0}, &missing, 4)
           .ok(),
      "sparse holes remain unauthorized");
  auto changed =
      take(Footprint::from_regions(schema(true).tensors[0].sample_shape(),
                                   {Region({{1, 1}, {0, 1}, {1, 1}, {1, 1}})}));
  auto dirty = take(result.dependencies.potential_dirty("mask", changed));
  require(take(dirty.at("out").element_count()) == 4,
          "mask support maps to exactly one color");
  auto empty = take(d.context->execute_fragments(
      frozen, {{"out", take(Footprint::none(wanted.shape()))}}));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty remains empty");
}
}  // namespace
int main() {
  try {
    mix_cancellation_residual();
    brush_oracle();
    run();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
