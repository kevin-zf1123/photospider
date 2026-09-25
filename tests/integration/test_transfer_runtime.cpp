#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/transfer_runtime.hpp"
#include "photospider/execution/resource_allocator.hpp"
#include "support/fmt_handoff.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using ps::handoff_testing::require;
using ps::handoff_testing::take;
using ps::plugin_internal::numeric_ops::SequenceProfile;
using Params = std::map<std::string, ParameterValue>;
Params parameters() {
  return {{"curve", std::string("pq")},
          {"metadata_mode", std::string("raw")},
          {"components", std::string("all")}};
}
PlanarImage source(ImagePlaneOrder order) {
  PlanarImageConfig config;
  config.order = order;
  auto image =
      take(PlanarImage::create({ElementType::Float64, {1, 64, 1}}, config));
  std::array<double, 64> bytes;
  bytes.fill(.5);
  take(image.publish(Region::whole(image.descriptor().shape),
                     reinterpret_cast<const std::uint8_t*>(bytes.data()),
                     sizeof(bytes)));
  return image;
}
WorkflowDocument document(const PlanarImage& image, const std::string& key) {
  auto result = ps::handoff_testing::probe_document(image);
  result.nodes[0].operation = key;
  result.nodes[0].parameters = parameters();
  return result;
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
    const auto image = source(order);
    GraphContext graph(document(image, "color.transfer_decode_strict"));
    auto plan = take(Compiler(registry).compile(graph));
    for (auto fuel : {UINT64_C(0), UINT64_C(64), UINT64_C(4096), UINT64_MAX}) {
      ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.managed_resources = ResourceLimits{};
      config.managed_resources->maximum_work = fuel;
      ExecutionContext context(registry, config);
      for (unsigned replay = 0; replay < 2; ++replay) {
        auto result = context.execute(
            plan.plan, ps::handoff_testing::probe_bindings(image), {},
            options());
        const auto issued =
            take(context.resource_budget()).statistics().issued.work;
        if (fuel == UINT64_MAX) {
          auto success = take(std::move(result));
          std::uint64_t evaluated = 0;
          for (const auto& t : success.diagnostics.operation_timings)
            evaluated += t.numeric.evaluated_values;
          require(evaluated == 64 && issued > 64,
                  "planar refinement work must reach root budget");
        } else {
          require(!result.ok() &&
                      result.status().code == ErrorCode::ResourceExhausted,
                  "planar numeric work bypassed managed root");
          require(issued <= fuel, "managed root work overspent");
        }
      }
    }
  }
}
void mid_refinement() {
  // Invoke the real transfer operation through Workflow/registry. Wrap ONLY
  // its borrowed service to replace the graph at a deterministic inner work
  // checkpoint, without sleeps, extra product APIs, or timing-dependent races.
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto registry = std::make_shared<OperationRegistry>();
    auto op = ps::plugin_internal::transfer_ops::operation(
        false, SequenceProfile::Strict, "_runtime_probe");
    const auto original = op.planar_callback;
    GraphContext* graph = nullptr;
    WorkflowDocument doc;
    CancellationSource stop;
    unsigned checkpoints = 0;
    ErrorCode inner = ErrorCode::Ok;
    std::uint64_t math_calls = 0;
    op.planar_callback = [&](const PlanarOperationInvocation& call) {
      require(static_cast<bool>(call.consume_work), "missing host checkpoint");
      auto instrumented = call;
      instrumented.consume_work = [&](std::uint64_t amount) {
        if (++checkpoints == 256) {
          if (mode != 0)
            stop.cancel();
          if (mode != 1)
            static_cast<void>(graph->replace(doc));
        }
        const auto status = call.consume_work(amount);
        if (!status.ok())
          inner = status.code;
        return status;
      };
      instrumented.report_numeric = [&](const NumericDiagnostics& report) {
        math_calls += report.strict_math_calls;
        return call.report_numeric(report);
      };
      return original(instrumented);
    };
    const auto key = op.key;
    take(registry->register_operation(std::move(op)));
    take(registry->freeze());
    auto image = source(ImagePlaneOrder::Tiled);
    doc = document(image, key);
    GraphContext context_graph(doc);
    graph = &context_graph;
    auto plan = take(Compiler(registry).compile(*graph));
    ExecutionContext context(registry);
    auto result =
        context.execute(plan.plan, ps::handoff_testing::probe_bindings(image),
                        stop.token(), options());
    const auto expected = mode == 0 ? ErrorCode::Stale : ErrorCode::Cancelled;
    require(!result.ok() && result.status().code == expected,
            "obsolete/cancelled transfer completed successfully");
    require(inner == expected && checkpoints == 256 && math_calls == 1,
            "FMT09 did not stop at the injected inner refinement checkpoint");
  }
}
void host_services() {
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto registry = std::make_shared<OperationRegistry>();
    auto probe = std::make_shared<ps::handoff_testing::Probe>();
    auto op = ps::handoff_testing::probe_operation(probe);
    auto original = op.planar_callback;
    GraphContext* graph = nullptr;
    WorkflowDocument doc;
    const ResourceBudget* root = nullptr;
    ErrorCode observed = ErrorCode::Ok;
    op.planar_callback = [&](const PlanarOperationInvocation& call) {
      const auto* tls = resource_internal::metadata_budget();
      require(tls && root && tls->same_owner(*root), "planar TLS root missing");
      ResourceVector<std::uint64_t> temporary(32, 7);
      require(temporary.get_allocator().owned_by(*root),
              "temporary not root-owned");
      require(static_cast<bool>(call.consume_work), "host service missing");
      take(call.consume_work(0));
      if (mode == 1)
        static_cast<void>(graph->replace(doc));
      auto status = call.consume_work(mode == 2 ? UINT64_C(101) : 7);
      observed = status.code;
      if (mode == 3) {
        // An ignored managed allocation failure is also sticky. Nothing gets
        // physically allocated for this impossible reservation.
        try {
          ResourceVector<std::uint8_t> denied;
          denied.resize(1U << 20);
        } catch (const std::bad_alloc&) {
        }
      }
      // Deliberately ignore Stale/WorkLimit: host must prevent publication.
      return original(call);
    };
    take(registry->register_operation(std::move(op)));
    take(registry->freeze());
    auto image = ps::handoff_testing::probe_image();
    doc = ps::handoff_testing::probe_document(image);
    GraphContext context_graph(doc);
    graph = &context_graph;
    auto plan = take(Compiler(registry).compile(*graph));
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->maximum_work = 100;
    if (mode == 3)
      config.managed_resources->capacity[ResourceKind::Metadata] = 1U << 18;
    ExecutionContext context(registry, config);
    auto budget = take(context.resource_budget());
    root = &budget;
    auto result =
        context.execute(plan.plan, ps::handoff_testing::probe_bindings(image));
    if (mode == 0) {
      take(std::move(result));
      require(observed == ErrorCode::Ok && budget.statistics().issued.work == 7,
              "planar service did not charge exact work");
    } else {
      const auto expected =
          mode == 1 ? ErrorCode::Stale : ErrorCode::ResourceExhausted;
      require(!result.ok() && result.status().code == expected,
              "ignored host-service failure escaped publication gate");
      if (mode != 3)
        require(observed == expected, "failure not seen within callback");
    }
  }
}
void dirty_mapping() {
  auto registry = make_default_operation_registry();
  for (bool encode : {false, true})
    for (unsigned rank = 1; rank <= 5; ++rank) {
      std::vector<std::uint64_t> shape(rank, 5);
      OperationMetadata metadata;
      metadata.descriptor = {ElementType::Float64, shape};
      auto params = parameters();
      params["curve"] = std::string("srgb");
      if (rank > 1) {
        params["components"] = std::string("1,3");
        params["axis"] = std::int64_t{1};
      }
      auto prepared = take(
          registry->prepare_operation(encode ? "color.transfer_encode_strict"
                                             : "color.transfer_decode_strict",
                                      {metadata}, params));
      const auto& output = prepared->traits().outputs[0];
      require(output.static_dependency_pieces.has_value(),
              "missing exact relation");
      auto all = take(Footprint::all(shape));
      auto relation = take(DependencyCertificate::create_mapped(
          "fmt09-dirty-test", all, {shape}, *output.static_dependency_pieces));
      for (unsigned channel = 0; channel < 5; ++channel) {
        std::vector<RegionDimension> dims(rank, {1, 1});
        dims[rank > 1 ? 1 : 0] = {channel, 1};
        auto dirty = take(Footprint::from_regions(shape, {Region(dims)}));
        auto mapped = take(relation.transpose(
            {0, static_cast<std::uint32_t>(DependencyRole::Data), dirty, {}}));
        require(mapped == dirty, "dirty mapping widened to peer components");
        auto backward = take(relation.backward(dirty));
        require(backward.size() == 1 && backward[0].samples == dirty,
                "input demand is not exact pointwise support");
      }
    }
}
void whole_failure() {
  for (const auto* curve : {"linear", "srgb"}) {
    auto registry = make_default_operation_registry(false);
    OperationDefinition producer;
    producer.key = "test.fmt09_whole_failure";
    auto& output = producer.traits.outputs[0];
    output.output_element_type = ElementType::Float64;
    output.shape_rule = OperationShapeRule::Fixed;
    output.fixed_output_shape = {2, 3, 4};
    output.region_rule = OperationRegionRule::Whole;
    unsigned calls = 0;
    producer.callback = [&](const OperationInvocation& call) {
      ++calls;
      require(take(call.output_region.element_count()) == 24,
              "Whole producer was incorrectly restricted");
      return Result<Value>(
          Status{ErrorCode::OperationFailed, "whole-upstream-sentinel"});
    };
    take(registry->register_operation(std::move(producer)));
    take(registry->freeze());
    auto params = parameters();
    params["curve"] = std::string(curve);
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
            "identity/alpha-only request bypassed Whole upstream failure");
  }
}
void retained_identity() {
  const std::array<std::uint64_t, 5> words = {
      0, UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000055),
      UINT64_C(0xfff800000000abcd), UINT64_C(0x3ff0000000000000)};
  const Region roi({{1, 3}});
  for (const auto* policy : {"auto", "view", "materialize"}) {
    Value retained;
    std::weak_ptr<const CpuStorage> old_owner;
    {
      std::vector<std::uint8_t> bytes(sizeof(words));
      std::memcpy(bytes.data(), words.data(), sizeof(words));
      auto source = take(Value::create({ElementType::Float64, {5}},
                                       Region::whole({5}), {0, {8}}, bytes));
      old_owner = source.storage();
      auto registry = make_default_operation_registry();
      auto params = parameters();
      params["curve"] = std::string("linear");
      params["layout"] = std::string(policy);
      WorkflowDocument doc;
      doc.inputs = {{1,
                     "source",
                     source.descriptor(),
                     source.region(),
                     source.layout(),
                     {}}};
      doc.nodes = {{1,
                    "color.transfer_encode_strict",
                    {WorkflowInputReference{1}},
                    params}};
      doc.outputs = {{"result", 1, "values"}};
      GraphContext graph(doc);
      PlanningOptions planning;
      planning.output_regions = {{"result", roi}};
      auto plan = take(Compiler(registry).compile(graph, planning));
      ExecutionContext context(registry);
      auto result = take(context.execute(plan.plan, {{{"source", source}}}));
      retained = result.values.at("result");
      require((retained.storage()->bytes().data() ==
               source.storage()->bytes().data()) ==
                  (std::string(policy) != "materialize"),
              "identity storage policy mismatch");
    }
    require(
        old_owner.expired() == (std::string(policy) == "materialize"),
        "retained view did not own its source beyond graph/context lifetime");
    for (std::uint64_t i = 1; i < 4; ++i) {
      std::uint64_t bits = 0;
      std::memcpy(
          &bits, retained.bytes().data() + take(retained.byte_address({i})), 8);
      require(bits == words[i],
              "view/materialize changed raw signed-zero/NaN bits");
    }
    retained = {};
    require(old_owner.expired(), "last view release failed to retire owner");
  }
  for (auto order : {ImagePlaneOrder::Tiled, ImagePlaneOrder::Continuous})
    for (const auto* policy : {"auto", "view", "materialize"}) {
      PlanarImage retained;
      const Region region({{0, 1}, {1, 3}, {0, 1}});
      {
        PlanarImageConfig config;
        config.order = order;
        auto image = take(
            PlanarImage::create({ElementType::Float64, {1, 5, 1}}, config));
        take(image.publish(Region::whole({1, 5, 1}),
                           reinterpret_cast<const std::uint8_t*>(words.data()),
                           sizeof(words)));
        auto params = parameters();
        params["curve"] = std::string("linear");
        params["layout"] = std::string(policy);
        auto doc = document(image, "color.transfer_decode_strict");
        doc.nodes[0].parameters = params;
        auto registry = make_default_operation_registry();
        GraphContext graph(doc);
        PlanningOptions planning;
        planning.output_regions = {{"result", region}};
        auto plan = take(Compiler(registry).compile(graph, planning));
        ExecutionContext context(registry);
        auto result = take(context.execute(
            plan.plan, ps::handoff_testing::probe_bindings(image)));
        retained = result.images.at("result");
        require((retained.owner_token() == image.owner_token()) ==
                    (std::string(policy) != "materialize"),
                "planar identity storage policy mismatch");
      }
      std::array<std::uint64_t, 3> actual{};
      take(retained.read(region, reinterpret_cast<std::uint8_t*>(actual.data()),
                         sizeof(actual)));
      for (unsigned i = 0; i < 3; ++i)
        require(actual[i] == words[i + 1],
                "planar retained owner/bit equivalence");
    }
}
}  // namespace
int main() try {
  dirty_mapping();
  whole_failure();
  retained_identity();
  host_services();
  root_budget();
  mid_refinement();
  std::cout << "FMT-09 runtime: root budget, TLS allocation, sticky failures, "
               "inner Stale/cancellation, Whole failure, exact dirty map, "
               "retained views PASS\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FMT-09 runtime: " << error.what() << '\n';
  return 1;
}
