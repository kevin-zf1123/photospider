#include <fenv.h>  // NOLINT(build/c++11)

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
ps::SchemaTemplate payload_schema() {
  ps::SchemaTemplate schema;
  schema.id = "manual.payload";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ps::ElementType::Float64, {2}};
  tensor.atomic_trailing_axes = 1;
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
struct PayloadProbe {
  bool borrow, ready = false;
  std::atomic<unsigned>* calls;
  explicit PayloadProbe(bool reference,
                        std::atomic<unsigned>* counter = nullptr)
      : borrow(reference), calls(counter) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    if (calls)
      calls->fetch_add(1, std::memory_order_relaxed);
    using Answer = ps::Result<ps::ResultProgramPoll>;
    if (borrow && !ready) {
      ready = true;
      ps::ResultProgramNeed need;
      need.tiles.push_back(
          {0, 0, false, take(ps::TileSet::range(phase.resources, 0, 1))});
      return Answer(std::move(need));
    }
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}));
    ps::Status status;
    if (borrow) {
      auto window =
          take(phase.tensors->at({0, 0}).acquire(ps::Region::whole({2})));
      ps::ResultTensorViewTransform transform;
      transform.source_axes = {{0, 0, 1}};
      status = builder.publish_tensor_view(0, ps::Region::whole({2}), window,
                                           transform);
    } else {
      auto allocation = phase.resources.allocator().allocate(16);
      if (!allocation.ok())
        return Answer(allocation.status());
      auto bytes = allocation.take_value();
      std::memset(bytes.data(), 0, bytes.size());
      status = builder.publish_tensor(0, ps::Region::whole({2}), {0, {8}},
                                      std::move(bytes).freeze());
    }
    if (!status.ok())
      return Answer(status);
    return Answer(ps::ResultPublication{take(builder.seal()), true});
  }
};
ps::Result<ps::ResultProgramPoll> foreign_payload(
    const ps::ResultProgramPhase& phase) {
  ps::ResourceBudget other;
  auto allocator = other.allocator();
  ps::ResultProgramPhase foreign{phase.query,        phase.results,
                                 allocator,          other,
                                 phase.consume_work, phase.failure};
  PayloadProbe probe(false);
  return probe.poll(foreign);
}
struct PrefixPayloadProbe {
  bool reuse;
  unsigned round = 0;
  std::optional<ps::ResultBuilder> builder;
  std::shared_ptr<const ps::CpuStorage> storage;
  explicit PrefixPayloadProbe(bool shared) : reuse(shared) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    if (!builder) {
      builder = take(ps::ResultBuilder::start(phase.resources,
                                              *phase.query.output.result_schema,
                                              phase.query.semantic_key));
    }
    if (!reuse || !storage) {
      auto bytes = take(phase.resources.allocator().allocate(8));
      std::memset(bytes.data(), 0, bytes.size());
      storage = std::move(bytes).freeze();
    }
    require(builder
                ->publish_tensor(0, ps::Region({{round, 1}}), {0, {8}, {round}},
                                 storage)
                .ok(),
            "prefix payload");
    ++round;
    auto result = round == 2 ? take(builder->seal()) : builder->reference();
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{std::move(result), round == 2});
  }
};
ps::OperationDefinition payload_probe(bool borrow,
                                      std::atomic<unsigned>* calls = nullptr) {
  ps::OperationDefinition definition;
  definition.key = "manual.output_payload";
  auto& traits = definition.traits;
  traits.input_count = borrow ? 1 : 0;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.result_schema_id = "manual.payload";
    input.result_schema_version = 1;
    input.tensor_key = "samples";
  }
  traits.workspace_bytes = 8;
  auto& output = traits.outputs[0];

  output.output_schema.result_schema_id = "manual.payload";
  output.output_schema.result_schema_version = 1;
  output.output_schema.tensor_key = "samples";
  output.result_schema = payload_schema();
  output.continuation_bytes = sizeof(PayloadProbe);
  output.maximum_dependency_stages = 2;
  output.maximum_output_payload_bytes = borrow ? 0 : 8;
  definition.start_result = [borrow, calls](const auto&,
                                            const auto& allocator) {
    return ps::ResultContinuation::make<PayloadProbe>(allocator, borrow, calls);
  };
  if (!borrow) {
    traits.outputs.push_back(output);
    traits.outputs.back().key = "other";
  }
  return definition;
}
struct TilePayloadProbe {
  std::shared_ptr<std::shared_ptr<const ps::CpuStorage>> storage;
  unsigned mode;
  TilePayloadProbe(
      std::shared_ptr<std::shared_ptr<const ps::CpuStorage>> storage,
      unsigned mode)
      : storage(std::move(storage)), mode(mode) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    auto owner = *storage;
    if (!owner || mode != 2) {
      auto bytes = take(phase.resources.allocator().allocate(8));
      std::memset(bytes.data(), 0, bytes.size());
      owner = std::move(bytes).freeze();
      if (mode == 2)
        *storage = owner;
    }
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    const auto at = phase.query.tile_unit;
    require(
        builder.publish_tensor(0, ps::Region({{at, 1}}), {0, {8}, {at}}, owner)
            .ok(),
        "per-tile payload");
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};
void cumulative_tile_payload() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto storage = std::make_shared<std::shared_ptr<const ps::CpuStorage>>();
    auto definition = payload_probe(false);
    auto& outputs = definition.traits.outputs;
    outputs.resize(1);
    outputs[0].result_schema->tensors[0].atomic_trailing_axes = 0;
    outputs[0].maximum_output_payload_bytes = mode == 1 ? 16 : 8;
    outputs[0].continuation_bytes = sizeof(TilePayloadProbe);
    definition.start_result = [storage, mode](const auto&,
                                              const auto& allocator) {
      return ps::ResultContinuation::make<TilePayloadProbe>(allocator, storage,
                                                            mode);
    };
    auto registry = std::make_shared<ps::OperationRegistry>();
    require(registry->register_operation(std::move(definition)).ok(),
            "register per-tile payload");
    require(registry->freeze().ok(), "freeze per-tile payload");
    ps::WorkflowDocument document;
    document.nodes = {{1, "manual.output_payload", {}, {}}};
    document.outputs = {{"value", 1, "value"}};
    ps::GraphContext graph(document);
    ps::PlanningOptions planning;
    planning.tile_extent = 1;
    auto plan = take(ps::Compiler(registry).compile(graph, planning)).plan;
    ps::ExecutionContextConfig config;
    config.result_cache_bytes = 1048576;
    config.cpu_workers = 1;
    ps::ExecutionContext execution(registry, config);
    for (unsigned run = 0; run < 2; ++run) {
      auto result = execution.execute(plan);
      require(mode ? result.ok()
                   : !result.ok() && result.status().reason ==
                                         ps::FailureReason::CapacityLimit,
              "logical output counts physical owners across tile actors");
      if (mode && run)
        require(result.value().diagnostics.cache_hits == 2,
                "cached tiles preserve cumulative owner accounting");
    }
  }
}
struct MetadataControl {
  std::optional<ps::ResourceBudget> root;
  ps::ResourceKind kind = ps::ResourceKind::Metadata;
  std::uint64_t limit = 131072;
  bool prior_protocol = false;
};
struct MetadataProbe {
  std::shared_ptr<MetadataControl> control;
  ps::ResourceLease fill;
  explicit MetadataProbe(std::shared_ptr<MetadataControl> shared)
      : control(std::move(shared)) {}
  void fill_limit() {
    const auto live = control->root->statistics().live[control->kind];
    require(live < control->limit, "metadata probe must reach callback");
    ps::ResourceCapacity amount;
    if (control->kind == ps::ResourceKind::Metadata) {
      const auto bytes =
          control->limit - live - ps::ResourceBudget::lease_metadata_bytes();
      amount = ps::ResourceCapacity::host(bytes, bytes);
    } else {
      amount[control->kind] = control->limit - live;
    }
    fill = take(control->root->reserve(amount));
  }
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    if (control->prior_protocol) {
      double ignored = 0;
      static_cast<void>(phase.read_tensor(999, 0, {0}, &ignored, 8));
      try {
        ps::ResourceVector<std::uint8_t> metadata;
        metadata.resize(control->limit + 1);
      } catch (const std::bad_alloc&) {
      }
      return ps::Result<ps::ResultProgramPoll>(
          ps::Status{ps::ErrorCode::OperationFailed, "later callback failure"});
    }
    PayloadProbe probe(false);
    auto result = probe.poll(phase);
    fill_limit();
    return result;
  }
};
void worker_metadata_limits(ps::ResourceKind kind,
                            bool prior_protocol = false) {
  auto control = std::make_shared<MetadataControl>();
  control->kind = kind;
  control->prior_protocol = prior_protocol;
  control->limit = kind == ps::ResourceKind::Metadata ? 131072 : 128;
  auto definition = payload_probe(false);
  definition.traits.workspace_bytes = 0;
  for (auto& output : definition.traits.outputs) {
    output.maximum_output_payload_bytes = 16;
    output.continuation_bytes = sizeof(MetadataProbe);
  }
  definition.start_result = [control](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<MetadataProbe>(allocator, control);
  };
  auto registry = std::make_shared<ps::OperationRegistry>();
  require(registry->register_operation(std::move(definition)).ok(),
          "register metadata probe");
  require(registry->freeze().ok(), "freeze metadata probe");
  ps::WorkflowDocument document;
  document.nodes = {{1, "manual.output_payload", {}, {}}};
  document.outputs = {{"a", 1, "value"}, {"b", 1, "other"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  {
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    config.managed_resources->capacity[kind] = control->limit;
    ps::ExecutionContext execution(registry, config);
    control->root = take(execution.resource_budget());
    auto result = execution.execute(compiled.plan);
    if (prior_protocol) {
      require(
          !result.ok() &&
              result.status().detail.origin == ps::FailureOrigin::Protocol &&
              result.status().reason == ps::FailureReason::UnauthorizedRead,
          "prior protocol failure must survive later metadata exhaustion");
    } else {
      require(!result.ok() &&
                  result.status().code == ps::ErrorCode::ResourceExhausted,
              "worker bookkeeping must respect managed resource limits");
    }
    if (!prior_protocol)
      require(control->root->statistics().peak[kind] == control->limit,
              "probe reached selected resource limit");
  }
  require(control->root->statistics().live[kind] == 0,
          "worker bookkeeping and probe lease must retire");
  std::cout << "managed bookkeeping limit kind=" << static_cast<unsigned>(kind)
            << " passed\n";
}
void output_payload_bounds() {
  std::atomic<unsigned> calls{0};
  auto registry = std::make_shared<ps::OperationRegistry>();
  const auto registration =
      registry->register_operation(payload_probe(false, &calls));
  require(registration.ok(), registration.message.c_str());
  require(registry->freeze().ok(), "freeze payload probe");
  ps::ResourceBudget root;
  {
    ps::ResourceAllocationScope scope(root);
    ps::ResultProgramMetadata metadata;
    metadata.output.result_schema =
        std::make_shared<const ps::SchemaTemplate>(payload_schema());
    const std::map<std::string, ps::ParameterValue> parameters;
    ps::ResultProgramQuery request(metadata, parameters);
    request.semantic_key = "payload-probe";
    request.snapshot_identity = "payload-inputs";
    auto session = take(registry->start_result("manual.output_payload", request,
                                               root.allocator()));

    ps::ResultObjectInputs no_results;
    auto allocator = root.allocator();
    auto failure =
        std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok);
    ps::ResultProgramPhase phase{
        request,
        no_results,
        allocator,
        root,
        [root](auto work) { return root.consume({work}); },
        failure};
    auto prior = take(registry->start_result("manual.output_payload", request,
                                             root.allocator()));
    auto stopped_phase = phase;
    stopped_phase.failure = std::make_shared<std::atomic<ps::ErrorCode>>(
        ps::ErrorCode::InvalidArgument);
    const auto before = calls.load(std::memory_order_relaxed);
    require(prior.poll(stopped_phase).status().code ==
                    ps::ErrorCode::InvalidArgument &&
                calls.load(std::memory_order_relaxed) == before,
            "preexisting direct protocol failure prevents bound callback");
    const auto status = session.poll(phase).status();
    require(status.code == ps::ErrorCode::ResourceExhausted &&
                status.reason == ps::FailureReason::CapacityLimit,
            "workspace cannot expand the declared Result output payload bound");
    require(
        session.poll(phase).status().reason == ps::FailureReason::CapacityLimit,
        "Result output bound failure remains sticky");
  }
  for (auto live : root.statistics().live.values)
    require(live == 0, "direct Result payload failures release resources");
  ps::ResourceLimits guard_limits;
  guard_limits.capacity[ps::ResourceKind::Metadata] = 65536;
  ps::ResourceBudget guard_root(guard_limits);
  {
    ps::ResourceAllocationScope scope(guard_root);
    ps::ResultProgramMetadata metadata;
    metadata.output.result_schema =
        std::make_shared<const ps::SchemaTemplate>(payload_schema());
    const std::map<std::string, ps::ParameterValue> parameters;
    ps::ResultProgramQuery query(metadata, parameters);
    query.semantic_key = "payload.guard";
    auto state = take(registry->start_result("manual.output_payload", query,
                                             guard_root.allocator()));
    const auto live = guard_root.statistics().live[ps::ResourceKind::Metadata];
    auto occupied = take(guard_root.reserve(ps::ResourceCapacity::host(
        65536 - live - ps::ResourceBudget::lease_metadata_bytes(),
        65536 - live - ps::ResourceBudget::lease_metadata_bytes())));

    ps::ResultObjectInputs no_results;
    auto allocator = guard_root.allocator();
    ps::ResultProgramPhase phase{
        query,
        no_results,
        allocator,
        guard_root,
        [guard_root](auto work) { return guard_root.consume({work}); },
        std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok)};
    const auto before = calls.load(std::memory_order_relaxed);
    require(
        state.poll(phase).status().code == ps::ErrorCode::ResourceExhausted &&
            calls.load(std::memory_order_relaxed) == before,
        "Root accounts bound guard before entering producer callback");
  }
  for (auto live : guard_root.statistics().live.values)
    require(live == 0, "bound guard admission failure releases Root resources");
  auto foreign_registry = std::make_shared<ps::OperationRegistry>();
  auto foreign_definition = payload_probe(false);
  foreign_definition.start_result = [](const auto&, const auto&) {
    return ps::ResultContinuation::stateless<foreign_payload>();
  };
  require(
      foreign_registry->register_operation(std::move(foreign_definition)).ok(),
      "foreign payload probe");
  {
    ps::ResourceBudget normal_root;
    ps::ResourceAllocationScope scope(normal_root);
    ps::ResultProgramMetadata metadata;
    metadata.output.result_schema =
        std::make_shared<const ps::SchemaTemplate>(payload_schema());
    const std::map<std::string, ps::ParameterValue> parameters;
    ps::ResultProgramQuery query(metadata, parameters);
    query.semantic_key = "foreign.payload";
    auto state = take(foreign_registry->start_result(
        "manual.output_payload", query, normal_root.allocator()));

    ps::ResultObjectInputs no_results;
    auto allocator = normal_root.allocator();
    ps::ResultProgramPhase phase{
        query,
        no_results,
        allocator,
        normal_root,
        [normal_root](auto work) { return normal_root.consume({work}); },
        std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok)};
    auto failed = state.poll(phase).status();
    require(failed.code == ps::ErrorCode::InvalidArgument &&
                failed.detail.origin == ps::FailureOrigin::Protocol,
            "foreign Root cannot bypass direct output payload bound");
  }
  ps::WorkflowDocument document;
  document.nodes = {{1, "manual.output_payload", {}, {}}};
  document.outputs = {{"a", 1, "value"}, {"b", 1, "other"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContext context(registry);
  auto denied = context.execute(compiled.plan);
  require(!denied.ok() &&
              denied.status().reason == ps::FailureReason::CapacityLimit,
          "workflow rejects excessive new output backing");
  for (bool reuse : {false, true}) {
    auto prefix_definition = payload_probe(false);
    auto prefix_schema = payload_schema();
    prefix_schema.tensors[0].atomic_trailing_axes = 0;
    prefix_definition.traits.outputs.resize(1);
    prefix_definition.traits.outputs[0].result_schema = prefix_schema;
    prefix_definition.traits.outputs[0].continuation_bytes =
        sizeof(PrefixPayloadProbe);
    prefix_definition.start_result = [reuse](const auto&,
                                             const auto& allocator) {
      return ps::ResultContinuation::make<PrefixPayloadProbe>(allocator, reuse);
    };
    ps::OperationRegistry prefix_registry;
    auto admitted =
        prefix_registry.register_operation(std::move(prefix_definition));
    require(admitted.ok(), admitted.message.c_str());
    ps::ResourceBudget prefix_root;
    {
      ps::ResourceAllocationScope scope(prefix_root);
      ps::ResultProgramMetadata metadata;
      metadata.output.result_schema =
          std::make_shared<const ps::SchemaTemplate>(prefix_schema);
      const std::map<std::string, ps::ParameterValue> parameters;
      ps::ResultProgramQuery query(metadata, parameters);
      query.semantic_key = "prefix-payload";
      auto state = take(prefix_registry.start_result(
          "manual.output_payload", query, prefix_root.allocator()));

      ps::ResultObjectInputs no_results;
      auto allocator = prefix_root.allocator();
      ps::ResultProgramPhase phase{
          query,
          no_results,
          allocator,
          prefix_root,
          [prefix_root](auto work) { return prefix_root.consume({work}); },
          std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok)};
      auto first = take(state.poll(phase));
      require(!std::get<ps::ResultPublication>(first).complete,
              "first eight-byte prefix fits output bound");
      auto captured =
          take(std::get<ps::ResultPublication>(first).result.capture());
      auto second = state.poll(phase);
      require(reuse ? second.ok()
                    : !second.ok() && second.status().reason ==
                                          ps::FailureReason::CapacityLimit,
              "prefix cap counts cumulative unique physical allocations");
      require(
          take(captured.descriptor(false)).tensor_coverage(0) ==
              take(ps::Footprint::from_regions({2}, {ps::Region({{0, 1}})})),
          "captured prefix coverage remains immutable");
    }
    require(prefix_root.statistics().live[ps::ResourceKind::Payload] == 0,
            "prefix owner release retires output payload");
  }
  auto mixed = std::make_shared<ps::OperationRegistry>();
  auto mixed_definition = payload_probe(false);
  mixed_definition.traits.outputs[1].maximum_output_payload_bytes = 16;
  require(mixed->register_operation(std::move(mixed_definition)).ok(),
          "mixed output bounds");
  require(mixed->freeze().ok(), "freeze mixed output bounds");
  ps::GraphContext mixed_graph(document);
  auto mixed_plan = take(ps::Compiler(mixed).compile(mixed_graph)).plan;
  ps::ExecutionContext mixed_context(mixed);
  auto frozen = take(mixed_context.freeze(mixed_plan));
  auto bad = mixed_context.execute_fragments(
      frozen,
      {{"a", ps::TileRequest{{{0, take(ps::TileSet::range(root, 0, 1))}}}}});
  require(!bad.ok() && bad.status().reason == ps::FailureReason::CapacityLimit,
          "selected output enforces its own payload bound");
  auto good = take(mixed_context.execute_fragments(
      frozen,
      {{"b", ps::TileRequest{{{0, take(ps::TileSet::range(root, 0, 1))}}}}}));
  require(good.results.size() == 1 && good.results.at("b").valid(),
          "independent request permits the legal sibling output");
  auto cancellation_registry = std::make_shared<ps::OperationRegistry>();
  bool returned = false;
  struct CancellingProbe {
    bool* returned;
    explicit CancellingProbe(bool* flag) : returned(flag) {}
    ps::Result<ps::ResultProgramPoll> poll(
        const ps::ResultProgramPhase& phase) {
      PayloadProbe probe(false);
      auto result = probe.poll(phase);
      *returned = true;
      return result;
    }
  };
  auto cancelling_definition = payload_probe(false);
  for (auto& output : cancelling_definition.traits.outputs)
    output.maximum_output_payload_bytes = 16;
  for (auto& output : cancelling_definition.traits.outputs)
    output.continuation_bytes = sizeof(CancellingProbe);
  cancelling_definition.start_result = [&returned](const auto&,
                                                   const auto& allocator) {
    return ps::ResultContinuation::make<CancellingProbe>(allocator, &returned);
  };
  require(cancellation_registry
              ->register_operation(std::move(cancelling_definition))
              .ok(),
          "cancel cap fixture");
  {
    ps::ResourceBudget cancellation_root;
    ps::ResourceAllocationScope scope(cancellation_root);
    ps::ResultProgramMetadata metadata;
    metadata.output.result_schema =
        std::make_shared<const ps::SchemaTemplate>(payload_schema());
    const std::map<std::string, ps::ParameterValue> parameters;
    ps::CancellationSource stop;
    ps::ResultProgramQuery first_query(metadata, parameters);
    first_query.semantic_key = "payload.cancel.first";
    first_query.snapshot_identity = "payload.cancel.inputs";
    first_query.cancellation = stop.token();
    auto state = take(cancellation_registry->start_result(
        "manual.output_payload", first_query, cancellation_root.allocator()));

    ps::ResultObjectInputs no_results;
    auto allocator = cancellation_root.allocator();
    auto work = [cancellation_root](auto amount) {
      return cancellation_root.consume({amount});
    };
    ps::ResultProgramPhase first{
        first_query,
        no_results,
        allocator,
        cancellation_root,
        [&](auto amount) {
          if (returned)
            stop.cancel();
          return work(amount);
        },
        std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok)};
    auto stopped = state.poll(first);
    require(returned && !stopped.ok() &&
                stopped.status().code == ps::ErrorCode::Cancelled,
            "cancellation during output payload validation ends the request");
  }
  auto borrowing = std::make_shared<ps::OperationRegistry>();
  require(borrowing->register_operation(payload_probe(true)).ok(),
          "register borrowed Result probe");
  require(borrowing->freeze().ok(), "freeze borrowed Result probe");
  ps::ExecutionContextConfig borrowed_config;
  borrowed_config.result_cache_bytes = 1048576;
  ps::ExecutionContext borrowed_context(borrowing, borrowed_config);
  auto resources = take(borrowed_context.resource_budget());
  auto builder = take(
      ps::ResultBuilder::start(resources, payload_schema(), "payload.source"));
  auto input_bytes = take(resources.allocator().allocate(16));
  std::memset(input_bytes.data(), 0, input_bytes.size());
  require(builder
              .publish_tensor(0, ps::Region::whole({2}), {0, {8}},
                              std::move(input_bytes).freeze())
              .ok(),
          "borrowed source payload");
  auto input = take(builder.seal());
  auto source = take(input.acquire_tensor(take(input.descriptor()), 0,
                                          ps::Region::whole({2})));
  ps::WorkflowInputDeclaration declaration;
  declaration.id = 1;
  declaration.name = "input";
  declaration.result_schema =
      std::make_shared<const ps::SchemaTemplate>(input.schema());
  document.inputs = {declaration};
  document.nodes[0].inputs = {ps::WorkflowInputReference{1}};
  document.outputs.resize(1);
  ps::GraphContext borrowed_graph(document);
  auto plan = take(ps::Compiler(borrowing).compile(borrowed_graph)).plan;
  auto result = take(borrowed_context.execute(plan, {{{"input", input}}}));
  auto output = result.results.at("a");
  auto window = take(output.acquire_tensor(take(output.descriptor()), 0,
                                           ps::Region::whole({2})));
  require(
      source.storage_owner_token() == window.storage_owner_token() &&
          resources.statistics().live[ps::ResourceKind::Payload] == 16,
      "zero new-payload bound permits a Need-authorized borrowed Result view");
  auto hit = take(borrowed_context.execute(plan, {{{"input", input}}}));
  require(hit.diagnostics.cache_hits == 1,
          "same borrowed owner can reuse a zero-cap output");
  auto replacement_builder = take(
      ps::ResultBuilder::start(resources, payload_schema(), "payload.source"));
  const double zeros[2] = {};
  require(
      replacement_builder
          .publish_tensor(0, ps::Region::whole({2}),
                          ps::ByteView(reinterpret_cast<const uint8_t*>(zeros),
                                       sizeof(zeros)))
          .ok(),
      "replacement source payload");
  auto replacement = take(replacement_builder.seal());
  auto rerun = take(borrowed_context.execute(plan, {{{"input", replacement}}}));
  require(rerun.diagnostics.cache_hits == 0,
          "cache storage outside the payload allowance falls back to compute");
  auto replaced_source = take(replacement.acquire_tensor(
      take(replacement.descriptor()), 0, ps::Region::whole({2})));
  const auto& replaced_output = rerun.results.at("a");
  auto replaced_window = take(replaced_output.acquire_tensor(
      take(replaced_output.descriptor()), 0, ps::Region::whole({2})));
  require(replaced_window.storage_owner_token() ==
              replaced_source.storage_owner_token(),
          "cache miss borrows the current input owner");
  auto replacement_hit =
      take(borrowed_context.execute(plan, {{{"input", replacement}}}));
  require(replacement_hit.diagnostics.cache_hits == 1,
          "recomputed borrowed payload can be cached normally");
  std::cout << "Result output payload: direct/workflow bounds and "
               "borrowed view passed\n";
}
}  // namespace
int main() {
  try {
    output_payload_bounds();
    cumulative_tile_payload();
    worker_metadata_limits(ps::ResourceKind::Metadata, true);
    for (auto kind : {ps::ResourceKind::Metadata, ps::ResourceKind::Entries})
      worker_metadata_limits(kind);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
