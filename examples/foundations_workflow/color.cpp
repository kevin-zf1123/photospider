#include <array>
#include <iostream>
#include <vector>

#include "workflow.hpp"  // NOLINT(build/include_subdir)

namespace foundations {
namespace {
ps::Value image(const ps::SemanticDescriptor& descriptor,
                const std::vector<float>& pixels) {
  return array(pixels,
               {1, pixels.size() / descriptor.channels.size(),
                descriptor.channels.size()},
               facets(descriptor));
}
}  // namespace
void channels() {
  const auto target = ps::rgba_semantics();
  const auto input = image(target, {-2, 3, 4, .5F});
  auto compose = [&](float factor) {
    std::vector<ps::WorkflowNode> nodes;
    for (std::int64_t c = 0; c < 4; ++c)
      nodes.push_back({static_cast<std::uint64_t>(c + 1),
                       "channel.extract",
                       {ps::WorkflowInputReference{1}},
                       {{"index", c}}});
    nodes.push_back(
        {5,
         "numeric.multiply",
         {ps::WorkflowNodeOutput{1, "value"}, ps::WorkflowInputReference{2}},
         {}});
    nodes.push_back({6,
                     "channel.merge",
                     {ps::WorkflowNodeOutput{5, "value"},
                      ps::WorkflowNodeOutput{2, "value"},
                      ps::WorkflowNodeOutput{3, "value"},
                      ps::WorkflowNodeOutput{4, "value"}},
                     interpretation(target)});
    return output(evaluate({input, array<float>({factor}, {1, 1})}, nodes));
  };
  exact<float>(compose(1), {-2, 3, 4, .5F});
  exact<float>(compose(2), {-4, 3, 4, .5F});
  exact<float>(output(evaluate({input}, {{1,
                                          "channel.swizzle",
                                          {ps::WorkflowInputReference{1}},
                                          indices({2, 1, 0, 3})},
                                         next(2, "channel.swizzle", 1,
                                              indices({2, 1, 0, 3}))})),
               {-2, 3, 4, .5F});
  std::cout << "channels identity=passed red_twice=[-4,3,4,.5] "
               "bgr_roundtrip=passed oracle=passed\n";
}
void alpha_color() {
  auto straight = ps::rgba_semantics();
  straight.association = "straight";
  const std::vector<float> pixels = {-2, 3, 4, .5F, -2, 3, 4, 1e-30F};
  auto converted = output(
      evaluate({image(straight, pixels)},
               {{1, "alpha.associate", {ps::WorkflowInputReference{1}}, {}},
                next(2, "alpha.unassociate", 1)}));
  close(converted, pixels);
  for (unsigned i : {3U, 7U})
    require(std::memcmp(converted.bytes().data() + i * 4, pixels.data() + i,
                        4) == 0,
            "alpha bits changed");
  exact<float>(output(evaluate(
                   {image(straight, {-2, 4, 1, 0})},
                   {{1, "alpha.associate", {ps::WorkflowInputReference{1}}, {}},
                    next(2, "alpha.unassociate", 1)})),
               {0, 0, 0, 0});
  rejected(operation("color.rgb_to_xyz",
                     {image(ps::rgba_semantics(), {1, 2, 3, .5F})}),
           ps::ErrorCode::TypeMismatch);
  auto rgb = straight;
  rgb.association = "none";
  rgb.channels.pop_back();
  const auto color = image(rgb, {-1, .5F, 3});
  close(output(evaluate(
            {color},
            {{1, "color.rgb_to_xyz", {ps::WorkflowInputReference{1}}, {}},
             next(2, "color.xyz_to_lab", 1),
             next(3, "color.lab_to_xyz", 2),
             next(4, "color.xyz_to_rgb", 3)})),
        {-1, .5F, 3});
  auto xyz = take(ps::decode_semantic(
      output(operation("color.rgb_to_xyz", {color})).facets()[0]));
  for (const auto white :
       {ps::rgba_semantics().white, std::array<double, 3>{.96422, 1, .82521}}) {
    xyz.white = white;
    std::vector<float> samples = {static_cast<float>(white[0]),
                                  1,
                                  static_cast<float>(white[2]),
                                  -.1F,
                                  .2F,
                                  2};
    close(output(evaluate(
              {image(xyz, samples)},
              {{1, "color.xyz_to_lab", {ps::WorkflowInputReference{1}}, {}},
               next(2, "color.lab_to_xyz", 1)})),
          samples);
    if (white != ps::rgba_semantics().white)
      rejected(operation("color.xyz_to_rgb", {image(xyz, samples)}),
               ps::ErrorCode::TypeMismatch);
  }
  rejected(operation("color.rgb_to_xyz", {array<float>({1, 2, 3}, {1, 1, 3})}),
           ps::ErrorCode::TypeMismatch);
  auto wrong = rgb;
  wrong.unit = "nits";
  for (auto& c : wrong.channels)
    c.unit = "nits";
  require(!ps::encode_semantic(wrong).ok(), "invalid RGB units accepted");
  auto invalid = ps::rgba_semantics();
  rejected(operation("alpha.unassociate", {image(invalid, {1, 0, 0, 0})}),
           ps::ErrorCode::InvalidArgument);
  std::cout
      << "alpha-color straight_roundtrip=passed hidden_zero_alpha=[0,0,0,0] "
         "tiny_alpha=1e-30 alpha_bits=exact signed_hdr=passed "
         "D65_D50_Lab=passed invalid_semantics=rejected oracle=passed\n";
}
}  // namespace foundations
