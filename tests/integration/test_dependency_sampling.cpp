#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
Value data(ElementType type, std::vector<std::uint64_t> shape,
           const std::vector<T>& samples, std::vector<ValueFacet> facets = {}) {
  auto writer = MutableValue::allocate({type, shape}, Region::whole(shape),
                                       BufferAllocator{})
                    .take_value();
  if (writer.size() != samples.size() * sizeof(T))
    throw std::invalid_argument("fixture size");
  std::memcpy(writer.data(), samples.data(), writer.size());
  return std::move(writer).publish(std::move(facets)).take_value();
}
Footprint footprint(const std::vector<std::uint64_t>& shape,
                    const Region& region) {
  return Footprint::from_regions(shape, {region}).take_value();
}
Result<DependencyResult> resolve(
    const std::shared_ptr<OperationRegistry>& registry, const std::string& key,
    const std::vector<Value>& inputs, Footprint query,
    const std::map<std::string, ParameterValue>& parameters = {}) {
  DependencyRequest request;
  for (const auto& input : inputs)
    request.inputs.push_back({input.descriptor(), input.facets()});
  request.outputs = std::move(query);
  request.parameters = parameters;
  request.snapshot_identity = "fixture";
  auto started = registry->start_dependency(key, request);
  if (!started.ok())
    return Result<DependencyResult>(started.status());
  auto session = started.take_value();
  for (;;) {
    auto progress = session->poll();
    if (!progress.ok())
      return Result<DependencyResult>(progress.status());
    auto event = progress.take_value();
    if (auto* result = std::get_if<DependencyResult>(&event))
      return Result<DependencyResult>(std::move(*result));
    auto needs = session->pending_reads().take_value();
    std::vector<ValueFragments> ready;
    for (std::size_t port = 0; port < inputs.size(); ++port) {
      auto samples =
          Footprint::none(inputs[port].descriptor().shape).take_value();
      for (const auto& need : needs)
        if (need.port == port)
          samples = samples.unite(need.samples).take_value();
      auto part = ValueFragments::create(inputs[port].descriptor(),
                                         inputs[port].facets(), samples,
                                         {inputs[port]});
      if (!part.ok())
        return Result<DependencyResult>(part.status());
      ready.push_back(part.take_value());
    }
    auto status = session->supply(std::move(ready), "fixture");
    if (!status.ok())
      return Result<DependencyResult>(status);
  }
}
Footprint support(const DependencyResult& result, std::uint32_t port,
                  DependencyRole role) {
  auto found =
      Footprint::none(result.certificate->input_shapes()[port]).take_value();
  for (const auto& need :
       result.certificate->backward(result.certificate->coverage())
           .take_value())
    if (need.port == port && (need.roles & static_cast<std::uint32_t>(role)))
      found = found.unite(need.samples).take_value();
  return found;
}
int radius_oracles(const std::shared_ptr<OperationRegistry>& registry) {
  std::mt19937 random(20260911);
  for (unsigned trial = 0; trial < 80; ++trial) {
    const std::uint64_t n = 1 + random() % 8;
    std::vector<double> samples(n);
    std::vector<std::int64_t> radii(n);
    for (std::uint64_t i = 0; i < n; ++i) {
      samples[i] = static_cast<int>(random() % 9) - 4;
      radii[i] = random() % 4;
    }
    const std::vector<Value> inputs{data(ElementType::Float64, {n}, samples),
                                    data(ElementType::Int64, {n}, radii)};
    for (bool scatter : {false, true})
      for (std::uint64_t o = 0; o < n; ++o) {
        auto result = resolve(
            registry,
            scatter ? "numeric.radius_scatter" : "numeric.radius_gather",
            inputs, footprint({n}, Region({{o, 1}})));
        PS_CHECK(result.ok());
        double expected = 0, actual = 0;
        std::vector<Region> positive;
        for (std::uint64_t i = 0; i < n; ++i) {
          const auto distance = i > o ? i - o : o - i;
          if (distance <= static_cast<std::uint64_t>(radii[scatter ? i : o])) {
            expected += samples[i];
            positive.emplace_back(Region({{i, 1}}));
          }
        }
        PS_CHECK(result.value().value.read({o}, &actual, 8).ok() &&
                 actual == expected);
        PS_CHECK(support(result.value(), 0, DependencyRole::Data) ==
                 Footprint::from_regions({n}, positive).take_value());
        const auto controls = scatter ? Region::whole({n}) : Region({{o, 1}});
        PS_CHECK(support(result.value(), 1, DependencyRole::Control) ==
                 footprint({n}, controls));
      }
  }
  // A remote radius edit adds a new edge, even if the output bytes stay equal.
  const auto source =
      data(ElementType::Float64, {4}, std::vector<double>{1, 2, 3, 0});
  const auto old_radius =
      data(ElementType::Int64, {4}, std::vector<std::int64_t>{0, 0, 0, 0});
  const auto new_radius =
      data(ElementType::Int64, {4}, std::vector<std::int64_t>{0, 0, 0, 3});
  const auto q = footprint({4}, Region({{0, 1}}));
  auto old =
      resolve(registry, "numeric.radius_scatter", {source, old_radius}, q)
          .take_value();
  auto changed =
      resolve(registry, "numeric.radius_scatter", {source, new_radius}, q)
          .take_value();
  double before = 0, after = 0;
  PS_CHECK(old.value.read({0}, &before, 8).ok() &&
           changed.value.read({0}, &after, 8).ok() && before == after);
  PS_CHECK(support(old, 0, DependencyRole::Data) !=
           support(changed, 0, DependencyRole::Data));
  DependencyNeed edit{1, 2, footprint({4}, Region({{3, 1}})), {}};
  PS_CHECK(old.certificate->transpose(edit).take_value() == q);
  auto bad =
      data(ElementType::Int64, {4}, std::vector<std::int64_t>{0, 0, -1, 0});
  PS_CHECK(resolve(registry, "numeric.radius_scatter", {source, bad}, q)
               .status()
               .code == ErrorCode::OperationFailed);
  PS_CHECK(resolve(registry, "numeric.radius_gather", {source, bad}, q).ok());
  // More than one control/data chunk preserves source order exactly.
  std::vector<double> long_data(130, 0);
  long_data[0] = 1e16;
  long_data[64] = -1e16;
  long_data[129] = 1;
  std::vector<std::int64_t> long_radius(130, 130);
  auto folded = resolve(registry, "numeric.radius_scatter",
                        {data(ElementType::Float64, {130}, long_data),
                         data(ElementType::Int64, {130}, long_radius)},
                        footprint({130}, Region({{0, 1}})))
                    .take_value();
  PS_CHECK(folded.value.read({0}, &after, 8).ok() && after == 1);
  return 0;
}
int stmap_oracles(const std::shared_ptr<OperationRegistry>& registry) {
  const auto facets =
      std::vector<ValueFacet>{encode_semantic(rgba_semantics()).take_value()};
  const auto image = data(ElementType::Float32, {2, 2, 4},
                          std::vector<float>{0, 0, 0, 1, .25F, 0, 0, 1, .5F, 0,
                                             0, 1, .75F, 0, 0, 1},
                          facets);
  const auto map =
      data(ElementType::Float64, {1, 1, 2}, std::vector<double>{1, 1});
  const auto q = footprint({1, 1, 4}, Region::whole({1, 1, 4}));
  auto result = resolve(registry, "image.stmap", {image, map}, q,
                        {{"boundary", std::string("clamp")}});
  PS_CHECK(result.ok());
  float red = 0;
  PS_CHECK(result.value().value.read({0, 0, 0}, &red, 4).ok() && red == .375F);
  PS_CHECK(support(result.value(), 0, DependencyRole::Data) ==
           Footprint::all({2, 2, 4}).take_value());
  PS_CHECK(support(result.value(), 1, DependencyRole::Control) ==
           Footprint::all({1, 1, 2}).take_value());
  const auto outside =
      data(ElementType::Float64, {1, 1, 2}, std::vector<double>{-10, -10});
  auto transparent = resolve(registry, "image.stmap", {image, outside}, q,
                             {{"boundary", std::string("constant")}})
                         .take_value();
  PS_CHECK(transparent.value.read({0, 0, 0}, &red, 4).ok() && red == 0);
  PS_CHECK(support(transparent, 0, DependencyRole::Data).empty());
  PS_CHECK(!support(transparent, 1, DependencyRole::Control).empty());
  auto invalid =
      data(ElementType::Float64, {1, 1, 2},
           std::vector<double>{std::numeric_limits<double>::infinity(), 0});
  PS_CHECK(resolve(registry, "image.stmap", {image, invalid}, q,
                   {{"boundary", std::string("clamp")}})
               .status()
               .code == ErrorCode::OperationFailed);
  // At pixel center, the zero-weight neighbour is nevertheless an actual tap.
  auto center =
      data(ElementType::Float64, {1, 1, 2}, std::vector<double>{.5, .5});
  auto centered = resolve(registry, "image.stmap", {image, center}, q,
                          {{"boundary", std::string("clamp")}})
                      .take_value();
  PS_CHECK(support(centered, 0, DependencyRole::Data) ==
           Footprint::all({2, 2, 4}).take_value());
  PS_CHECK(centered.value.read({0, 0, 0}, &red, 4).ok() && red == 0);
  const auto line = data(
      ElementType::Float32, {1, 3, 4},
      std::vector<float>{.125F, 0, 0, 1, .25F, 0, 0, 1, .5F, 0, 0, 1}, facets);
  const std::vector<unsigned> reflect{2, 1, 0, 0, 1, 2, 2, 1, 0};
  const std::vector<unsigned> mirror{1, 2, 1, 0, 1, 2, 1, 0, 1};
  const float red_values[] = {.125F, .25F, .5F};
  for (bool repeated_endpoints : {false, true}) {
    for (int index = -3; index <= 5; ++index) {
      const auto coordinates = data(ElementType::Float64, {1, 1, 2},
                                    std::vector<double>{index + .5, .5});
      auto mapped =
          resolve(registry, "image.stmap", {line, coordinates}, q,
                  {{"boundary",
                    std::string(repeated_endpoints ? "reflect" : "mirror")}})
              .take_value();
      PS_CHECK(mapped.value.read({0, 0, 0}, &red, 4).ok());
      PS_CHECK(red ==
               red_values[(repeated_endpoints ? reflect : mirror)[index + 3]]);
    }
  }
  const auto singleton = data(ElementType::Float32, {1, 1, 4},
                              std::vector<float>{.25F, 0, 0, 1}, facets);
  for (const std::string mode : {"clamp", "wrap", "reflect", "mirror"}) {
    auto mapped = resolve(registry, "image.stmap", {singleton, outside}, q,
                          {{"boundary", mode}})
                      .take_value();
    PS_CHECK(mapped.value.read({0, 0, 0}, &red, 4).ok() && red == .25F);
  }
  auto broken = data(
      ElementType::Float32, {2, 2, 4},
      std::vector<float>{0, 0, 0, 1, std::numeric_limits<float>::quiet_NaN(), 0,
                         0, 1, .5F, 0, 0, 1, .75F, 0, 0, 1},
      facets);
  PS_CHECK(resolve(registry, "image.stmap", {broken, center}, q,
                   {{"boundary", std::string("clamp")}})
               .status()
               .code == ErrorCode::OperationFailed);
  return 0;
}
int runtime_fragments(const std::shared_ptr<OperationRegistry>& registry) {
  const auto facets =
      std::vector<ValueFacet>{encode_semantic(rgba_semantics()).take_value()};
  const auto map =
      data(ElementType::Float64, {1, 1, 2}, std::vector<double>{0, .5});
  WorkflowDocument document;
  document.inputs = {
      {1,
       "source",
       {ElementType::Float32, {1, 1024, 4}},
       Region::whole({1, 1024, 4}),
       {0, {16384, 16, 4}},
       facets},
      {2, "map", map.descriptor(), map.region(), map.layout(), {}}};
  document.nodes = {
      {1, "core.identity", {WorkflowInputReference{2}}, {}},
      {2,
       "image.stmap",
       {WorkflowInputReference{1}, WorkflowNodeOutput{1, "value"}},
       {{"boundary", std::string("wrap")}}}};
  document.outputs = {{"sample", 2, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto plan = compiler.compile(graph);
  PS_CHECK(plan.ok());
  std::vector<std::uint64_t> addresses;
  auto source = std::make_shared<RegionalSource>();
  source->descriptor = document.inputs[0].descriptor;
  source->facets = facets;
  source->read = [&](const Region& region, std::uint8_t* bytes,
                     std::uint64_t size, const BufferAllocator&,
                     const CancellationToken&) {
    const auto x = region.dimensions()[1].offset;
    if (size != 16 || region.dimensions()[1].extent != 1 ||
        (x != 0 && x != 1023))
      return Result<Region>(Status::failure(ErrorCode::OperationFailed,
                                            "undeclared middle source read"));
    addresses.push_back(x);
    const float pixel[] = {x == 0 ? .25F : .75F, 0, 0, 1};
    std::memcpy(bytes, pixel, sizeof(pixel));
    return Result<Region>(region);
  };
  ExecutionContext execution(registry, {1, false, 8, 512});
  auto result = execution.execute(plan.value().plan,
                                  {{{"source", {}, source, {}}, {"map", map}}});
  PS_CHECK(result.ok());
  float red = 0;
  std::memcpy(&red, result.value().values.at("sample").bytes().data(), 4);
  PS_CHECK(red == .5F && addresses == std::vector<std::uint64_t>({0, 1023}));
  PS_CHECK(result.value().diagnostics.source_read_bytes == 32);
  PS_CHECK(result.value().diagnostics.peak_live_bytes <= 512);
  return 0;
}

int static_and_rounding(const std::shared_ptr<OperationRegistry>& registry) {
  const auto facets =
      std::vector<ValueFacet>{encode_semantic(rgba_semantics()).take_value()};
  const auto image = data(ElementType::Float32, {1, 1, 4},
                          std::vector<float>{0, 0, 0, 1}, facets);
  const auto malformed =
      data(ElementType::Float64, {1, 1, 3}, std::vector<double>{0, 0, 0});
  const auto map =
      data(ElementType::Float64, {1, 1, 2}, std::vector<double>{0, 0});
  for (bool empty : {false, true}) {
    auto q = empty ? Footprint::none({1, 1, 4}).take_value()
                   : Footprint::all({1, 1, 4}).take_value();
    PS_CHECK(resolve(registry, "image.stmap", {image, malformed}, q,
                     {{"boundary", std::string("clamp")}})
                 .status()
                 .code == ErrorCode::TypeMismatch);
    PS_CHECK(resolve(registry, "image.stmap", {image, map}, q,
                     {{"boundary", std::string("bogus")}})
                 .status()
                 .code == ErrorCode::InvalidArgument);
    auto radius_q = empty ? Footprint::none({3}).take_value()
                          : footprint({3}, Region({{0, 1}}));
    PS_CHECK(
        resolve(
            registry, "numeric.radius_gather",
            {data(ElementType::Float64, {3}, std::vector<double>{0, 0, 0}),
             data(ElementType::Int64, {2}, std::vector<std::int64_t>{0, 0})},
            radius_q)
            .status()
            .code == ErrorCode::TypeMismatch);
  }
  WorkflowDocument document;
  document.inputs = {{1, "image", image.descriptor(), image.region(),
                      image.layout(), image.facets()},
                     {2,
                      "map",
                      malformed.descriptor(),
                      malformed.region(),
                      malformed.layout(),
                      {}}};
  document.nodes = {{1,
                     "image.stmap",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {{"boundary", std::string("clamp")}}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  PS_CHECK(compiler.compile(graph).status().code == ErrorCode::TypeMismatch);
  struct Restore {
    int mode = std::fegetround();
    ~Restore() { std::fesetround(mode); }
  } restore;
  const auto input =
      data(ElementType::Float64, {3}, std::vector<double>{1e16, 1, -1e16});
  const auto radii =
      data(ElementType::Int64, {3}, std::vector<std::int64_t>{2, 2, 2});
  std::vector<double> long_samples(130, 0);
  long_samples[0] = 1e16;
  long_samples[1] = 1;
  long_samples[64] = -1e16;
  long_samples[129] = 1;
  const auto long_input = data(ElementType::Float64, {130}, long_samples);
  const auto long_radii =
      data(ElementType::Int64, {130}, std::vector<std::int64_t>(130, 130));
  for (const auto mode :
       {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
    PS_CHECK(std::fesetround(mode) == 0);
    for (const std::string key :
         {"numeric.radius_gather", "numeric.radius_scatter"}) {
      auto result = resolve(registry, key, {input, radii},
                            footprint({3}, Region({{1, 1}})))
                        .take_value();
      double value = 7;
      PS_CHECK(result.value.read({1}, &value, 8).ok() && value == 0 &&
               !std::signbit(value));
      auto longer = resolve(registry, key, {long_input, long_radii},
                            footprint({130}, Region({{0, 1}})))
                        .take_value();
      PS_CHECK(longer.value.read({0}, &value, 8).ok() && value == 1);
      PS_CHECK(std::fegetround() == mode);
    }
  }
  return 0;
}

}  // namespace
int main() {
  auto registry = ps::make_default_operation_registry();
  PS_CHECK(static_and_rounding(registry) == 0);
  PS_CHECK(radius_oracles(registry) == 0);
  PS_CHECK(stmap_oracles(registry) == 0);
  PS_CHECK(runtime_fragments(registry) == 0);
  return 0;
}
