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
struct PayloadProbe {
  bool borrow, ready = false;
  explicit PayloadProbe(bool reference) : borrow(reference) {}
  ps::Result<ps::DependencyPoll> poll(const ps::DependencyPhase& phase) {
    if (borrow && !ready) {
      ready = true;
      return ps::Result<ps::DependencyPoll>(ps::DependencyNeedBatch{
          {{{0}, {{0, 5, take(ps::Footprint::all({2})), {}}}}},
          {}});
    }
    std::shared_ptr<const ps::CpuStorage> owner;
    if (borrow) {
      owner = phase.inputs[0].fragments()[0].storage();
    } else {
      auto allocation = phase.allocator.allocate(16);
      if (!allocation.ok())
        return ps::Result<ps::DependencyPoll>(allocation.status());
      auto bytes = allocation.take_value();
      std::memset(bytes.data(), 0, bytes.size());
      owner = std::move(bytes).freeze();
    }
    auto value = take(ps::Value::from_storage(phase.query.output.descriptor,
                                              phase.query.outputs.boxes()[0],
                                              {0, {8}}, std::move(owner)));
    return ps::Result<ps::DependencyPoll>(take(ps::ValueFragments::create(
        phase.query.output.descriptor, {}, phase.query.outputs, {value})));
  }
};
struct PayloadJointProbe {
  ps::Result<std::vector<ps::DependencyAtomOutcome>> poll(
      const ps::DependencyJointPhase& phase) {
    std::vector<ps::DependencyAtomOutcome> outcomes;
    for (const auto* member : phase.members) {
      PayloadProbe probe(false);
      outcomes.push_back(
          {take(ps::dependency_atom_key(member->query)), probe.poll(*member)});
    }
    return ps::Result<std::vector<ps::DependencyAtomOutcome>>(
        std::move(outcomes));
  }
};
ps::OperationDefinition payload_probe(bool borrow) {
  ps::OperationDefinition definition;
  definition.key = "manual.output_payload";
  auto& traits = definition.traits;
  traits.input_count = borrow ? 1 : 0;
  traits.input_schema.resize(traits.input_count);
  traits.workspace_bytes = 8;
  auto& output = traits.outputs[0];
  output.shape_rule = ps::OperationShapeRule::Fixed;
  output.fixed_output_shape = {2};
  output.atomic_trailing_axes = 1;
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(PayloadProbe);
  output.maximum_dependency_stages = 2;
  output.maximum_output_payload_bytes = borrow ? 0 : 8;
  output.failure_delivery = ps::FailureDelivery::PerAtomOutcome;
  definition.start_dependency = [borrow](const auto&, const auto& allocator) {
    return ps::DependencyContinuation::make<PayloadProbe>(allocator, borrow);
  };
  if (!borrow) {
    traits.outputs.push_back(output);
    traits.outputs.back().key = "other";
    traits.joint_contract = 2;
    traits.joint_continuation_bytes = sizeof(PayloadJointProbe);
    definition.start_joint = [](const auto&, const auto& allocator) {
      return ps::DependencyJointContinuation::make<PayloadJointProbe>(
          allocator);
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
  ps::Result<ps::DependencyPoll> poll(const ps::DependencyPhase& phase) {
    if (control->prior_protocol) {
      double ignored = 0;
      static_cast<void>(phase.read(999, {0}, &ignored, 8));
      try {
        ps::ResourceVector<std::uint8_t> metadata;
        metadata.resize(control->limit + 1);
      } catch (const std::bad_alloc&) {
      }
      return ps::Result<ps::DependencyPoll>(
          ps::Status{ps::ErrorCode::OperationFailed, "later callback failure"});
    }
    PayloadProbe probe(false);
    auto result = probe.poll(phase);
    fill_limit();
    return result;
  }
  ps::Result<std::vector<ps::DependencyAtomOutcome>> poll(
      const ps::DependencyJointPhase& phase) {
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
  definition.start_dependency = [control](const auto&, const auto& allocator) {
    return ps::DependencyContinuation::make<MetadataProbe>(allocator, control);
  };
  definition.start_joint = [control](const auto&, const auto& allocator) {
    return ps::DependencyJointContinuation::make<MetadataProbe>(allocator,
                                                                control);
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
  auto registry = std::make_shared<ps::OperationRegistry>();
  require(registry->register_operation(payload_probe(false)).ok(),
          "register payload probe");
  require(registry->freeze().ok(), "freeze payload probe");
  ps::DependencyRequest request{{},
                                {},
                                take(ps::Footprint::all({2})),
                                "payload-probe"};
  auto session =
      take(registry->start_dependency("manual.output_payload", request));
  const auto failure = session->poll().status();
  require(failure.code == ps::ErrorCode::ResourceExhausted &&
              failure.reason == ps::FailureReason::CapacityLimit,
          "workspace cannot expand the declared output payload bound");
  auto second = request;
  second.output_index = 1;
  auto joint =
      take(registry->start_joint("manual.output_payload", {request, second}));
  auto events = take(joint->poll());
  require(events.size() == 2, "joint payload event count");
  for (const auto& event : events)
    require(!event.outcome.ok() && event.outcome.status().reason ==
                                       ps::FailureReason::CapacityLimit,
            "joint must enforce each output payload bound");

  auto borrowing = std::make_shared<ps::OperationRegistry>();
  require(borrowing->register_operation(payload_probe(true)).ok(),
          "register borrowed probe");
  require(borrowing->freeze().ok(), "freeze borrowed probe");
  ps::ResourceBudget resources(ps::ResourceLimits{});
  auto input_bytes = take(resources.allocator().allocate(16));
  std::memset(input_bytes.data(), 0, input_bytes.size());
  auto input = take(ps::Value::from_storage({ps::ElementType::Float64, {2}},
                                            ps::Region::whole({2}), {0, {8}},
                                            std::move(input_bytes).freeze()));
  require(input.storage()->accounted(), "borrowed owner is admitted");
  request.inputs = {{input.descriptor(), {}}};
  auto borrowed =
      take(borrowing->start_dependency("manual.output_payload", request));
  require(borrowed->poll().ok(), "borrowed view requests source");
  require(borrowed
              ->supply({take(ps::ValueFragments::create(
                           input.descriptor(), {},
                           take(ps::Footprint::all({2})), {input}))},
                       "payload-probe")
              .ok(),
          "supply borrowed owner");
  auto done = take(borrowed->poll());
  require(std::get<ps::DependencyResult>(done).value.fragments()[0].storage() ==
              input.storage(),
          "zero new-payload bound permits admitted borrowed owner");
  std::cout << "output payload bound enforced separately from workspace; "
               "borrowed view accepted\n";
}
void constant(const std::string& profile, const std::string& shape,
              ps::ElementType type, std::uint64_t bits) {
  const auto value = scalar(type, bits);
  auto registry = ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  document.inputs = {
      {1, "value", value.descriptor(), value.region(), value.layout(), {}}};
  document.nodes = {{1,
                     "numeric.constant" + profile,
                     {ps::WorkflowInputReference{1}},
                     {{"shape", shape}, {"layout", std::string("view")}}}};
  document.outputs = {{"values", 1, "values"}};
  auto original = take(registry->find_traits(document.nodes[0].operation));
  auto unresolved = ps::infer_operation_outputs(
      original, {{value.descriptor(), {}}}, document.nodes[0].parameters);
  require(!unresolved.ok(),
          "unresolved shape template must not infer placeholder shape");
  ps::GraphContext graph(document);
  const auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 65536;
  config.managed_resources = ps::ResourceLimits{};
  ps::ValueFragments held;
  std::shared_ptr<const ps::ResourceBudget> root;
  const auto width = ps::Value::element_size(type);
  {
    ps::ExecutionContext context(registry, config);
    root = std::make_shared<const ps::ResourceBudget>(
        take(context.resource_budget()));
    ps::ExecutionBindings bindings;
    bindings.inputs = {{"value", value}};
    auto frozen = take(context.freeze(compiled.plan, bindings));
    const auto& descriptor = compiled.plan.steps().back().output_descriptor;
    ps::DemandQuery query;
    query = {{"values", take(ps::Footprint::all(descriptor.shape))}};
    auto result = take(context.execute_fragments(frozen, query));
    auto ordinary = take(context.execute(compiled.plan, bindings));
    require(ordinary.values.at("values").bytes().size() == width,
            "ordinary execute must preserve the declared view");
    const std::vector<ps::Value> direct_inputs{value};
    const std::vector<ps::Region> direct_demands{value.region()};
    ps::OperationInvocation invocation(direct_inputs, direct_demands,
                                       document.nodes[0].parameters,
                                       ps::Backend::Cpu);
    auto direct =
        take(registry->invoke(document.nodes[0].operation, invocation));
    require(direct.bytes().size() == width &&
                direct.layout().byte_strides.back() == 0,
            "direct invocation must preserve the declared view");
    held = result.values.at("values");
    require(held.fragments().size() == 1,
            "constant view needs one owner/fragment");
    const auto& view = held.fragments()[0];
    require(view.bytes().size() == width && view.facets().empty(),
            "scalar-size generic view");
    for (auto stride : view.layout().byte_strides)
      require(stride == 0, "constant view must have zero strides");
    require(root->statistics().peak[ps::ResourceKind::Payload] < 4096,
            "large view must not reserve logical dense bytes");
    std::vector<std::uint64_t> last;
    for (auto extent : descriptor.shape)
      last.push_back(extent - 1);
    std::uint64_t actual = 0;
    require(held.read(last, &actual, width).ok() &&
                std::memcmp(&actual, &bits, width) == 0,
            "constant last element bits");
  }
  require(root->statistics().live[ps::ResourceKind::Payload] >= width,
          "view owner survives context");
  held = {};
  require(root->statistics().live[ps::ResourceKind::Payload] == 0,
          "last owner releases scalar payload");
  std::cout << "constant view shape=" << shape << " stored_bytes=" << width
            << " passed\n";
}
void dense_and_schema() {
  const auto value = scalar(ps::ElementType::Int64, 7);
  auto registry = ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  document.inputs = {
      {1, "value", value.descriptor(), value.region(), value.layout(), {}}};
  document.nodes = {
      take(ps::numeric::constant_node(1, ps::WorkflowInputReference{1}, {2, 3},
                                      ps::numeric::ArrayLayout::Dense))};
  document.outputs = {{"values", 1, "values"}};
  ps::ExecutionBindings bindings;
  bindings.inputs = {{"value", value}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContext context(registry);
  auto result = take(context.execute(compiled.plan, bindings));
  const std::int64_t expected[] = {7, 7, 7, 7, 7, 7};
  require(result.values.at("values").bytes().size() == sizeof(expected) &&
              std::memcmp(result.values.at("values").bytes().data(), expected,
                          sizeof(expected)) == 0,
          "dense constant must contain six packed sevens");
  for (const auto* shape : {"", "01,3", "2, 3", "2,", "0,3", "1048576,1048577",
                            "+1", "1,1,1,1,1,1,1,1,1"}) {
    document.nodes[0].parameters["shape"] = std::string(shape);
    ps::GraphContext invalid(document);
    const auto status = ps::Compiler(registry).compile(invalid).status();
    require(status.code == ps::ErrorCode::InvalidArgument &&
                status.reason == ps::FailureReason::InvalidDomain &&
                status.detail.origin == ps::FailureOrigin::Schema,
            "invalid canonical shape must fail as schema error");
  }
  std::cout << "constant dense [2,3]=[7,7,7,7,7,7], authoring and shape errors "
               "passed\n";
}
void array_boundaries(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  const auto value = scalar(ps::ElementType::Int64, 7);
  const std::vector<ps::Value> inputs{value};
  const std::vector<ps::Region> demands{value.region()};
  auto node =
      take(ps::numeric::broadcast_node(1, ps::WorkflowInputReference{1}, {3},
                                       {0}, ps::numeric::ArrayLayout::Dense));
  node.operation = "numeric.broadcast" + profile;
  ps::CancellationSource cancel;
  unsigned output_allocations = 0;
  ps::BufferAllocator allocator([&](std::uint64_t size) {
    if (size == 24 && ++output_allocations == 1)
      cancel.cancel();
    return ps::Result<std::shared_ptr<void>>(std::make_shared<int>(0));
  });
  ps::OperationInvocation invocation(inputs, demands, node.parameters,
                                     ps::Backend::Cpu, cancel.token(), {},
                                     allocator);
  auto cancelled = registry->invoke(node.operation, invocation);
  require(output_allocations == 1 &&
              cancelled.status().code == ps::ErrorCode::Cancelled,
          "cancellation after Whole dense output allocation");
  for (std::uint64_t bits = 0; bits < 256; ++bits) {
    const auto byte = scalar(ps::ElementType::UInt8, bits);
    const std::vector<ps::Value> bytes{byte};
    const std::vector<ps::Region> byte_demands{byte.region()};
    for (auto layout :
         {ps::numeric::ArrayLayout::View, ps::numeric::ArrayLayout::Dense}) {
      auto fill = take(ps::numeric::constant_node(
          1, ps::WorkflowInputReference{1}, {2, 3}, layout));
      ps::OperationInvocation call(bytes, byte_demands, fill.parameters);
      auto result = take(registry->invoke("numeric.constant" + profile, call));
      for (std::uint64_t i = 0; i < 2; ++i)
        for (std::uint64_t j = 0; j < 3; ++j)
          require(
              result.bytes().data()[take(result.byte_address({i, j}))] == bits,
              "exhaustive UInt8 constant bits");
    }
  }
  // Unaligned, negative-stride source and axis permutation have a distinct
  // oracle.
  auto storage = take(ps::BufferAllocator{}.allocate(49));
  const std::int64_t matrix[] = {1, 2, 3, 4, 5, 6};
  std::memcpy(storage.data() + 1, matrix, 48);
  auto reversed = take(ps::Value::from_storage(
      {ps::ElementType::Int64, {2, 3}}, ps::Region::whole({2, 3}),
      {25, {-24, 8}}, std::move(storage).freeze()));
  const std::vector<ps::Value> matrix_inputs{reversed};
  const std::vector<ps::Region> matrix_demands{reversed.region()};
  for (auto layout :
       {ps::numeric::ArrayLayout::View, ps::numeric::ArrayLayout::Dense}) {
    auto permutation = take(ps::numeric::broadcast_node(
        1, ps::WorkflowInputReference{1}, {3, 4, 2}, {2, 0}, layout));
    ps::OperationInvocation call(matrix_inputs, matrix_demands,
                                 permutation.parameters);
    auto result = take(registry->invoke("numeric.broadcast" + profile, call));
    for (std::uint64_t j = 0; j < 3; ++j)
      for (std::uint64_t k = 0; k < 4; ++k)
        for (std::uint64_t i = 0; i < 2; ++i) {
          std::int64_t actual = 0;
          std::memcpy(
              &actual,
              result.bytes().data() + take(result.byte_address({j, k, i})), 8);
          require(actual == matrix[(1 - i) * 3 + j],
                  "negative unaligned permutation oracle");
        }
  }
  std::cout << "array boundaries: cancellation, 256 UInt8 values, "
               "negative unaligned permutation passed\n";
}
struct ArraySplitSource {
  unsigned* calls;
  explicit ArraySplitSource(unsigned* counter) : calls(counter) {}
  ps::Result<ps::DependencyPoll> poll(const ps::DependencyPhase& phase) {
    ++*calls;
    std::vector<ps::Value> parts;
    for (std::uint64_t i = 0; i < 2; ++i) {
      auto writer = take(
          ps::MutableValue::allocate(phase.query.output.descriptor,
                                     ps::Region({{i, 1}}), phase.allocator));
      const std::int64_t value = 10 + i;
      std::memcpy(writer.data(), &value, 8);
      parts.push_back(take(std::move(writer).publish()));
    }
    return ps::Result<ps::DependencyPoll>(take(
        ps::ValueFragments::create(phase.query.output.descriptor, {},
                                   phase.query.outputs, std::move(parts))));
  }
};
void staged_array_support(const std::string& profile) {
  auto registry = ps::make_default_operation_registry(false);
  const auto facet = take(ps::encode_semantic(ps::rgba_semantics()));
  const float rgba[] = {1, 2, 3, 2};
  auto buffer = take(ps::BufferAllocator{}.allocate(16));
  std::memcpy(buffer.data(), rgba, 16);
  auto invalid = take(ps::Value::from_storage(
      {ps::ElementType::Float32, {1, 1, 4}}, ps::Region::whole({1, 1, 4}),
      {0, {16, 16, 4}}, std::move(buffer).freeze(), {facet}));
  const std::vector<ps::Value> inputs{invalid};
  const std::vector<ps::Region> demands{invalid.region()};
  const std::map<std::string, ps::ParameterValue> parameters{
      {"shape", std::string("1,1,4")},
      {"axis_map", std::string("0,1,2")},
      {"layout", std::string("view")}};
  ps::OperationInvocation call(inputs, demands, parameters);
  require(
      !registry->invoke("numeric.broadcast" + profile, call).ok(),
      "Whole broadcast validates complete typed input including invalid alpha");
  unsigned calls = 0;
  ps::OperationDefinition split;
  split.key = "manual.array_split";
  split.traits.input_count = 0;
  split.traits.input_schema.clear();
  auto& output = split.traits.outputs[0];
  output.shape_rule = ps::OperationShapeRule::Fixed;
  output.fixed_output_shape = {2};
  output.output_element_type = ps::ElementType::Int64;
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.regional_atomic = true;
  output.preserve_output_views = true;
  output.continuation_bytes = sizeof(ArraySplitSource);
  output.maximum_dependency_stages = 1;
  split.start_dependency = [&](const auto&, const auto& allocator) {
    return ps::DependencyContinuation::make<ArraySplitSource>(allocator,
                                                              &calls);
  };
  require(registry->register_operation(std::move(split)).ok() &&
              registry->freeze().ok(),
          "split source registration");
  ps::WorkflowDocument document;
  auto broadcast = take(ps::numeric::broadcast_node(
      2, ps::WorkflowNodeOutput{1, "value"}, {3, 2}, {1}));
  broadcast.operation = "numeric.broadcast" + profile;
  document.nodes = {{1, "manual.array_split", {}, {}}, broadcast};
  document.outputs = {{"values", 2, "values"}};
  ps::GraphContext graph(document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContext context(registry);
  auto failed = context.execute(plan.plan);
  require(!failed.ok() && failed.status().message.find("ViewUnavailable") !=
                              std::string::npos,
          "Whole broadcast View rejects multi-owner input");
  auto snapshot = take(context.freeze(plan.plan));
  const auto before = calls;
  require(context.execute_fragments(
                     snapshot, {{"values", take(ps::Footprint::none({3, 2}))}})
                  .ok() &&
              calls == before,
          "Empty broadcast never executes source");
  document.nodes[1].parameters["layout"] = std::string("dense");
  ps::GraphContext dense_graph(document);
  auto dense_plan = take(ps::Compiler(registry).compile(dense_graph));
  auto dense = take(context.execute(dense_plan.plan));
  const std::int64_t expected[] = {10, 11, 10, 11, 10, 11};
  require(
      std::memcmp(dense.values.at("values").bytes().data(), expected, 48) == 0,
      "Dense broadcast collects multiple owners");
  std::cout << "Whole typed validation, multi-owner View failure/Dense collect "
               "and Empty passed\n";
}
void array_bitpatterns(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
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
      const auto input = scalar(type, pattern);
      const std::vector<ps::Value> inputs{input};
      const std::vector<ps::Region> demands{input.region()};
      for (auto layout :
           {ps::numeric::ArrayLayout::View, ps::numeric::ArrayLayout::Dense}) {
        auto fill = take(ps::numeric::constant_node(
            1, ps::WorkflowInputReference{1}, {2, 3}, layout));
        auto broadcast = take(ps::numeric::broadcast_node(
            1, ps::WorkflowInputReference{1}, {2, 3}, {0}, layout));
        for (auto node : {fill, broadcast}) {
          node.operation.replace(node.operation.find("_strict"), 7, profile);
          ps::OperationInvocation call(inputs, demands, node.parameters);
          auto output = take(registry->invoke(node.operation, call));
          auto address = take(output.byte_address({1, 2}));
          require(std::memcmp(output.bytes().data() + address, &pattern,
                              ps::Value::element_size(type)) == 0,
                  "integer extrema and IEEE bitpattern copy");
        }
      }
    }
  }
  std::cout << "array bitpatterns: integer extrema, signed zeros, infinities, "
               "subnormals, NaN payloads passed\n";
}
void broadcast_cache() {
  auto registry = ps::make_default_operation_registry();
  auto buffer = take(ps::BufferAllocator{}.allocate(24));
  const std::int64_t values[] = {10, 20, 30};
  std::memcpy(buffer.data(), values, 24);
  auto input = take(ps::Value::from_storage({ps::ElementType::Int64, {3}},
                                            ps::Region::whole({3}), {0, {8}},
                                            std::move(buffer).freeze()));
  ps::WorkflowDocument document;
  document.inputs = {
      {1, "input", input.descriptor(), input.region(), input.layout(), {}}};
  document.nodes = {take(ps::numeric::broadcast_node(
      1, ps::WorkflowInputReference{1}, {2, 3}, {1}))};
  document.outputs = {{"values", 1, "values"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::InputSnapshotStore snapshots;
  auto original = take(snapshots.import_value(input));
  ps::ExecutionBindings bindings;
  bindings.inputs = {{"input", {}}};
  bindings.inputs[0].snapshot =
      std::make_shared<const ps::InputSnapshot>(original);
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 65536;
  config.result_cache_bytes = 32768;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  auto demand = take(execution.open_demand(compiled.plan, bindings));
  const ps::DemandQuery query{
      {"values", take(ps::Footprint::from_regions(
                     {2, 3}, {ps::Region({{0, 2}, {0, 1}})}))}};
  take(demand.request(query));
  require(take(demand.request(query)).diagnostics.cache_hits > 0,
          "warm broadcast cache");
  auto patch_bytes = scalar(ps::ElementType::Int64, 99);
  auto patch =
      take(ps::Value::from_storage(input.descriptor(), ps::Region({{1, 1}}),
                                   {0, {8}, {1}}, patch_bytes.storage()));
  auto changed = take(snapshots.patch(original, patch));
  bindings.inputs[0].snapshot =
      std::make_shared<const ps::InputSnapshot>(changed);
  require(demand.replace_bindings(bindings).ok(),
          "replace unobserved broadcast sample");
  auto unchanged = take(demand.request(query));
  require(unchanged.diagnostics.cache_hits == 0,
          "Whole broadcast invalidates on any active source edit");
  std::int64_t actual = 0;
  require(unchanged.values.at("values").read({1, 0}, &actual, 8).ok() &&
              actual == 10,
          "unobserved change preserves broadcast value");
  patch = take(ps::Value::from_storage(input.descriptor(), ps::Region({{0, 1}}),
                                       {0, {8}, {0}}, patch_bytes.storage()));
  bindings.inputs[0].snapshot = std::make_shared<const ps::InputSnapshot>(
      take(snapshots.patch(changed, patch)));
  require(demand.replace_bindings(bindings).ok(),
          "replace observed broadcast sample");
  auto updated = take(demand.request(query));
  require(
      updated.values.at("values").read({1, 0}, &actual, 8).ok() && actual == 99,
      "observed source edit invalidates replicated cache entries");
  std::cout << "broadcast cache: warm hit, Whole edit invalidation, observed "
               "edit=99 passed\n";
}
void whole_array_budgets(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  const auto value = scalar(ps::ElementType::Int64, 7);
  std::vector<ps::Value> inputs{value};
  std::vector<ps::Region> demands{value.region()};
  const std::uint64_t count = 1048576;
  for (const char* operation : {"constant", "broadcast"}) {
    std::map<std::string, ps::ParameterValue> parameters{
        {"shape", std::to_string(count)},
        {"layout", std::string("dense")}};
    if (std::string(operation) == "broadcast")
      parameters["axis_map"] = std::string("0");
    const auto key = "numeric." + std::string(operation) + profile;
    for (bool work : {false, true}) {
      ps::ResourceLimits limits;
      if (work)
        limits.maximum_work = 4096;
      else
        limits.capacity[ps::ResourceKind::Payload] = 1024;
      ps::ResourceBudget budget(limits);
      {
        ps::ResourceAllocationScope scope(budget);
        ps::OperationInvocation call(
            inputs, demands, parameters, ps::Backend::Cpu, {},
            ps::Region::whole({count}), budget.allocator());
        auto result = registry->invoke(key, call);
        require(!result.ok() &&
                    result.status().code == ps::ErrorCode::ResourceExhausted,
                "Whole array output/work budget rejection");
      }
      require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
              "array failure releases full output");
    }
    ps::ResourceBudget budget(ps::ResourceLimits{});
    ps::CancellationSource cancellation;
    std::atomic<bool> ready{false}, done{false};
    std::thread watcher([&] {
      ready.store(true);
      while (!done.load() && budget.statistics().issued.work < 100000)
        std::this_thread::yield();
      if (!done.load())
        cancellation.cancel();
    });
    while (!ready.load())
      std::this_thread::yield();
    ps::Status status;
    try {
      ps::ResourceAllocationScope scope(budget);
      ps::OperationInvocation call(
          inputs, demands, parameters, ps::Backend::Cpu, cancellation.token(),
          ps::Region::whole({count}), budget.allocator());
      status = registry->invoke(key, call).status();
    } catch (...) {
      done.store(true);
      watcher.join();
      throw;
    }
    done.store(true);
    watcher.join();
    require(status.code == ps::ErrorCode::Cancelled &&
                budget.statistics().issued.work >= 100000 &&
                budget.statistics().live[ps::ResourceKind::Payload] == 0,
            "cancel active Whole array copying and release output");
  }
  std::cout << "Whole arrays: full output/work admission and cancellation "
               "during copying passed\n";
}
void array_schema_and_capacity(const std::string& profile) {
  auto registry = ps::make_default_operation_registry();
  const auto value = scalar(ps::ElementType::Int64, 7);
  const std::vector<ps::OperationMetadata> inputs{{value.descriptor(), {}}};
  auto node = take(ps::numeric::broadcast_node(1, ps::WorkflowInputReference{1},
                                               {2, 3}, {1}));
  for (const auto& text : {"", "0", "-1", "01", "1,", "1,,2", "1, 2",
                           "1099511627777", "1,1,1,1,1,1,1,1,1"}) {
    auto parameters = node.parameters;
    parameters["shape"] = std::string(text);
    auto status =
        registry
            ->resolve_traits("numeric.broadcast" + profile, inputs, parameters)
            .status();
    require(status.code == ps::ErrorCode::InvalidArgument,
            "malformed shape schema");
  }
  auto parameters = node.parameters;
  parameters["axis_map"] = std::string("2");
  require(registry->resolve_traits("numeric.broadcast" + profile, inputs,
                                   parameters)
                  .status()
                  .code == ps::ErrorCode::InvalidArgument,
          "out-of-range map schema");
  parameters["axis_map"] = std::string("0,1");
  require(registry->resolve_traits("numeric.broadcast" + profile, inputs,
                                   parameters)
                  .status()
                  .code == ps::ErrorCode::TypeMismatch,
          "map length schema");
  parameters["axis_map"] = std::string("0,0");
  const std::vector<ps::OperationMetadata> matrix{
      {{ps::ElementType::Int64, {2, 3}}, {}}};
  require(registry->resolve_traits("numeric.broadcast" + profile, matrix,
                                   parameters)
                  .status()
                  .code == ps::ErrorCode::InvalidArgument,
          "duplicate map schema");
  parameters["axis_map"] = std::string("0,1");
  parameters["shape"] = std::string("4,3");
  require(registry->resolve_traits("numeric.broadcast" + profile, matrix,
                                   parameters)
                  .status()
                  .code == ps::ErrorCode::TypeMismatch,
          "broadcast does not tile non-singleton axes");
  const std::vector<ps::Value> values{value};
  const std::vector<ps::Region> demands{value.region()};
  node = take(ps::numeric::broadcast_node(1, ps::WorkflowInputReference{1},
                                          {1048576, 1048576}, {1},
                                          ps::numeric::ArrayLayout::Dense));
  auto quota = ps::BufferAllocator{}.limited(4096);
  ps::OperationInvocation call(values, demands, node.parameters,
                               ps::Backend::Cpu, {}, {}, quota);
  auto failure = registry->invoke("numeric.broadcast" + profile, call);
  require(failure.status().code == ps::ErrorCode::ResourceExhausted,
          "large dense refuses insufficient allocation budget");
  const std::string unavailable =
      profile == "_accelerated_x86_64" ? "_accelerated_apple_silicon" :
#if defined(__aarch64__)
                                       "_accelerated_x86_64";
#else
                                       "_accelerated_apple_silicon";
#endif
  require(registry->resolve_traits("numeric.broadcast" + unavailable, inputs,
                                   node.parameters)
                  .status()
                  .code == ps::ErrorCode::BackendUnavailable,
          "incompatible profile does not fall back");
  std::cout << "array shape/map schema, dense capacity and unavailable profile "
               "passed\n";
}
void array_owner_and_payload_cache() {
  auto registry = ps::make_default_operation_registry();
  ps::Value copied;
  std::weak_ptr<const ps::CpuStorage> original_owner;
  {
    auto bytes = take(ps::BufferAllocator{}.allocate(4096));
    const std::uint64_t bits = UINT64_C(0x7ff0000000001234);
    std::memcpy(bytes.data() + 1001, &bits, 8);
    auto input = take(ps::Value::from_storage(
        {ps::ElementType::Float64, {1}}, ps::Region::whole({1}), {1001, {0}},
        std::move(bytes).freeze()));
    original_owner = input.storage();
    const std::vector<ps::Value> inputs{input};
    const std::vector<ps::Region> demands{input.region()};
    auto node = take(
        ps::numeric::constant_node(1, ps::WorkflowInputReference{1}, {2, 3}));
    ps::OperationInvocation call(inputs, demands, node.parameters);
    copied = take(registry->invoke(node.operation, call));
    require(copied.bytes().size() == 8 && copied.storage() != input.storage(),
            "constant owns only scalar copy");
  }
  require(original_owner.expired(),
          "oversized unaligned scalar source released before constant output");
  ps::WorkflowDocument document;
  auto scalar_input =
      scalar(ps::ElementType::Float64, UINT64_C(0x7ff0000000001234));
  document.inputs = {{1,
                      "input",
                      scalar_input.descriptor(),
                      scalar_input.region(),
                      scalar_input.layout(),
                      {}}};
  document.nodes = {take(
      ps::numeric::constant_node(1, ps::WorkflowInputReference{1}, {2, 3}))};
  document.outputs = {{"values", 1, "values"}};
  ps::GraphContext graph(document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  ps::InputSnapshotStore snapshots;
  ps::ExecutionBindings bindings;
  bindings.inputs = {{"input", {}}};
  bindings.inputs[0].snapshot = std::make_shared<const ps::InputSnapshot>(
      take(snapshots.import_value(scalar_input)));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 65536;
  config.result_cache_bytes = 32768;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  auto demand = take(execution.open_demand(plan.plan, bindings));
  const ps::DemandQuery query{{"values", take(ps::Footprint::all({2, 3}))}};
  auto cold = take(demand.request(query));
  require(take(demand.request(query)).diagnostics.cache_hits > 0,
          "constant NaN warm cache");
  require(take(take(cold.dependencies.potential_dirty(
                        "input", take(ps::Footprint::all({1}))))
                   .at("values")
                   .element_count()) == 6,
          "scalar dirty covers complete constant");
  const auto replacement = UINT64_C(0xfff8000000005678);
  bindings.inputs[0].snapshot = std::make_shared<const ps::InputSnapshot>(take(
      snapshots.import_value(scalar(ps::ElementType::Float64, replacement))));
  require(demand.replace_bindings(bindings).ok(), "replace NaN payload");
  auto changed = take(demand.request(query));
  std::uint64_t actual = 0;
  require(changed.values.at("values").read({1, 2}, &actual, 8).ok() &&
              actual == replacement,
          "constant cache distinguishes NaN payload and sign");
  ps::Value held;
  std::optional<ps::ResourceBudget> resources;
  {
    ps::ExecutionContext context(registry, config);
    resources = take(context.resource_budget());
    auto bytes = take(resources->allocator().allocate(24));
    const std::int64_t values[] = {10, 20, 30};
    std::memcpy(bytes.data(), values, 24);
    auto input = take(ps::Value::from_storage({ps::ElementType::Int64, {3}},
                                              ps::Region::whole({3}), {0, {8}},
                                              std::move(bytes).freeze()));
    ps::WorkflowDocument broadcast;
    broadcast.inputs = {
        {1, "input", input.descriptor(), input.region(), input.layout(), {}}};
    broadcast.nodes = {take(ps::numeric::broadcast_node(
        1, ps::WorkflowInputReference{1}, {2, 3}, {1}))};
    broadcast.outputs = {{"values", 1, "values"}};
    ps::GraphContext broadcast_graph(broadcast);
    auto compiled = take(ps::Compiler(registry).compile(broadcast_graph));
    ps::ExecutionBindings bound;
    bound.inputs = {{"input", input}};
    held = take(context.execute(compiled.plan, bound)).values.at("values");
  }
  std::int64_t last = 0;
  std::memcpy(&last, held.bytes().data() + take(held.byte_address({1, 2})), 8);
  require(last == 30 &&
              resources->statistics().live[ps::ResourceKind::Payload] == 24,
          "broadcast source owner survives context and original source");
  held = {};
  require(resources->statistics().live[ps::ResourceKind::Payload] == 0,
          "broadcast final owner releases managed source capacity");
  std::cout << "array owners: oversized source released, NaN cache bits "
               "updated, borrowed owner final release passed\n";
}
struct StructuredLast {
  bool ready = false;
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    using Answer = ps::Result<ps::ResultProgramPoll>;
    const auto& shape = phase.query.inputs[0].descriptor.shape;
    if (!ready) {
      ready = true;
      ps::ResultProgramNeed need;
      need.values = ps::ResourceVector<ps::ResultValueNeed>(
          ps::ResourceAllocator<ps::ResultValueNeed>(phase.resources));
      need.values.push_back({0, take(ps::Footprint::all(shape))});
      return Answer(std::move(need));
    }
    std::vector<std::uint64_t> last;
    for (auto extent : shape)
      last.push_back(extent - 1);
    std::int64_t value = 0;
    auto status = phase.read(0, last, &value, 8);
    if (!status.ok())
      return Answer(status);
    auto writer = take(ps::MutableValue::allocate(phase.query.output.descriptor,
                                                  ps::Region::whole({1}),
                                                  phase.allocator));
    std::memcpy(writer.data(), &value, 8);
    auto fragments = take(ps::ValueFragments::create(
        phase.query.output.descriptor, {}, *phase.query.value_outputs,
        {take(std::move(writer).publish())}));
    auto relation = take(ps::ResultRelation::cartesian(
        phase.resources, 1,
        {0, 15, 0, take(ps::Footprint::all(shape)).element_count().value()}));
    return Answer(
        ps::ResultValuePublication{std::move(fragments), std::move(relation)});
  }
};
ps::OperationDefinition structured_last() {
  ps::OperationDefinition operation;
  operation.key = "manual.structured_last";
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  auto& output = operation.traits.outputs[0];
  output.output_element_type = ps::ElementType::Int64;
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.dependency_version = 2;
  output.continuation_bytes = sizeof(StructuredLast);
  output.maximum_dependency_stages = 2;
  operation.start_result = [](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<StructuredLast>(allocator);
  };
  return operation;
}
void structured_views() {
  auto registry = ps::make_default_operation_registry(false);
  require(!registry->frozen() && !registry->persistent_cache_identity().empty(),
          "explicit mutable built-in registry");
  require(registry->register_operation(structured_last()).ok(),
          "register public consumer");
  require(registry->persistent_cache_identity().empty(),
          "custom registry cache identity");
  require(registry->freeze().ok(), "freeze extended built-ins");
  const auto input = scalar(ps::ElementType::Int64, 7);
  ps::WorkflowDocument document;
  document.inputs = {
      {1, "input", input.descriptor(), input.region(), input.layout(), {}}};
  document.nodes = {
      take(ps::numeric::constant_node(1, ps::WorkflowInputReference{1},
                                      {1048576, 1048576})),
      take(ps::numeric::broadcast_node(2, ps::WorkflowInputReference{1},
                                       {UINT64_C(274877906944), 3}, {1})),
      {3, "manual.structured_last", {ps::WorkflowNodeOutput{2, "values"}}, {}}};
  document.outputs = {{"constant", 1, "values"}, {"last", 3, "value"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 65536;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  ps::ExecutionBindings bindings;
  bindings.inputs = {{"input", input}};
  auto result = take(execution.execute(compiled.plan, bindings));
  require(result.values.at("constant").bytes().size() == 8,
          "structured named constant must preserve view");
  std::int64_t last = 0;
  std::memcpy(&last, result.values.at("last").bytes().data(), 8);
  require(last == 7, "structured large broadcast last element");
  std::cout << "structured giant broadcast and named constant: 8-byte views, "
               "last=7 passed\n";
}
void broadcast_examples(const std::string& profile, bool large) {
  const std::int64_t values[] = {10, 20, 30};
  auto buffer = take(ps::BufferAllocator{}.allocate(sizeof(values)));
  std::memcpy(buffer.data(), values, sizeof(values));
  auto input = take(ps::Value::from_storage({ps::ElementType::Int64, {3}},
                                            ps::Region::whole({3}), {0, {8}},
                                            std::move(buffer).freeze()));
  auto registry = ps::make_default_operation_registry();
  const std::vector<std::uint64_t> shape =
      large ? std::vector<std::uint64_t>{UINT64_C(274877906944), 3}
            : std::vector<std::uint64_t>{2, 3, 4};
  ps::WorkflowDocument document;
  document.inputs = {
      {1, "input", input.descriptor(), input.region(), input.layout(), {}}};
  document.nodes = {take(ps::numeric::broadcast_node(
      1, ps::WorkflowInputReference{1}, shape, {1}))};
  document.nodes[0].operation = "numeric.broadcast" + profile;
  document.outputs = {{"values", 1, "values"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 65536;
  config.result_cache_bytes = 32768;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext execution(registry, config);
  ps::ExecutionBindings bindings;
  bindings.inputs = {{"input", input}};
  auto frozen = take(execution.freeze(compiled.plan, bindings));
  const auto all = take(ps::Footprint::all(shape));
  auto result = take(execution.execute_fragments(frozen, {{"values", all}}));
  const auto& view = result.values.at("values");
  require(view.fragments().size() == 1 &&
              view.fragments()[0].bytes().size() == sizeof(values),
          "broadcast view must retain only the source backing");
  require(view.fragments()[0].layout().byte_strides[0] == 0 &&
              view.fragments()[0].layout().byte_strides[1] == 8,
          "broadcast replicated/mapped strides");
  std::vector<std::uint64_t> last;
  for (auto extent : shape)
    last.push_back(extent - 1);
  std::int64_t actual = 0;
  require(view.read(last, &actual, 8).ok() && actual == 30,
          "broadcast last logical sample");
  const auto support = take(result.dependencies.source_support());
  require(support.at("input") == take(ps::Footprint::all({3})),
          "broadcast full support must deduplicate to three samples");
  auto dirty = take(result.dependencies.potential_dirty(
      "input", take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})}))));
  require(take(dirty.at("values").element_count()) == take(all.element_count()),
          "Whole broadcast dirty covers all output observations");
  auto ordinary = take(execution.execute(compiled.plan, bindings));
  require(ordinary.values.at("values").bytes().size() == sizeof(values),
          "ordinary broadcast must preserve view");
  if (!large) {
    const auto sparse = take(ps::Footprint::from_regions(
        shape, {ps::Region({{0, 1}, {0, 1}, {0, 1}}),
                ps::Region({{1, 1}, {2, 1}, {3, 1}})}));
    auto selected =
        take(execution.execute_fragments(frozen, {{"values", sparse}}));
    require(
        take(selected.dependencies.source_support()).at("input") ==
            take(ps::Footprint::all({3})),
        "Whole broadcast sparse request still retains complete input support");
    document.nodes[0].parameters["layout"] = std::string("dense");
    ps::GraphContext dense_graph(document);
    auto dense_plan = take(ps::Compiler(registry).compile(dense_graph));
    auto dense = take(execution.execute(dense_plan.plan, bindings));
    require(dense.values.at("values").bytes().size() == 24 * 8,
            "dense broadcast packed size");
    require(dense.diagnostics.operation_timings.size() == 1,
            "dense Whole broadcast uses one callback; numeric counters "
            "unavailable");
    for (std::uint64_t i = 0; i < 24; ++i) {
      std::int64_t sample = 0;
      std::memcpy(&sample, dense.values.at("values").bytes().data() + i * 8, 8);
      require(sample == values[(i / 4) % 3],
              "independent dense broadcast coordinate oracle");
    }
  }
  std::cout << "broadcast " << (large ? "large" : "[2,3,4]")
            << ": 3 source samples, exact view/dirty/support passed\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string profile = argc > 1 ? argv[1] : "_strict";
    dense_and_schema();
    structured_views();
    array_boundaries(profile);
    staged_array_support(profile);
    fenv_t saved;
    require(fegetenv(&saved) == 0, "save array fenv");
    for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "set array fenv");
      array_bitpatterns(profile);
      require(
          fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "bitcopy preserves fenv");
    }
    require(fesetenv(&saved) == 0, "restore array fenv");
    whole_array_budgets(profile);
    broadcast_cache();
    array_schema_and_capacity(profile);
    array_owner_and_payload_cache();
    output_payload_bounds();
    worker_metadata_limits(false, ps::ResourceKind::Metadata, true);
    for (auto kind : {ps::ResourceKind::Metadata, ps::ResourceKind::Entries}) {
      worker_metadata_limits(false, kind);
      worker_metadata_limits(true, kind);
    }
    broadcast_examples(profile, false);
    broadcast_examples(profile, true);
    constant(profile, "2,3", ps::ElementType::Int64, 7);
    constant(profile, "1048576,1048576", ps::ElementType::Int64, 7);
    constant(profile, "2,3", ps::ElementType::Float64,
             UINT64_C(0x7ff0000000000001));
    constant(profile, "2,3", ps::ElementType::Float32, UINT64_C(0x7f800001));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
