#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "photospider/plugin/tile_helpers.hpp"
#include "support/icc_profile_fixture.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct WorkflowCounts {
  unsigned starts = 0, destroys = 0, polls = 0;
};
struct TensorSource {
  std::shared_ptr<WorkflowCounts> counts;
  bool fail;
  TensorSource(std::shared_ptr<WorkflowCounts> observed, bool fail)
      : counts(std::move(observed)), fail(fail) {}
  ~TensorSource() { ++counts->destroys; }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++counts->polls;
    const auto& schema = *phase.query.output.result_schema;
    const auto& demand = *phase.query.tensor_outputs;
    if (fail && !demand.empty())
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed,
                 "source domain failure",
                 FailureReason::InvalidDomain,
                 {FailureOrigin::Domain, FailureScope::Group}});
    auto builder = take(ResultBuilder::start(phase.resources, schema,
                                             phase.query.semantic_key, {}, {},
                                             phase.query.tile_extent));

    for (const auto& box : demand.boxes()) {
      ResourceVector<double> values{ResourceAllocator<double>(phase.resources)};
      for (std::uint64_t i = 0; i < box.dimensions().back().extent; ++i)
        values.push_back(box.dimensions().back().offset + i + 2);
      auto bytes = take(phase.allocator.allocate(values.size() * 8));
      std::memcpy(bytes.data(), values.data(), bytes.size());
      check(builder.publish_tensor(
          0, box,
          StridedLayout{
              0,
              {static_cast<std::int64_t>(values.size() * 8), 8},
              {box.dimensions()[0].offset, box.dimensions()[1].offset}},
          std::move(bytes).freeze()));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
OperationDefinition workflow_source(
    const std::shared_ptr<WorkflowCounts>& counts, bool fail, unsigned width) {
  OperationDefinition op;
  op.key = "test.tensor_source";
  op.traits.workspace_bytes = width * 8;
  op.traits.outputs = {multi_result::output(
      "value", multi_result::schema(ElementType::Float64, {1, width}))};
  op.traits.outputs[0].result_schema->tensors[0].layout.spatial = true;
  op.traits.outputs[0].result_schema->tensors[0].layout.channel_axis.reset();
  op.traits.outputs[0].continuation_bytes = sizeof(TensorSource);
  op.start_result = [counts, fail](const auto&, const auto& allocator) {
    ++counts->starts;
    return ResultContinuation::make<TensorSource>(allocator, counts, fail);
  };
  return op;
}
struct FailedScalar {
  Result<ResultProgramPoll> poll(const ResultProgramPhase&) {
    return Result<ResultProgramPoll>(
        Status{ErrorCode::OperationFailed,
               "scalar source failed",
               FailureReason::ShortIo,
               {FailureOrigin::Io, FailureScope::Group}});
  }
};
int empty_scalar_guard() {
  auto source_counts = std::make_shared<WorkflowCounts>();
  auto output_counts = std::make_shared<WorkflowCounts>();
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition source;
  source.key = "test.failed_scalar";
  source.traits.outputs = {multi_result::output(
      "value", multi_result::schema(ElementType::Float32, {1}))};
  source.start_result = [source_counts](const auto&, const auto& allocator) {
    ++source_counts->starts;
    return ResultContinuation::make<FailedScalar>(allocator);
  };
  check(registry->register_operation(std::move(source)));
  auto output = workflow_source(output_counts, 0, 4);
  output.key = "test.empty_scalar_guard";
  output.traits.input_count = 1;
  output.traits.input_schema.resize(1);
  auto& input = output.traits.input_schema[0];
  input.kind = OperationPortKind::Result;
  input.result_schema_id = "test.multi_output";
  input.result_schema_version = 1;
  input.scalar_bounds = true;
  input.minimum = 0;
  input.maximum = 1;
  check(registry->register_operation(std::move(output)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {
      {10, "test.failed_scalar", {}, {}},
      {20, "test.empty_scalar_guard", {WorkflowNodeOutput{10, "value"}}, {}}};
  document.outputs = {{"out", 20, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(
                       graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1}))
                  .plan;
  ExecutionContext context(registry, {1, false, 16, 1048576, 0});
  auto root = take(context.resource_budget());
  auto frozen = take(context.freeze(plan, {}));
  auto empty = context.execute_fragments(frozen, {{"out", TileRequest{}}});
  PS_CHECK(empty.ok());
  PS_CHECK(source_counts->starts == 0 && output_counts->starts == 0);
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
struct MetadataConsumer {
  unsigned mode, round = 0;
  bool denied;
  std::optional<ResultTensorInput>* retained;
  MetadataConsumer(unsigned selected, bool reject,
                   std::optional<ResultTensorInput>* escaped)
      : mode(selected), denied(reject), retained(escaped) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    const auto point =
        take(Footprint::from_regions({1, 4}, {Region({{0, 1}, {2, 1}})}));
    if (!round++) {
      ResultProgramNeed need;
      need.tiles.push_back({0, false, true, {}});
      if (mode == 2)
        need.tiles.push_back(take(sdk::tile_need_for_samples(
            phase.resources, phase.query, 0, 0, point)));
      return Answer(std::move(need));
    }
    const auto& input = phase.tensors->at({0, 0});
    if (input.spec().sample_shape() != std::vector<std::uint64_t>{1, 4} ||
        input.coverage() !=
            (mode == 2 || round > 2 ? point : take(Footprint::none({1, 4}))) ||
        (input.object_id() != 0) != (mode == 2 || round > 2) ||
        !phase.results.empty())
      return Answer(Status{ErrorCode::OperationFailed,
                           "metadata-only tensor capability facts"});
    *retained = input;
    if (denied) {
      double ignored = 0;
      static_cast<void>(input.read({0, 0}, &ignored, 8));
    }
    if ((mode == 3 || mode == 4) && round == 2) {
      ResultProgramNeed need;
      need.tiles.push_back(take(sdk::tile_need_for_samples(
          phase.resources, phase.query, 0, 0, point)));
      return Answer(std::move(need));
    }
    double value = 4;
    if (mode >= 2)
      check(input.read({0, 2}, &value, 8));
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));

    check(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)}));
    return Answer(ResultPublication{take(builder.seal()), true});
  }
};
int metadata_tensor_need(unsigned mode, bool denied = false,
                         bool failing_source = false) {
  auto counts = std::make_shared<WorkflowCounts>();
  auto registry = std::make_shared<OperationRegistry>();
  check(
      registry->register_operation(workflow_source(counts, failing_source, 4)));
  std::optional<ResultTensorInput> retained;
  OperationDefinition consumer;
  consumer.key = "test.metadata_consumer";
  consumer.traits.input_count = 1;
  consumer.traits.input_schema.resize(1);
  consumer.traits.input_schema[0].kind = OperationPortKind::Result;
  consumer.traits.input_schema[0].element_type =
      static_cast<unsigned>(ElementType::Float64);
  consumer.traits.outputs = {multi_result::output("value")};
  consumer.traits.outputs[0].maximum_dependency_stages = 3;
  consumer.traits.outputs[0].continuation_bytes = sizeof(MetadataConsumer);
  consumer.start_result = [&](const auto&, const auto& allocator) {
    return ResultContinuation::make<MetadataConsumer>(allocator, mode, denied,
                                                      &retained);
  };
  check(registry->register_operation(std::move(consumer)));
  check(registry->freeze());
  ResourceBudget root;
  ResultRef held;
  {
    WorkflowDocument document;
    document.nodes = {
        {10, "test.tensor_source", {}, {}},
        {20, "test.metadata_consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
    document.outputs = {{"out", 20, "value"}, {"source", 10, "value"}};
    GraphContext graph(document);
    auto plan =
        take(Compiler(registry).compile(
                 graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1}))
            .plan;
    ExecutionContext context(registry, {1, false, 16, 1048576, 0});
    root = take(context.resource_budget());
    auto frozen = take(context.freeze(plan));
    ExecutionOptions options;
    auto run = context.execute_fragments(
        frozen, {{"out", TileRequest{true, {}}}}, {}, options);
    if (denied) {
      PS_CHECK(!run.ok() &&
               run.status().reason == FailureReason::UnauthorizedRead &&
               run.status().detail.origin == FailureOrigin::Protocol);
    } else if (failing_source && mode >= 2) {
      PS_CHECK(!run.ok() && run.status().detail.node_id == 10 &&
               run.status().reason == FailureReason::InvalidDomain);
    } else {
      if (!run.ok())
        std::cerr << "metadata tensor mode=" << mode << " "
                  << run.status().message << '\n';
      PS_CHECK(run.ok());
      held = run.value().results.at("out");
      PS_CHECK(multi_result::number(held) == 4 &&
               held.association().size() == (mode >= 2 ? 1u : 0u));
    }
    const auto data_calls = mode >= 2 ? 1U : 0U;
    PS_CHECK(counts->starts == data_calls && counts->polls == data_calls);
    if (!failing_source || mode < 2) {
      // A later data query validates its own requested payload.
      auto actual = context.execute_fragments(
          frozen,
          {{"source",
            TileRequest{false, take(TileSet::from_indices(root, {0}))}}},
          {}, options);
      if (failing_source) {
        PS_CHECK(!actual.ok() && actual.status().detail.node_id == 10 &&
                 actual.status().reason == FailureReason::InvalidDomain);
      } else {
        PS_CHECK(actual.ok() &&
                 multi_result::number(actual.value().results.at("source"),
                                      {0, 0}) == 2);
      }
      PS_CHECK(counts->starts == data_calls + 1);
    }
  }
  PS_CHECK(retained && retained->spec().sample_shape() ==
                           (std::vector<std::uint64_t>{1, 4}));
  held = {};
  retained.reset();
  for (auto live : root.statistics().live.values)
    PS_CHECK(!live);
  return 0;
}

struct BroadConsumer {
  unsigned mode;
  bool requested = false;
  std::optional<ResultTensorInput>* retained;
  ResultTensorReadWindow* retained_window;
  unsigned* block_calls;
  BroadConsumer(unsigned mode, std::optional<ResultTensorInput>* retained,
                ResultTensorReadWindow* window, unsigned* block_calls)
      : mode(mode),
        retained(retained),
        retained_window(window),
        block_calls(block_calls) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    const bool sparse = mode == 1 || mode == 2;
    const auto shape = mode == 5 ? std::vector<std::uint64_t>{65}
                                 : std::vector<std::uint64_t>{1, 65};
    const auto coordinate = [&](std::uint64_t x) {
      return mode == 5 ? std::vector<std::uint64_t>{x}
                       : std::vector<std::uint64_t>{0, x};
    };
    auto samples =
        sparse ? take(Footprint::from_regions(
                     shape, {Region({{0, 1}, {0, 1}}), Region({{0, 1}, {2, 1}}),
                             Region({{0, 1}, {4, 1}})}))
               : take(Footprint::all(shape));
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back(take(sdk::tile_need_for_samples(
          phase.resources, phase.query, 0, 0, samples)));
      need.tiles.push_back(need.tiles.front());
      return Answer(std::move(need));
    }
    const auto& input = phase.tensors->at({0, 0});
    if (!input.object_id() || input.coverage() != samples ||
        phase.results.size() != (mode == 5 ? 1u : 0u) || !phase.association ||
        phase.association->size() != 1)
      return Answer(Status{ErrorCode::OperationFailed,
                           "tensor grant identity or object permission"});
    for (auto id : *phase.association)
      if (!id)
        return Answer(
            Status{ErrorCode::OperationFailed, "null source in association"});
    *retained = input;
    if (mode == 2) {
      double ignored = 0;
      static_cast<void>(input.read(coordinate(1), &ignored, 8));
    }
    double total = 0;
    if (mode == 3 || mode == 5) {
      auto window = take(input.acquire(Region::whole(shape)));
      std::uint64_t next = 0;
      while (next < 65) {
        auto row = take(window.row_run(coordinate(next)));
        if (!row.samples || row.samples > 65 - next)
          return Answer(Status{ErrorCode::OperationFailed,
                               "tensor row exceeds coverage"});
        for (std::uint64_t i = 0; i < row.samples; ++i) {
          double value = 0;
          std::memcpy(&value, row.data + i * row.sample_stride_bytes,
                      sizeof(value));
          total += value;
        }
        next += row.samples;
      }
      *retained_window = std::move(window);
    } else if (mode == 4) {
      FootprintLimits limits;
      limits.consume_work = phase.consume_work;
      auto plan = take(FragmentAtlasPlan::prepare(input, {1, 4}, limits));
      auto atlas = take(plan.materialize(input, phase.allocator, limits));
      for (unsigned i = 0; i < 65; ++i) {
        const auto offset = take(atlas.address(coordinate(i)));
        double value = 0;
        std::memcpy(&value, atlas.payload.bytes().data() + offset,
                    sizeof(value));
        total += value;
      }
    } else {
      check(samples.visit(
          [&](const auto& at) {
            double value = 0;
            auto status = input.read(at, &value, sizeof(value));
            total += value;
            return status;
          },
          65));
    }
    if (mode == 6 || mode == 7) {
      auto incoming =
          multi_result::binding(phase.resources, "incoming", 0).result;
      const auto compute = [&]() -> Result<ResultRef> {
        ++*block_calls;
        double sum = 0;
        for (unsigned i = 0; i < 65; ++i) {
          double value = 0;
          auto status = input.read(coordinate(i), &value, 8);
          if (!status.ok())
            return Result<ResultRef>(status);
          sum += value;
        }
        return Result<ResultRef>(
            multi_result::binding(phase.resources, "computed", sum).result);
      };
      auto first = take(phase.block(1, 0, 65, 0, incoming, compute));
      auto second = take(phase.block(1, 0, 65, 0, incoming, compute));
      if (multi_result::number(first) != total ||
          multi_result::number(second) != total)
        return Answer(Status{ErrorCode::OperationFailed, "tensor block sum"});
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        {phase.association->begin(), phase.association->end()}));

    if (mode == 5) {
      ResultTensorViewTransform transform;
      transform.source_axes.push_back({0, 0, 1, 1});
      check(builder.publish_tensor_view(0, Region::whole(shape),
                                        *retained_window, transform));
    } else {
      check(builder.publish_tensor(
          0, Region::whole({1}),
          {reinterpret_cast<const std::uint8_t*>(&total), sizeof(total)}));
    }
    return Answer(ResultPublication{take(builder.seal()), true});
  }
};
SchemaTemplate rich_tensor_schema() {
  auto schema = multi_result::schema(ElementType::Float64, {65});
  schema.id = "test.rich_tensor";
  schema.fields.push_back(
      {"associated", ElementType::Float64, {ResultExtentKind::Fixed, 1}, {}});
  return schema;
}
struct ResourceOwners {
  std::vector<std::weak_ptr<const CpuStorage>> profiles;
};
struct RichTensorSource {
  std::shared_ptr<WorkflowCounts> counts;
  std::vector<WeakResultRef>* observed;
  ResourceOwners* resource_owners;
  bool exhaust_host, exhaust_work;
  unsigned stage = 0;
  std::optional<ResultBuilder> builder;
  ResourceVector<ResultTensorReadWindow> windows;
  ResourceLease fill;
  RichTensorSource(std::shared_ptr<WorkflowCounts> counts,
                   std::vector<WeakResultRef>* observed,
                   ResourceOwners* owners = nullptr, bool exhaust_host = false,
                   bool exhaust_work = false)
      : counts(std::move(counts)),
        observed(observed),
        resource_owners(owners),
        exhaust_host(exhaust_host),
        exhaust_work(exhaust_work) {}
  ~RichTensorSource() { ++counts->destroys; }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    ++counts->polls;
    if (!stage++) {
      ResultProgramNeed need;
      need.tiles.push_back({0, true, false, {}});
      return Answer(std::move(need));
    }
    if (stage == 2) {
      const auto& input = phase.tensors->at({0, 0});
      for (const auto& box : phase.query.tensor_outputs->boxes())
        windows.push_back(take(input.acquire(box)));
      ResourceBindings resources;
      if (resource_owners) {
        const auto bytes = icc_profile_fixture::fixture();
        auto profile = take(
            IccProfile::import({bytes.data(), bytes.size()}, phase.resources));
        resource_owners->profiles.push_back(profile.storage());
        resources = take(ResourceBindings::create({profile}, phase.resources));
      }
      if (exhaust_host) {
        const auto available =
            phase.resources.available_capacity()[ResourceKind::Host];
        fill = take(phase.resources.reserve(ResourceCapacity::host(
            available - ResourceBudget::lease_metadata_bytes())));
      }
      auto started = ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {},
          {phase.association->begin(), phase.association->end()}, 128,
          std::move(resources));
      if (!started.ok())
        return Answer(started.status());
      builder = started.take_value();
      if (exhaust_work) {
        auto charged = phase.consume_work(100000001);
        if (!charged.ok())
          return Answer(charged);
      }

