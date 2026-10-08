#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../support/transfer_result_fixture.hpp"
#include "numeric_workflow/icc_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using transfer_fixture::require;
using transfer_fixture::take;
using Params = std::map<std::string, ParameterValue>;
Params raw(std::uint32_t axis = 2) {
  return {{"metadata_mode", std::string("raw")},
          {"axis", static_cast<std::int64_t>(axis)},
          {"components", std::string("0,1,2")}};
}
void fill(channel_fixture::Source* source, const std::vector<double>& values) {
  for (std::size_t i = 0; i < source->bytes.size() / 8; ++i)
    std::memcpy(source->bytes.data() + 8 * i, &values[i % values.size()], 8);
}
Result<DemandResult> observe(const channel_fixture::Source& source,
                             const std::string& key, const Params& parameters,
                             const Footprint& query,
                             std::shared_ptr<transfer_fixture::Hooks> hooks) {
  auto registry = transfer_fixture::registry(key, std::move(hooks));
  ExecutionContext context(registry);
  auto value =
      channel_fixture::publish(take(context.resource_budget()), source);
  GraphContext graph(transfer_fixture::document(value, key, parameters));
  auto compiled = Compiler(registry).compile(graph, {}, source.resources);
  if (!compiled.ok())
    return Result<DemandResult>(compiled.status());
  auto frozen = take(
      context.freeze(compiled.value().plan, transfer_fixture::bindings(value)));
  ExecutionOptions options;
  options.maximum_dependency_work = options.dependencies.maximum_work =
      UINT64_MAX;
  return context.execute_fragments(frozen, {{"result", query}}, {}, options);
}
void roles_and_constants() {
  const std::array<const char*, 16> names{
      "xyz_to_cielab",    "cielab_to_xyz",    "cielab_to_cielch",
      "cielch_to_cielab", "xyz_to_oklab",     "oklab_to_xyz",
      "oklab_to_oklch",   "oklch_to_oklab",   "rgb_to_hsl",
      "hsl_to_rgb",       "rgb_to_hsv",       "hsv_to_rgb",
      "rgb_to_ycbcr_ncl", "ycbcr_ncl_to_rgb", "xyz_to_xyy",
      "xyy_to_xyz"};
  const unsigned masks[16][3] = {{2, 3, 6}, {3, 1, 5}, {1, 6, 6}, {1, 6, 6},
                                 {7, 7, 7}, {7, 7, 7}, {1, 6, 6}, {1, 6, 6},
                                 {7, 7, 7}, {7, 7, 7}, {7, 7, 7}, {7, 7, 7},
                                 {7, 7, 7}, {5, 7, 3}, {7, 7, 2}, {7, 4, 7}};
  auto source = channel_fixture::source({ElementType::Float64, {1, 3}});
  fill(&source, {.5, .5, .5});
  for (unsigned member = 0; member < 16; ++member) {
    auto params = raw(1);
    if (member < 2) {
      params["white_x"] = .25;
      params["white_y"] = .25;
    }
    if (member == 2 || member == 6 || member == 8 || member == 10)
      params["output_hue_unit"] = std::string("pi_multiple");
    if (member == 3 || member == 7 || member == 9 || member == 11)
      params["input_hue_unit"] = std::string("pi_multiple");
    if (member == 12 || member == 13)
      params["ncl_matrix"] = std::string("bt709");
    for (unsigned component = 0; component < 3; ++component) {
      std::vector<Region> expected;
      for (unsigned c = 0; c < 3; ++c)
        if (masks[member][component] & (1U << c))
          expected.emplace_back(Region({{0, 1}, {c, 1}}));
      auto hook = std::make_shared<transfer_fixture::Hooks>();
      bool needed = false;
      hook->need = [&](const ResultProgramNeed& need) {
        require(need.tensors.size() == 1 && need.tensors[0].roles == 9,
                "raw exact roles");
        require(need.tensors[0].samples ==
                    take(Footprint::from_regions({1, 3}, expected)),
                "exact component support");
        needed = true;
      };
      const auto query = take(
          Footprint::from_regions({1, 3}, {Region({{0, 1}, {component, 1}})}));
      auto result =
          observe(source, std::string("color.") + names[member] + "_strict",
                  params, query, hook);
      require(result.ok() && needed, "raw member request failed");
    }
  }
  auto scalar =
      channel_fixture::source({ElementType::Float64, {2, 3}}, {}, {}, {2});
  fill(&scalar, {std::numeric_limits<double>::quiet_NaN()});
  Params expansion{{"metadata_mode", std::string("raw")},
                   {"axis_free", true},
                   {"output_axis", std::int64_t{1}},
                   {"gray_kind", std::string("cielab_l")}};
  auto hook = std::make_shared<transfer_fixture::Hooks>();
  bool descriptor_only = false;
  hook->need = [&](const ResultProgramNeed& need) {
    descriptor_only = need.tensors.size() == 1 && need.tensors[0].roles == 8 &&
                      need.tensors[0].samples.empty();
  };
  const Region zeros({{1, 1}, {0, 2}, {1, 2}, {0, 3}});
  const auto query = take(Footprint::from_regions({2, 2, 3, 3}, {zeros}));
  auto result = take(
      observe(scalar, "color.gray_to_color_strict", expansion, query, hook));
  require(descriptor_only, "constant R outputs fetched Gray data");
  require(channel_fixture::read(result.results.at("result"), zeros) ==
              std::vector<std::uint8_t>(96),
          "R constants are not +0");
  auto binary = channel_fixture::source({ElementType::Float64, {1}});
  fill(&binary, {std::numeric_limits<double>::quiet_NaN()});
  for (bool threshold : {false, true}) {
    Params p{{"metadata_mode", std::string("raw")}, {"axis_free", true}};
    if (threshold) {
      p["threshold"] = .5;
    } else {
      p["black_value"] = std::string("f64:0000000000000000");
      p["white_value"] = std::string("f64:3ff0000000000000");
    }
    unsigned observed_roles = 0;
    hook->need = [&](const ResultProgramNeed& need) {
      observed_roles = need.tensors[0].roles;
    };
    auto answer = observe(binary,
                          threshold ? "mask.threshold_channel_strict"
                                    : "color.black_white_to_gray_strict",
                          p, take(Footprint::all({1})), hook);
    require(observed_roles == (threshold ? 9U : 13U),
            "S/T intrinsic validation roles");
    require(threshold ? answer.ok()
                      : !answer.ok() && answer.status().reason ==
                                            FailureReason::InvalidDomain,
            "S/T NaN behavior");
  }
}
void batch_insertion() {
  for (unsigned storage = 0; storage < 3; ++storage)
    for (unsigned axis = 0; axis < 3; ++axis) {
      ResultTensorLayout layout;
      layout.spatial = storage != 0;
      layout.order =
          storage == 2 ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
      layout.channel_axis.reset();
      auto source = channel_fixture::source({ElementType::Float64, {2, 3}}, {},
                                            layout, {2, 2});
      std::vector<double> values(24);
      for (unsigned i = 0; i < values.size(); ++i)
        values[i] = i + 1;
      fill(&source, values);
      Params p{{"metadata_mode", std::string("raw")},
               {"axis_free", true},
               {"output_axis", static_cast<std::int64_t>(axis)},
               {"gray_kind", std::string("linear_y")},
               {"gray_white_x", .25},
               {"gray_white_y", .25}};
      auto shape = std::vector<std::uint64_t>{2, 2, 2, 3};
      shape.insert(shape.begin() + 2 + axis, 3);
      auto dims = Region::whole(shape).dimensions();
      dims[0] = {1, 1};
      dims[1] = {0, 1};
      const Region roi(dims);
      auto result = take(observe(source, "color.gray_to_color_strict", p,
                                 take(Footprint::from_regions(shape, {roi})),
                                 std::make_shared<transfer_fixture::Hooks>()));
      const auto& output = result.results.at("result");
      require(output.schema().tensors[0].batch_axes ==
                  source.schema.tensors[0].batch_axes,
              "batch prefix changed");
      for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 3; ++x)
          for (unsigned c = 0; c < 3; ++c) {
            std::vector<std::uint64_t> at{1, 0, y, x};
            at.insert(at.begin() + 2 + axis, c);
            double value = 0;
            take(output.read_tensor(take(output.descriptor()), 0, at, &value,
                                    8));
            require(value == (13 + y * 3 + x) * (c == 2 ? 2 : 1),
                    "batch insertion sample mapping");
          }
    }
}
void q_groups_owners() {
  for (const auto* policy : {"auto", "view", "materialize"}) {
    ResultRef retained;
    ResourceBudget root;
    ColorProfileIdentity identity;
    {
      auto registry = make_default_operation_registry();
      ExecutionContext context(registry);
      root = take(context.resource_budget());
      ResourceBudget profile_root;
      const auto icc = numeric_fixture::fixture();
      auto profile = take(
          IccProfile::import(ByteView(icc.data(), icc.size()), profile_root));
      identity = profile.identity();
      TensorDescription d;
      d.channel_axis = 2;
      d.channels.resize(4);
      d.channels[3].interpretation.emplace();
      d.channels[3].interpretation->profile = identity;
      TensorColorGroup g;
      g.name = "xyz";
      g.indices = {0, 1, 2};
      g.components = {{"X", "x", "1"}, {"Y", "y", "1"}, {"Z", "z", "1"}};
      g.interpretation.model = "xyz";
      g.interpretation.reference = "scene";
      g.interpretation.white = std::array<double, 2>{.25, .25};
      g.interpretation.coordinates = TensorModelCoordinates{};
      g.interpretation.coordinates->scale = "relative";
      d.groups = {g};
      ResultTensorLayout layout;
      layout.spatial = true;
      layout.order = ImagePlaneOrder::Tiled;
      layout.groups = {{"components", 0, 3}};
      auto source = channel_fixture::source(
          {ElementType::Float64, {2, 3, 4}},
          {take(encode_tensor_description(d))}, layout, {2});
      source.resources =
          take(ResourceBindings::create({profile}, profile_root));
      fill(&source, {std::numeric_limits<double>::quiet_NaN(), 2,
                     std::numeric_limits<double>::quiet_NaN(), 7});
      auto value = channel_fixture::publish(root, source);
      const Params p{{"group", std::string("xyz")},
                     {"layout", std::string(policy)}};
      GraphContext graph(
          transfer_fixture::document(value, "color.color_to_gray_strict", p));
      const Region roi({{1, 1}, {0, 2}, {0, 3}, {0, 1}});
      PlanningOptions planning;
      planning.output_regions = {{"result", roi}};
      auto compiled =
          take(Compiler(registry).compile(graph, planning, source.resources));
      const auto bindings = transfer_fixture::bindings(value);
      auto future = std::async(std::launch::async, [&] {
        return context.execute(compiled.plan, bindings);
      });
      auto second = take(context.execute(compiled.plan, bindings));
      auto first = take(future.get());
      retained = first.results.at("result");
      require(retained.schema().tensors[0].layout.groups.empty(),
              "Q kept obsolete physical groups");
      require(channel_fixture::read(retained, roi) ==
                  channel_fixture::read(second.results.at("result"), roi),
              "concurrent Q changed output");
      const Region input_roi({{1, 1}, {0, 2}, {0, 3}, {1, 1}});
      const bool shared = channel_fixture::owner(retained, roi) ==
                          channel_fixture::owner(value, input_roi);
      require(shared == (std::string(policy) != "materialize"),
              "Q layout owner policy");
      require(retained.association().size() == 1 &&
                  retained.association()[0] == value.object_id(),
              "Q association");
      require(retained.resources().icc_profile(identity).ok(),
              "Q dropped bypass resource");
      const auto observations = take(first.dependencies.source_observations());
      bool validated = false;
      for (const auto& observation : observations)
        if (observation.target == ResultSupportTarget::Tensor) {
          require(observation.samples ==
                      take(Footprint::from_regions({2, 2, 3, 4}, {input_roi})),
                  "Q semantic support widened");
          validated |= (observation.roles & 4) != 0;
        }
      require(validated, "Q semantic validation witness missing");
    }
    require(retained.resources().icc_profile(identity).ok(),
            "Q resource retirement");
    const auto bytes = channel_fixture::read(
        retained, Region({{1, 1}, {0, 2}, {0, 3}, {0, 1}}));
    for (unsigned i = 0; i < 6; ++i) {
      double value;
      std::memcpy(&value, bytes.data() + 8 * i, 8);
      require(value == 2, "Q retained bytes");
    }
    retained = {};
    require(root.statistics().live[ResourceKind::Payload] == 0,
            "Q final owner payload leak");
  }
}
void fragmented_query_work() {
  for (bool rectangle : {false, true})
    for (bool limited : {false, true}) {
      auto hooks = std::make_shared<transfer_fixture::Hooks>();
      std::uint64_t issued = 0;
      hooks->charge = [&](std::uint64_t n, const ResultProgramPhase& phase) {
        issued += n;
        return limited && issued > 512
                   ? Status{ErrorCode::ResourceExhausted,
                            "fragment lookup fuel", FailureReason::WorkLimit}
                   : phase.consume_work(n);
      };
      auto registry =
          transfer_fixture::registry("color.color_to_gray_strict", hooks);
      ExecutionContext context(registry);
      const auto root = take(context.resource_budget());
      const std::vector<std::uint64_t> shape =
          rectangle ? std::vector<std::uint64_t>{32, 3}
                    : std::vector<std::uint64_t>{3, 32};
      auto spec = channel_fixture::source({ElementType::Float64, shape}).schema;
      auto builder =
          take(ResultBuilder::start(root, spec, "fragmented.source"));
      take(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0}))));
      const auto relation =
          take(ResultRelation::cartesian(root, 96, {0, 1, 0, 0}));
      const std::array<double, 3> values{1, 2, 3};
      for (unsigned i = 0; i < 32; ++i) {
        auto buffer = take(root.allocator().allocate(24));
        std::memcpy(buffer.data(), values.data(), 24);
        const auto region =
            rectangle ? Region({{i, 1}, {0, 3}}) : Region({{0, 3}, {i, 1}});
        const StridedLayout layout = rectangle
                                         ? StridedLayout{0, {24, 8}, {i, 0}}
                                         : StridedLayout{0, {8, 8}, {0, i}};
        take(builder.publish_tensor(0, region, layout,
                                    std::move(buffer).freeze(), relation,
                                    {true, true, true, true}));
      }
      const auto input = take(builder.seal());
      auto params = raw(rectangle ? 1 : 0);
      params["gray_kind"] = std::string("linear_y");
      params["layout"] = std::string("view");
      GraphContext graph(transfer_fixture::document(
          input, "color.color_to_gray_strict", params));
      auto compiled = take(Compiler(registry).compile(graph));
      auto result =
          context.execute(compiled.plan, transfer_fixture::bindings(input));
      if (limited) {
        require(!result.ok() &&
                    result.status().code == ErrorCode::ResourceExhausted &&
                    issued > 512,
                "fragmented lookup escaped its work limit");
      } else {
        require(result.ok() && issued > 32 * 32,
                "fragmented lookup work was not charged");
        const auto& output = result.value().results.at("result");
        const auto data = channel_fixture::read(
            output, Region::whole(output.schema().tensors[0].sample_shape()));
        require(data.size() == 256, "fragmented Q coverage");
        for (unsigned i = 0; i < 32; ++i) {
          double value;
          std::memcpy(&value, data.data() + 8 * i, 8);
          require(value == 2, "fragmented Q sample");
        }
      }
    }
}
void explicit_reference_diagnostics() {
#if defined(__APPLE__) && defined(__aarch64__)
  const std::string suffix = "accelerated_apple_silicon";
#elif defined(__x86_64__)
  const std::string suffix = "accelerated_x86_64";
#else
  return;
#endif
#if defined(__x86_64__) || (defined(__APPLE__) && defined(__aarch64__))
  auto source = channel_fixture::source({ElementType::Float64, {1, 3}});
  fill(&source, {.5, .5, .5});
  auto params = raw(1);
  params["white_x"] = .25;
  params["white_y"] = .25;
  params["algorithm"] = std::string("reference");
  const auto query =
      take(Footprint::from_regions({1, 3}, {Region({{0, 1}, {0, 1}})}));
  auto observed = observe(source, "color.xyz_to_cielab_" + suffix, params,
                          query, std::make_shared<transfer_fixture::Hooks>());
  if (!observed.ok() && observed.status().code == ErrorCode::BackendUnavailable)
    return;
  auto result = take(std::move(observed));
  std::uint64_t calls = 0, fallback = 0;
  for (const auto& timing : result.diagnostics.operation_timings) {
    calls += timing.numeric.strict_math_calls;
    fallback += timing.numeric.strict_fallbacks;
  }
  require(calls > 0 && fallback == 0,
          "explicit reference reported a fast-path fallback");
#endif
}
}  // namespace
int main() try {
  roles_and_constants();
  batch_insertion();
  q_groups_owners();
  fragmented_query_work();
  explicit_reference_diagnostics();
  std::cout << "FMT-11 Result boundary checks PASS\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
