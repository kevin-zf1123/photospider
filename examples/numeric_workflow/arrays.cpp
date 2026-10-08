#include "photospider/numeric/arrays.hpp"

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
#include "result_fixture.hpp"  // NOLINT(build/include_subdir)

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
ps::Value scalar(ps::ElementType type, std::uint64_t bits) {
  auto buffer =
      take(ps::BufferAllocator{}.allocate(ps::Value::element_size(type)));
  std::memcpy(buffer.data(), &bits, buffer.size());
  return take(
      ps::Value::from_storage({type, {1}}, ps::Region::whole({1}),
                              {0, {static_cast<std::int64_t>(buffer.size())}},
                              std::move(buffer).freeze()));
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
  unsigned* calls;
  explicit PayloadProbe(bool reference, unsigned* counter = nullptr)
      : borrow(reference), calls(counter) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    if (calls)
      ++*calls;
    using Answer = ps::Result<ps::ResultProgramPoll>;
    if (borrow && !ready) {
      ready = true;
      ps::ResultProgramNeed need;
      need.tensors.push_back({0, 0, take(ps::Footprint::all({2})), 5});
      return Answer(std::move(need));
    }
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association ? std::vector<uint64_t>(phase.association->begin(),
                                                  phase.association->end())
                          : std::vector<uint64_t>{}));
    require(builder
                .bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(phase.resources, 1, {})))
                .ok(),
            "payload descriptor relation");
    auto relation = take(ps::ResultRelation::cartesian(
        phase.resources, 2,
        borrow
            ? ps::ResultSupport{0, 5, 0, 2, ps::ResultSupportTarget::Tensor, 0}
            : ps::ResultSupport{}));
    ps::Status status;
    if (borrow) {
      auto window =
          take(phase.tensors->at({0, 0}).acquire(ps::Region::whole({2})));
      ps::ResultTensorViewTransform transform;
      transform.source_axes = {{0, 0, 1, 1}};
      status = builder.publish_tensor_view(0, ps::Region::whole({2}), window,
                                           transform, std::move(relation),
                                           {true, true, true, true});
    } else {
      auto allocation = phase.resources.allocator().allocate(16);
      if (!allocation.ok())
        return Answer(allocation.status());
      auto bytes = allocation.take_value();
      std::memset(bytes.data(), 0, bytes.size());
      status = builder.publish_tensor(
          0, ps::Region::whole({2}), {0, {8}}, std::move(bytes).freeze(),
          std::move(relation), {true, true, true, true});
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
  ps::ResultProgramPhase foreign{
      phase.query, phase.results,      phase.io,     allocator,
      other,       phase.consume_work, phase.failure};
  PayloadProbe probe(false);
  return probe.poll(foreign);
}
struct PayloadJointProbe {
  ps::Result<ps::ResourceVector<ps::ResultJointOutcome>> poll(
      const ps::ResultJointPhase& phase) {
    ps::ResourceVector<ps::ResultJointOutcome> outcomes;
    for (const auto* member : phase.members) {
      PayloadProbe probe(false);
      outcomes.push_back(
          {take(ps::result_atom_key(member->query)), probe.poll(*member)});
    }
    return ps::Result<ps::ResourceVector<ps::ResultJointOutcome>>(
        std::move(outcomes));
  }
};
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
      require(builder
                  ->bind_descriptor_relation(take(
                      ps::ResultRelation::cartesian(phase.resources, 1, {})))
                  .ok(),
              "prefix descriptor");
    }
    if (!reuse || !storage) {
      auto bytes = take(phase.resources.allocator().allocate(8));
      std::memset(bytes.data(), 0, bytes.size());
      storage = std::move(bytes).freeze();
    }
    require(builder
                ->publish_tensor(
                    0, ps::Region({{round, 1}}), {0, {8}, {round}}, storage,
                    take(ps::ResultRelation::cartesian(phase.resources, 2, {})),
                    {true, true, true, true})
                .ok(),
            "prefix payload");
    ++round;
    auto result = round == 2 ? take(builder->seal()) : builder->reference();
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{std::move(result), round == 2});
  }
};
ps::OperationDefinition payload_probe(bool borrow, unsigned* calls = nullptr) {
  ps::OperationDefinition definition;
  definition.key = "manual.output_payload";
  auto& traits = definition.traits;
  traits.input_count = borrow ? 1 : 0;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.kind = ps::OperationPortKind::Result;
    input.result_schema_id = "manual.payload";
    input.result_schema_version = 1;
    input.tensor_key = "samples";
  }
  traits.workspace_bytes = 8;
  auto& output = traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = "manual.payload";
  output.output_schema.result_schema_version = 1;
  output.output_schema.tensor_key = "samples";
  output.result_schema = payload_schema();
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.continuation_bytes = sizeof(PayloadProbe);
  output.maximum_dependency_stages = 2;
  output.maximum_output_payload_bytes = borrow ? 0 : 8;
  definition.start_result = [borrow, calls](const auto&,
                                            const auto& allocator) {
    return ps::ResultContinuation::make<PayloadProbe>(allocator, borrow, calls);
  };
  if (!borrow) {
    output.failure_delivery = ps::FailureDelivery::PerAtomOutcome;
    traits.outputs.push_back(output);
    traits.outputs.back().key = "other";
    traits.joint_contract = 2;
    traits.joint_continuation_bytes = sizeof(PayloadJointProbe);
    definition.start_result_joint = [](const auto&, const auto& allocator) {
      return ps::ResultJointContinuation::make<PayloadJointProbe>(allocator);
    };
  }
  return definition;
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
  ps::Result<ps::ResourceVector<ps::ResultJointOutcome>> poll(
      const ps::ResultJointPhase& phase) {
    if (control->prior_protocol) {
      double ignored = 0;
      static_cast<void>(
          phase.members.front()->read_tensor(999, 0, {0}, &ignored, 8));
      try {
        ps::ResourceVector<std::uint8_t> metadata;
        metadata.resize(control->limit + 1);
      } catch (const std::bad_alloc&) {
      }
      return ps::Result<ps::ResourceVector<ps::ResultJointOutcome>>(
          ps::Status{ps::ErrorCode::OperationFailed, "later callback failure"});
    }
    PayloadJointProbe probe;
    auto result = probe.poll(phase);
    fill_limit();
    return result;
  }
};
void worker_metadata_limits(bool joint, ps::ResourceKind kind,
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
  definition.traits.joint_continuation_bytes = sizeof(MetadataProbe);
  definition.start_result = [control](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<MetadataProbe>(allocator, control);
  };
  definition.start_result_joint = [control](const auto&,
                                            const auto& allocator) {
    return ps::ResultJointContinuation::make<MetadataProbe>(allocator, control);
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
    ps::ExecutionOptions options;
    options.enable_joint = joint;
    auto result =
        execution.execute_atoms(compiled.plan, {},
                                {{"a", take(ps::Footprint::all({2}))},
                                 {"b", take(ps::Footprint::all({2}))}},
                                {}, options);
    if (prior_protocol) {
      require(
          !result.ok() &&
              result.status().detail.origin == ps::FailureOrigin::Protocol &&
              result.status().reason == ps::FailureReason::UnauthorizedRead,
          "prior protocol failure must survive later metadata exhaustion");
    } else if (result.ok()) {
      require(result.value().atoms.size() == 2, "metadata failure atoms");
      for (const auto& atom : result.value().atoms)
        require(!atom.outcome.ok() && atom.outcome.status().code ==
                                          ps::ErrorCode::ResourceExhausted,
                "worker bookkeeping must respect managed resource limits");
    } else {
      require(result.status().code == ps::ErrorCode::ResourceExhausted,
              "metadata failure status");
    }
    if (!prior_protocol)
      require(control->root->statistics().peak[kind] == control->limit,
              "probe reached selected resource limit");
  }
  require(control->root->statistics().live[kind] == 0,
          "worker bookkeeping and probe lease must retire");
  std::cout << "managed bookkeeping limit joint=" << joint
            << " kind=" << static_cast<unsigned>(kind) << " passed\n";
}
void output_payload_bounds() {
  unsigned calls = 0;
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
    request.tensor_outputs = take(ps::Footprint::all({2}));
    request.semantic_key = "payload-probe";
    request.snapshot_identity = "payload-inputs";
    auto session = take(registry->start_result("manual.output_payload", request,
                                               root.allocator()));

    ps::ResultObjectInputs no_results;
    ps::ResourceVector<ps::ResultIoReply> no_io;
    auto allocator = root.allocator();
    auto failure =
        std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok);
    ps::ResultProgramPhase phase{
        request, no_results,
        no_io,   allocator,
        root,    [root](auto work) { return root.consume({work}); },
        failure};
    auto prior = take(registry->start_result("manual.output_payload", request,
                                             root.allocator()));
    auto stopped_phase = phase;
    stopped_phase.failure = std::make_shared<std::atomic<ps::ErrorCode>>(
        ps::ErrorCode::InvalidArgument);
    const auto before = calls;
    require(prior.poll(stopped_phase).status().code ==
                    ps::ErrorCode::InvalidArgument &&
                calls == before,
            "preexisting direct protocol failure prevents bound callback");
    const auto status = session.poll(phase).status();
    require(status.code == ps::ErrorCode::ResourceExhausted &&
                status.reason == ps::FailureReason::CapacityLimit,
            "workspace cannot expand the declared Result output payload bound");
    require(
        session.poll(phase).status().reason == ps::FailureReason::CapacityLimit,
        "Result output bound failure remains sticky");
    auto second = request;
    second.output_index = 1;
    auto joint = take(registry->start_result_joint("manual.output_payload",
                                                   {request, second}, root));
    auto joint_failure =
        std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok);
    ps::ResultProgramPhase first{
        request,      no_results,
        no_io,        allocator,
        root,         [root](auto work) { return root.consume({work}); },
        joint_failure};
    ps::ResultProgramPhase other{second,       no_results, no_io,
                                 allocator,    root,       first.consume_work,
                                 joint_failure};
    ps::ResourceVector<const ps::ResultProgramPhase*> members{&first, &other};
    auto events = take(joint.poll({members, allocator, first.consume_work}));
    require(events.size() == 2, "Result joint payload event count");
    for (const auto& event : events)
      require(
          !event.outcome.ok() &&
              event.outcome.status().reason ==
                  ps::FailureReason::CapacityLimit &&
              event.outcome.status().detail.scope == ps::FailureScope::Atom &&
              event.outcome.status().detail.atom == event.key,
          "joint enforces each Result member's output payload bound");
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
    query.tensor_outputs = take(ps::Footprint::all({2}));
    query.semantic_key = "payload.guard";
    auto state = take(registry->start_result("manual.output_payload", query,
                                             guard_root.allocator()));
    const auto live = guard_root.statistics().live[ps::ResourceKind::Metadata];
    auto occupied = take(guard_root.reserve(ps::ResourceCapacity::host(
        65536 - live - ps::ResourceBudget::lease_metadata_bytes(),
        65536 - live - ps::ResourceBudget::lease_metadata_bytes())));

    ps::ResultObjectInputs no_results;
    ps::ResourceVector<ps::ResultIoReply> no_io;
    auto allocator = guard_root.allocator();
    ps::ResultProgramPhase phase{
        query,
        no_results,
        no_io,
        allocator,
        guard_root,
        [guard_root](auto work) { return guard_root.consume({work}); },
        std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok)};
    const auto before = calls;
    require(
        state.poll(phase).status().code == ps::ErrorCode::ResourceExhausted &&
            calls == before,
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
    query.tensor_outputs = take(ps::Footprint::all({2}));
    query.semantic_key = "foreign.payload";
    auto state = take(foreign_registry->start_result(
        "manual.output_payload", query, normal_root.allocator()));

    ps::ResultObjectInputs no_results;
    ps::ResourceVector<ps::ResultIoReply> no_io;
    auto allocator = normal_root.allocator();
    ps::ResultProgramPhase phase{
        query,
        no_results,
        no_io,
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
  for (bool grouping : {false, true}) {
    ps::ExecutionOptions options;
    options.enable_joint = grouping;
    auto result =
        take(context.execute_atoms(compiled.plan, {},
                                   {{"a", take(ps::Footprint::all({2}))},
                                    {"b", take(ps::Footprint::all({2}))}},
                                   {}, options));
    require(result.atoms.size() == 2,
            "workflow payload failures are per-member");
    for (const auto& atom : result.atoms)
      require(!atom.outcome.ok() && atom.outcome.status().reason ==
                                        ps::FailureReason::CapacityLimit,
              "workflow rejects excessive new output backing");
  }
  for (bool reuse : {false, true}) {
    auto prefix_definition = payload_probe(false);
    auto prefix_schema = payload_schema();
    prefix_schema.publication = ps::PublishPolicy::IndependentChunks;
    prefix_schema.tensors[0].atomic_trailing_axes = 0;
    prefix_definition.traits.outputs.resize(1);
    prefix_definition.traits.outputs[0].result_schema = prefix_schema;
    prefix_definition.traits.outputs[0].failure_delivery =
        ps::FailureDelivery::RequestFailureOnly;
    prefix_definition.traits.outputs[0].continuation_bytes =
        sizeof(PrefixPayloadProbe);
    prefix_definition.traits.joint_contract = 0;
    prefix_definition.traits.joint_continuation_bytes = 0;
    prefix_definition.start_result_joint = {};
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
      query.tensor_outputs = take(ps::Footprint::all({2}));
      query.semantic_key = "prefix-payload";
      auto state = take(prefix_registry.start_result(
          "manual.output_payload", query, prefix_root.allocator()));

      ps::ResultObjectInputs no_results;
      ps::ResourceVector<ps::ResultIoReply> no_io;
      auto allocator = prefix_root.allocator();
      ps::ResultProgramPhase phase{
          query,
          no_results,
          no_io,
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
  for (bool grouping : {false, true}) {
    ps::ExecutionOptions options;
    options.enable_joint = grouping;
    auto result =
        take(mixed_context.execute_atoms(mixed_plan, {},
                                         {{"a", take(ps::Footprint::all({2}))},
                                          {"b", take(ps::Footprint::all({2}))}},
                                         {}, options));
    require(result.atoms.size() == 2 && !result.atoms[0].outcome.ok() &&
                result.atoms[0].outcome.status().reason ==
                    ps::FailureReason::CapacityLimit &&
                result.atoms[1].outcome.ok(),
            "one member's output bound failure preserves legal sibling");
  }
  auto cancellation_registry = std::make_shared<ps::OperationRegistry>();
  bool returned = false;
  struct CancellingJoint {
    bool* returned;
    explicit CancellingJoint(bool* flag) : returned(flag) {}
    ps::Result<ps::ResourceVector<ps::ResultJointOutcome>> poll(
        const ps::ResultJointPhase& phase) {
      PayloadJointProbe probe;
      auto result = probe.poll(phase);
      *returned = true;
      return result;
    }
  };
  auto cancelling_definition = payload_probe(false);
  for (auto& output : cancelling_definition.traits.outputs)
    output.maximum_output_payload_bytes = 16;
  cancelling_definition.traits.joint_continuation_bytes =
      sizeof(CancellingJoint);
  cancelling_definition.start_result_joint =
      [&returned](const auto&, const auto& allocator) {
        return ps::ResultJointContinuation::make<CancellingJoint>(allocator,
                                                                  &returned);
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
    first_query.tensor_outputs = take(ps::Footprint::all({2}));
    first_query.cancellation = stop.token();
    auto second_query = first_query;
    second_query.output_index = 1;
    second_query.semantic_key = "payload.cancel.other";
    second_query.cancellation = {};
    auto state = take(cancellation_registry->start_result_joint(
        "manual.output_payload", {first_query, second_query},
        cancellation_root));

    ps::ResultObjectInputs no_results;
    ps::ResourceVector<ps::ResultIoReply> no_io;
    auto allocator = cancellation_root.allocator();
    auto work = [cancellation_root](auto amount) {
      return cancellation_root.consume({amount});
    };
    ps::ResultProgramPhase first{
        first_query,
        no_results,
        no_io,
        allocator,
        cancellation_root,
        [&](auto amount) {
          if (returned)
            stop.cancel();
          return work(amount);
        },
        std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok)};
    ps::ResultProgramPhase second{
        second_query,
        no_results,
        no_io,
        allocator,
        cancellation_root,
        work,
        std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok)};
    ps::ResourceVector<const ps::ResultProgramPhase*> members{&first, &second};
    auto events = take(state.poll({members, allocator, work}));
    require(events.size() == 2 && !events[0].outcome.ok() &&
                events[0].outcome.status().code == ps::ErrorCode::Cancelled &&
                events[1].outcome.ok(),
            "cancellation during payload validation preserves healthy peer");
  }
  auto borrowing = std::make_shared<ps::OperationRegistry>();
  require(borrowing->register_operation(payload_probe(true)).ok(),
          "register borrowed Result probe");
  require(borrowing->freeze().ok(), "freeze borrowed Result probe");
  ps::ExecutionContext borrowed_context(borrowing);
  auto resources = take(borrowed_context.resource_budget());
  auto builder = take(
      ps::ResultBuilder::start(resources, payload_schema(), "payload.source"));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(resources, 1, {})))
              .ok(),
          "borrowed source descriptor");
  auto input_bytes = take(resources.allocator().allocate(16));
  std::memset(input_bytes.data(), 0, input_bytes.size());
  require(
      builder
          .publish_tensor(0, ps::Region::whole({2}), {0, {8}},
                          std::move(input_bytes).freeze(),
                          take(ps::ResultRelation::cartesian(resources, 2, {})),
                          {true, true, true, true})
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
  std::cout << "Result output payload: direct/joint/workflow bounds and "
               "borrowed view passed\n";
}
struct ArrayWorkflow {
  std::shared_ptr<ps::OperationRegistry> registry;
  std::unique_ptr<ps::ExecutionContext> context;
  ps::ResourceBudget root;
  explicit ArrayWorkflow(std::shared_ptr<ps::OperationRegistry> operations =
                             ps::make_default_operation_registry(),
                         std::uint64_t payload = 65536, std::uint64_t cache = 0,
                         ps::ResourceLimits limits = {})
      : registry(std::move(operations)) {
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = payload;
    config.result_cache_bytes = cache;
    config.managed_resources = std::move(limits);
    context = std::make_unique<ps::ExecutionContext>(registry, config);
    root = take(context->resource_budget());
  }
  ps::ResultRef source(const ps::Value& value) {
    auto schema = numeric_result_fixture::source_schema(value);
    schema.id = "manual.array.input";
    return numeric_result_fixture::source(root, value, &schema);
  }
  static ps::WorkflowDocument document(ps::WorkflowNode node,
                                       const ps::ResultRef& input) {
    ps::WorkflowDocument document;
    ps::WorkflowInputDeclaration declaration;
    declaration.id = 1;
    declaration.name = "input";
    declaration.result_schema =
        std::make_shared<ps::SchemaTemplate>(input.schema());
    document.inputs.push_back(std::move(declaration));
    document.nodes.push_back(std::move(node));
    document.outputs = {{"values", document.nodes[0].id, "values"}};
    return document;
  }
  ps::ResultRef run(ps::WorkflowNode node, const ps::ResultRef& input) {
    ps::GraphContext graph(document(std::move(node), input));
    auto plan = take(ps::Compiler(registry).compile(graph)).plan;
    return take(context->execute(plan, {{{"input", input}}}))
        .results.at("values");
  }
};
std::uint64_t array_bits(const ps::ResultRef& result,
                         const std::vector<std::uint64_t>& at) {
  std::uint64_t bits = 0;
  require(numeric_result_fixture::read(
              result, at, &bits,
              ps::Value::element_size(
                  result.schema().tensors[0].descriptor.element_type))
              .ok(),
          "array Result sample read");
  return bits;
}
void constant(const std::string& profile, const std::string& shape,
              ps::ElementType type, std::uint64_t bits) {
  ps::ResultRef held;
  std::optional<ps::ResourceBudget> root;
  const auto width = ps::Value::element_size(type);
  {
    ArrayWorkflow workflow;
    root = workflow.root;
    auto input = workflow.source(scalar(type, bits));
    ps::WorkflowNode node{1,
                          "numeric.constant" + profile,
                          {ps::WorkflowInputReference{1}},
                          {{"shape", shape}, {"layout", std::string("view")}}};
    auto original = take(workflow.registry->find_traits(node.operation));
    ps::OperationMetadata metadata;
    metadata.result_schema =
        std::make_shared<ps::SchemaTemplate>(input.schema());
    require(!ps::infer_operation_outputs(original, {metadata}, node.parameters)
                 .ok(),
            "unresolved shape template must not infer placeholder shape");
    ps::GraphContext graph(ArrayWorkflow::document(node, input));
    auto compiled = take(ps::Compiler(workflow.registry).compile(graph));
    auto frozen =
        take(workflow.context->freeze(compiled.plan, {{{"input", input}}}));
    const auto& tensor =
        compiled.plan.steps().back().output_result_schema->tensors[0];
    auto result = take(workflow.context->execute_fragments(
        frozen, {{"values", take(ps::Footprint::all(tensor.sample_shape()))}}));
    auto ordinary =
        take(workflow.context->execute(compiled.plan, {{{"input", input}}}));
    for (const auto& output :
         {ordinary.results.at("values"), result.results.at("values")}) {
      auto window =
          take(output.acquire_tensor(take(output.descriptor()), 0,
                                     ps::Region::whole(tensor.sample_shape())));
      auto rectangle = take(window.rectangle_run(
          std::vector<std::uint64_t>(tensor.descriptor.shape.size(), 0)));
      require(rectangle.row.bytes == width &&
                  rectangle.row.sample_stride_bytes == 0 &&
                  rectangle.row_stride_bytes == 0 && tensor.facets.empty(),
              "ordinary and frozen Result constant preserve scalar-size view");
    }
    held = result.results.at("values");
    require(root->statistics().peak[ps::ResourceKind::Payload] < 4096,
            "large view must not reserve logical dense bytes");
    std::vector<std::uint64_t> last;
    for (auto extent : tensor.descriptor.shape)
      last.push_back(extent - 1);
    require(array_bits(held, last) == bits,
            "constant Result last element bits");
  }
  require(root->statistics().live[ps::ResourceKind::Payload] >= width,
          "view owner survives context");
  held = {};
  require(root->statistics().live[ps::ResourceKind::Payload] == 0,
          "last owner releases scalar payload");
  std::cout << "constant Result view shape=" << shape
            << " stored_bytes=" << width << " passed\n";
}
void dense_and_schema() {
  ArrayWorkflow workflow;
  auto input = workflow.source(scalar(ps::ElementType::Int64, 7));
  auto node =
      take(ps::numeric::constant_node(1, ps::WorkflowInputReference{1}, {2, 3},
                                      ps::numeric::ArrayLayout::Dense));
  auto result = workflow.run(node, input);
  auto window = take(result.acquire_tensor(take(result.descriptor()), 0,
                                           ps::Region::whole({2, 3})));
  auto rectangle = take(window.rectangle_run({0, 0}));
  require(rectangle.row.samples == 3 && rectangle.rows == 2 &&
              rectangle.row.sample_stride_bytes == 8 &&
              rectangle.row_stride_bytes == 24,
          "dense Result constant publishes packed [2,3] rectangle");
  for (std::uint64_t i = 0; i < 2; ++i)
    for (std::uint64_t j = 0; j < 3; ++j)
      require(array_bits(result, {i, j}) == 7,
              "dense constant must contain six packed sevens");
  auto document = ArrayWorkflow::document(node, input);
  for (const auto* shape : {"", "01,3", "2, 3", "2,", "0,3", "1048576,1048577",
                            "+1", "1,1,1,1,1,1,1,1,1"}) {
    document.nodes[0].parameters["shape"] = std::string(shape);
    ps::GraphContext invalid(document);
    const auto status =
        ps::Compiler(workflow.registry).compile(invalid).status();
    require(status.code == ps::ErrorCode::InvalidArgument &&
                status.reason == ps::FailureReason::InvalidDomain &&
                status.detail.origin == ps::FailureOrigin::Schema,
            "invalid canonical shape must fail as schema error");
  }
  std::cout << "constant Result dense [2,3]=[7,7,7,7,7,7], authoring and shape "
               "errors passed\n";
}
struct CancelArrayProgram {
  ps::ResultContinuation inner;
  ps::CancellationSource cancellation;
  bool during_copy;
  std::uint64_t output_bytes, copied_work = 0;
  std::shared_ptr<bool> observed;
  CancelArrayProgram(ps::ResultContinuation continuation,
                     ps::CancellationSource stop, bool copying,
                     std::uint64_t bytes, std::shared_ptr<bool> entered)
      : inner(std::move(continuation)),
        cancellation(std::move(stop)),
        during_copy(copying),
        output_bytes(bytes),
        observed(std::move(entered)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    auto controlled = phase;
    controlled.consume_work = [&](std::uint64_t amount) {
      auto status = phase.consume_work(amount);
      if (status.ok() &&
          phase.resources.statistics().live[ps::ResourceKind::Payload] >=
              output_bytes) {
        copied_work += amount;
        if (!during_copy || copied_work >= 100000) {
          *observed = true;
          cancellation.cancel();
        }
      }
      return status;
    };
    return inner.poll(controlled);
  }
};
void cancel_dense_array(const std::string& profile, bool constant,
                        bool during_copy) {
  constexpr std::uint64_t count = 1048576;
  auto registry = ps::make_default_operation_registry(false);
  auto seed = scalar(ps::ElementType::Int64, 7);
  auto input_schema = numeric_result_fixture::source_schema(seed);
  input_schema.id = "manual.array.input";
  auto node = constant ? take(ps::numeric::constant_node(
                             1, ps::WorkflowInputReference{1}, {count},
                             ps::numeric::ArrayLayout::Dense))
                       : take(ps::numeric::broadcast_node(
                             1, ps::WorkflowInputReference{1}, {count}, {0},
                             ps::numeric::ArrayLayout::Dense));
  node.operation.replace(node.operation.find("_strict"), 7, profile);
  const auto original = node.operation;
  ps::OperationMetadata metadata;
  metadata.result_schema =
      std::make_shared<const ps::SchemaTemplate>(input_schema);
  auto traits =
      take(registry->resolve_traits(original, {metadata}, node.parameters));
  traits.requires_metadata_specialization = false;
  ps::CancellationSource cancellation;
  auto observed = std::make_shared<bool>(false);
  ps::OperationDefinition controlled;
  controlled.key = "test.cancel_dense_array";
  controlled.traits = std::move(traits);
  controlled.traits.outputs[0].continuation_bytes +=
      sizeof(CancelArrayProgram) + 1024;
  const auto weak = std::weak_ptr<ps::OperationRegistry>(registry);
  controlled.start_result = [weak, original, cancellation, observed,
                             during_copy](
                                const ps::ResultProgramQuery& query,
                                const ps::BufferAllocator& allocator) {
    auto owner = weak.lock();
    if (!owner)
      return ps::Result<ps::ResultContinuation>(
          ps::Status{ps::ErrorCode::Stale, {}});
    auto nested = query;
    nested.prepared.reset();
    auto inner = owner->start_result(original, nested, allocator);
    if (!inner.ok())
      return inner;
    return ps::ResultContinuation::make<CancelArrayProgram>(
        allocator, inner.take_value(), cancellation, during_copy, count * 8,
        observed);
  };
  require(registry->register_operation(std::move(controlled)).ok(),
          "controlled array registration");
  require(registry->freeze().ok(), "controlled array registry");
  node.operation = "test.cancel_dense_array";
  ArrayWorkflow workflow(registry, 16 * 1048576);
  auto input = workflow.source(seed);
  ps::GraphContext graph(ArrayWorkflow::document(node, input));
  auto compiled = take(ps::Compiler(registry).compile(graph));
  auto frozen =
      take(workflow.context->freeze(compiled.plan, {{{"input", input}}}));
  const auto before = workflow.root.statistics();
  const auto status =
      workflow.context
          ->execute_fragments(frozen,
                              {{"values", take(ps::Footprint::all({count}))}},
                              cancellation.token())
          .status();
  const auto after = workflow.root.statistics();
  require(
      *observed && status.code == ps::ErrorCode::Cancelled &&
          after.peak[ps::ResourceKind::Payload] >= count * 8 &&
          after.live[ps::ResourceKind::Payload] ==
              before.live[ps::ResourceKind::Payload],
      "cancel allocated/active Whole Result dense output and release payload");
}