      auto buffer = take(phase.allocator.allocate(8));
      const double value = 1000;
      std::memcpy(buffer.data(), &value, sizeof(value));
      check(builder->append(0, 1, {buffer.data(), buffer.size()}));
    }
    check(builder->publish(0, 1));
    for (const auto& window : windows) {
      ResultTensorViewTransform identity;
      for (unsigned axis = 0; axis < window.region().rank(); ++axis)
        identity.source_axes.push_back(
            {static_cast<std::int32_t>(axis), 0, 1, 1});
      check(builder->publish_tensor_view(0, window.region(), window, identity));
    }
    auto result = take(builder->seal());
    observed->push_back(result.weak());
    return Answer(ResultPublication{std::move(result), true});
  }
};
int broad_tensor_need(unsigned mode) {
  const auto shape = mode == 5 ? std::vector<std::uint64_t>{65}
                               : std::vector<std::uint64_t>{1, 65};
  const auto coordinate = [&](std::uint64_t x) {
    return mode == 5 ? std::vector<std::uint64_t>{x}
                     : std::vector<std::uint64_t>{0, x};
  };
  auto counts = std::make_shared<WorkflowCounts>();
  auto registry = std::make_shared<OperationRegistry>();
  std::vector<WeakResultRef> observed;
  auto source = workflow_source(counts, 0, 65);
  if (mode == 6 || mode == 7) {
    source.traits.cacheable = false;
  }
  if (mode == 5) {
    source.traits.workspace_bytes = 8;
    source.traits.input_count = 1;
    source.traits.input_schema.resize(1);
    source.traits.input_schema[0].kind = OperationPortKind::Result;
    source.traits.input_schema[0].result_schema_id = "test.multi_output";
    source.traits.input_schema[0].result_schema_version = 1;
    source.traits.outputs[0] =
        multi_result::output("value", rich_tensor_schema());
    source.traits.outputs[0].maximum_dependency_stages = 4;
    source.traits.outputs[0].continuation_bytes = sizeof(RichTensorSource);
    source.start_result = [counts, &observed](const auto&,
                                              const auto& allocator) {
      ++counts->starts;
      return ResultContinuation::make<RichTensorSource>(allocator, counts,
                                                        &observed);
    };
  }
  check(registry->register_operation(std::move(source)));
  std::optional<ResultTensorInput> held_input;
  ResultTensorReadWindow held_window;
  unsigned block_calls = 0;
  OperationDefinition consumer;
  consumer.key = "test.broad_consumer";
  consumer.traits.input_count = 1;
  consumer.traits.input_schema.resize(1);
  consumer.traits.input_schema[0].kind = OperationPortKind::Result;
  consumer.traits.input_schema[0].result_schema_id =
      mode == 5 ? "test.rich_tensor" : "test.multi_output";
  consumer.traits.input_schema[0].result_schema_version = 1;
  consumer.traits.workspace_bytes = 65536;
  consumer.traits.outputs[0] = multi_result::output(
      "value",
      multi_result::schema(ElementType::Float64, {mode == 5 ? 65u : 1u}));
  consumer.traits.outputs[0].continuation_bytes = sizeof(BroadConsumer);
  consumer.start_result = [mode, &held_input, &held_window, &block_calls](
                              const auto&, const auto& allocator) {
    return ResultContinuation::make<BroadConsumer>(allocator, mode, &held_input,
                                                   &held_window, &block_calls);
  };
  check(registry->register_operation(std::move(consumer)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {
      {10, "test.tensor_source", {}, {}},
      {20, "test.broad_consumer", {WorkflowNodeOutput{10, "value"}}, {}}};
  if (mode == 5) {
    document.inputs = {multi_result::declaration(
        1, "source", multi_result::schema(ElementType::Float64, {65}))};
    document.nodes[0].inputs = {WorkflowInputReference{1}};
  }
  document.outputs = {{"value", 20, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(
                       graph, PlanningOptions{ExecutionMode::CpuExact, {}, 1}))
                  .plan;
  ResourceBudget root;
  ExecutionResult held;
  {
    ExecutionContextConfig config{1, false, 16, 4194304,
                                  mode == 6 || mode == 7 ? 1048576u : 0u};
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    ExecutionBindings bindings;
    if (mode == 5) {
      auto builder = take(ResultBuilder::start(
          root, *document.inputs[0].result_schema, "base"));

      std::array<double, 65> values{};
      for (unsigned i = 0; i < values.size(); ++i)
        values[i] = i + 2;
      check(builder.publish_tensor(
          0, Region::whole(shape),
          {reinterpret_cast<const std::uint8_t*>(values.data()),
           sizeof(values)}));
      bindings.inputs = {{"source", take(builder.seal())}};
    }
    ExecutionOptions options;
    options.maximum_dependency_work = 10000000;
    options.maximum_dependency_cache_work = mode == 6 ? 1 : 10000000;
    options.dependencies.sets.maximum_work = 10000000;
    auto run = context.execute(plan, bindings, {}, options);
    if (mode == 2) {
      PS_CHECK(!run.ok() &&
               run.status().reason == FailureReason::UnauthorizedRead &&
               run.status().detail.origin == FailureOrigin::Protocol);
    } else {
      if (!run.ok())
        std::cerr << "broad mode=" << mode
                  << " code=" << static_cast<unsigned>(run.status().code)
                  << " node=" << run.status().detail.node_id << " "
                  << run.status().message << '\n';
      PS_CHECK(run.ok());
      held = run.take_value();
      PS_CHECK(multi_result::number(held.results.at("value")) == (mode == 1 ? 12
                                                                  : mode == 5
                                                                      ? 2
                                                                      : 2210));
      PS_CHECK(held.results.at("value").association().size() == 1);
      if (mode == 6 || mode == 7) {
        PS_CHECK(block_calls == (mode == 6 ? 2u : 1u));
        PS_CHECK(held.diagnostics.dependency_cache_work <=
                 options.maximum_dependency_cache_work);
        PS_CHECK(context.cache_statistics().entries == (mode == 6 ? 0u : 1u));
      }
    }
    PS_CHECK(counts->starts == (mode == 5                ? 1u
                                : mode == 1 || mode == 2 ? 3u
                                                         : 65u));
    PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  }
  PS_CHECK(held_input && held_input->object_id() != 0);
  if (mode == 0) {
    CancellationSource cancelled;
    cancelled.cancel();
    auto before = root.statistics().live;
    auto denied = held_input->acquire(Region::whole(shape), cancelled.token());
    PS_CHECK(!denied.ok() && denied.status().code == ErrorCode::Cancelled);
    PS_CHECK(root.statistics().live.values == before.values);
    const auto available = root.available_capacity()[ResourceKind::Host];
    const auto metadata = ResourceBudget::lease_metadata_bytes();
    auto fill =
        take(root.reserve(ResourceCapacity::host(available - metadata)));
    auto refused = held_input->acquire(Region::whole(shape));
    PS_CHECK(!refused.ok() &&
             refused.status().code == ErrorCode::ResourceExhausted);
    fill = {};
    PS_CHECK(root.statistics().live.values == before.values);
    auto recovered = take(held_input->acquire(Region::whole(shape)));
    auto last = take(recovered.row_run(coordinate(64)));
    double value = 0;
    std::memcpy(&value, last.data, sizeof(value));
    PS_CHECK(value == 66);
  }
  double last = 0;
  check(held_input->read(coordinate(mode == 1 || mode == 2 ? 4u : 64u), &last,
                         8));
  PS_CHECK(last == (mode == 1 || mode == 2 ? 6 : 66));
  if (held_window.valid()) {
    auto row = take(held_window.row_run(coordinate(64)));
    std::memcpy(&last, row.data, sizeof(last));
    PS_CHECK(last == 66 && row.samples == 1);
  }
  held_input.reset();
  held_window = {};
  if (mode == 5) {
    PS_CHECK(observed.size() == 1);
    for (const auto& weak : observed) {
      auto object = weak.lock();
      PS_CHECK(object.valid());
      auto facts = take(object.descriptor());
      PS_CHECK(facts.field_count() == 1 && facts.rows(0) == 1);
      auto data = take(take(object.prepare_read(facts, 0, 0, 1)).load(8));
      std::memcpy(&last, data->bytes().data(), sizeof(last));
      const auto at =
          facts.tensor_coverage(0).boxes()[0].dimensions()[0].offset;
      PS_CHECK(last == 1000 + at);
    }
    PS_CHECK(multi_result::number(held.results.at("value"), {64}) == 66);
  }
  held = {};
  for (const auto& weak : observed)
    PS_CHECK(!weak.lock().valid());
  for (auto live : root.statistics().live.values)
    PS_CHECK(!live);
  return 0;
}

struct ResourceViewConsumer {
  bool requested = false;
  std::optional<ResultTensorInput>* input;
  ResultTensorReadWindow* window;
  ResourceViewConsumer(std::optional<ResultTensorInput>* input,
                       ResultTensorReadWindow* window)
      : input(input), window(window) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back({0, true, false, {}});
      return Answer(std::move(need));
    }
    *input = phase.tensors->at({0, 0});
    *window = take(input->value().acquire(Region::whole({65, 4})));
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        {phase.association->begin(), phase.association->end()}, 128,
        phase.query.resources));

    ResultTensorViewTransform identity;
    identity.source_axes = {{0, 0, 1, 1}, {1, 0, 1, 1}};
    check(builder.publish_tensor_view(0, Region::whole({65, 4}), *window,
                                      identity));
    return Answer(ResultPublication{take(builder.seal()), true});
  }
};
int profile_owners(bool exhaust = false, bool work_exhaust = false) {
  ResourceBudget profile_root;
  ResourceBudget root;
  ResourceOwners owners;
  std::vector<WeakResultRef> observed;
  std::optional<ResultTensorInput> retained_input;
  ResultTensorReadWindow retained_window;
  ResultRef output;
  ColorProfileIdentity identity;
  std::size_t profile_bytes = 0;
  std::uintptr_t base_last_row = 0;
  {
    const auto bytes = icc_profile_fixture::fixture();
    auto profile =
        take(IccProfile::import({bytes.data(), bytes.size()}, profile_root));
    identity = profile.identity();
    profile_bytes = profile.bytes().size();
    auto resources = take(ResourceBindings::create({profile}, profile_root));
    auto schema = multi_result::schema(ElementType::Float64, {65, 4});
    schema.tensors[0].atomic_trailing_axes = 1;
    TensorDescription description;
    description.channel_axis = 1;
    description.model = "cmyk";
    description.profile = identity;
    description.channels = {{"C", "cyan", "relative"},
                            {"M", "magenta", "relative"},
                            {"Y", "yellow", "relative"},
                            {"K", "black", "relative"}};
    schema.tensors[0].facets = {take(encode_tensor_description(description))};
    auto rich = schema;
    rich.id = "test.resource_tensor";
    rich.fields.push_back(
        {"associated", ElementType::Float64, {ResultExtentKind::Fixed, 1}, {}});
    auto counts = std::make_shared<WorkflowCounts>();
    auto registry = std::make_shared<OperationRegistry>();
    auto source = workflow_source(counts, 0, 65);
    source.traits.workspace_bytes = 8;
    source.traits.input_count = 1;
    source.traits.input_schema.resize(1);
    source.traits.input_schema[0].kind = OperationPortKind::Result;
    source.traits.input_schema[0].result_schema_id = "test.multi_output";
    source.traits.input_schema[0].result_schema_version = 1;
    source.traits.outputs[0] = multi_result::output("value", rich);
    source.traits.outputs[0].maximum_dependency_stages = 4;
    source.traits.outputs[0].continuation_bytes = sizeof(RichTensorSource);
    source.start_result = [counts, &observed, &owners, exhaust, work_exhaust](
                              const auto&, const auto& allocator) {
      ++counts->starts;
      return ResultContinuation::make<RichTensorSource>(
          allocator, counts, &observed, &owners, exhaust, work_exhaust);
    };
    check(registry->register_operation(std::move(source)));
    OperationDefinition consumer;
    consumer.key = "test.resource_view";
    consumer.traits.input_count = 1;
    consumer.traits.input_schema.resize(1);
    consumer.traits.input_schema[0].kind = OperationPortKind::Result;
    consumer.traits.input_schema[0].result_schema_id = "test.resource_tensor";
    consumer.traits.input_schema[0].result_schema_version = 1;
    consumer.traits.outputs[0] = multi_result::output("value", schema);
    consumer.traits.outputs[0].continuation_bytes =
        sizeof(ResourceViewConsumer);
    consumer.start_result = [&retained_input, &retained_window](
                                const auto&, const auto& allocator) {
      return ResultContinuation::make<ResourceViewConsumer>(
          allocator, &retained_input, &retained_window);
    };
    check(registry->register_operation(std::move(consumer)));
    check(registry->freeze());
    WorkflowDocument document;
    document.inputs = {multi_result::declaration(1, "source", schema)};
    document.nodes = {
        {10, "test.tensor_source", {WorkflowInputReference{1}}, {}},
        {20, "test.resource_view", {WorkflowNodeOutput{10, "value"}}, {}}};
    document.outputs = {{"value", 20, "value"}};
    GraphContext graph(document);
    auto plan = take(Compiler(registry).compile(graph, {}, resources)).plan;
    {
      ExecutionContextConfig config{1, false, 16, 4194304, 0};
      config.managed_resources = ResourceLimits{};
      config.managed_resources->maximum_work = 100000000;
      config.managed_resources->capacity[ResourceKind::Host] = 4194304;
      config.managed_resources->capacity[ResourceKind::Metadata] = 4194304;
      config.managed_resources->capacity[ResourceKind::Payload] =
          2 * profile_bytes + 260 * 8 + 8192;
      ExecutionContext context(registry, config);
      root = take(context.resource_budget());
      auto builder = take(
          ResultBuilder::start(root, schema, "base", {}, {}, 128, resources));

      std::array<double, 260> values{};
      for (unsigned i = 0; i < values.size(); ++i)
        values[i] = i + 1;
      check(builder.publish_tensor(
          0, Region::whole({65, 4}),
          {reinterpret_cast<const std::uint8_t*>(values.data()),
           sizeof(values)}));
      ExecutionBindings bindings{{{"source", take(builder.seal())}}};
      {
        const auto& base = bindings.inputs[0].result;
        auto window = take(base.acquire_tensor(take(base.descriptor()), 0,
                                               Region({{64, 1}, {0, 4}})));
        base_last_row = reinterpret_cast<std::uintptr_t>(
            take(window.row_run({64, 0})).data);
      }
      ExecutionOptions options;
      options.maximum_dependency_work = 100000000;
      options.dependencies.sets.maximum_work = 100000000;
      auto run = context.execute(plan, bindings, {}, options);
      if (exhaust || work_exhaust) {
        PS_CHECK(!run.ok() &&
                 run.status().code == ErrorCode::ResourceExhausted);
        PS_CHECK(owners.profiles.size() == 1);
        if (work_exhaust)
          PS_CHECK(run.status().reason == FailureReason::WorkLimit &&
                   run.status().message == "managed resource work exhausted");
      } else {
        if (!run.ok())
          std::cerr << "resource tensor code="
                    << static_cast<unsigned>(run.status().code)
                    << " reason=" << static_cast<unsigned>(run.status().reason)
                    << " scope="
                    << static_cast<unsigned>(run.status().detail.scope)
                    << " node=" << run.status().detail.node_id
                    << " profiles=" << owners.profiles.size()
                    << " objects=" << observed.size() << " host_peak="
                    << root.statistics().peak[ResourceKind::Host]
                    << " payload_peak="
                    << root.statistics().peak[ResourceKind::Payload] << " "
                    << run.status().message << '\n';
        PS_CHECK(run.ok());
        output = run.value().results.at("value");
        PS_CHECK(counts->starts == 1);
        PS_CHECK(output.association().size() == 1);
        PS_CHECK(retained_input && retained_input->object_id());
      }
    }
  }
  if (!exhaust && !work_exhaust) {
    PS_CHECK(owners.profiles.size() == 1 && observed.size() == 1);
    std::set<const CpuStorage*> storage, retained_profiles;
    for (const auto& weak : owners.profiles) {
      auto owner = weak.lock();
      PS_CHECK(owner && owner->bytes().size() == profile_bytes);
      storage.insert(owner.get());
    }
    PS_CHECK(storage.size() == 1);
    retained_input.reset();
    retained_window = {};
    for (const auto& weak : observed) {
      auto source = weak.lock();
      PS_CHECK(source.valid());
      auto retained_profile = take(source.resources().icc_profile(identity));
      PS_CHECK(storage.count(retained_profile.storage().get()) == 1);
      retained_profiles.insert(retained_profile.storage().get());
      auto facts = take(source.descriptor());
      auto field = take(take(source.prepare_read(facts, 0, 0, 1)).load(8));
      double number = 0;
      std::memcpy(&number, field->bytes().data(), 8);
      PS_CHECK(number ==
               1000 +
                   facts.tensor_coverage(0).boxes()[0].dimensions()[0].offset);
    }
    PS_CHECK(retained_profiles.size() == 1);
    PS_CHECK(multi_result::number(output, {64, 3}) == 260);
    auto final_window = take(output.acquire_tensor(take(output.descriptor()), 0,
                                                   Region({{64, 1}, {0, 4}})));
    output = {};
    for (const auto& weak : owners.profiles)
      PS_CHECK(!weak.expired());
    auto row = take(final_window.row_run({64, 0}));
    PS_CHECK(reinterpret_cast<std::uintptr_t>(row.data) == base_last_row);
    double last = 0;
    std::memcpy(&last, row.data + 3 * row.sample_stride_bytes, 8);
    PS_CHECK(last == 260);
    final_window = {};
  }
  for (const auto& weak : owners.profiles)
    PS_CHECK(weak.expired());
  for (const auto& weak : observed)
    PS_CHECK(!weak.lock().valid());
  for (auto live : root.statistics().live.values)
    PS_CHECK(!live);
  for (auto live : profile_root.statistics().live.values)
    PS_CHECK(!live);
  return 0;
}

}  // namespace
int main() try {
  PS_CHECK(profile_owners() == 0);
  PS_CHECK(profile_owners(true) == 0);
  PS_CHECK(profile_owners(false, true) == 0);
  for (unsigned mode = 0; mode <= 4; ++mode)
    PS_CHECK(metadata_tensor_need(mode) == 0);
  PS_CHECK(metadata_tensor_need(0, true) == 0);
  PS_CHECK(metadata_tensor_need(1, true) == 0);
  for (unsigned mode : {0U, 1U, 3U, 4U})
    PS_CHECK(metadata_tensor_need(mode, false, true) == 0);
  for (unsigned mode = 0; mode <= 7; ++mode)
    PS_CHECK(broad_tensor_need(mode) == 0);
  PS_CHECK(empty_scalar_guard() == 0);
  return 0;
} catch (const multi_result::Failure& failure) {
  std::cerr << failure.what() << '\n';
  return 1;
}
