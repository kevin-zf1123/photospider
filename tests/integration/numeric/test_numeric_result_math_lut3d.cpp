#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
WorkflowNode lut3d_node(
    bool tetrahedral, const ColorArrayDescriptor& input,
    const ColorArrayDescriptor& output,
    ElementType dtype = ElementType::Float64,
    CpuNumericProfile profile = CpuNumericProfile::Strict,
    numeric::CurveDomain domain = numeric::CurveDomain::Reject) {
  numeric::Lut3dOptions options;
  options.dtype = dtype;
  options.profile = profile;
  options.out_of_domain = domain;
  const auto helper = tetrahedral ? numeric::apply_lut3d_tetrahedral_node
                                  : numeric::apply_lut3d_trilinear_node;
  return take(helper(7, WorkflowInputReference{11}, WorkflowInputReference{12},
                     WorkflowInputReference{13}, ElementType::Float32, input,
                     output, options));
}
void lut3d_workflows() {
  Driver d;
  auto color = lut3d_description();
  auto target = color;
  target.primaries =
      take(color_primary_coordinates(ColorPrimaryPreset::DisplayP3)).primaries;
  target.transfer = ColorTransfer{ColorTransferKind::Srgb, {}};
  const auto source_facet = take(encode_color_array(color));
  const auto target_facet = take(encode_color_array(target));
  const unsigned quarters[12][3] = {{3, 2, 1}, {3, 1, 2}, {2, 3, 1}, {2, 1, 3},
                                    {1, 3, 2}, {1, 2, 3}, {2, 2, 2}, {3, 3, 1},
                                    {0, 0, 0}, {4, 4, 4}, {4, 1, 2}, {0, 2, 4}};
  // Independent integer barycentric weights on stored-index tetrahedra.
  const uint8_t tetra_quarters[8][36] = {
      {2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 2, 1, 2, 1, 1, 2, 1,
       2, 2, 2, 3, 1, 1, 0, 0, 0, 4, 4, 4, 1, 1, 2, 0, 2, 0},
      {1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 0,
       0, 2, 0, 2, 1, 0, 0, 0, 0, 4, 4, 4, 1, 1, 2, 0, 2, 0},
      {1, 0, 1, 0, 0, 2, 1, 0, 1, 0, 0, 2, 0, 1, 1, 0, 1, 1,
       0, 0, 2, 2, 0, 1, 0, 0, 0, 4, 4, 4, 1, 0, 2, 0, 2, 0},
      {2, 0, 0, 1, 0, 1, 2, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0,
       2, 0, 0, 3, 0, 0, 0, 0, 0, 4, 4, 4, 1, 0, 2, 0, 2, 0},
      {2, 0, 0, 1, 0, 1, 2, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0,
       2, 0, 0, 3, 0, 0, 0, 0, 0, 4, 4, 4, 1, 0, 2, 0, 2, 0},
      {1, 0, 1, 0, 0, 2, 1, 0, 1, 0, 0, 2, 0, 1, 1, 0, 1, 1,
       0, 0, 2, 2, 0, 1, 0, 0, 0, 4, 4, 4, 1, 0, 2, 0, 2, 0},
      {1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 0,
       0, 2, 0, 2, 1, 0, 0, 0, 0, 4, 4, 4, 1, 1, 2, 0, 2, 0},
      {2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 2, 1, 2, 1, 1, 2, 1,
       2, 2, 2, 3, 1, 1, 0, 0, 0, 4, 4, 4, 1, 1, 2, 0, 2, 0}};
  std::vector<uint64_t> input_words;
  for (const auto& q : quarters)
    for (unsigned c = 3; c > 0; --c)
      input_words.push_back(float_bits(q[c - 1] * .25F));
  auto input =
      d.source({ElementType::Float32, {6, 3}}, input_words, {9, {72, 12, -4}},
               {source_facet}, {2}, nullptr, "custom.color", 5);
  ResultRef retained;
  for (unsigned direction = 0; direction < 8; ++direction) {
    std::vector<uint64_t> table_words, axis_words;
    for (unsigned r = 0; r < 2; ++r)
      for (unsigned g = 0; g < 2; ++g)
        for (unsigned b = 0; b < 2; ++b) {
          const unsigned x = direction & 1 ? 1 - r : r;
          const unsigned y = direction & 2 ? 1 - g : g;
          const unsigned z = direction & 4 ? 1 - b : b;
          table_words.insert(
              table_words.end(),
              {double_bits(z * x), double_bits(y * z), double_bits(x * y)});
        }
    for (unsigned axis = 0; axis < 3; ++axis) {
      const bool reverse = direction & (1U << axis);
      axis_words.insert(axis_words.end(), {double_bits(reverse ? 1 : 0),
                                           double_bits(reverse ? 0 : 1),
                                           double_bits(reverse ? -1 : 1)});
    }
    auto table = d.source({ElementType::Float64, {2, 2, 3}}, table_words,
                          {17, {96, 48, 24, -8}}, {target_facet}, {2});
    auto axes =
        d.source({ElementType::Float64, {3, 3}}, axis_words, {1, {24, 8}});
    for (bool tetra : {false, true})
      for (auto dtype : {ElementType::Float64, ElementType::Float32})
        for (auto profile :
             {CpuNumericProfile::Strict, CpuNumericProfile::AppleSiliconNeon,
              CpuNumericProfile::X86Avx2}) {
          auto node = lut3d_node(tetra, color, target, dtype, profile);
          std::vector<OperationMetadata> metadata;
          for (const auto& source : {input, table, axes}) {
            OperationMetadata item;
            item.result_schema =
                std::make_shared<SchemaTemplate>(source.schema());
            metadata.push_back(std::move(item));
          }
          auto available = d.registry->resolve_traits(node.operation, metadata,
                                                      node.parameters);
          if (!available.ok()) {
            require(
                profile != CpuNumericProfile::Strict &&
                    available.status().code == ErrorCode::BackendUnavailable,
                "unavailable LUT3D profile remains explicit");
            continue;
          }
          auto query = take(Footprint::from_regions(
              {2, 6, 3}, {Region({{0, 1}, {1, 1}, {0, 1}})}));
          auto result = take(d.run(node.operation, {input, table, axes}, query,
                                   node.parameters));
          retained = result.results.at("out");
          require(retained.schema().id == "photospider.tensor" &&
                      retained.schema().tensors[0].key == "samples" &&
                      retained.schema().tensors[0].atomic_trailing_axes == 1 &&
                      retained.schema().tensors[0].facets.size() == 1 &&
                      retained.schema().tensors[0].facets[0].payload ==
                          target_facet.payload &&
                      retained.association().size() == 3,
                  "LUT3D publishes target ColorArray v1 and tuple closure "
                  "without implicit color conversion");
          for (unsigned row = 0; row < 12; ++row)
            for (unsigned c = 0; c < 3; ++c) {
              const double expected =
                  tetra ? tetra_quarters[direction][3 * row + c] * .25
                        : quarters[row][c] * quarters[row][(c + 1) % 3] * .0625;
              require(
                  read_bits(retained, {row / 6, row % 6, c}) ==
                      (dtype == ElementType::Float64 ? double_bits(expected)
                                                     : float_bits(expected)),
                  "LUT3D all six tetrahedra/ties/directions and full typed "
                  "batch traversal match independent dyadic weights");
            }
          const auto closed =
              take(retained.schema().tensors[0].close_samples(query));
          for (unsigned port = 0; port < 3; ++port)
            for (uint32_t role : {1U, 4U})
              require(
                  take(result.dependencies.potential_dirty(
                           "input" + std::to_string(port),
                           take(Footprint::all(metadata[port]
                                                   .result_schema->tensors[0]
                                                   .sample_shape())),
                           role))
                          .at("out") == closed,
                  "LUT3D whole data/validation dirty closes the recorded "
                  "component demand to complete colors");
        }
  }
  const double table_values[24] = {-.8, -.5, .2, .7,  -.3,  .1,  -.1, .4,
                                   -.6, .9,  .8, -.9, .3,   -.7, .6,  -.4,
                                   1.1, -.2, .5, .2,  -1.3, 1.7, -.9, .4};
  const uint64_t golden64[2][12] = {
      {0x3fe2c710cb295e9e, 0x3fa6ae7d566cf423, 0xbfc563886594af4f,
       0x3fbeecbfb15b573f, 0xbfc67d566cf41f20, 0x3fcd4c985f06f694,
       0x3fc07c84b5dcc63f, 0x3fd025aee631f8a1, 0xbfe58a0902de00d2,
       0xbfa0ff9724745396, 0xbfc36ae7d566cf42, 0xbfad3c36113404e9},
      {0x3fe6147ae147ae15, 0xbfc47ae147ae147b, 0xbfaeb851eb851eb6,
       0x3fc851eb851eb853, 0xbfd5c28f5c28f5c2, 0x3fd851eb851eb852,
       0x3fc0a3d70a3d70a4, 0x3fc1eb851eb851ed, 0xbfe1eb851eb851eb,
       0xbfa99999999999a4, 0xbfe3d70a3d70a3d7, 0x3fd0a3d70a3d70a4}};
  const uint64_t golden32[2][12] = {
      {0x3f163886, 0x3d3573eb, 0xbe2b1c43, 0x3df765fe, 0xbe33eab3, 0x3e6a64c3,
       0x3e03e426, 0x3e812d77, 0xbf2c5048, 0xbd07fcb9, 0xbe1b573f, 0xbd69e1b1},
      {0x3f30a3d7, 0xbe23d70a, 0xbd75c28f, 0x3e428f5c, 0xbeae147b, 0x3ec28f5c,
       0x3e051eb8, 0x3e0f5c29, 0xbf0f5c29, 0xbd4ccccd, 0xbf1eb852, 0x3e851eb8}};
  std::vector<uint64_t> table_words, queries;
  for (double v : table_values)
    table_words.push_back(double_bits(v));
  for (double v : {.1, .3, .9, .9, .1, .3, .3, .9, .1, .3, .3, .3})
    queries.push_back(double_bits(v));
  auto query = d.source({ElementType::Float64, {4, 3}}, queries, {1, {24, 8}});
  auto table = d.source({ElementType::Float64, {2, 2, 2, 3}}, table_words,
                        {1, {96, 48, 24, 8}});
  auto axes = d.source({ElementType::Float64, {3, 3}},
                       {0, double_bits(1), double_bits(1), 0, double_bits(1),
                        double_bits(1), 0, double_bits(1), double_bits(1)},
                       {1, {24, 8}});
  for (bool tetra : {false, true})
    for (auto dtype : {ElementType::Float64, ElementType::Float32}) {
      auto node = lut3d_node(tetra, color, target, dtype);
      std::fenv_t environment;
      require(std::fegetenv(&environment) == 0, "save LUT3D fenv");
      std::fesetround(FE_UPWARD);
      std::feclearexcept(FE_ALL_EXCEPT);
      std::feraiseexcept(FE_DIVBYZERO);
      auto result =
          d.run(node.operation, {query, table, axes}, {}, node.parameters);
      const bool preserved = std::fegetround() == FE_UPWARD &&
                             std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
      std::fesetenv(&environment);
      require(preserved, "LUT3D preserves caller floating environment");
      auto output = take(std::move(result)).results.at("out");
      for (unsigned i = 0; i < 12; ++i)
        require(read_bits(output, {i / 3, i % 3}) ==
                    (dtype == ElementType::Float64 ? golden64[tetra][i]
                                                   : golden32[tetra][i]),
                "LUT3D independent Fraction weight and whole-sum oracle");
    }
  for (auto model : {ColorModel::Rgb, ColorModel::Xyz, ColorModel::Cielab,
                     ColorModel::Oklab, ColorModel::Cielch, ColorModel::Oklch,
                     ColorModel::Hsl, ColorModel::Ycbcr}) {
    auto description = lut3d_description(model);
    auto point = d.source({ElementType::Float32, {1, 3}}, {float_bits(.5F)},
                          {1, {0, 0}});
    auto ones = d.source({ElementType::Float32, {2, 2, 2, 3}}, {float_bits(1)},
                         {1, {0, 0, 0, 0}});
    for (bool tetra : {false, true}) {
      auto node = lut3d_node(tetra, description, description);
      auto output =
          take(d.run(node.operation, {point, ones, axes}, {}, node.parameters))
              .results.at("out");
      require(read_bits(output, {0, 0}) == double_bits(1) &&
                  read_bits(output, {0, 1}) == double_bits(1) &&
                  read_bits(output, {0, 2}) == double_bits(1) &&
                  take(decode_color_array(output.schema().tensors[0].facets[0]))
                          .model == model,
              "LUT3D all eight current ColorArray v1 models remain explicit");
    }
  }
  for (auto model : {ColorModel::Cielch, ColorModel::Oklch, ColorModel::Hsl}) {
    auto description = lut3d_description(model);
    auto point = d.source({ElementType::Float64, {1, 3}}, {double_bits(.5)},
                          {1, {0, 0}});
    const auto hue = model == ColorModel::Hsl ? 0U : 2U;
    std::vector<uint64_t> color_words(3, double_bits(1));
    color_words[hue] = double_bits(4);
    auto colors = d.source({ElementType::Float64, {2, 2, 2, 3}}, color_words,
                           {1, {0, 0, 0, 8}});
    std::vector<uint64_t> winding_words;
    for (unsigned r = 0; r < 2; ++r)
      for (unsigned g = 0; g < 2; ++g)
        for (unsigned b = 0; b < 2; ++b) {
          auto color = color_words;
          color[hue] = double_bits(4.0 * r);
          winding_words.insert(winding_words.end(), color.begin(), color.end());
        }
    auto winding = d.source({ElementType::Float64, {2, 2, 2, 3}}, winding_words,
                            {1, {96, 48, 24, 8}});
    auto winding_queries =
        d.source({ElementType::Float64, {3, 3}},
                 {double_bits(0), double_bits(0), double_bits(0),
                  double_bits(.5), double_bits(.5), double_bits(.5),
                  double_bits(1), double_bits(1), double_bits(1)},
                 {1, {24, 8}});
    for (bool tetra : {false, true}) {
      auto node = lut3d_node(tetra, description, description);
      auto output = take(d.run(node.operation, {point, colors, axes}, {},
                               node.parameters))
                        .results.at("out");
      require(
          read_bits(output, {0, hue}) == double_bits(4) &&
              take(decode_color_array(output.schema().tensors[0].facets[0]))
                      .hue == ColorHueUnit::PiMultiple,
          "LUT3D polar/HSL hue retains unnormalized winding in explicit units");
      auto interpolated =
          take(d.run(node.operation, {winding_queries, winding, axes}, {},
                     node.parameters))
              .results.at("out");
      for (unsigned row = 0; row < 3; ++row)
        require(read_bits(interpolated, {row, hue}) == double_bits(2.0 * row),
                "LUT3D hue 0-to-4 interpolation retains midpoint 2 and both "
                "endpoints without normalization");
    }
  }
  d.context.reset();
  require(read_bits(retained, {0, 1, 0}) == float_bits(.25F),
          "LUT3D typed output and backing survive context retirement");
}
void lut3d_boundaries() {
  Driver d;
  const auto color = lut3d_description();
  auto axes = d.source({ElementType::Float64, {3, 3}},
                       {0, double_bits(1), double_bits(1), 0, double_bits(1),
                        double_bits(1), 0, double_bits(1), double_bits(1)},
                       {1, {24, 8}});
  auto center =
      d.source({ElementType::Float64, {1, 3}}, {double_bits(.5)}, {1, {0, 0}});
  auto origin = d.source({ElementType::Float32, {1, 3}}, {0}, {1, {0, 0}});
  std::vector<uint64_t> diagonal(24, 0x7ff8000000000001);
  for (unsigned c = 0; c < 3; ++c) {
    diagonal[c] = 0;
    diagonal[21 + c] = double_bits(1);
  }
  auto generic = d.source({ElementType::Float64, {2, 2, 2, 3}}, diagonal,
                          {1, {96, 48, 24, 8}});
  auto tetra = lut3d_node(true, color, color);
  auto output = take(d.run(tetra.operation, {center, generic, axes}, {},
                           tetra.parameters))
                    .results.at("out");
  require(read_bits(output, {0, 0}) == double_bits(.5) &&
              read_bits(output, {0, 2}) == double_bits(.5),
          "tetrahedral diagonal skips zero-weight generic NaN vertices");
  auto tri = lut3d_node(false, color, color);
  auto failed =
      d.run(tri.operation, {center, generic, axes}, {}, tri.parameters);
  require(
      !failed.ok() && failed.status().code == ErrorCode::OperationFailed &&
          failed.status().message.find("port=1") != std::string::npos &&
          failed.status().detail.scope == FailureScope::Run &&
          !failed.status().detail.atom,
      "trilinear center uses all eight vertices and fails the complete Run");
  auto typed =
      d.source({ElementType::Float64, {2, 2, 2, 3}}, diagonal,
               {1, {96, 48, 24, 8}}, {take(encode_color_array(color))});
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  failed = d.run(tetra.operation, {center, typed, axes}, {}, tetra.parameters);
  require(!failed.ok() && failed.status().code == ErrorCode::InvalidArgument &&
              failed.status().detail.input_id == 12 &&
              d.root.statistics().live[ResourceKind::Payload] == payload,
          "typed LUT3D validates unused vertices and preserves source id");
  auto empty = take(d.run(tetra.operation, {center, typed, axes},
                          take(Footprint::none({1, 3})), tetra.parameters));
  require(take(empty.results.at("out").descriptor()).tensor_coverage(0).empty(),
          "Empty LUT3D skips typed table payload");
  auto queries = d.source(
      {ElementType::Float64, {2, 3}},
      {double_bits(.5), double_bits(.5), double_bits(.5), double_bits(2), 0, 0},
      {1, {24, 8}});
  failed =
      d.run(tri.operation, {queries, generic, axes},
            take(Footprint::from_regions({2, 3}, {Region({{0, 1}, {0, 1}})})),
            tri.parameters);
  require(
      !failed.ok() && failed.status().message.find("query outside axis=0") !=
                          std::string::npos,
      "LUT3D validates every original query before table arithmetic outside Q");
  auto bad_axes =
      d.source({ElementType::Float64, {3, 3}},
               {0, double_bits(1), double_bits(.5), 0, double_bits(1),
                double_bits(1), 0, double_bits(1), double_bits(1)},
               {1, {24, 8}});
  failed =
      d.run(tri.operation, {queries, generic, bad_axes}, {}, tri.parameters);
  require(!failed.ok() &&
              failed.status().message.find("inconsistent") != std::string::npos,
          "LUT3D global axis validation precedes query/domain errors");
  auto lch = lut3d_description(ColorModel::Cielch);
  auto clamped =
      lut3d_node(true, lch, lch, ElementType::Float64,
                 CpuNumericProfile::Strict, numeric::CurveDomain::Clamp);
  auto negative_chroma = d.source(
      {ElementType::Float64, {1, 3}},
      {double_bits(.5), double_bits(-1), double_bits(.5)}, {1, {24, 8}});
  auto ones = d.source({ElementType::Float32, {2, 2, 2, 3}}, {float_bits(1)},
                       {1, {0, 0, 0, 0}});
  failed = d.run(clamped.operation, {negative_chroma, ones, axes}, {},
                 clamped.parameters);
  require(!failed.ok() &&
              failed.status().message.find("negative LUT3D chroma; port=0") !=
                  std::string::npos,
          "LUT3D validates original chroma before a clamp can hide it");
  auto outside = d.source({ElementType::Float64, {1, 3}},
                          {double_bits(-1), double_bits(2), double_bits(.5)},
                          {1, {24, 8}});
  auto rgb_clamp =
      lut3d_node(false, color, color, ElementType::Float64,
                 CpuNumericProfile::Strict, numeric::CurveDomain::Clamp);
  output = take(d.run(rgb_clamp.operation, {outside, ones, axes}, {},
                      rgb_clamp.parameters))
               .results.at("out");
  require(read_bits(output, {0, 0}) == double_bits(1),
          "LUT3D component clamp remains explicit for finite HDR queries");
  auto other = color;
  other.primaries =
      take(color_primary_coordinates(ColorPrimaryPreset::DisplayP3)).primaries;
  auto mismatched = d.source({ElementType::Float64, {1, 3}}, {0}, {1, {0, 0}},
                             {take(encode_color_array(other))});
  std::vector<OperationMetadata> metadata;
  for (const auto& source : {mismatched, ones, axes}) {
    OperationMetadata item;
    item.result_schema = std::make_shared<SchemaTemplate>(source.schema());
    metadata.push_back(std::move(item));
  }
  auto rejected =
      d.registry->resolve_traits(tri.operation, metadata, tri.parameters);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "LUT3D attached input description mismatch is a preflight failure");
  metadata[0].result_schema = std::make_shared<SchemaTemplate>(origin.schema());
  auto wrong_table = ones.schema();
  wrong_table.tensors[0].facets = {take(encode_color_array(other))};
  metadata[1].result_schema = std::make_shared<SchemaTemplate>(wrong_table);
  rejected =
      d.registry->resolve_traits(tri.operation, metadata, tri.parameters);
  require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
          "LUT3D attached table description must match output description");
  auto negative_zero = d.source({ElementType::Float32, {2, 2, 2, 3}},
                                {0x80000000}, {1, {0, 0, 0, 0}});
  for (bool tetrahedral : {false, true}) {
    auto node = lut3d_node(tetrahedral, color, color, ElementType::Float32);
    output = take(d.run(node.operation, {center, negative_zero, axes}, {},
                        node.parameters))
                 .results.at("out");
    require(read_bits(output, {0, 0}) == 0x80000000 &&
                read_bits(output, {0, 2}) == 0x80000000,
            "LUT3D preserves all-negative-zero positive-weight mixtures");
    std::vector<uint64_t> cancellation(24);
    for (unsigned i = 0; i < 24; ++i)
      cancellation[i] =
          i < 12 ? UINT64_C(0x7fefffffffffffff) : UINT64_C(0xffefffffffffffff);
    auto wide = d.source({ElementType::Float64, {2, 2, 2, 3}}, cancellation,
                         {1, {96, 48, 24, 8}});
    output =
        take(d.run(node.operation, {center, wide, axes}, {}, node.parameters))
            .results.at("out");
    require(read_bits(output, {0, 0}) == 0 && read_bits(output, {0, 2}) == 0,
            "LUT3D exact large opposite vertex sums cancel before narrowing");
    failed = d.run(node.operation, {origin, wide, axes}, {}, node.parameters);
    require(!failed.ok() &&
                failed.status().reason == FailureReason::ArithmeticOverflow,
            "LUT3D selected-vertex narrowing overflow remains explicit");
    std::vector<uint64_t> tiny(24, 0);
    for (unsigned c = 0; c < 3; ++c)
      tiny[21 + c] = 0x80000001;
    auto subnormal = d.source({ElementType::Float32, {2, 2, 2, 3}}, tiny,
                              {1, {48, 24, 12, 4}});
    auto quarter = d.source({ElementType::Float64, {1, 3}}, {double_bits(.25)},
                            {1, {0, 0}});
    output = take(d.run(node.operation, {quarter, subnormal, axes}, {},
                        node.parameters))
                 .results.at("out");
    require(read_bits(output, {0, 0}) == 0x80000000,
            "LUT3D nonzero negative underflow preserves sign");
  }
  std::vector<uint64_t> affine;
  for (unsigned r = 0; r < 2; ++r)
    for (unsigned g = 0; g < 3; ++g)
      for (unsigned b = 0; b < 5; ++b) {
        const double x = r, y = g * .5, z = b * .25;
        affine.insert(affine.end(), {double_bits(x + 2 * y - 3 * z),
                                     double_bits(2 * x - y + .25 * z),
                                     double_bits(1 + x - y + z)});
      }
  auto unequal = d.source({ElementType::Float64, {2, 3, 5, 3}}, affine,
                          {1, {360, 120, 24, 8}});
  auto unequal_axes =
      d.source({ElementType::Float64, {3, 3}},
               {0, double_bits(1), double_bits(1), 0, double_bits(1),
                double_bits(.5), 0, double_bits(1), double_bits(.25)},
               {1, {24, 8}});
  auto point = d.source(
      {ElementType::Float32, {1, 3}},
      {float_bits(.25F), float_bits(.375F), float_bits(.625F)}, {1, {12, 4}});
  for (bool tetrahedral : {false, true}) {
    auto node = lut3d_node(tetrahedral, color, color);
    output = take(d.run(node.operation, {point, unequal, unequal_axes}, {},
                        node.parameters))
                 .results.at("out");
    require(read_bits(output, {0, 0}) == double_bits(-.875) &&
                read_bits(output, {0, 1}) == double_bits(.28125) &&
                read_bits(output, {0, 2}) == double_bits(1.5),
            "LUT3D unequal [2,3,5] axes preserve affine mapping without "
            "conversion");
  }
}
void lut3d_preparation() {
  Driver d;
  const auto original = d.registry;
  auto count = std::make_shared<unsigned>(0);
  const std::string key = "curve.apply_lut3d_trilinear_strict";
  d.registry = std::make_shared<OperationRegistry>();
  require(d.registry->register_operation(observed_numeric(original, key, count))
              .ok(),
          "LUT3D preparation observer registration");
  require(d.registry->freeze().ok(), "LUT3D preparation observer freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  d.context = std::make_unique<ExecutionContext>(d.registry, config);
  d.root = take(d.context->resource_budget());
  const auto color = lut3d_description();
  auto node = lut3d_node(false, color, color);
  auto input =
      d.source({ElementType::Float64, {1, 3}}, {double_bits(.5)}, {1, {0, 0}});
  std::vector<uint64_t> identity;
  for (unsigned r = 0; r < 2; ++r)
    for (unsigned g = 0; g < 2; ++g)
      for (unsigned b = 0; b < 2; ++b)
        identity.insert(identity.end(),
                        {double_bits(r), double_bits(g), double_bits(b)});
  auto table = d.source({ElementType::Float64, {2, 2, 2, 3}}, identity,
                        {1, {96, 48, 24, 8}});
  auto axes = d.source({ElementType::Float64, {3, 3}},
                       {0, double_bits(1), double_bits(1)}, {1, {0, 8}});
  auto prepared =
      d.prepare(node.operation, {input, table, axes}, node.parameters);
  const auto calls = *count;
  const auto owner = prepared.plan.steps()[0].prepared;
  require(calls && owner->state(), "LUT3D compilation owns static preparation");
  auto first = take(d.context->execute_fragments(
      take(d.context->freeze(prepared.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1, 3}))}}));
  require(*count == calls &&
              read_bits(first.results.at("out"), {0, 1}) == double_bits(.5),
          "LUT3D compiled start uses the owning preparation without reparsing");
  auto changed_input = d.source(
      {ElementType::Float64, {1, 3}},
      {double_bits(.25), double_bits(.75), double_bits(.5)}, {1, {24, 8}});
  for (auto& bits : identity) {
    double v;
    std::memcpy(&v, &bits, 8);
    bits = double_bits(v * 2);
  }
  auto changed_table = d.source({ElementType::Float64, {2, 2, 2, 3}}, identity,
                                {1, {96, 48, 24, 8}});
  prepared.bindings.inputs[0].result = changed_input;
  prepared.bindings.inputs[1].result = changed_table;
  auto second = take(d.context->execute_fragments(
      take(d.context->freeze(prepared.plan, prepared.bindings)),
      {{"out", take(Footprint::all({1, 3}))}}));
  auto retained = second.results.at("out");
  require(*count == calls && prepared.plan.steps()[0].prepared == owner &&
              read_bits(retained, {0, 0}) == double_bits(.5) &&
              read_bits(retained, {0, 1}) == double_bits(1.5) &&
              read_bits(retained, {0, 2}) == double_bits(1) &&
              retained.association() ==
                  ResourceVector<uint64_t>{changed_input.object_id(),
                                           changed_table.object_id(),
                                           axes.object_id()},
          "LUT3D same-plan rebinding reuses preparation and observes new "
          "Result owners");
  d.context.reset();
  require(read_bits(retained, {0, 1}) == double_bits(1.5),
          "LUT3D rebinding output survives execution retirement");
}
void lut3d_resources() {
  for (bool tetra : {false, true}) {
    Driver d;
    const auto original = d.registry;
    auto stop = std::make_shared<CancellationSource>();
    auto triggered = std::make_shared<bool>(false);
    auto count = std::make_shared<unsigned>(0);
    const auto color = lut3d_description();
    const auto node = lut3d_node(tetra, color, color);
    d.registry = std::make_shared<OperationRegistry>();
    require(d.registry
                ->register_operation(observed_numeric(original, node.operation,
                                                      count, stop, triggered))
                .ok(),
            "LUT3D cancellation observer registration");
    require(d.registry->freeze().ok(), "LUT3D cancellation observer freeze");
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    d.context = std::make_unique<ExecutionContext>(d.registry, config);
    d.root = take(d.context->resource_budget());
    auto point = d.source({ElementType::Float64, {1, 3}}, {double_bits(.5)},
                          {1, {0, 0}});
    auto table = d.source({ElementType::Float32, {2, 2, 2, 3}}, {float_bits(1)},
                          {1, {0, 0, 0, 0}});
    auto axes = d.source({ElementType::Float64, {3, 3}},
                         {0, double_bits(1), double_bits(1)}, {1, {0, 8}});
    const auto baseline = d.root.statistics().live;
    auto failed = d.run(node.operation, {point, table, axes}, {},
                        node.parameters, stop->token());
    require(*triggered && !failed.ok() &&
                failed.status().code == ErrorCode::Cancelled &&
                d.root.statistics().live[ResourceKind::Payload] ==
                    baseline[ResourceKind::Payload],
            "LUT3D cancellation after entering exact 640-limb work rolls back "
            "output");
    d.context.reset();
    require(d.root.statistics().live.values == baseline.values,
            "LUT3D cancelled continuation releases all Root resources");
    Driver limited(30000);
    point = limited.source({ElementType::Float64, {1, 3}}, {double_bits(.5)},
                           {1, {0, 0}});
    table = limited.source({ElementType::Float32, {2, 2, 2, 3}},
                           {float_bits(1)}, {1, {0, 0, 0, 0}});
    axes = limited.source({ElementType::Float64, {3, 3}},
                          {0, double_bits(1), double_bits(1)}, {1, {0, 8}});
    const auto before = limited.root.statistics().live;
    failed =
        limited.run(node.operation, {point, table, axes}, {}, node.parameters);
    require(!failed.ok() &&
                failed.status().reason == FailureReason::WorkLimit &&
                limited.root.statistics().live[ResourceKind::Payload] ==
                    before[ResourceKind::Payload],
            "LUT3D WorkLimit discards unpublished output");
    limited.context.reset();
    require(limited.root.statistics().live.values == before.values,
            "LUT3D WorkLimit retires all execution resources");
  }
  Driver d;
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Payload] = 1048576;
  d.context = std::make_unique<ExecutionContext>(d.registry, config);
  d.root = take(d.context->resource_budget());
  auto point =
      d.source({ElementType::Float64, {1, 3}}, {double_bits(.5)}, {1, {0, 0}});
  auto table = d.source({ElementType::Float32, {65, 65, 65, 3}},
                        {float_bits(7)}, {1, {0, 0, 0, 0}});
  auto axes = d.source({ElementType::Float64, {3, 3}},
                       {0, double_bits(1), double_bits(0x1p-6)}, {1, {0, 8}});
  auto node = lut3d_node(false, lut3d_description(), lut3d_description());
  const auto payload = d.root.statistics().live[ResourceKind::Payload];
  auto output =
      take(d.run(node.operation, {point, table, axes}, {}, node.parameters))
          .results.at("out");
  require(read_bits(output, {0, 0}) == double_bits(7) &&
              d.root.statistics().live[ResourceKind::Payload] == payload + 24,
          "LUT3D authorized zero-stride table avoids a >3 MiB packed copy "
          "under 1 MiB payload cap");
  auto many = d.source({ElementType::Float64, {65536, 3}}, {double_bits(.5)},
                       {1, {0, 0}});
  const auto baseline = d.root.statistics().live;
  auto failed = d.run(
      node.operation, {many, table, axes},
      take(Footprint::from_regions({65536, 3}, {Region({{0, 1}, {0, 1}})})),
      node.parameters);
  require(!failed.ok() &&
              failed.status().code == ErrorCode::ResourceExhausted &&
              d.root.statistics().live[ResourceKind::Payload] ==
                  baseline[ResourceKind::Payload],
          "one-component LUT3D request still requires complete 1.5 MiB output");
}
}  // namespace
int main() {
  try {
    lut3d_workflows();
    lut3d_boundaries();
    lut3d_preparation();
    lut3d_resources();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
