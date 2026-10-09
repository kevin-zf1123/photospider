#include <array>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "channel_extraction_workflow/source.hpp"
#include "photospider/ops/format/channel.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using channel_fixture::check;
using channel_fixture::read;
using channel_fixture::require;
using channel_fixture::take;
using Params = std::map<std::string, ParameterValue>;
std::string assertion(const SchemaTemplate& schema) {
  const auto digest = format::detail::schema_assertion(schema);
  const auto layout =
      format::detail::layout_assertion(schema.tensors[0].layout);
  return "result-v1:" + std::to_string(digest.size()) + ":" + digest + ":" +
         std::to_string(layout.size()) + ":" + layout;
}
std::string hex(const std::vector<std::uint8_t>& bits) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (auto byte : bits) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}
struct Fixture {
  SchemaTemplate schema;
  std::shared_ptr<OperationRegistry> registry =
      make_default_operation_registry();
  ExecutionContext context{registry};
  WorkflowDocument document;
  ExecutionBindings bindings;
  ResourceBindings resources;
  std::unique_ptr<GraphContext> graph;
  Fixture(ElementType dtype, std::vector<std::uint64_t> shape,
          ResultTensorLayout layout = {},
          std::vector<std::uint32_t> batches = {}) {
    schema.id = "literal.test";
    ResultTensorSpec tensor;
    tensor.key = "pixels";
    tensor.descriptor = {dtype, std::move(shape)};
    tensor.layout = std::move(layout);
    tensor.batch_axes.assign(batches.begin(), batches.end());
    schema.tensors.push_back(std::move(tensor));
    bind();
  }
  void bind() {
    auto root = take(context.resource_budget());
    auto builder = take(ResultBuilder::start(root, schema, "descriptor.only"));
    require(builder.bind_descriptor_relation(take(ResultRelation::cartesian(
        root, 1, {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0}))));
    // No samples exist: any accidental Data/Validation Need fails.
    bindings.inputs = {{"source", take(builder.seal())}};
    WorkflowInputDeclaration declaration;
    declaration.id = 1;
    declaration.name = "source";
    declaration.result_schema = std::make_shared<const SchemaTemplate>(schema);
    document.inputs = {std::move(declaration)};
  }
  void node(const std::vector<std::uint8_t>& bits, int axis, bool keep,
            const std::string& layout = "auto") {
    TensorDescription description;
    Params params{
        {"bits", hex(bits)},
        {"keepdims", keep},
        {"layout", layout},
        {"authoring_member", std::string("FMT-05B")},
        {"output_description", take(tensor_description_parameter(description))},
        {"expected_inputs", assertion(schema)}};
    if (axis >= 0)
      params["axis"] = static_cast<std::int64_t>(axis);
    document.nodes = {{1,
                       "channel.literal_like_strict",
                       {WorkflowInputReference{1}},
                       std::move(params)}};
    document.outputs = {{"literal", 1, "values"}};
  }
  auto compile(const PlanningOptions& planning = {}) {
    graph = std::make_unique<GraphContext>(document);
    return Compiler(registry).compile(*graph, planning, resources);
  }
  Result<ExecutionResult> execute(const PlanningOptions& planning = {}) {
    auto plan = compile(planning);
    if (!plan.ok())
      return Result<ExecutionResult>(plan.status());
    return context.execute(plan.value().plan, bindings);
  }
};
void oracle(const ExecutionResult& run, const Region& q,
            const std::vector<std::uint8_t>& bits) {
  const auto& result = run.results.at("literal");
  const auto shape = result.schema().tensors[0].sample_shape();
  const auto query = take(Footprint::from_regions(shape, {q}));
  check(take(result.descriptor()).tensor_coverage(0) == query,
        "literal exact coverage");
  const auto bytes = read(result, q);
  for (std::size_t i = 0; i < bytes.size(); ++i)
    check(bytes[i] == bits[i % bits.size()], "literal exact bit repetition");
  require(take(result.tensor_relation(0))
              .project(query, [](auto support, const Footprint*) {
                check(support.roles == 8 &&
                          support.target == ResultSupportTarget::Descriptor &&
                          support.first == 0 && support.count == 1,
                      "literal has descriptor-only support");
                return Status::success();
              }));
  const auto sources = take(run.dependencies.source_support());
  check(!sources.count("source") || sources.at("source").empty(),
        "literal requests no source samples");
}
void bit_patterns() {
  const std::vector<std::pair<ElementType, std::vector<std::uint8_t>>> patterns{
      {ElementType::UInt8, {255}},
      {ElementType::UInt16, {0xff, 3}},
      {ElementType::Int8, {0x80}},
      {ElementType::Int16, {0xff, 0x7f}},
      {ElementType::Int64, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f}},
      {ElementType::Float32, {0x12, 0, 0x80, 0x7f}},
      {ElementType::Float32, {0, 0, 0, 0x80}},
      {ElementType::Float64, {0x34, 0, 0, 0, 0, 0, 0xf0, 0x7f}},
      {ElementType::Float64, {0, 0, 0, 0, 0, 0, 0, 0x80}}};
  for (const auto& [dtype, bits] : patterns)
    for (const auto* policy : {"auto", "materialize"})
      for (bool keep : {false, true}) {
        Fixture f(dtype, {2, 3, 4});
        f.node(bits, 1, keep, policy);
        const Region q(
            keep ? std::vector<RegionDimension>{{1, 1}, {0, 1}, {1, 2}}
                 : std::vector<RegionDimension>{{1, 1}, {1, 2}});
        PlanningOptions planning;
        planning.output_regions = {{"literal", q}};
        auto run = take(f.execute(planning));
        oracle(run, q, bits);
        const auto all =
            take(Footprint::all(f.schema.tensors[0].sample_shape()));
        check(take(run.dependencies.potential_dirty(
                       "source", all, 1, {}, ResultSupportTarget::Tensor, 0))
                  .at("literal")
                  .empty(),
              "source sample edits leave literal clean");
      }
}
void planar_batches() {
  for (bool chw : {false, true})
    for (bool keep : {false, true}) {
      ResultTensorLayout layout;
      layout.spatial = true;
      layout.height_axis = chw ? 1 : 0;
      layout.width_axis = chw ? 2 : 1;
      layout.channel_axis = chw ? 0 : 2;
      layout.groups = {{"components", 0, 3}};
      Fixture f(ElementType::UInt16,
                chw ? std::vector<std::uint64_t>{3, 3, 260}
                    : std::vector<std::uint64_t>{3, 260, 3},
                layout, {2, 2});
      f.node({0xff, 3}, chw ? 0 : 2, keep);
      std::vector<RegionDimension> dims{{1, 1}, {0, 2}, {1, 2}, {126, 5}};
      if (keep)
        dims.insert(dims.begin() + (chw ? 2 : 4), {0, 1});
      const Region q(dims);
      PlanningOptions planning;
      planning.output_regions = {{"literal", q}};
      auto run = take(f.execute(planning));
      oracle(run, q, {0xff, 3});
      const auto& tensor = run.results.at("literal").schema().tensors[0];
      check(tensor.layout.spatial && tensor.layout.groups.empty() &&
                tensor.batch_axes == f.schema.tensors[0].batch_axes,
            "literal preserves batches and clears physical group claims");
    }
}
void disjoint_metadata_lifetime() {
  ResourceBudget root;
  std::optional<ResultTensorReadWindow> retained;
  {
    Fixture f(ElementType::UInt8, {3, 260, 2});
    f.schema.tensors[0].facets = {{"example.annotation", 1, {1, 2, 3}}};
    f.schema.tensors[0].atomic_trailing_axes = 1;
    f.bind();
    f.node({91}, 2, false);
    auto plan = take(f.compile());
    auto frozen = take(f.context.freeze(plan.plan, f.bindings));
    auto q = take(Footprint::from_regions(
        {3, 260}, {Region({{0, 1}, {1, 2}}), Region({{2, 1}, {129, 3}})}));
    auto run = take(f.context.execute_fragments(frozen, {{"literal", q}}));
    const auto& result = run.results.at("literal");
    const auto& tensor = result.schema().tensors[0];
    check(result.schema().id == f.schema.id && tensor.key == "pixels" &&
              tensor.atomic_trailing_axes == 0 &&
              tensor.facets.front().key == "example.annotation" &&
              tensor.facets.front().payload ==
                  f.schema.tensors[0].facets.front().payload,
          "literal retains opaque facets/id/key and projects atomic axes");
    check(take(result.descriptor()).tensor_coverage(0) == q,
          "disjoint generation does not bridge gaps");
    for (const auto& box : q.boxes())
      for (auto byte : read(result, box))
        check(byte == 91, "disjoint bytes");
    retained.emplace(take(
        result.acquire_tensor(take(result.descriptor()), 0, q.boxes().back())));
    root = take(f.context.resource_budget());
  }
  check(root.statistics().live[ResourceKind::Payload] > 0,
        "literal owning window survives context");
  check(*take(retained->row_run({2, 129})).data == 91,
        "literal owning window retains bytes");
  retained.reset();
  check(root.statistics().live[ResourceKind::Payload] == 0,
        "last literal window releases payload");
}
void resource_lifetime() {
  ResourceBudget budget;
  ResultRef surviving;
  ColorProfileIdentity identity;
  {
    OcioConfigSnapshot snapshot;
    snapshot.config = {'c'};
    snapshot.spaces = {{"working", "scene"}};
    snapshot.build_identity = "fixture-build";
    snapshot.settings = "reference";
    auto config = take(OcioConfigResource::import(snapshot, budget));
    identity = config.identity();
    Fixture f(ElementType::UInt8, {2, 3});
    f.resources = take(ResourceBindings::create({}, {config}, budget));
    f.node({73}, -1, false);
    TensorDescription description;
    description.component = TensorChannelDescription{"R", "red", "relative"};
    TensorInterpretation interpretation;
    interpretation.model = "rgb";
    interpretation.convention = "ocio-native";
    interpretation.configured =
        TensorConfiguredSpace{identity, "working", "scene"};
    description.component->interpretation = interpretation;
    f.document.nodes[0].parameters["output_description"] =
        take(tensor_description_parameter(description));
    surviving = take(f.execute()).results.at("literal");
    check(surviving.resources().config_count() == 1,
          "literal output retains referenced configuration");
    f.resources = {};
    check(!f.compile().ok(),
          "literal description requires its resource binding");
  }
  check(surviving.resources().ocio_config(identity).ok() &&
            read(surviving, Region::whole({2, 3}))[0] == 73,
        "literal resource and samples survive source/context retirement");
  surviving = {};
  check(budget.statistics().live[ResourceKind::Host] == 0,
        "literal final Result releases configuration owner");
}
void errors_empty_and_reuse() {
  Fixture f(ElementType::UInt8, {2, 3});
  f.node({73}, 1, false, "view");
  auto failed = f.execute();
  check(
      !failed.ok() && failed.status().reason == FailureReason::InvalidDomain &&
          failed.status().message.find("ViewUnavailable") != std::string::npos,
      "nonempty forced view cannot generate");
  auto plan = take(f.compile());
  auto frozen = take(f.context.freeze(plan.plan, f.bindings));
  auto root = take(f.context.resource_budget());
  const auto before = root.statistics().peak[ResourceKind::Payload];
  auto empty = take(f.context.execute_fragments(
      frozen, {{"literal", take(Footprint::none({2}))}}));
  check(take(empty.results.at("literal").descriptor())
                .tensor_coverage(0)
                .empty() &&
            root.statistics().peak[ResourceKind::Payload] == before,
        "Empty has no payload or state allocation");
  f.node({73}, -1, false);
  plan = take(f.compile());
  auto a = std::async(std::launch::async,
                      [&] { return f.context.execute(plan.plan, f.bindings); });
  auto b = std::async(std::launch::async,
                      [&] { return f.context.execute(plan.plan, f.bindings); });
  oracle(take(a.get()), Region::whole({2, 3}), {73});
  oracle(take(b.get()), Region::whole({2, 3}), {73});
#if defined(__APPLE__) && defined(__aarch64__)
  f.document.nodes[0].operation =
      "channel.literal_like_accelerated_apple_silicon";
  oracle(take(f.execute()), Region::whole({2, 3}), {73});
  f.document.nodes[0].operation = "channel.literal_like_accelerated_x86_64";
  check(!f.compile().ok(), "unavailable profile rejects before execution");
  f.node({73}, -1, false);
  plan = take(f.compile());
#endif
  CancellationSource cancelled;
  cancelled.cancel();
  check(f.context.execute(plan.plan, f.bindings, cancelled.token())
                .status()
                .code == ErrorCode::Cancelled,
        "literal precancellation");
  ExecutionOptions work;
  work.maximum_dependency_work = 1;
  check(f.context.execute(plan.plan, f.bindings, {}, work).status().code ==
            ErrorCode::ResourceExhausted,
        "literal work budget");
  for (const char* key :
       {"bits", "expected_inputs", "layout", "output_description"}) {
    f.node({73}, -1, false);
    f.document.nodes[0].parameters[key] = std::string("invalid");
    check(!f.compile().ok(), "invalid static parameter");
  }
  Fixture rank_one(ElementType::UInt8, {3});
  rank_one.node({12}, 0, false);
  check(!rank_one.compile().ok(), "rank-zero rejected");
  Fixture spatial(ElementType::UInt8, {2, 3, 4}, [] {
    ResultTensorLayout layout;
    layout.spatial = true;
    return layout;
  }());
  for (bool keep : {false, true}) {
    spatial.node({12}, 0, keep);
    check(!spatial.compile().ok(),
          "literal cannot project a physical spatial axis");
  }
  f.node({73}, -1, false);
  f.document.nodes[0].parameters["bits"] = std::string("FF");
  check(!f.compile().ok(), "literal requires canonical lowercase hexadecimal");
  rank_one.node({12}, 0, true);
  oracle(take(rank_one.execute()), Region::whole({1}), {12});
}
}  // namespace
int main() try {
  bit_patterns();
  planar_batches();
  disjoint_metadata_lifetime();
  resource_lifetime();
  errors_empty_and_reuse();
  std::cout << "Result literal: raw bits, descriptor-only support, planar "
               "batches, Empty and reuse passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