void array_boundaries(const std::string& profile) {
  cancel_dense_array(profile, false, false);
  ArrayWorkflow workflow;
  for (std::uint64_t bits = 0; bits < 256; ++bits) {
    auto input = workflow.source(scalar(ps::ElementType::UInt8, bits));
    for (auto layout :
         {ps::numeric::ArrayLayout::View, ps::numeric::ArrayLayout::Dense}) {
      auto node = take(ps::numeric::constant_node(
          1, ps::WorkflowInputReference{1}, {2, 3}, layout));
      node.operation = "numeric.constant" + profile;
      auto output = workflow.run(node, input);
      for (std::uint64_t i = 0; i < 2; ++i)
        for (std::uint64_t j = 0; j < 3; ++j)
          require(array_bits(output, {i, j}) == bits,
                  "exhaustive UInt8 Result constant bits");
    }
  }
  auto storage = take(ps::BufferAllocator{}.allocate(49));
  const std::int64_t matrix[] = {1, 2, 3, 4, 5, 6};
  std::memcpy(storage.data() + 1, matrix, 48);
  auto input = workflow.source(take(ps::Value::from_storage(
      {ps::ElementType::Int64, {2, 3}}, ps::Region::whole({2, 3}),
      {25, {-24, 8}}, std::move(storage).freeze())));
  for (auto layout :
       {ps::numeric::ArrayLayout::View, ps::numeric::ArrayLayout::Dense}) {
    auto node = take(ps::numeric::broadcast_node(
        1, ps::WorkflowInputReference{1}, {3, 4, 2}, {2, 0}, layout));
    node.operation = "numeric.broadcast" + profile;
    auto output = workflow.run(node, input);
    for (std::uint64_t j = 0; j < 3; ++j)
      for (std::uint64_t k = 0; k < 4; ++k)
        for (std::uint64_t i = 0; i < 2; ++i) {
          std::int64_t actual = 0;
          require(numeric_result_fixture::read(output, {j, k, i}, &actual, 8)
                          .ok() &&
                      actual == matrix[(1 - i) * 3 + j],
                  "negative unaligned Result permutation oracle");
        }
  }
  std::cout << "array Result boundaries: cancellation, 256 UInt8 values, "
               "negative unaligned permutation passed\n";
}
struct ArraySplitSource {
  unsigned* calls;
  explicit ArraySplitSource(unsigned* counter) : calls(counter) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    ++*calls;
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key));
    require(builder
                .bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(phase.resources, 1, {})))
                .ok(),
            "split source descriptor");
    for (std::uint64_t i = 0; i < 2; ++i) {
      auto bytes = take(phase.resources.allocator().allocate(8));
      const std::int64_t value = 10 + i;
      std::memcpy(bytes.data(), &value, 8);
      require(builder
                  .publish_tensor(0, ps::Region({{i, 1}}), {0, {8}, {i}},
                                  std::move(bytes).freeze(),
                                  take(ps::ResultRelation::cartesian(
                                      phase.resources, 2, {})),
                                  {true, true, true, true})
                  .ok(),
              "split source independent backing");
    }
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};
void staged_array_support(const std::string& profile) {
  auto registry = ps::make_default_operation_registry(false);
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  const float rgba[] = {1, 2, 3, 2};
  unsigned calls = 0;
  ps::OperationDefinition split;
  split.key = "manual.array_split";
  split.traits.input_count = 0;
  split.traits.input_schema.clear();
  ps::SchemaTemplate schema;
  schema.id = "manual.array.split";
  ps::ResultTensorSpec tensor;
  tensor.key = "parts";
  tensor.descriptor = {ps::ElementType::Int64, {2}};
  schema.tensors.push_back(std::move(tensor));
  auto& output = split.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = std::move(schema);
  output.region_rule = ps::OperationRegionRule::Whole;
  output.continuation_bytes = sizeof(ArraySplitSource);
  output.maximum_dependency_stages = 1;
  split.start_result = [&](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<ArraySplitSource>(allocator, &calls);
  };
  require(registry->register_operation(std::move(split)).ok() &&
              registry->freeze().ok(),
          "split Result source registration");
  ArrayWorkflow workflow(registry, 1048576);
  ps::SchemaTemplate image_schema;
  image_schema.id = "manual.array.image";
  ps::ResultTensorSpec pixels;
  pixels.key = "pixels";
  pixels.descriptor = {ps::ElementType::Float32, {1, 1, 4}};
  pixels.layout.spatial = true;
  pixels.facets = {facet};
  image_schema.tensors.push_back(std::move(pixels));
  auto image = take(ps::ResultBuilder::start(workflow.root, image_schema,
                                             "array.invalid.image"));
  require(image
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(workflow.root, 1, {})))
              .ok(),
          "image source descriptor");
  require(image
              .publish_tensor(
                  0, ps::Region::whole({1, 1, 4}),
                  ps::ByteView(reinterpret_cast<const std::uint8_t*>(rgba),
                               sizeof(rgba)),
                  take(ps::ResultRelation::cartesian(workflow.root, 4, {})),
                  {true, true, true, true})
              .ok(),
          "image source tensor");
  auto input = take(image.seal());
  auto invalid_node = take(ps::numeric::broadcast_node(
      1, ps::WorkflowInputReference{1}, {1, 1, 4}, {0, 1, 2}));
  invalid_node.operation = "numeric.broadcast" + profile;
  ps::GraphContext invalid_graph(ArrayWorkflow::document(invalid_node, input));
  auto invalid_plan = take(ps::Compiler(registry).compile(invalid_graph));
  auto rejected =
      workflow.context->execute(invalid_plan.plan, {{{"input", input}}});
  require(
      !rejected.ok() &&
          rejected.status().code == ps::ErrorCode::InvalidArgument,
      ("Whole Result typed alpha: code=" +
       std::to_string(static_cast<unsigned>(rejected.status().code)) +
       " reason=" +
       std::to_string(static_cast<unsigned>(rejected.status().reason)) +
       " origin=" +
       std::to_string(static_cast<unsigned>(rejected.status().detail.origin)) +
       " peak=" +
       std::to_string(
           workflow.root.statistics().peak[ps::ResourceKind::Payload]) +
       " live=" +
       std::to_string(
           workflow.root.statistics().live[ps::ResourceKind::Payload]) +
       " message=" + rejected.status().message)
          .c_str());
  ps::WorkflowDocument document;
  auto broadcast = take(ps::numeric::broadcast_node(
      2, ps::WorkflowNodeOutput{1, "value"}, {3, 2}, {1}));
  broadcast.operation = "numeric.broadcast" + profile;
  document.nodes = {{1, "manual.array_split", {}, {}}, broadcast};
  document.outputs = {{"values", 2, "values"}};
  ps::GraphContext graph(document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  auto failed = workflow.context->execute(plan.plan);
  require(!failed.ok() && failed.status().message.find("ViewUnavailable") !=
                              std::string::npos,
          "Whole Result broadcast View rejects multi-owner input");
  auto frozen = take(workflow.context->freeze(plan.plan));
  const auto before = calls;
  auto empty = take(workflow.context->execute_fragments(
      frozen, {{"values", take(ps::Footprint::none({3, 2}))}}));
  require(calls == before &&
              take(empty.results.at("values").descriptor())
                  .tensor_coverage(0)
                  .empty() &&
              take(empty.dependencies.source_support()).empty(),
          "Empty Result broadcast never executes source or reads payload");
  document.nodes[1].parameters["layout"] = std::string("dense");
  ps::GraphContext dense_graph(document);
  auto dense_plan = take(ps::Compiler(registry).compile(dense_graph));
  auto dense = take(workflow.context->execute(dense_plan.plan));
  for (std::uint64_t i = 0; i < 3; ++i)
    for (std::uint64_t j = 0; j < 2; ++j)
      require(array_bits(dense.results.at("values"), {i, j}) == 10 + j,
              "Dense Result broadcast collects multiple owners");
  std::cout << "Whole Result typed validation, multi-owner View failure/Dense "
               "collect "
               "and Empty passed\n";
}
void array_bitpatterns(const std::string& profile) {
  ArrayWorkflow workflow;
  for (auto type : {ps::ElementType::Int64, ps::ElementType::Float32,
                    ps::ElementType::Float64}) {
    const std::vector<std::uint64_t> bits =
        type == ps::ElementType::Float32
            ? std::vector<std::uint64_t>{0,          0x80000000, 0x7f800000,
                                         0xff800000, 0x7f800001, 0x7fc01234,
                                         1,          0x7f7fffff}
            : std::vector<std::uint64_t>{0,
                                         UINT64_C(0x8000000000000000),
                                         UINT64_C(0x7ff0000000000000),
                                         UINT64_C(0xfff0000000000000),
                                         UINT64_C(0x7ff0000000000001),
                                         UINT64_C(0x7ff8000000001234),
                                         1,
                                         UINT64_C(0x7fffffffffffffff),
                                         UINT64_MAX};
    for (auto pattern : bits) {
      const auto input = workflow.source(scalar(type, pattern));
      for (auto layout :
           {ps::numeric::ArrayLayout::View, ps::numeric::ArrayLayout::Dense}) {
        auto fill = take(ps::numeric::constant_node(
            1, ps::WorkflowInputReference{1}, {2, 3}, layout));
        auto broadcast = take(ps::numeric::broadcast_node(
            1, ps::WorkflowInputReference{1}, {2, 3}, {0}, layout));
        for (auto node : {fill, broadcast}) {
          node.operation.replace(node.operation.find("_strict"), 7, profile);
          auto output = workflow.run(node, input);
          require(array_bits(output, {1, 2}) == pattern,
                  "integer extrema and IEEE bitpattern copy");
        }
      }
    }
  }
  std::cout << "array bitpatterns: integer extrema, signed zeros, infinities, "
               "subnormals, NaN payloads passed\n";
}
ps::Value vector_value(const std::vector<std::int64_t>& values) {
  auto bytes = take(ps::BufferAllocator{}.allocate(values.size() * 8));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return take(ps::Value::from_storage({ps::ElementType::Int64, {values.size()}},
                                      ps::Region::whole({values.size()}),
                                      {0, {8}}, std::move(bytes).freeze()));
}
void broadcast_cache() {
  ArrayWorkflow workflow(ps::make_default_operation_registry(), 65536, 32768);
  auto input = workflow.source(vector_value({10, 20, 30}));
  auto node = take(ps::numeric::broadcast_node(1, ps::WorkflowInputReference{1},
                                               {2, 3}, {1}));
  ps::GraphContext graph(ArrayWorkflow::document(node, input));
  auto compiled = take(ps::Compiler(workflow.registry).compile(graph));
  ps::ExecutionBindings bindings{{{"input", input}}};
  auto frozen = take(workflow.context->freeze(compiled.plan, bindings));
  auto demand = take(workflow.context->open_demand(compiled.plan, bindings));
  const ps::DemandQuery query{
      {"values", take(ps::Footprint::from_regions(
                     {2, 3}, {ps::Region({{0, 2}, {0, 1}})}))}};
  auto cold = take(demand.request(query));
  auto repeated = take(demand.request(query));
  require(repeated.results.at("values").object_id() ==
              cold.results.at("values").object_id(),
          "same frozen Result demand reuses completed producer identity");
  auto equivalent = workflow.source(vector_value({10, 20, 30}));
  auto warm_handle = take(
      workflow.context->open_demand(compiled.plan, {{{"input", equivalent}}}));
  auto warm = take(warm_handle.request(query));
  require(warm.diagnostics.cache_hits > 0 &&
              warm.results.at("values").association() ==
                  ps::ResourceVector<std::uint64_t>{equivalent.object_id()},
          "fresh equivalent Result source reuses warm broadcast cache");
  auto changed = workflow.source(vector_value({10, 99, 30}));
  bindings.inputs[0].result = changed;
  require(demand.replace_bindings(bindings).ok(),
          "replace unobserved Result broadcast sample");
  auto unchanged = take(demand.request(query));
  require(unchanged.diagnostics.cache_hits == 0 &&
              array_bits(unchanged.results.at("values"), {1, 0}) == 10,
          "Whole Result broadcast invalidates on any active source edit");
  auto updated_input = workflow.source(vector_value({99, 99, 30}));
  bindings.inputs[0].result = updated_input;
  require(demand.replace_bindings(bindings).ok(),
          "replace observed Result broadcast sample");
  auto updated = take(demand.request(query));
  require(array_bits(updated.results.at("values"), {1, 0}) == 99 &&
              array_bits(
                  take(workflow.context->execute(frozen)).results.at("values"),
                  {1, 0}) == 10,
          "Result rebinding updates demand and preserves old frozen input");
  std::cout << "Result broadcast cache: warm hit, Whole edit invalidation, "
               "immutable frozen input passed\n";
}
void whole_array_budgets(const std::string& profile) {
  const std::uint64_t count = 1048576;
  for (bool constant : {true, false}) {
    for (bool work : {false, true}) {
      std::optional<ps::ResourceBudget> root;
      {
        ps::ResourceLimits limits;
        if (work)
          limits.maximum_work = 100000;
        ArrayWorkflow workflow(ps::make_default_operation_registry(),
                               work ? 16 * 1048576 : 1024, 0, limits);
        root = workflow.root;
        auto input = workflow.source(scalar(ps::ElementType::Int64, 7));
        auto node = constant ? take(ps::numeric::constant_node(
                                   1, ps::WorkflowInputReference{1}, {count},
                                   ps::numeric::ArrayLayout::Dense))
                             : take(ps::numeric::broadcast_node(
                                   1, ps::WorkflowInputReference{1}, {count},
                                   {0}, ps::numeric::ArrayLayout::Dense));
        node.operation.replace(node.operation.find("_strict"), 7, profile);
        ps::GraphContext graph(ArrayWorkflow::document(node, input));
        auto compiled = take(ps::Compiler(workflow.registry).compile(graph));
        const auto before = root->statistics().live[ps::ResourceKind::Payload];
        auto result =
            workflow.context->execute(compiled.plan, {{{"input", input}}});
        require(
            !result.ok() &&
                result.status().code == ps::ErrorCode::ResourceExhausted &&
                result.status().reason ==
                    (work ? ps::FailureReason::WorkLimit
                          : ps::FailureReason::CapacityLimit) &&
                root->statistics().live[ps::ResourceKind::Payload] == before &&
                root->statistics().peak[ps::ResourceKind::Payload] < count * 8,
            ("Whole Result budget: constant=" + std::to_string(constant) +
             " work=" + std::to_string(work) +
             " before=" + std::to_string(before) + " live=" +
             std::to_string(
                 root->statistics().live[ps::ResourceKind::Payload]) +
             " peak=" +
             std::to_string(
                 root->statistics().peak[ps::ResourceKind::Payload]) +
             " code=" +
             std::to_string(static_cast<unsigned>(result.status().code)) +
             " reason=" +
             std::to_string(static_cast<unsigned>(result.status().reason)))
                .c_str());
      }
      require(root->statistics().live[ps::ResourceKind::Payload] == 0,
              "array failed run releases final managed source payload");
    }
    cancel_dense_array(profile, constant, true);
  }
  std::cout << "Whole Result arrays: output/work admission and cancellation "
               "during copying passed\n";
}
void array_schema_and_capacity(const std::string& profile) {
  ArrayWorkflow workflow;
  auto input = workflow.source(scalar(ps::ElementType::Int64, 7));
  ps::OperationMetadata metadata;
  metadata.result_schema = std::make_shared<ps::SchemaTemplate>(input.schema());
  const std::vector<ps::OperationMetadata> inputs{metadata};
  auto node = take(ps::numeric::broadcast_node(1, ps::WorkflowInputReference{1},
                                               {2, 3}, {1}));
  for (const auto& text : {"", "0", "-1", "01", "1,", "1,,2", "1, 2",
                           "1099511627777", "1,1,1,1,1,1,1,1,1"}) {
    auto parameters = node.parameters;
    parameters["shape"] = std::string(text);
    auto status =
        workflow.registry
            ->resolve_traits("numeric.broadcast" + profile, inputs, parameters)
            .status();
    require(status.code == ps::ErrorCode::InvalidArgument,
            "malformed Result array shape schema");
  }
  auto parameters = node.parameters;
  parameters["axis_map"] = std::string("2");
  require(workflow.registry
                  ->resolve_traits("numeric.broadcast" + profile, inputs,
                                   parameters)
                  .status()
                  .code == ps::ErrorCode::InvalidArgument,
          "out-of-range map schema");
  parameters["axis_map"] = std::string("0,1");
  require(workflow.registry
                  ->resolve_traits("numeric.broadcast" + profile, inputs,
                                   parameters)
                  .status()
                  .code == ps::ErrorCode::TypeMismatch,
          "map length schema");
  parameters["axis_map"] = std::string("0,0");
  auto matrix_schema = *metadata.result_schema;
  matrix_schema.tensors[0].descriptor.shape = {2, 3};
  ps::OperationMetadata matrix;
  matrix.result_schema = std::make_shared<ps::SchemaTemplate>(matrix_schema);
  require(workflow.registry
                  ->resolve_traits("numeric.broadcast" + profile, {matrix},
                                   parameters)
                  .status()
                  .code == ps::ErrorCode::InvalidArgument,
          "duplicate map schema");
  parameters["axis_map"] = std::string("0,1");
  parameters["shape"] = std::string("4,3");
  require(workflow.registry
                  ->resolve_traits("numeric.broadcast" + profile, {matrix},
                                   parameters)
                  .status()
                  .code == ps::ErrorCode::TypeMismatch,
          "broadcast does not tile non-singleton axes");
  node = take(ps::numeric::broadcast_node(1, ps::WorkflowInputReference{1},
                                          {1048576, 1048576}, {1},
                                          ps::numeric::ArrayLayout::Dense));
  node.operation = "numeric.broadcast" + profile;
  ps::GraphContext graph(ArrayWorkflow::document(node, input));
  auto compiled = take(ps::Compiler(workflow.registry).compile(graph));
  auto failure = workflow.context->execute(compiled.plan, {{{"input", input}}});
  require(failure.status().code == ps::ErrorCode::ResourceExhausted &&
              workflow.root.statistics().live[ps::ResourceKind::Payload] == 8,
          "large Result dense refuses capacity and leaves source payload only");
  const std::string unavailable =
      profile == "_accelerated_x86_64" ? "_accelerated_apple_silicon" :
#if defined(__aarch64__)
                                       "_accelerated_x86_64";
#else
                                       "_accelerated_apple_silicon";
#endif
  require(workflow.registry
                  ->resolve_traits("numeric.broadcast" + unavailable, inputs,
                                   node.parameters)
                  .status()
                  .code == ps::ErrorCode::BackendUnavailable,
          "incompatible profile does not fall back");
  std::cout << "Result array shape/map schema, dense capacity and unavailable "
               "profile passed\n";
}
void array_owner_and_payload_cache() {
  ps::ResultRef copied;
  ps::WeakResultRef original_owner;
  std::optional<ps::ResourceBudget> copy_root;
  const auto bits = UINT64_C(0x7ff0000000001234);
  {
    ArrayWorkflow workflow;
    copy_root = workflow.root;
    auto bytes = take(ps::BufferAllocator{}.allocate(4096));
    std::memcpy(bytes.data() + 1001, &bits, 8);
    auto input = workflow.source(take(ps::Value::from_storage(
        {ps::ElementType::Float64, {1}}, ps::Region::whole({1}), {1001, {0}},
        std::move(bytes).freeze())));
    original_owner = input.weak();
    auto source = take(input.acquire_tensor(take(input.descriptor()), 0,
                                            ps::Region::whole({1})));
    copied = workflow.run(take(ps::numeric::constant_node(
                              1, ps::WorkflowInputReference{1}, {2, 3})),
                          input);
    auto output = take(copied.acquire_tensor(take(copied.descriptor()), 0,
                                             ps::Region::whole({2, 3})));
    require(take(output.row_run({0, 0})).bytes == 8 &&
                source.storage_owner_token() != output.storage_owner_token(),
            "constant Result owns one independent scalar copy");
  }
  require(!original_owner.lock().valid() &&
              array_bits(copied, {1, 2}) == bits &&
              copy_root->statistics().live[ps::ResourceKind::Payload] == 8,
          "oversized unaligned source retires before constant Result output");
  copied = {};
  require(copy_root->statistics().live[ps::ResourceKind::Payload] == 0,
          "copied scalar Result releases final payload");
  ArrayWorkflow workflow(ps::make_default_operation_registry(), 65536, 32768);
  auto input = workflow.source(scalar(ps::ElementType::Float64, bits));
  auto node = take(
      ps::numeric::constant_node(1, ps::WorkflowInputReference{1}, {2, 3}));
  ps::GraphContext graph(ArrayWorkflow::document(node, input));
  auto plan = take(ps::Compiler(workflow.registry).compile(graph));
  ps::ExecutionBindings bindings{{{"input", input}}};
  auto frozen = take(workflow.context->freeze(plan.plan, bindings));
  auto demand = take(workflow.context->open_demand(plan.plan, bindings));
  const ps::DemandQuery query{{"values", take(ps::Footprint::all({2, 3}))}};
  auto cold = take(demand.request(query));
  require(take(demand.request(query)).results.at("values").object_id() ==
              cold.results.at("values").object_id(),
          "same frozen Result demand reuses constant producer");
  auto equivalent = workflow.source(scalar(ps::ElementType::Float64, bits));
  auto warm_handle =
      take(workflow.context->open_demand(plan.plan, {{{"input", equivalent}}}));
  require(take(warm_handle.request(query)).diagnostics.cache_hits > 0,
          "fresh equivalent NaN Result source reuses warm constant cache");
  require(take(take(cold.dependencies.potential_dirty(
                        "input", take(ps::Footprint::all({1}))))
                   .at("values")
                   .element_count()) == 6,
          "scalar dirty covers complete Result constant");
  const auto replacement = UINT64_C(0xfff8000000005678);
  bindings.inputs[0].result =
      workflow.source(scalar(ps::ElementType::Float64, replacement));
  require(demand.replace_bindings(bindings).ok(), "replace Result NaN payload");
  auto changed = take(demand.request(query));
  require(array_bits(changed.results.at("values"), {1, 2}) == replacement &&
              array_bits(
                  take(workflow.context->execute(frozen)).results.at("values"),
                  {1, 2}) == bits,
          "Result constant cache distinguishes NaN bits and preserves frozen "
          "input");
  ps::ResultRef held;
  ps::ResultTensorReadWindow held_window;
  std::optional<ps::ResourceBudget> root;
  {
    ArrayWorkflow borrowed;
    root = borrowed.root;
    auto source = borrowed.source(vector_value({10, 20, 30}));
    held = borrowed.run(take(ps::numeric::broadcast_node(
                            1, ps::WorkflowInputReference{1}, {2, 3}, {1})),
                        source);
    held_window = take(held.acquire_tensor(take(held.descriptor()), 0,
                                           ps::Region::whole({2, 3})));
  }
  require(array_bits(held, {1, 2}) == 30 &&
              root->statistics().live[ps::ResourceKind::Payload] == 24,
          "broadcast Result owner survives original source and context");
  held = {};
  std::int64_t retained_last = 0;
  std::memcpy(&retained_last, take(held_window.row_run({1, 2})).data, 8);
  require(
      retained_last == 30 &&
          root->statistics().live[ps::ResourceKind::Payload] == 24,
      "owning Result window retains broadcast backing after Result retirement");
  held_window = {};
  require(root->statistics().live[ps::ResourceKind::Payload] == 0,
          "last broadcast Result window releases managed source capacity");
  std::cout << "Result array owners: oversized source, NaN cache bits, "
               "borrowed window release passed\n";
}
struct StructuredLast {
  bool ready = false;
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    using Answer = ps::Result<ps::ResultProgramPoll>;
    const auto& input = phase.query.inputs[0].result_schema->tensors[0];
    const auto shape = input.sample_shape();
    if (!ready) {
      ready = true;
      ps::ResultProgramNeed need;
      need.tensors.push_back({0, 0, take(ps::Footprint::all(shape)), 13});
      return Answer(std::move(need));
    }
    std::vector<std::uint64_t> last;
    for (auto extent : shape)
      last.push_back(extent - 1);
    std::int64_t value = 0;
    auto status = phase.read_tensor(0, 0, last, &value, 8);
    if (!status.ok())
      return Answer(status);
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    auto relation = take(ps::ResultRelation::cartesian(
        phase.resources, 1,
        {0, 13, 0, take(ps::Footprint::all(shape)).element_count().value(),
         ps::ResultSupportTarget::Tensor, 0}));
    require(builder.bind_descriptor_relation(relation).ok(),
            "structured consumer descriptor relation");
    auto bytes = take(phase.resources.allocator().allocate(8));
    std::memcpy(bytes.data(), &value, 8);
    status = builder.publish_tensor(
        0, ps::Region::whole({1}), {0, {8}}, std::move(bytes).freeze(),
        std::move(relation), {true, true, true, true},
        phase.query.cancellation);
    if (!status.ok())
      return Answer(status);
    return Answer(ps::ResultPublication{take(builder.seal()), true});
  }
};
ps::OperationDefinition structured_last() {
  ps::OperationDefinition operation;
  operation.key = "manual.structured_last";
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  operation.traits.input_schema[0].kind = ps::OperationPortKind::Result;
  operation.traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ps::ElementType::Int64);
  ps::SchemaTemplate schema;
  schema.id = "manual.array.last";
  ps::ResultTensorSpec tensor;
  tensor.key = "last";
  tensor.descriptor = {ps::ElementType::Int64, {1}};
  schema.tensors.push_back(std::move(tensor));
  auto& output = operation.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = std::move(schema);
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.continuation_bytes = sizeof(StructuredLast);
  output.maximum_dependency_stages = 2;
  operation.start_result = [](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<StructuredLast>(allocator);
  };
  return operation;
}
void structured_views() {
  auto registry = ps::make_default_operation_registry(false);
  require(!registry->frozen(), "explicit mutable built-in registry");
  auto registered = registry->register_operation(structured_last());
  if (!registered.ok())
    throw std::runtime_error(registered.message);
  require(registry->freeze().ok(), "freeze extended built-ins");
  ps::ResultRef constant, last;
  std::optional<ps::ResourceBudget> root;
  {
    ArrayWorkflow workflow(registry);
    root = workflow.root;
    auto input = workflow.source(scalar(ps::ElementType::Int64, 7));
    auto document = ArrayWorkflow::document(
        take(ps::numeric::constant_node(1, ps::WorkflowInputReference{1},
                                        {1048576, 1048576})),
        input);
    document.nodes.push_back(take(ps::numeric::broadcast_node(
        2, ps::WorkflowInputReference{1}, {UINT64_C(274877906944), 3}, {1})));
    document.nodes.push_back({3,
                              "manual.structured_last",
                              {ps::WorkflowNodeOutput{2, "values"}},
                              {}});
    document.outputs = {{"constant", 1, "values"}, {"last", 3, "value"}};
    ps::GraphContext graph(document);
    auto compiled = take(ps::Compiler(registry).compile(graph));
    auto result =
        take(workflow.context->execute(compiled.plan, {{{"input", input}}}));
    constant = result.results.at("constant");
    last = result.results.at("last");
    auto window = take(constant.acquire_tensor(
        take(constant.descriptor()), 0, ps::Region::whole({1048576, 1048576})));
    require(take(window.row_run({0, 0})).bytes == 8 &&
                root->statistics().peak[ps::ResourceKind::Payload] < 4096,
            "structured giant Result views use scalar-size storage");
  }
  require(array_bits(constant, {1048575, 1048575}) == 7 &&
              array_bits(last, {0}) == 7,
          "structured consumer Result outputs survive context retirement");
  constant = {};
  last = {};
  require(root->statistics().live[ps::ResourceKind::Payload] == 0,
          "structured Result outputs release final managed payload");
  {
    ArrayWorkflow workflow(registry);
    ps::SchemaTemplate schema;
    schema.id = "manual.array.batch";
    ps::ResultTensorSpec tensor;
    tensor.key = "batched";
    tensor.descriptor = {ps::ElementType::Int64, {3}};
    tensor.batch_axes = {2};
    schema.tensors.push_back(std::move(tensor));
    const std::int64_t values[] = {1, 2, 3, 4, 5, 7};
    auto bytes = take(workflow.root.allocator().allocate(sizeof(values)));
    std::memcpy(bytes.data(), values, sizeof(values));
    auto builder = take(
        ps::ResultBuilder::start(workflow.root, schema, "array.batch.input"));
    require(builder
                .bind_descriptor_relation(take(ps::ResultRelation::cartesian(
                    workflow.root, 1, {0, 8, 0, 0})))
                .ok(),
            "batch source descriptor");
    require(builder
                .publish_tensor(0, ps::Region::whole({2, 3}), {0, {24, 8}},
                                std::move(bytes).freeze(),
                                take(ps::ResultRelation::cartesian(
                                    workflow.root, 6, {0, 1, 0, 0})),
                                {true, true, true, true})
                .ok(),
            "batch source tensor");
    auto input = take(builder.seal());
    auto document = ArrayWorkflow::document(
        {1, "manual.structured_last", {ps::WorkflowInputReference{1}}, {}},
        input);
    document.outputs[0].port = "value";
    ps::GraphContext graph(document);
    auto compiled = take(ps::Compiler(registry).compile(graph));
    auto result =
        take(workflow.context->execute(compiled.plan, {{{"input", input}}}));
    require(
        array_bits(result.results.at("values"), {0}) == 7,
        "structured consumer includes every batch axis in its last coordinate");
  }
  std::cout << "structured giant Result broadcast and named constant: 8-byte "
               "views, last=7 passed\n";
}
void broadcast_examples(const std::string& profile, bool large) {
  ArrayWorkflow workflow;
  auto input = workflow.source(vector_value({10, 20, 30}));
  const std::vector<std::uint64_t> shape =
      large ? std::vector<std::uint64_t>{UINT64_C(274877906944), 3}
            : std::vector<std::uint64_t>{2, 3, 4};
  auto node = take(ps::numeric::broadcast_node(1, ps::WorkflowInputReference{1},
                                               shape, {1}));
  node.operation = "numeric.broadcast" + profile;
  auto document = ArrayWorkflow::document(node, input);
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(workflow.registry).compile(graph));
  ps::ExecutionBindings bindings{{{"input", input}}};
  auto frozen = take(workflow.context->freeze(compiled.plan, bindings));
  const auto all = take(ps::Footprint::all(shape));
  auto result =
      take(workflow.context->execute_fragments(frozen, {{"values", all}}));
  const auto& view = result.results.at("values");
  auto original = take(input.acquire_tensor(take(input.descriptor()), 0,
                                            ps::Region::whole({3})));
  auto window = take(view.acquire_tensor(take(view.descriptor()), 0,
                                         ps::Region::whole(shape)));
  const auto row =
      take(window.row_run(std::vector<std::uint64_t>(shape.size(), 0)));
  const auto rectangle =
      take(window.rectangle_run(std::vector<std::uint64_t>(shape.size(), 0)));
  require(window.storage_owner_token() == original.storage_owner_token() &&
              workflow.root.statistics().live[ps::ResourceKind::Payload] == 24,
          "broadcast Result view retains only source backing");
  require(
      (large ? row.sample_stride_bytes == 8 && rectangle.row_stride_bytes == 0
             : row.sample_stride_bytes == 0 && rectangle.row_stride_bytes == 8),
      "broadcast Result mapped and replicated strides");
  std::vector<std::uint64_t> last;
  for (auto extent : shape)
    last.push_back(extent - 1);
  require(array_bits(view, last) == 30, "broadcast Result last sample");
  const auto support = take(result.dependencies.source_support());
  require(support.at("input") == take(ps::Footprint::all({3})),
          "broadcast full support deduplicates to three samples");
  auto dirty = take(result.dependencies.potential_dirty(
      "input", take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})}))));
  require(take(dirty.at("values").element_count()) == take(all.element_count()),
          "Whole Result broadcast dirty covers all output observations");
  auto ordinary = take(workflow.context->execute(compiled.plan, bindings));
  auto ordinary_window = take(ordinary.results.at("values").acquire_tensor(
      take(ordinary.results.at("values").descriptor()), 0,
      ps::Region::whole(shape)));
  require(
      ordinary_window.storage_owner_token() == original.storage_owner_token(),
      "ordinary Result broadcast preserves borrowed view");
  if (!large) {
    const auto sparse = take(ps::Footprint::from_regions(
        shape, {ps::Region({{0, 1}, {0, 1}, {0, 1}}),
                ps::Region({{1, 1}, {2, 1}, {3, 1}})}));
    auto selected =
        take(workflow.context->execute_fragments(frozen, {{"values", sparse}}));
    require(take(selected.dependencies.source_support()).at("input") ==
                take(ps::Footprint::all({3})),
            "Whole Result sparse demand retains complete input support");
    document.nodes[0].parameters["layout"] = std::string("dense");
    ps::GraphContext dense_graph(document);
    auto dense_plan =
        take(ps::Compiler(workflow.registry).compile(dense_graph));
    auto dense = take(workflow.context->execute(dense_plan.plan, bindings));
    auto output = dense.results.at("values");
    auto dense_window = take(output.acquire_tensor(take(output.descriptor()), 0,
                                                   ps::Region::whole(shape)));
    auto packed = take(dense_window.rectangle_run({0, 0, 0}));
    require(packed.row.sample_stride_bytes == 8 &&
                packed.row_stride_bytes == 32 &&
                dense.diagnostics.operation_timings.size() == 1,
            "dense Whole Result broadcast publishes packed output with one "
            "operation timing");
    const std::int64_t values[] = {10, 20, 30};
    for (std::uint64_t i = 0; i < 2; ++i)
      for (std::uint64_t j = 0; j < 3; ++j)
        for (std::uint64_t k = 0; k < 4; ++k)
          require(array_bits(output, {i, j, k}) ==
                      static_cast<std::uint64_t>(values[j]),
                  "independent dense Result broadcast coordinate oracle");
  }
  std::cout << "Result broadcast " << (large ? "large" : "[2,3,4]")
            << ": 3 source samples, exact view/dirty/support passed\n";
}
void array_profile_examples(const std::string& profile) {
  array_boundaries(profile);
  staged_array_support(profile);
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save Result array fenv");
  for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "set Result array fenv");
    array_bitpatterns(profile);
    require(fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "Result bitcopy preserves fenv");
  }
  require(fesetenv(&saved) == 0, "restore Result array fenv");
  whole_array_budgets(profile);
  array_schema_and_capacity(profile);
  broadcast_examples(profile, false);
  broadcast_examples(profile, true);
  constant(profile, "2,3", ps::ElementType::Int64, 7);
  constant(profile, "1048576,1048576", ps::ElementType::Int64, 7);
  constant(profile, "2,3", ps::ElementType::Float64,
           UINT64_C(0x7ff0000000000001));
  constant(profile, "2,3", ps::ElementType::Float32, UINT64_C(0x7f800001));
}
[[maybe_unused]] void result_array_examples() {
  dense_and_schema();
  structured_views();
  broadcast_cache();
  array_owner_and_payload_cache();
  for (const auto* profile :
       {"_strict", "_accelerated_apple_silicon", "_accelerated_x86_64"}) {
    ArrayWorkflow workflow;
    auto input = workflow.source(scalar(ps::ElementType::Int64, 7));
    ps::OperationMetadata metadata;
    metadata.result_schema =
        std::make_shared<ps::SchemaTemplate>(input.schema());
    auto available = workflow.registry->resolve_traits(
        std::string("numeric.constant") + profile, {metadata},
        {{"shape", std::string("2,3")}, {"layout", std::string("view")}});
    if (!available.ok()) {
      require(std::string(profile) != "_strict" &&
                  available.status().code == ps::ErrorCode::BackendUnavailable,
              "unavailable array profile reports BackendUnavailable");
      continue;
    }
    array_profile_examples(profile);
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
#if defined(PS_RESULT_ARRAY_TEST)
    result_array_examples();
    return 0;
#endif
#if defined(PS_RESULT_PAYLOAD_TEST)
    output_payload_bounds();
    worker_metadata_limits(false, ps::ResourceKind::Metadata, true);
    worker_metadata_limits(true, ps::ResourceKind::Metadata, true);
    for (auto kind : {ps::ResourceKind::Metadata, ps::ResourceKind::Entries}) {
      worker_metadata_limits(false, kind);
      worker_metadata_limits(true, kind);
    }
    return 0;
#endif
    const std::string profile = argc > 1 ? argv[1] : "_strict";
    dense_and_schema();
    structured_views();
    broadcast_cache();
    array_owner_and_payload_cache();
    array_profile_examples(profile);
    output_payload_bounds();
    worker_metadata_limits(false, ps::ResourceKind::Metadata, true);
    worker_metadata_limits(true, ps::ResourceKind::Metadata, true);
    for (auto kind : {ps::ResourceKind::Metadata, ps::ResourceKind::Entries}) {
      worker_metadata_limits(false, kind);
      worker_metadata_limits(true, kind);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
