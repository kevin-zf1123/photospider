#include <array>
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
#include "photospider/execution/resource_allocator.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using transfer_fixture::require;
using transfer_fixture::take;
using Params = std::map<std::string, ParameterValue>;
Params parameters(const std::string& curve = "pq") {
  return {{"curve", curve},
          {"metadata_mode", std::string("raw")},
          {"components", std::string("all")}};
}
channel_fixture::Source source(ImagePlaneOrder order) {
  ResultTensorLayout layout;
  layout.spatial = true;
  layout.order = order;
  auto source =
      channel_fixture::source({ElementType::Float64, {1, 64, 1}}, {}, layout);
  const double half = .5;
  for (std::size_t i = 0; i < 64; ++i)
    std::memcpy(source.bytes.data() + i * 8, &half, 8);
  return source;
}
ExecutionOptions options() {
  ExecutionOptions result;
  result.maximum_dependency_work = UINT64_MAX;
  result.dependencies.maximum_work = UINT64_MAX;
  result.dependencies.sets.maximum_work = UINT64_MAX;
  return result;
}
void root_budget() {
  auto registry = make_default_operation_registry();
  for (auto order : {ImagePlaneOrder::Tiled, ImagePlaneOrder::Continuous}) {
    const auto input = source(order);
    ExecutionContext measuring(registry);
    auto baseline =
        channel_fixture::publish(take(measuring.resource_budget()), input);
    const auto setup =
        take(measuring.resource_budget()).statistics().issued.work;
    GraphContext graph(transfer_fixture::document(
        baseline, "color.transfer_decode_strict", parameters()));
    auto plan = take(Compiler(registry).compile(graph));
    for (auto fuel : {UINT64_C(0), UINT64_C(64), UINT64_C(4096), UINT64_MAX}) {
      ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.managed_resources = ResourceLimits{};
      config.managed_resources->maximum_work =
          fuel == UINT64_MAX ? fuel : setup + fuel;
      ExecutionContext context(registry, config);
      const auto root = take(context.resource_budget());
      auto bound = channel_fixture::publish(root, input);
      for (unsigned replay = 0; replay < 2; ++replay) {
        auto result = context.execute(
            plan.plan, transfer_fixture::bindings(bound), {}, options());
        const auto issued = root.statistics().issued.work;
        if (fuel == UINT64_MAX) {
          auto success = take(std::move(result));
          std::uint64_t evaluated = 0;
          for (const auto& timing : success.diagnostics.operation_timings)
            evaluated += timing.numeric.evaluated_values;
          require(evaluated == 64 && issued > setup + 64,
                  "refinement work reaches Root");
        } else {
          require(!result.ok() &&
                      result.status().code == ErrorCode::ResourceExhausted,
                  "conversion bypassed managed Root work");
          require(issued <= setup + fuel, "managed Root work overspent");
        }
      }
    }
  }
}
void mid_refinement() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto hooks = std::make_shared<transfer_fixture::Hooks>();
    GraphContext* graph = nullptr;
    WorkflowDocument doc;
    CancellationSource stop;
    unsigned checkpoints = 0;
    ErrorCode inner = ErrorCode::Ok;
    std::uint64_t math_calls = 0;
    hooks->charge = [&](std::uint64_t n, const ResultProgramPhase& phase) {
      if (++checkpoints == 256) {
        if (mode != 0)
          stop.cancel();
        if (mode != 1)
          static_cast<void>(graph->replace(doc));
      }
      auto status = phase.consume_work(n);
      if (!status.ok())
        inner = status.code;
      return status;
    };
    hooks->report = [&](const NumericDiagnostics& report,
                        const ResultProgramPhase& phase) {
      math_calls += report.strict_math_calls;
      return phase.report_numeric(report);
    };
    auto registry =
        transfer_fixture::registry("color.transfer_decode_strict", hooks);
    ExecutionContext context(registry);
    auto bound = channel_fixture::publish(take(context.resource_budget()),
                                          source(ImagePlaneOrder::Tiled));
    doc = transfer_fixture::document(bound, "color.transfer_decode_strict",
                                     parameters());
    GraphContext actual(doc);
    graph = &actual;
    auto plan = take(Compiler(registry).compile(actual));
    auto result = context.execute(plan.plan, transfer_fixture::bindings(bound),
                                  stop.token(), options());
    const auto expected = mode == 0 ? ErrorCode::Stale : ErrorCode::Cancelled;
    require(!result.ok() && result.status().code == expected,
            "inner stop state lost");
    if (inner != expected || checkpoints != 256 || math_calls != 1)
      std::cerr << "inner=" << static_cast<int>(inner)
                << " checkpoints=" << checkpoints
                << " math_calls=" << math_calls << '\n';
    require(inner == expected && checkpoints == 256 && math_calls == 1,
            "refinement continued beyond injected checkpoint");
  }
}
void host_services() {
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto hooks = std::make_shared<transfer_fixture::Hooks>();
    GraphContext* graph = nullptr;
    WorkflowDocument doc;
    const ResourceBudget* root = nullptr;
    ErrorCode observed = ErrorCode::Ok;
    std::uint64_t charged = 0;
    constexpr std::uint64_t limit = 1000000;
    hooks->before = [&](const ResultProgramPhase& phase) {
      const auto* tls = resource_internal::metadata_budget();
      require(tls && root && tls->same_owner(*root), "Result TLS Root missing");
      ResourceVector<std::uint64_t> temporary(32, 7);
      require(temporary.get_allocator().owned_by(*root),
              "temporary not Root-owned");
      take(phase.consume_work(0));
      if (mode == 1)
        static_cast<void>(graph->replace(doc));
      const auto before = root->statistics().issued.work;
      auto status = phase.consume_work(mode == 2 ? limit - before + 1 : 7);
      charged = root->statistics().issued.work - before;
      observed = status.code;
      if (mode == 3) {
        try {
          ResourceVector<std::uint8_t> denied;
          denied.resize(1U << 20);
        } catch (const std::bad_alloc&) {
        }
      }
    };
    auto registry =
        transfer_fixture::registry("color.transfer_decode_strict", hooks);
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->maximum_work = limit;
    if (mode == 3)
      config.managed_resources->capacity[ResourceKind::Metadata] = 1U << 18;
    ExecutionContext context(registry, config);
    const auto budget = take(context.resource_budget());
    root = &budget;
    auto bound =
        channel_fixture::publish(budget, source(ImagePlaneOrder::Continuous));
    doc = transfer_fixture::document(bound, "color.transfer_decode_strict",
                                     parameters("linear"));
    GraphContext actual(doc);
    graph = &actual;
    auto plan = take(Compiler(registry).compile(actual));
    auto result = context.execute(plan.plan, transfer_fixture::bindings(bound),
                                  {}, options());
    if (!mode) {
      take(std::move(result));
      require(observed == ErrorCode::Ok && charged == 7,
              "host work charged incorrectly");
    } else {
      const auto expected =
          mode == 1 ? ErrorCode::Stale : ErrorCode::ResourceExhausted;
      require(!result.ok() && result.status().code == expected,
              "ignored host failure escaped publication gate");
      if (mode != 3)
        require(observed == expected, "callback did not observe host failure");
    }
  }
}
void dirty_mapping() {
  auto registry = make_default_operation_registry();
  for (bool encode : {false, true})
    for (unsigned rank = 1; rank <= 5; ++rank) {
      std::vector<std::uint64_t> shape(rank, 5);
      auto input = channel_fixture::source({ElementType::Float64, shape});
      const double half = .5;
      for (std::size_t i = 0; i < input.bytes.size(); i += 8)
        std::memcpy(input.bytes.data() + i, &half, 8);
      ExecutionContext context(registry);
      auto bound =
          channel_fixture::publish(take(context.resource_budget()), input);
      auto params = parameters("power_gamma");
      params["gamma"] = 2.;
      if (rank > 1) {
        params["components"] = std::string("1,3");
        params["axis"] = std::int64_t{1};
      }
      GraphContext graph(
          transfer_fixture::document(bound,
                                     encode ? "color.transfer_encode_strict"
                                            : "color.transfer_decode_strict",
                                     params));
      auto plan = take(Compiler(registry).compile(graph));
      auto frozen =
          take(context.freeze(plan.plan, transfer_fixture::bindings(bound)));
      for (unsigned channel = 0; channel < 5; ++channel) {
        std::vector<RegionDimension> dims(rank, {1, 1});
        dims[rank > 1 ? 1 : 0] = {channel, 1};
        auto dirty = take(Footprint::from_regions(shape, {Region(dims)}));
        auto result = take(context.execute_fragments(
            frozen, {{"result", dirty}}, {}, options()));
        auto mapped =
            take(result.dependencies.potential_dirty(
                     "input", dirty, 1, {}, ResultSupportTarget::Tensor, 0))
                .at("result");
        require(mapped == dirty, "dirty support widened to peers");
        require(take(result.dependencies.source_support()).at("input") == dirty,
                "backward demand not exact");
      }
    }
}
struct WholeFailure {
  unsigned* calls;
  explicit WholeFailure(unsigned* c) : calls(c) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++*calls;
    require(phase.query.tensor_outputs &&
                take(phase.query.tensor_outputs->element_count()) == 24,
            "Whole producer restricted to downstream ROI");
    return Result<ResultProgramPoll>(
        Status{ErrorCode::OperationFailed, "whole-upstream-sentinel"});
  }
};
void whole_failure() {
  for (const auto* curve : {"linear", "srgb"}) {
    auto registry = make_default_operation_registry(false);
    OperationDefinition producer;
    producer.key = "test.fmt09_whole_failure";
    producer.traits.input_count = 0;
    auto& output = producer.traits.outputs[0];
    output.output_schema.kind = OperationPortKind::Result;
    output.result_schema =
        channel_fixture::source({ElementType::Float64, {2, 3, 4}}).schema;
    output.output_schema.result_schema_id = output.result_schema->id;
    output.output_schema.result_schema_version = output.result_schema->version;
    output.output_schema.tensor_key = "samples";
    output.region_rule = OperationRegionRule::Whole;
    output.dependency_version = 2;
    output.maximum_dependency_stages = 1;
    output.continuation_bytes = sizeof(WholeFailure);
    unsigned calls = 0;
    producer.start_result = [&](const auto&, const auto& allocator) {
      return ResultContinuation::make<WholeFailure>(allocator, &calls);
    };
    take(registry->register_operation(std::move(producer)));
    take(registry->freeze());
    auto params = parameters(curve);
    params["components"] = std::string("0,1,2");
    params["axis"] = std::int64_t{2};
    WorkflowDocument doc;
    doc.nodes = {{1, "test.fmt09_whole_failure", {}, {}},
                 {2,
                  "color.transfer_decode_strict",
                  {WorkflowNodeOutput{1, "value"}},
                  params}};
    doc.outputs = {{"result", 2, "values"}};
    GraphContext graph(doc);
    PlanningOptions planning;
    planning.output_regions = {{"result", Region({{1, 1}, {2, 1}, {3, 1}})}};
    auto plan = take(Compiler(registry).compile(graph, planning));
    ExecutionContext context(registry);
    auto result = context.execute(plan.plan);
    require(!result.ok() &&
                result.status().code == ErrorCode::OperationFailed &&
                calls == 1,
            "alpha-only identity bypassed Whole upstream failure");
  }
}
void retained_identity() {
  const std::array<std::uint64_t, 5> words = {
      0, UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000055),
      UINT64_C(0xfff800000000abcd), UINT64_C(0x3ff0000000000000)};
  for (unsigned storage = 0; storage < 3; ++storage)
    for (const auto* policy : {"auto", "view", "materialize"}) {
      ResultRef retained;
      ResourceBudget root;
      const Region region =
          storage ? Region({{0, 1}, {1, 3}, {0, 1}}) : Region({{1, 3}});
      {
        ResultTensorLayout layout;
        layout.spatial = storage != 0;
        layout.order =
            storage == 2 ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
        auto fixture = channel_fixture::source(
            {ElementType::Float64, storage ? std::vector<std::uint64_t>{1, 5, 1}
                                           : std::vector<std::uint64_t>{5}},
            {}, layout);
        std::memcpy(fixture.bytes.data(), words.data(), sizeof(words));
        auto registry = make_default_operation_registry();
        ExecutionContext context(registry);
        root = take(context.resource_budget());
        auto source = channel_fixture::publish(root, fixture);
        auto params = parameters("linear");
        params["layout"] = std::string(policy);
        GraphContext graph(transfer_fixture::document(
            source, "color.transfer_decode_strict", params));
        PlanningOptions planning;
        planning.output_regions = {{"result", region}};
        auto plan = take(Compiler(registry).compile(graph, planning));
        auto result = take(context.execute(
            plan.plan, transfer_fixture::bindings(source), {}, options()));
        retained = result.results.at("result");
        require((channel_fixture::owner(retained, region) ==
                 channel_fixture::owner(source, region)) ==
                    (std::string(policy) != "materialize"),
                "identity owner policy mismatch");
        std::uint64_t viewed = 0, copied = 0;
        for (const auto& timing : result.diagnostics.operation_timings) {
          viewed += timing.numeric.view_elements;
          copied += timing.numeric.copied_elements;
        }
        require(viewed == (std::string(policy) == "materialize" ? 0U : 3U) &&
                    copied == (std::string(policy) == "materialize" ? 3U : 0U),
                "identity publication counters");
      }
      const auto bytes = channel_fixture::read(retained, region);
      require(bytes.size() == 24 &&
                  !std::memcmp(bytes.data(), words.data() + 1, 24),
              "retained signed-zero and NaN bits");
      retained = {};
      require(root.statistics().live[ResourceKind::Payload] == 0,
              "last view release retained payload");
    }
}
std::vector<std::uint8_t> rgb_profile() {
  const auto base = numeric_fixture::fixture();
  const auto word = [&](std::size_t at) {
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) {
      n = (n << 8) | base[at + i];
    }
    return n;
  };
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> tags;
  for (unsigned i = 0; i < 3; ++i) {
    const auto offset = word(136 + 12 * i), size = word(140 + 12 * i);
    tags.push_back(
        {std::string(reinterpret_cast<const char*>(base.data() + 132 + 12 * i),
                     4),
         {base.begin() + offset, base.begin() + offset + size}});
  }
  tags.push_back({"A2B0", numeric_fixture::table(3, 3)});
  tags.push_back({"B2A0", numeric_fixture::table(3, 3)});
  std::vector<std::uint8_t> out(base.begin(), base.begin() + 128);
  out.resize(132 + 12 * tags.size());
  numeric_fixture::word(&out, 128, tags.size());
  numeric_fixture::key(&out, 12, "mntr");
  numeric_fixture::key(&out, 16, "RGB ");
  numeric_fixture::key(&out, 20, "XYZ ");
  for (std::size_t i = 0; i < tags.size(); ++i) {
    numeric_fixture::key(&out, 132 + 12 * i, tags[i].first.c_str());
    numeric_fixture::word(&out, 136 + 12 * i, out.size());
    numeric_fixture::word(&out, 140 + 12 * i, tags[i].second.size());
    out.insert(out.end(), tags[i].second.begin(), tags[i].second.end());
    while (out.size() % 4) {
      out.push_back(0);
    }
  }
  numeric_fixture::word(&out, 0, out.size());
  return out;
}
void result_batches_metadata_resources() {
  for (unsigned storage = 0; storage < 3; ++storage) {
    ResultRef retained;
    ColorProfileIdentity identity;
    ResourceBudget root;
    const Region roi({{1, 1}, {0, 1}, {1, 1}, {0, 3}, {0, 1}});
    {
      ResourceBudget profile_root;
      const auto bytes = rgb_profile();
      auto profile = take(IccProfile::import(
          ByteView(bytes.data(), bytes.size()), profile_root));
      identity = profile.identity();
      TensorDescription d;
      d.channel_axis = 2;
      d.channels = {{"R", "red", "relative"},
                    {"G", "green", "relative"},
                    {"B", "blue", "relative"},
                    {"A", "alpha", "coverage"},
                    {"B2", "blue", "relative"}};
      TensorColorGroup g;
      g.name = "color";
      g.indices = {0, 1, 2};
      g.alpha = 3;
      g.components = {d.channels[0], d.channels[1], d.channels[2]};
      auto& interpretation = g.interpretation;
      interpretation.model = "rgb";
      interpretation.primaries = "srgb";
      interpretation.transfer = "linear";
      interpretation.reference = "scene_relative";
      interpretation.association = "straight";
      interpretation.profile = identity;
      interpretation.analytic_binding.emplace();
      auto& binding = *interpretation.analytic_binding;
      binding.model = "rgb";
      binding.primaries = "srgb";
      binding.transfer = "linear";
      binding.reference = "scene_relative";
      binding.roles = {"red", "green", "blue"};
      binding.units = {"relative", "relative", "relative"};
      d.channels[4].interpretation = interpretation;
      d.groups.push_back(g);
      g.name = "full";
      d.groups.push_back(g);
      g.name = "partial";
      g.indices = {0, 1, 4};
      g.components = {d.channels[0], d.channels[1], d.channels[4]};
      g.components[2].interpretation.reset();
      d.groups.push_back(g);
      ResultTensorLayout layout;
      layout.spatial = storage != 0;
      layout.order =
          storage == 2 ? ImagePlaneOrder::Tiled : ImagePlaneOrder::Continuous;
      auto input = channel_fixture::source({ElementType::Float64, {2, 3, 5}},
                                           {take(encode_tensor_description(d))},
                                           layout, {2, 2});
      input.resources = take(ResourceBindings::create({profile}, profile_root));
      const std::array<double, 5> values{
          .25, std::numeric_limits<double>::quiet_NaN(), .5,
          std::numeric_limits<double>::quiet_NaN(), .75};
      for (std::size_t i = 0; i < input.bytes.size() / 8; ++i)
        std::memcpy(input.bytes.data() + i * 8, &values[i % 5], 8);
      auto registry = make_default_operation_registry();
      auto params = Params{{"group", std::string("color")},
                           {"curve", std::string("linear")},
                           {"layout", std::string("view")}};
      // Static interpretation conflicts are distinct from sample validation.
      for (bool absent : {false, true}) {
        auto bad = d;
        bad.groups.resize(1);
        if (absent)
          bad.groups[0].interpretation.analytic_binding.reset();
        else
          bad.groups[0].interpretation.analytic_binding->transfer = "srgb";
        auto schema = input.schema;
        schema.tensors[0].facets = {take(encode_tensor_description(bad))};
        OperationMetadata metadata;
        metadata.result_schema = std::make_shared<SchemaTemplate>(schema);
        auto rejected = registry->prepare_operation(
            "color.transfer_decode_strict", {metadata}, params);
        require(!rejected.ok(), "analytic binding absence/conflict accepted");
      }
      ExecutionContext context(registry);
      root = take(context.resource_budget());
      auto source = channel_fixture::publish(root, input);
      auto doc = transfer_fixture::document(
          source, "color.transfer_decode_strict", params);
      GraphContext graph(doc);
      PlanningOptions planning;
      planning.output_regions = {{"result", roi}};
      auto compiled =
          take(Compiler(registry).compile(graph, planning, input.resources));
      const auto bindings = transfer_fixture::bindings(source);
      auto a = std::async(std::launch::async, [&] {
        return context.execute(compiled.plan, bindings, {}, options());
      });
      auto b = std::async(std::launch::async, [&] {
        return context.execute(compiled.plan, bindings, {}, options());
      });
      auto first = take(a.get());
      auto second = take(b.get());
      retained = first.results.at("result");
      require(channel_fixture::read(retained, roi) ==
                  channel_fixture::read(second.results.at("result"), roi),
              "concurrent preparation reuse changed identity");
      require(channel_fixture::owner(retained, roi) ==
                  channel_fixture::owner(source, roi),
              "semantic identity lost legal owner view");
      require(retained.association().size() == 1 &&
                  retained.association()[0] == source.object_id(),
              "transfer input association");
      const auto& spec = transfer_fixture::spec(retained);
      require(spec.batch_axes == input.schema.tensors[0].batch_axes &&
                  spec.atomic_trailing_axes == 0,
              "transfer lost batches or retained tuple publication");
      const auto description = take(decode_tensor_description(spec.facets[0]));
      require(description.groups.size() == 2 &&
                  description.groups[1].name == "full" &&
                  !description.groups[0].interpretation.profile &&
                  description.channels[4].interpretation->profile == identity,
              "overlap projection or analytic binding update");
      require(retained.resources().icc_profile(identity).ok(),
              "unselected ICC owner lost");
      const auto shape = spec.sample_shape();
      const auto query = take(Footprint::from_regions(shape, {roi}));
      require(take(first.dependencies.source_support()).at("input") == query,
              "batched input support widened");
      auto dirty =
          take(first.dependencies.potential_dirty(
                   "input", query, 4, {}, ResultSupportTarget::Tensor, 0))
              .at("result");
      require(dirty == query, "semantic Validation mapping lost");
      auto converted_doc =
          transfer_fixture::document(source, "color.transfer_encode_strict",
                                     {{"group", std::string("color")},
                                      {"curve", std::string("power_gamma")},
                                      {"gamma", 2.0},
                                      {"layout", std::string("materialize")}});
      GraphContext converted_graph(converted_doc);
      auto converted_plan = take(Compiler(registry).compile(
          converted_graph, planning, input.resources));
      auto converted =
          take(context.execute(converted_plan.plan, bindings, {}, options()));
      const auto& output = converted.results.at("result");
      require(channel_fixture::owner(output, roi) !=
                  channel_fixture::owner(source, roi),
              "batched arithmetic did not materialize");
      const auto transformed = channel_fixture::read(output, roi);
      require(transformed.size() == 24, "batched arithmetic coverage");
      for (unsigned i = 0; i < 3; ++i) {
        double value;
        std::memcpy(&value, transformed.data() + i * 8, 8);
        require(value == .5, "batched gamma encoding sample");
      }
    }
    require(retained.resources().icc_profile(identity).ok(),
            "ICC lifetime ended with context");
    auto bytes = channel_fixture::read(retained, roi);
    require(bytes.size() == 24, "batched retained coverage");
    for (unsigned i = 0; i < 3; ++i) {
      double value;
      std::memcpy(&value, bytes.data() + i * 8, 8);
      require(value == .25, "batched retained sample");
    }
    retained = {};
    require(root.statistics().live[ResourceKind::Payload] == 0,
            "transfer final owner did not release payload");
  }
}
}  // namespace
int main() try {
  std::cerr << "dirty mapping\n";
  dirty_mapping();
  std::cerr << "Whole failure\n";
  whole_failure();
  std::cerr << "retained identity\n";
  retained_identity();
  std::cerr << "host services\n";
  host_services();
  std::cerr << "Result batches/metadata/resources\n";
  result_batches_metadata_resources();
  std::cerr << "Root budget\n";
  root_budget();
  std::cerr << "mid-refinement stop\n";
  mid_refinement();
  std::cout << "FMT-09 Result runtime checks PASS\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FMT-09 runtime: " << error.what() << '\n';
  return 1;
}
