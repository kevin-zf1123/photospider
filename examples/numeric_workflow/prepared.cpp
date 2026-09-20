#include <atomic>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
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
struct Counts {
  std::atomic<unsigned> prepared{0}, destroyed{0}, started{0};
};
struct Program {
  std::shared_ptr<Counts> counts;
  explicit Program(std::shared_ptr<Counts> stats) : counts(std::move(stats)) {}
  ~Program() { ++counts->destroyed; }
  const double value = 7;
};
struct State {
  const Program* program;
  explicit State(const Program* value) : program(value) {}
  ps::Result<ps::DependencyPoll> poll(const ps::DependencyPhase& phase) {
    require(phase.query.prepared && phase.query.prepared->state() == program,
            "session owns exact immutable program");
    auto writer = take(ps::MutableValue::allocate(
        phase.query.output.descriptor, phase.query.outputs.boxes()[0],
        phase.allocator));
    const auto count = take(phase.query.outputs.element_count());
    for (std::uint64_t i = 0; i < count; ++i)
      std::memcpy(writer.data() + i * 8, &program->value, 8);
    return ps::Result<ps::DependencyPoll>(take(ps::ValueFragments::create(
        phase.query.output.descriptor, {}, phase.query.outputs,
        {take(std::move(writer).publish())}, phase.sets)));
  }
};
struct Joint {
  ps::Result<std::vector<ps::DependencyAtomOutcome>> poll(
      const ps::DependencyJointPhase& phase) {
    std::vector<ps::DependencyAtomOutcome> result;
    for (const auto* member : phase.members) {
      require(member->query.prepared, "joint prepared pointer");
      State state(static_cast<const Program*>(member->query.prepared->state()));
      result.push_back(
          {take(ps::dependency_atom_key(member->query)), state.poll(*member)});
    }
    return ps::Result<std::vector<ps::DependencyAtomOutcome>>(
        std::move(result));
  }
};
ps::OperationDefinition definition(const std::shared_ptr<Counts>& counts) {
  ps::OperationDefinition operation;
  operation.key = "manual.prepared";
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].element_type_mask = 4;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"mask", ps::OperationParameterType::Float64}};
  traits.joint_contract = 2;
  traits.joint_continuation_bytes = sizeof(Joint);
  auto& value = traits.outputs[0];
  value.key = "values";
  value.region_rule = ps::OperationRegionRule::Dependency;
  value.dependency_version = 1;
  value.continuation_bytes = sizeof(State);
  value.maximum_dependency_stages = 1;
  value.failure_delivery = ps::FailureDelivery::PerAtomOutcome;
  traits.outputs.push_back(value);
  traits.outputs[1].key = "axis";
  operation.prepare_static =
      [counts](const auto& inputs,
               const auto&) -> ps::Result<ps::OperationPreparation> {
    if (inputs[0].descriptor.shape != std::vector<std::uint64_t>{1})
      return ps::Result<ps::OperationPreparation>(
          ps::Status{ps::ErrorCode::TypeMismatch, "expected scalar"});
    ++counts->prepared;
    ps::OperationPreparation prepared;
    prepared.outputs.resize(2);
    prepared.outputs[0].metadata.descriptor = {ps::ElementType::Float64, {5}};
    prepared.outputs[1].metadata.descriptor = {ps::ElementType::Float64, {3}};
    prepared.outputs[1].metadata.atomic_trailing_axes = 1;
    prepared.state = std::make_shared<const Program>(counts);
    return ps::Result<ps::OperationPreparation>(std::move(prepared));
  };
  operation.start_dependency = [counts](const auto& query,
                                        const auto& allocator) {
    ++counts->started;
    require(query.prepared, "prepared singleton query");
    return ps::DependencyContinuation::make<State>(
        allocator, static_cast<const Program*>(query.prepared->state()));
  };
  operation.start_joint = [](const auto& queries, const auto& allocator) {
    require(!queries.empty() && queries[0].prepared, "prepared joint start");
    for (const auto& query : queries)
      require(query.prepared == queries[0].prepared, "one joint program owner");
    return ps::DependencyJointContinuation::make<Joint>(allocator);
  };
  return operation;
}
std::shared_ptr<ps::OperationRegistry> registry(
    const std::shared_ptr<Counts>& counts) {
  auto result = std::make_shared<ps::OperationRegistry>();
  require(result->register_operation(definition(counts)).ok() &&
              result->freeze().ok(),
          "register prepared operation");
  return result;
}
void compiler_and_direct(const std::shared_ptr<Counts>& counts) {
  auto operations = registry(counts);
  auto scalar = ps::Value::from_float64(1);
  ps::WorkflowDocument document;
  document.inputs = {
      {1, "input", scalar.descriptor(), scalar.region(), scalar.layout(), {}}};
  document.nodes = {
      {1, "manual.prepared", {ps::WorkflowInputReference{1}}, {{"mask", 0.0}}}};
  document.outputs = {{"values", 1, "values"}, {"axis", 1, "axis"}};
  ps::GraphContext graph(document);
  const auto before = counts->prepared.load();
  auto compiled = take(ps::Compiler(operations).compile(graph));
  require(counts->prepared == before + 1, "compile prepares once");
  require(compiled.semantic.nodes()[0].prepared ==
                  compiled.optimized.nodes()[0].prepared &&
              compiled.plan.steps()[0].prepared ==
                  compiled.plan.steps()[1].prepared,
          "optimizer and outputs share program");
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(operations, config);
  ps::ExecutionBindings bindings;
  bindings.inputs = {{"input", scalar}};
  for (bool joint : {false, true}) {
    ps::ExecutionOptions options;
    options.enable_joint = joint;
    auto result = take(context.execute(compiled.plan, bindings, {}, options));
    require(result.values.size() == 2, "both compiled outputs");
  }
  bindings.inputs[0].value = ps::Value::from_float64(9);
  auto frozen = take(context.freeze(compiled.plan, bindings));
  auto result = take(context.execute_fragments(
      frozen,
      {{"values", take(ps::Footprint::from_regions(
                      {5}, {ps::Region({{1, 1}}), ps::Region({{4, 1}})}))}}));
  std::uint64_t raw = 0;
  require(result.values.at("values").read({4}, &raw, 8).ok() &&
              raw == UINT64_C(0x401c000000000000),
          "prepared sparse output");
  auto tile = take(compiled.plan.tile_plan("values", ps::Region({{2, 1}})));
  require(tile.steps()[0].prepared == compiled.plan.steps()[0].prepared,
          "tile keeps program");
  require(counts->prepared == before + 1,
          "multiple runs/outputs/ROI never reprepare");
  const std::vector<ps::Value> inputs{scalar};
  const std::vector<ps::Region> demands{scalar.region()};
  const std::map<std::string, ps::ParameterValue> parameters{{"mask", 0.0}};
  ps::OperationInvocation invocation(inputs, demands, parameters);
  auto direct = take(operations->invoke("manual.prepared", invocation));
  require(direct.descriptor().shape == std::vector<std::uint64_t>{5} &&
              counts->prepared == before + 2,
          "direct multi-atom prepares once");
}
void seals_and_lifetime(const std::shared_ptr<Counts>& counts) {
  static_assert(!std::is_copy_constructible_v<ps::PreparedOperation> &&
                !std::is_move_constructible_v<ps::PreparedOperation>);
  auto operations = registry(counts),
       other = registry(std::make_shared<Counts>());
  ps::DependencyRequest request;
  request.inputs = {{{ps::ElementType::Float64, {1}}, {}}};
  request.parameters = {{"mask", 0.0}};
  request.snapshot_identity = "static-owner";
  request.outputs = take(ps::Footprint::none({5}));
  request.prepared = take(operations->prepare_operation(
      "manual.prepared", request.inputs, request.parameters));
  const auto before = counts->prepared.load();
  require(operations->start_dependency("manual.prepared", request).ok(),
          "valid Empty preparation");
  auto foreign = other->start_dependency("manual.prepared", request);
  require(!foreign.ok() && foreign.status().code == ps::ErrorCode::Stale,
          "foreign preparation rejected");
  auto changed = request;
  changed.parameters["mask"] = -0.0;
  require(!operations->start_dependency("manual.prepared", changed).ok(),
          "signed zero static identity");
  changed = request;
  changed.inputs[0].descriptor.shape = {2};
  require(!operations->start_dependency("manual.prepared", changed).ok(),
          "metadata changed");
  std::uint64_t nan_bits = UINT64_C(0x7ff8000000000042);
  double nan = 0;
  std::memcpy(&nan, &nan_bits, 8);
  changed = request;
  changed.parameters["mask"] = nan;
  changed.prepared = take(operations->prepare_operation(
      "manual.prepared", changed.inputs, changed.parameters));
  require(operations->start_dependency("manual.prepared", changed).ok(),
          "identical NaN parameter bits match");
  ++nan_bits;
  std::memcpy(&nan, &nan_bits, 8);
  changed.parameters["mask"] = nan;
  require(!operations->start_dependency("manual.prepared", changed).ok(),
          "distinct NaN payload rejects");
  require(counts->prepared == before + 1, "validation never reparses");
  request.outputs =
      take(ps::Footprint::from_regions({5}, {ps::Region({{0, 1}})}));
  auto session = take(operations->start_dependency("manual.prepared", request));
  request.prepared.reset();
  changed.prepared.reset();
  operations.reset();
  auto completed = take(session->poll());
  require(std::holds_alternative<ps::DependencyResult>(completed),
          "session outlives registry and external program owner");
}
void direct_joint(const std::shared_ptr<Counts>& counts) {
  auto operations = registry(counts);
  ps::DependencyRequest first;
  first.inputs = {{{ps::ElementType::Float64, {1}}, {}}};
  first.parameters = {{"mask", 0.0}};
  first.snapshot_identity = "joint-static";
  first.outputs =
      take(ps::Footprint::from_regions({5}, {ps::Region({{0, 1}})}));
  auto second = first;
  second.outputs =
      take(ps::Footprint::from_regions({5}, {ps::Region({{1, 1}})}));
  const auto before = counts->prepared.load();
  auto session =
      take(operations->start_joint("manual.prepared", {first, second}));
  require(session->poll().ok() && counts->prepared == before + 1,
          "direct joint prepares once");
}
void whole_preparation() {
  auto counts = std::make_shared<Counts>();
  auto operations = std::make_shared<ps::OperationRegistry>();
  auto whole = definition(counts);
  whole.key = "manual.whole_prepared";
  whole.traits.joint_contract = 0;
  whole.traits.joint_continuation_bytes = 0;
  whole.start_dependency = {};
  whole.start_joint = {};
  for (auto& output : whole.traits.outputs) {
    output.region_rule = ps::OperationRegionRule::Whole;
    output.dependency_version = 0;
    output.continuation_bytes = 0;
    output.maximum_dependency_stages = 0;
    output.failure_delivery = ps::FailureDelivery::RequestFailureOnly;
    output.requires_dense_output = true;
  }
  auto prepare = whole.prepare_static;
  whole.prepare_static = [prepare](const auto& inputs, const auto& parameters) {
    auto result = prepare(inputs, parameters);
    if (!result.ok())
      return result;
    auto prepared = result.take_value();
    const auto mask = std::get<double>(parameters.at("mask"));
    for (auto& output : prepared.outputs) {
      output.input_indices = std::vector<std::uint32_t>{};
      if (mask == 1)
        output.input_indices = std::vector<std::uint32_t>{0};
      if (mask == 2)
        output.input_indices = std::vector<std::uint32_t>{0, 0};
      if (mask == 3)
        output.input_indices = std::vector<std::uint32_t>{1};
    }
    return ps::Result<ps::OperationPreparation>(std::move(prepared));
  };
  whole.callback = [counts](const ps::OperationInvocation& call) {
    ++counts->started;
    require(call.prepared && call.prepared->state(),
            "Whole owns prepared program");
    const auto* program = static_cast<const Program*>(call.prepared->state());
    const auto mask = std::get<double>(call.parameters.at("mask"));
    require(call.inputs.size() == (mask == 1 ? 1 : 0),
            "static projection reaches callback");
    const ps::ValueDescriptor descriptor{ps::ElementType::Float64,
                                         {call.output_index == 1 ? 3U : 5U}};
    auto writer = take(ps::MutableValue::allocate(
        descriptor, call.output_region, call.allocator));
    for (std::uint64_t i = 0; i < take(call.output_region.element_count()); ++i)
      std::memcpy(writer.data() + 8 * i, &program->value, 8);
    return std::move(writer).publish();
  };
  auto narrow = whole;
  narrow.key = "manual.whole_narrow";
  for (auto& output : narrow.traits.outputs)
    output.input_indices = std::vector<std::uint32_t>{};
  require(operations->register_operation(std::move(narrow)).ok(),
          "register empty input template");
  require(operations->register_operation(std::move(whole)).ok() &&
              operations->freeze().ok(),
          "register Whole prepared tuple");
  auto scalar = ps::Value::from_float64(1);
  ps::WorkflowDocument document;
  document.inputs = {
      {1, "input", scalar.descriptor(), scalar.region(), scalar.layout(), {}}};
  document.nodes = {{1,
                     "manual.whole_prepared",
                     {ps::WorkflowInputReference{1}},
                     {{"mask", 0.0}}}};
  document.outputs = {{"values", 1, "values"}, {"axis", 1, "axis"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(operations).compile(graph));
  require(counts->prepared == 1, "Whole compile prepares once");
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 0;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(operations, config);
  ps::ExecutionBindings bindings;
  bindings.inputs = {{"input", scalar}};
  auto snapshot = take(context.freeze(compiled.plan, bindings));
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    auto result = take(context.execute_fragments(
        snapshot, {{"axis", take(ps::Footprint::from_regions(
                                {3}, {ps::Region({{1, 1}})}))}}));
    double value = 0;
    require(result.values.at("axis").read({1}, &value, 8).ok() && value == 7,
            "Whole tuple projection");
  }
  require(counts->prepared == 1 && counts->started == 2,
          "Whole executions reuse compile owner");
  auto source = std::make_shared<ps::RegionalSource>();
  source->descriptor = scalar.descriptor();
  source->read = [](const auto&, auto*, auto, const auto&, const auto&) {
    return ps::Result<ps::Region>(
        ps::Status{ps::ErrorCode::OperationFailed, "excluded source"});
  };
  bindings.inputs[0].value = {};
  bindings.inputs[0].source = source;
  require(context.execute(compiled.plan, bindings).ok(),
          "Whole empty projection suppresses source I/O");
  const std::vector<ps::Value> inputs{scalar};
  const std::vector<ps::Region> demands{scalar.region()};
  auto parameters = document.nodes[0].parameters;
  auto broadened = parameters;
  broadened["mask"] = 1.0;
  require(!operations
               ->prepare_operation("manual.whole_narrow",
                                   {{scalar.descriptor(), {}}}, broadened)
               .ok(),
          "specializer cannot broaden registered empty projection");
  ps::OperationInvocation call(inputs, demands, parameters);
  call.prepared = compiled.plan.steps()[0].prepared;
  require(operations->invoke("manual.whole_prepared", call).ok(),
          "Whole direct preparation");
  auto first_call = std::async(std::launch::async, [&] {
    return operations->invoke("manual.whole_prepared", call);
  });
  auto second_call = std::async(std::launch::async, [&] {
    return operations->invoke("manual.whole_prepared", call);
  });
  require(first_call.get().ok() && second_call.get().ok(),
          "concurrent Whole preparation reuse");
  require(operations->invoke("manual.whole_narrow", call).status().code ==
              ps::ErrorCode::Stale,
          "Whole key seal");
  call.input_metadata = {{{ps::ElementType::Float32, {1}}, {}}};
  require(!operations->invoke("manual.whole_prepared", call).ok(),
          "Whole complete metadata mismatch rejected");
  call.input_metadata.clear();
  parameters["mask"] = -0.0;
  require(operations->invoke("manual.whole_prepared", call).status().code ==
              ps::ErrorCode::Stale,
          "Whole exact parameter seal");
  for (double mode : {2., 3.}) {
    parameters["mask"] = mode;
    require(!operations
                 ->prepare_operation("manual.whole_prepared",
                                     {{scalar.descriptor(), {}}}, parameters)
                 .ok(),
            "invalid specialized projections rejected");
  }
  std::cout << "Whole preparation: compile reuse, static projections, tuple, "
               "direct seals passed\n";
}
struct SplitOwner {
  bool shared;
  explicit SplitOwner(bool one = false) : shared(one) {}
  ps::Result<ps::DependencyPoll> poll(const ps::DependencyPhase& phase) {
    std::vector<ps::Value> parts;
    std::shared_ptr<const ps::CpuStorage> owner;
    if (shared) {
      auto buffer = take(phase.allocator.allocate(32));
      for (unsigned i = 0; i < 4; ++i) {
        double value = i;
        std::memcpy(buffer.data() + 8 * i, &value, 8);
      }
      owner = std::move(buffer).freeze();
    }
    for (const auto& box : phase.query.outputs.boxes()) {
      const auto range = box.dimensions()[0];
      for (std::uint64_t i = range.offset; i < range.offset + range.extent;
           ++i) {
        if (shared) {
          parts.push_back(take(ps::Value::from_storage(
              phase.query.output.descriptor, ps::Region({{i, 1}}),
              {8 * i, {0}, {i}}, owner)));
          continue;
        }
        auto writer = take(
            ps::MutableValue::allocate(phase.query.output.descriptor,
                                       ps::Region({{i, 1}}), phase.allocator));
        double value = static_cast<double>(i);
        std::memcpy(writer.data(), &value, 8);
        parts.push_back(take(std::move(writer).publish()));
      }
    }
    return ps::Result<ps::DependencyPoll>(take(ps::ValueFragments::create(
        phase.query.output.descriptor, {}, phase.query.outputs,
        std::move(parts), phase.sets)));
  }
};
void whole_views() {
  auto operations = std::make_shared<ps::OperationRegistry>();
  unsigned called = 0;
  ps::OperationDefinition view;
  view.key = "manual.whole_view";
  view.traits.input_count = 1;
  view.traits.input_schema.resize(1);
  auto& output = view.traits.outputs[0];
  output.shape_rule = ps::OperationShapeRule::MatchAllInputs;
  output.region_rule = ps::OperationRegionRule::Whole;
  output.preserve_output_views = true;
  output.requires_input_views = true;
  output.maximum_output_payload_bytes = 0;
  output.output_dtype_rule = ps::OperationDtypeRule::Input;
  view.callback = [&](const ps::OperationInvocation& call) {
    ++called;
    return ps::Result<ps::Value>(call.inputs[0]);
  };
  auto broken = view;
  broken.key = "manual.broken_view";
  broken.callback = [](const ps::OperationInvocation& call) {
    auto denied = call.allocator.allocate(1);
    return ps::Result<ps::Value>(call.inputs[0]);
  };
  require(operations->register_operation(std::move(broken)).ok(),
          "register bound probe");
  auto automatic = view;
  automatic.key = "manual.whole_auto";
  automatic.traits.outputs[0].requires_input_views = false;
  require(operations->register_operation(std::move(view)).ok() &&
              operations->register_operation(std::move(automatic)).ok(),
          "register Whole view policies");
  ps::OperationDefinition split;
  split.key = "manual.split_owner";
  split.traits.input_count = 0;
  split.traits.input_schema.clear();
  auto& split_output = split.traits.outputs[0];
  split_output.shape_rule = ps::OperationShapeRule::Fixed;
  split_output.fixed_output_shape = {4};
  split_output.output_element_type = ps::ElementType::Float64;
  split_output.region_rule = ps::OperationRegionRule::Dependency;
  split_output.dependency_version = 1;
  split_output.regional_atomic = true;
  split_output.preserve_output_views = true;
  split_output.continuation_bytes = sizeof(SplitOwner);
  split_output.maximum_dependency_stages = 1;
  split.start_dependency = [](const auto&, const auto& allocator) {
    return ps::DependencyContinuation::make<SplitOwner>(allocator);
  };
  auto shared_split = split;
  shared_split.key = "manual.shared_split";
  shared_split.start_dependency = [](const auto&, const auto& allocator) {
    return ps::DependencyContinuation::make<SplitOwner>(allocator, true);
  };
  require(operations->register_operation(std::move(shared_split)).ok(),
          "register compatible split owner");
  const std::uint64_t extent = UINT64_C(1) << 30;
  ps::OperationDefinition huge;
  huge.key = "manual.huge_view";
  huge.traits.input_count = 0;
  huge.traits.input_schema.clear();
  auto& huge_output = huge.traits.outputs[0];
  huge_output.region_rule = ps::OperationRegionRule::Whole;
  huge_output.shape_rule = ps::OperationShapeRule::Fixed;
  huge_output.fixed_output_shape = {extent};
  huge_output.output_element_type = ps::ElementType::Float64;
  huge_output.preserve_output_views = true;
  huge_output.maximum_output_payload_bytes = 8;
  huge.callback = [extent](const ps::OperationInvocation& call) {
    auto storage = take(call.allocator.allocate(8));
    const double value = 7;
    std::memcpy(storage.data(), &value, 8);
    return ps::Value::from_storage({ps::ElementType::Float64, {extent}},
                                   call.output_region, {0, {0}},
                                   std::move(storage).freeze());
  };
  require(operations->register_operation(std::move(split)).ok() &&
              operations->register_operation(std::move(huge)).ok() &&
              operations->freeze().ok(),
          "register split-owner and huge view producers");
  ps::WorkflowDocument document;
  document.nodes = {
      {1, "manual.huge_view", {}, {}},
      {2, "manual.whole_view", {ps::WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"values", 2, "value"}};
  ps::GraphContext graph(document);
  auto plan = take(ps::Compiler(operations).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 65536;
  config.result_cache_bytes = 0;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(operations, config);
  ps::ExecutionBindings bindings;
  auto result = take(context.execute(plan.plan, bindings));
  require(result.values.at("values").bytes().size() == 8 &&
              result.values.at("values").layout().byte_strides[0] == 0 &&
              called == 1,
          "Whole keeps huge zero-stride source without dense input/output "
          "allocation");
  document.inputs.clear();
  document.nodes = {
      {1, "manual.split_owner", {}, {}},
      {2, "manual.whole_view", {ps::WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"values", 2, "value"}};
  ps::GraphContext split_graph(document);
  auto split_plan = take(ps::Compiler(operations).compile(split_graph));
  auto failed = context.execute(split_plan.plan);
  require(!failed.ok() &&
              failed.status().message.find("ViewUnavailable") !=
                  std::string::npos &&
              called == 1,
          "explicit Whole View rejects multiple owners before callback");
  document.nodes[1].operation = "manual.whole_auto";
  ps::GraphContext auto_graph(document);
  auto auto_plan = take(ps::Compiler(operations).compile(auto_graph));
  auto collected = take(context.execute(auto_plan.plan));
  require(called == 2 && collected.values.at("values").bytes().size() == 32,
          "Whole Auto may collect multiple owners");
  const double expected[] = {0, 1, 2, 3};
  require(std::memcmp(collected.values.at("values").bytes().data(), expected,
                      32) == 0,
          "Auto collection preserves complete values");
  document.nodes[0].operation = "manual.shared_split";
  document.nodes[1].operation = "manual.whole_view";
  ps::GraphContext shared_graph(document);
  auto shared_plan = take(ps::Compiler(operations).compile(shared_graph));
  auto shared_result = take(context.execute(shared_plan.plan));
  require(std::memcmp(shared_result.values.at("values").bytes().data(),
                      expected, 32) == 0,
          "Whole View coalesces compatible same-owner fragment maps without "
          "copying");
  auto external_storage = take(ps::BufferAllocator{}.allocate(32));
  std::memcpy(external_storage.data(), expected, 32);
  auto external = take(ps::Value::from_storage(
      {ps::ElementType::Float64, {4}}, ps::Region::whole({4}), {0, {8}},
      std::move(external_storage).freeze()));
  document.inputs = {{1,
                      "external",
                      external.descriptor(),
                      external.region(),
                      external.layout(),
                      {}}};
  document.nodes = {
      {1, "manual.whole_view", {ps::WorkflowInputReference{1}}, {}}};
  document.outputs = {{"values", 1, "value"}};
  ps::GraphContext external_graph(document);
  auto external_plan = take(ps::Compiler(operations).compile(external_graph));
  ps::ExecutionBindings external_bindings;
  external_bindings.inputs = {{"external", external}};
  auto external_result =
      take(context.execute(external_plan.plan, external_bindings));
  require(external_result.values.at("values").storage() == external.storage(),
          "Whole view retains external input owner without post-callback copy");
  const std::vector<ps::Value> direct_inputs{ps::Value::from_float64(1)};
  const std::vector<ps::Region> direct_demands{ps::Region::whole({1})};
  const std::map<std::string, ps::ParameterValue> parameters;
  ps::OperationInvocation direct(direct_inputs, direct_demands, parameters);
  require(operations->invoke("manual.broken_view", direct).status().reason ==
              ps::FailureReason::CapacityLimit,
          "ignored Whole view allocation rejection remains sticky");
  std::cout << "Whole view: huge zero-stride backing, strict multi-owner "
               "failure and Auto collect passed\n";
}
void numeric_function_counters() {
  ps::NumericDiagnostics report;
  report.profile = ps::CpuNumericProfile::Strict;
  const char identity[] = "manual-function-counters/1";
  std::memcpy(report.implementation.data(), identity, sizeof(identity));
  report.strict_math_calls = 3;
  report.strict_fallbacks = 2;
  report.fallback_reasons[1] = 2;
  report.function_fallbacks[static_cast<unsigned>(ps::NumericMathFunction::Sin)]
                           [1] = 1;
  ps::NumericDiagnostics total;
  require(ps::merge_numeric_diagnostics(&total, report).ok() &&
              total.strict_math_calls == 3 &&
              total.function_fallbacks[0][1] == 1,
          "unattributed reason counts enter Other");
  require(ps::merge_numeric_diagnostics(&total, report).ok() &&
              total.strict_math_calls == 6 &&
              total.function_fallbacks[static_cast<unsigned>(
                  ps::NumericMathFunction::Sin)][1] == 2,
          "function counts accumulate");
  const auto saved = total;
  report.function_fallbacks[1][1] = 2;
  require(!ps::merge_numeric_diagnostics(&total, report).ok() &&
              total.function_fallbacks == saved.function_fallbacks &&
              total.strict_math_calls == saved.strict_math_calls,
          "malformed attribution preserves destination");
  report.function_fallbacks[1][1] = 0;
  total.strict_math_calls = UINT64_MAX;
  require(!ps::merge_numeric_diagnostics(&total, report).ok() &&
              total.strict_math_calls == UINT64_MAX,
          "math call overflow does not wrap");
}
}  // namespace
int main() {
  try {
    auto counts = std::make_shared<Counts>();
    numeric_function_counters();
    whole_preparation();
    whole_views();
    compiler_and_direct(counts);
    seals_and_lifetime(counts);
    direct_joint(counts);
    require(counts->prepared == counts->destroyed,
            "all immutable programs released");
    std::cout
        << "prepared programs: compile/direct/joint once, outputs/ROI/tile "
           "reuse, exact static seals and session lifetime passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
