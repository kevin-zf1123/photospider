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
  std::atomic<unsigned> joint_started{0}, joint_destroyed{0}, joint_polls{0};
};
struct Program {
  std::shared_ptr<Counts> counts;
  explicit Program(std::shared_ptr<Counts> stats) : counts(std::move(stats)) {}
  ~Program() { ++counts->destroyed; }
  const double value = 7;
};
ps::SchemaTemplate prepared_tensor_schema(std::string id, std::uint64_t size,
                                          bool atomic = false) {
  ps::SchemaTemplate schema;
  schema.id = std::move(id);
  ps::ResultTensorSpec tensor;
  tensor.key = "data";
  tensor.descriptor = {ps::ElementType::Float64, {size}};
  tensor.atomic_trailing_axes = atomic ? 1 : 0;
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
ps::OperationOutputTraits prepared_result_output(std::string key,
                                                 ps::SchemaTemplate schema) {
  ps::OperationOutputTraits output;
  output.key = std::move(key);
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = std::string(schema.id);
  output.output_schema.result_schema_version = schema.version;
  output.result_schema = std::move(schema);
  output.region_rule = ps::OperationRegionRule::Whole;
  output.dependency_version = 2;
  output.continuation_bytes = 1;
  output.maximum_dependency_stages = 2;
  return output;
}
ps::Result<ps::ResultProgramPoll> publish_prepared(
    const ps::ResultProgramPhase& phase) {
  require(phase.query.prepared && phase.query.prepared->state(),
          "prepared poll owns immutable program");
  const auto* program =
      static_cast<const Program*>(phase.query.prepared->state());
  const auto& schema = *phase.query.output.result_schema;
  const auto& spec = schema.tensors[0];
  const auto demand = phase.query.tensor_outputs
                          ? *phase.query.tensor_outputs
                          : take(ps::Footprint::all(spec.sample_shape()));
  auto builder = take(ps::ResultBuilder::start(phase.resources, schema,
                                               phase.query.semantic_key));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(phase.resources, 1, {})))
              .ok(),
          "prepared Result descriptor");
  for (const auto& box : demand.boxes()) {
    auto status = builder.publish_tensor_kernel(
        0, box,
        [&](const auto& writers) {
          for (const auto& writer : writers) {
            auto footprint = take(ps::Footprint::from_regions(
                spec.sample_shape(), {writer.region()}));
            auto written = footprint.visit(
                [&](const auto& at) {
                  auto row = writer.row_run(at);
                  if (!row.ok())
                    return row.status();
                  std::memcpy(row.value().data, &program->value,
                              sizeof(double));
                  return ps::Status::success();
                },
                take(footprint.element_count()));
            if (!written.ok())
              return written;
          }
          return ps::Status::success();
        },
        take(ps::ResultRelation::cartesian(phase.resources,
                                           take(spec.sample_count()), {})),
        {true, true, true, true});
    require(status.ok(), "prepared Result transactional publication");
  }
  return ps::Result<ps::ResultProgramPoll>(
      ps::ResultPublication{take(builder.seal()), true});
}
struct State {
  const Program* program;
  explicit State(const Program* value) : program(value) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    require(phase.query.prepared && phase.query.prepared->state() == program,
            "session owns exact immutable program");
    return publish_prepared(phase);
  }
};
struct Joint {
  const Program* program;
  explicit Joint(const Program* value) : program(value) {}
  ~Joint() { ++program->counts->joint_destroyed; }
  ps::Result<ps::ResourceVector<ps::ResultJointOutcome>> poll(
      const ps::ResultJointPhase& phase) {
    ++program->counts->joint_polls;
    ps::ResourceVector<ps::ResultJointOutcome> result;
    for (const auto* member : phase.members) {
      require(
          member->query.prepared && member->query.prepared->state() == program,
          "joint owns exact immutable program");
      result.push_back({take(ps::result_atom_key(member->query)),
                        publish_prepared(*member)});
    }
    return ps::Result<ps::ResourceVector<ps::ResultJointOutcome>>(
        std::move(result));
  }
};
ps::OperationDefinition definition(const std::shared_ptr<Counts>& counts) {
  ps::OperationDefinition operation;
  operation.key = "manual.prepared";
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].kind = ps::OperationPortKind::Result;
  traits.input_schema[0].result_schema_id = "manual.prepared.input";
  traits.input_schema[0].result_schema_version = 1;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"mask", ps::OperationParameterType::Float64}};
  traits.joint_contract = 2;
  traits.joint_continuation_bytes = sizeof(Joint);
  traits.outputs = {
      prepared_result_output(
          "values", prepared_tensor_schema("manual.prepared.values", 5)),
      prepared_result_output(
          "axis", prepared_tensor_schema("manual.prepared.axis", 3, true))};
  for (auto& output : traits.outputs) {
    output.region_rule = ps::OperationRegionRule::Dependency;
    output.continuation_bytes = sizeof(State);
    output.maximum_dependency_stages = 1;
    output.failure_delivery = ps::FailureDelivery::PerAtomOutcome;
  }
  operation.prepare_static = [counts](const auto& inputs, const auto&) {
    if (inputs.size() != 1 || !inputs[0].result_schema ||
        inputs[0].result_schema->tensors.size() != 1 ||
        inputs[0].result_schema->tensors[0].descriptor.element_type !=
            ps::ElementType::Float64 ||
        inputs[0].result_schema->tensors[0].sample_shape() !=
            std::vector<std::uint64_t>{1})
      return ps::Result<ps::OperationPreparation>(
          ps::Status{ps::ErrorCode::TypeMismatch, "expected Result scalar"});
    ++counts->prepared;
    ps::OperationPreparation prepared;
    prepared.outputs.resize(2);
    prepared.outputs[0].metadata.result_schema =
        std::make_shared<const ps::SchemaTemplate>(
            prepared_tensor_schema("manual.prepared.values", 5));
    prepared.outputs[1].metadata.result_schema =
        std::make_shared<const ps::SchemaTemplate>(
            prepared_tensor_schema("manual.prepared.axis", 3, true));
    prepared.state = std::make_shared<const Program>(counts);
    return ps::Result<ps::OperationPreparation>(std::move(prepared));
  };
  operation.start_result = [counts](const auto& query, const auto& allocator) {
    ++counts->started;
    require(query.prepared != nullptr, "prepared singleton query");
    return ps::ResultContinuation::make<State>(
        allocator, static_cast<const Program*>(query.prepared->state()));
  };
  operation.start_result_joint = [counts](const auto& queries,
                                          const auto& allocator) {
    ++counts->joint_started;
    require(!queries.empty() && queries[0].prepared, "prepared joint start");
    for (const auto& query : queries)
      require(query.prepared == queries[0].prepared, "one joint program owner");
    return ps::ResultJointContinuation::make<Joint>(
        allocator, static_cast<const Program*>(queries[0].prepared->state()));
  };
  return operation;
}
std::shared_ptr<ps::OperationRegistry> registry(
    const std::shared_ptr<Counts>& counts) {
  auto result = std::make_shared<ps::OperationRegistry>();
  require(result->register_operation(definition(counts)).ok() &&
              result->freeze().ok(),
          "register prepared Result operation");
  return result;
}
ps::ResultProgramMetadata prepared_metadata() {
  ps::ResultProgramMetadata metadata;
  metadata.inputs.resize(1);
  metadata.inputs[0].result_schema = std::make_shared<const ps::SchemaTemplate>(
      prepared_tensor_schema("manual.prepared.input", 1));
  metadata.output.result_schema = std::make_shared<const ps::SchemaTemplate>(
      prepared_tensor_schema("manual.prepared.values", 5));
  return metadata;
}
ps::ResultRef prepared_source(const ps::ResourceBudget& root, double number) {
  auto schema = prepared_tensor_schema("manual.prepared.input", 1);
  auto builder =
      take(ps::ResultBuilder::start(root, schema, "prepared.source"));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(root, 1, {})))
              .ok(),
          "prepared source descriptor");
  require(builder
              .publish_tensor(0, ps::Region::whole({1}),
                              {reinterpret_cast<const std::uint8_t*>(&number),
                               sizeof(number)},
                              take(ps::ResultRelation::cartesian(root, 1, {})),
                              {true, true, true, true})
              .ok(),
          "prepared source publication");
  return take(builder.seal());
}
void check_prepared_sample(const ps::ResultRef& result, std::uint64_t at) {
  std::uint64_t bits = 0;
  require(result.read_tensor(take(result.descriptor()), 0, {at}, &bits,
                             sizeof(bits))
                  .ok() &&
              bits == UINT64_C(0x401c000000000000),
          "prepared Float64 output bits");
}
ps::Result<ps::ResultProgramPoll> poll_direct(
    ps::ResultContinuation& continuation, const ps::ResultProgramQuery& query,
    const ps::ResourceBudget& root) {
  ps::ResultObjectInputs objects;
  ps::ResourceVector<ps::ResultIoReply> io;
  const auto allocator = root.allocator();
  ps::ResultProgramPhase phase{query, objects,
                               io,    allocator,
                               root,  [&](auto n) { return root.consume({n}); },
                               {}};
  return continuation.poll(phase);
}
void direct_joint(const std::shared_ptr<Counts>& counts, unsigned members = 2) {
  auto operations = registry(counts);
  auto metadata = prepared_metadata();
  const std::map<std::string, ps::ParameterValue> parameters{{"mask", 0.0}};
  ps::ResourceBudget root;
  ps::ResourceAllocationScope scope(root);
  ps::ResourceVector<ps::ResultProgramQuery> queries;
  queries.reserve(members);
  for (unsigned at = 0; at < members; ++at) {
    queries.emplace_back(metadata, parameters);
    queries.back().semantic_key = "prepared.direct-joint";
    queries.back().snapshot_identity = "prepared.joint-inputs";
    queries.back().tensor_outputs =
        take(ps::Footprint::from_regions({5}, {ps::Region({{at, 1}})}));
  }
  const auto before = counts->prepared.load();
  const auto starts = counts->joint_started.load();
  const auto polls = counts->joint_polls.load();
  auto session =
      take(operations->start_result_joint("manual.prepared", queries, root));
  require(counts->prepared == before + 1 && counts->joint_started == starts + 1,
          "direct joint prepares and starts once");
  ps::ResultObjectInputs objects;
  ps::ResourceVector<ps::ResultIoReply> io;
  const auto allocator = root.allocator();
  std::vector<std::unique_ptr<ps::ResultProgramPhase>> phases;
  ps::ResourceVector<const ps::ResultProgramPhase*> pointers;
  for (const auto& query : queries) {
    phases.push_back(std::make_unique<ps::ResultProgramPhase>(
        ps::ResultProgramPhase{query,
                               objects,
                               io,
                               allocator,
                               root,
                               [&](auto n) { return root.consume({n}); },
                               {}}));
    pointers.push_back(phases.back().get());
  }
  auto outcomes = take(session.poll(
      {pointers, allocator, [&](auto n) { return root.consume({n}); }}));
  require(outcomes.size() == members && counts->joint_polls == polls + 1,
          "direct joint actual callback outcomes");
  for (const auto& outcome : outcomes) {
    require(
        outcome.outcome.ok() && std::holds_alternative<ps::ResultPublication>(
                                    outcome.outcome.value()),
        "direct joint published Result");
    check_prepared_sample(
        std::get<ps::ResultPublication>(outcome.outcome.value()).result,
        outcome.key.coordinate[0]);
  }
}
void compiler_and_direct(const std::shared_ptr<Counts>& counts) {
  auto operations = registry(counts);
  ps::WorkflowDocument document;
  ps::WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "input";
  input.result_schema = prepared_metadata().inputs[0].result_schema;
  document.inputs = {input};
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
  config.result_cache_bytes = 0;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(operations, config);
  const auto root = take(context.resource_budget());
  ps::ExecutionBinding binding;
  binding.name = "input";
  binding.result = prepared_source(root, 1);
  const ps::DemandQuery complete{{"values", take(ps::Footprint::all({5}))},
                                 {"axis", take(ps::Footprint::all({3}))}};
  for (bool joint : {false, true}) {
    ps::ExecutionOptions options;
    options.enable_joint = joint;
    const auto starts = counts->joint_started.load();
    const auto singleton = counts->started.load();
    {
      auto result = take(context.execute_atoms(compiled.plan, {{binding}},
                                               complete, {}, options));
      require(result.atoms.size() == 6, "all five values and one axis Atom");
      unsigned values = 0, axes = 0;
      for (const auto& atom : result.atoms) {
        require(atom.outcome.ok(), "prepared Atom succeeded");
        if (atom.name == "values") {
          ++values;
          check_prepared_sample(atom.outcome.value(), atom.key.coordinate[0]);
        } else {
          require(atom.name == "axis", "known prepared output name");
          ++axes;
          require(take(atom.outcome.value().descriptor()).tensor_coverage(0) ==
                      take(ps::Footprint::all({3})),
                  "axis closes all components");
          for (std::uint64_t at = 0; at < 3; ++at)
            check_prepared_sample(atom.outcome.value(), at);
        }
      }
      require(values == 5 && axes == 1 && counts->started == singleton,
              "compiled C2 uses its required joint callback");
      const auto groups = counts->joint_started.load() - starts;
      require(
          joint ? groups > 0 && groups < 6 : groups == 6,
          "joint option changes actual callback grouping without cache reuse");
      std::cout << "C2 Result preparation: grouping=" << (joint ? "on" : "off")
                << " joint starts=" << groups << " values=" << values
                << " axis tuples=" << axes << '\n';
    }
    require(counts->joint_destroyed == counts->joint_started,
            "compiled joint states retired");
  }
  binding.result = prepared_source(root, 9);
  auto result = take(context.execute_atoms(
      compiled.plan, {{binding}},
      {{"values", take(ps::Footprint::from_regions(
                      {5}, {ps::Region({{1, 1}}), ps::Region({{4, 1}})}))}}));
  require(result.atoms.size() == 2,
          "prepared sparse query contains both requested atoms");
  for (const auto& atom : result.atoms)
    check_prepared_sample(atom.outcome.value(), atom.key.coordinate[0]);
  auto tile = take(compiled.plan.tile_plan("values", ps::Region({{2, 1}})));
  require(tile.steps()[0].prepared == compiled.plan.steps()[0].prepared,
          "tile keeps program");
  require(counts->prepared == before + 1,
          "runs, outputs and rebinding never reprepare");
  direct_joint(counts, 5);
}
void seals_and_lifetime(const std::shared_ptr<Counts>& counts) {
  static_assert(!std::is_copy_constructible_v<ps::PreparedOperation> &&
                !std::is_move_constructible_v<ps::PreparedOperation>);
  auto operations = registry(counts),
       other = registry(std::make_shared<Counts>());
  auto metadata = prepared_metadata();
  std::map<std::string, ps::ParameterValue> parameters{{"mask", 0.0}};
  ps::ResultProgramQuery query(metadata, parameters);
  query.semantic_key = "static-owner";
  query.snapshot_identity = "static-owner";
  query.tensor_outputs = take(ps::Footprint::none({5}));
  query.prepared = take(operations->prepare_operation(
      "manual.prepared", metadata.inputs, parameters));
  const auto before = counts->prepared.load();
  ps::ResourceBudget root;
  require(
      operations->start_result("manual.prepared", query, root.allocator()).ok(),
      "valid Empty preparation");
  auto foreign =
      other->start_result("manual.prepared", query, root.allocator());
  require(!foreign.ok() && foreign.status().code == ps::ErrorCode::Stale,
          "foreign preparation rejected");
  auto changed_metadata = metadata;
  auto changed_parameters = parameters;
  ps::ResultProgramQuery changed(changed_metadata, changed_parameters);
  changed.semantic_key = "static-owner";
  changed.snapshot_identity = "static-owner";
  changed.tensor_outputs = query.tensor_outputs;
  changed.prepared = query.prepared;
  changed_parameters["mask"] = -0.0;
  require(
      !operations->start_result("manual.prepared", changed, root.allocator())
           .ok(),
      "signed zero static identity");
  changed_parameters = parameters;
  auto changed_schema = *metadata.inputs[0].result_schema;
  changed_schema.tensors[0].descriptor.shape = {2};
  changed_metadata.inputs[0].result_schema =
      std::make_shared<const ps::SchemaTemplate>(changed_schema);
  require(
      !operations->start_result("manual.prepared", changed, root.allocator())
           .ok(),
      "complete Result metadata changed");
  changed_metadata = metadata;
  std::uint64_t nan_bits = UINT64_C(0x7ff8000000000042);
  double nan = 0;
  std::memcpy(&nan, &nan_bits, sizeof(nan));
  changed_parameters["mask"] = nan;
  changed.prepared = take(operations->prepare_operation(
      "manual.prepared", changed_metadata.inputs, changed_parameters));
  require(operations->start_result("manual.prepared", changed, root.allocator())
              .ok(),
          "identical NaN parameter bits match");
  ++nan_bits;
  std::memcpy(&nan, &nan_bits, sizeof(nan));
  changed_parameters["mask"] = nan;
  require(
      !operations->start_result("manual.prepared", changed, root.allocator())
           .ok(),
      "distinct NaN payload rejects");
  require(counts->prepared == before + 1, "static validation never reparses");
  query.tensor_outputs =
      take(ps::Footprint::from_regions({5}, {ps::Region({{0, 1}})}));
  auto session = take(
      operations->start_result("manual.prepared", query, root.allocator()));
  query.prepared.reset();
  changed.prepared.reset();
  operations.reset();
  auto completed = take(poll_direct(session, query, root));
  require(std::holds_alternative<ps::ResultPublication>(completed),
          "session outlives registry and external program owner");
  auto retained = std::get<ps::ResultPublication>(std::move(completed)).result;
  session = {};
  check_prepared_sample(retained, 0);
  require(root.statistics().live[ps::ResourceKind::Payload] == sizeof(double),
          "only retained Result payload survives continuation retirement");
  retained = {};
  require(root.statistics().live[ps::ResourceKind::Payload] == 0,
          "Result payload released after last owner");
}
struct WholePreparedState {
  const Program* program;
  bool requested = false;
  explicit WholePreparedState(const Program* program) : program(program) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    require(phase.query.prepared && phase.query.prepared->state() == program,
            "Whole owns exact immutable prepared program");
    const auto& selected = phase.query.prepared->traits()
                               .outputs.at(phase.query.output_index)
                               .input_indices;
    require(selected.has_value(), "Whole static projection resolved");
    const bool empty =
        phase.query.tensor_outputs && phase.query.tensor_outputs->empty();
    if (!empty && !requested && !selected->empty()) {
      requested = true;
      ps::ResultProgramNeed need;
      for (auto port : *selected)
        need.tensors.push_back({port, 0, take(ps::Footprint::all({1})), 13});
      return ps::Result<ps::ResultProgramPoll>(std::move(need));
    }
    require((phase.tensors ? phase.tensors->size() : 0) ==
                (empty ? 0 : selected->size()),
            "Whole static projection reaches Result callback");
    if (!empty) {
      for (auto port : *selected) {
        double input = 0;
        auto read = phase.read_tensor(port, 0, {0}, &input, sizeof(input));
        require(read.ok(), "Whole selected scalar read");
      }
    }
    const auto& schema = *phase.query.output.result_schema;
    auto builder = take(ps::ResultBuilder::start(phase.resources, schema,
                                                 phase.query.semantic_key));
    require(builder
                .bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(phase.resources, 1, {})))
                .ok(),
            "Whole descriptor relation");
    const auto& spec = schema.tensors[0];
    if (empty)
      return ps::Result<ps::ResultProgramPoll>(
          ps::ResultPublication{take(builder.seal()), true});
    auto written = builder.publish_tensor_kernel(
        0, ps::Region::whole(spec.sample_shape()),
        [&](const auto& windows) {
          for (const auto& window : windows) {
            const auto covered = take(ps::Footprint::from_regions(
                spec.sample_shape(), {window.region()}));
            auto status = covered.visit(
                [&](const auto& at) {
                  auto run = window.row_run(at);
                  if (!run.ok())
                    return run.status();
                  std::memcpy(run.value().data, &program->value,
                              sizeof(double));
                  return ps::Status::success();
                },
                take(covered.element_count()));
            if (!status.ok())
              return status;
          }
          return ps::Status::success();
        },
        take(ps::ResultRelation::cartesian(phase.resources,
                                           take(spec.sample_count()), {})),
        {true, true, true, true});
    require(written.ok(), "Whole prepared transactional publication");
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};
void whole_preparation() {
  auto counts = std::make_shared<Counts>();
  auto operations = std::make_shared<ps::OperationRegistry>();
  const auto input_schema = prepared_tensor_schema("manual.prepared.input", 1);
  const auto values_schema =
      prepared_tensor_schema("manual.prepared.values", 5);
  const auto axis_schema =
      prepared_tensor_schema("manual.prepared.axis", 3, true);
  ps::OperationDefinition whole;
  whole.key = "manual.whole_prepared";
  whole.traits.input_count = 1;
  whole.traits.input_schema.resize(1);
  whole.traits.input_schema[0].kind = ps::OperationPortKind::Result;
  whole.traits.input_schema[0].result_schema_id = std::string(input_schema.id);
  whole.traits.input_schema[0].result_schema_version = input_schema.version;
  whole.traits.requires_metadata_specialization = true;
  whole.traits.parameter_schema = {
      {"mask", ps::OperationParameterType::Float64}};
  whole.traits.outputs = {prepared_result_output("values", values_schema),
                          prepared_result_output("axis", axis_schema)};
  for (auto& output : whole.traits.outputs)
    output.continuation_bytes = sizeof(WholePreparedState);
  whole.prepare_static = [counts, values_schema, axis_schema](
                             const auto& inputs, const auto& parameters) {
    if (inputs.size() != 1 || !inputs[0].result_schema ||
        inputs[0].result_schema->tensors.size() != 1 ||
        inputs[0].result_schema->tensors[0].descriptor.element_type !=
            ps::ElementType::Float64 ||
        inputs[0].result_schema->tensors[0].sample_shape() !=
            std::vector<std::uint64_t>{1})
      return ps::Result<ps::OperationPreparation>(
          ps::Status{ps::ErrorCode::TypeMismatch, "expected Result scalar"});
    ++counts->prepared;
    ps::OperationPreparation prepared;
    prepared.outputs.resize(2);
    prepared.outputs[0].metadata.result_schema =
        std::make_shared<const ps::SchemaTemplate>(values_schema);
    prepared.outputs[1].metadata.result_schema =
        std::make_shared<const ps::SchemaTemplate>(axis_schema);
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
    prepared.state = std::make_shared<const Program>(counts);
    return ps::Result<ps::OperationPreparation>(std::move(prepared));
  };
  whole.start_result = [counts](const auto& query, const auto& allocator) {
    ++counts->started;
    require(query.prepared && query.prepared->state(), "Whole prepared start");
    return ps::ResultContinuation::make<WholePreparedState>(
        allocator, static_cast<const Program*>(query.prepared->state()));
  };
  auto narrow = whole;
  narrow.key = "manual.whole_narrow";
  for (auto& output : narrow.traits.outputs)
    output.input_indices = std::vector<std::uint32_t>{};
  require(operations->register_operation(std::move(narrow)).ok(),
          "register empty input template");
  unsigned source_starts = 0;
  ps::OperationDefinition excluded;
  excluded.key = "manual.excluded_result_source";
  excluded.traits.outputs = {prepared_result_output("value", input_schema)};
  excluded.start_result =
      [&](const auto&, const auto&) -> ps::Result<ps::ResultContinuation> {
    ++source_starts;
    return ps::Result<ps::ResultContinuation>(
        ps::Status{ps::ErrorCode::OperationFailed, "excluded source"});
  };
  auto excluded_registered =
      operations->register_operation(std::move(excluded));
  if (!excluded_registered.ok())
    throw std::runtime_error("register excluded Result source: " +
                             excluded_registered.message);
  auto registered = operations->register_operation(std::move(whole));
  if (!registered.ok())
    throw std::runtime_error("register Whole Result preparation: " +
                             registered.message);
  require(operations->freeze().ok(), "freeze Whole Result preparation");
  ps::WorkflowDocument document;
  ps::WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "input";
  input.result_schema =
      std::make_shared<const ps::SchemaTemplate>(input_schema);
  document.inputs = {input};
  document.nodes = {{1,
                     "manual.whole_prepared",
                     {ps::WorkflowInputReference{1}},
                     {{"mask", 0.0}}}};
  document.outputs = {{"values", 1, "values"}, {"axis", 1, "axis"}};
  ps::GraphContext graph(document);
  auto compiled = take(ps::Compiler(operations).compile(graph));
  require(counts->prepared == 1, "Whole compile prepares once");
  require(
      compiled.plan.steps()[0].prepared == compiled.plan.steps()[1].prepared &&
          compiled.semantic.nodes()[0].prepared ==
              compiled.optimized.nodes()[0].prepared,
      "Whole semantic, optimized and named outputs share owner");
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 0;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(operations, config);
  auto root = take(context.resource_budget());
  auto builder = take(ps::ResultBuilder::start(root, input_schema, "scalar"));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(root, 1, {})))
              .ok(),
          "scalar Result descriptor");
  const double scalar = 1;
  require(builder
              .publish_tensor(0, ps::Region::whole({1}),
                              {reinterpret_cast<const std::uint8_t*>(&scalar),
                               sizeof(scalar)},
                              take(ps::ResultRelation::cartesian(root, 1, {})),
                              {true, true, true, true})
              .ok(),
          "scalar Result publication");
  ps::ExecutionBinding binding;
  binding.name = "input";
  binding.result = take(builder.seal());
  auto snapshot = take(context.freeze(compiled.plan, {{binding}}));
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    auto result = take(context.execute_fragments(
        snapshot, {{"axis", take(ps::Footprint::from_regions(
                                {3}, {ps::Region({{1, 1}})}))}}));
    const auto& axis = result.results.at("axis");
    const auto facts = take(axis.descriptor());
    require(facts.tensor_coverage(0) == take(ps::Footprint::all({3})),
            "Whole axis closes all three atomic components");
    for (std::uint64_t component = 0; component < 3; ++component) {
      double value = 0;
      require(
          axis.read_tensor(facts, 0, {component}, &value, sizeof(value)).ok() &&
              value == 7,
          "Whole prepared tuple component");
    }
  }
  require(counts->prepared == 1 && counts->started == 2,
          "Whole executions reuse compile owner");
  auto excluded_document = document;
  excluded_document.inputs.clear();
  excluded_document.nodes = {{10, "manual.excluded_result_source", {}, {}},
                             {1,
                              "manual.whole_prepared",
                              {ps::WorkflowNodeOutput{10, "value"}},
                              {{"mask", 0.0}}}};
  ps::GraphContext excluded_graph(excluded_document);
  auto excluded_plan = take(ps::Compiler(operations).compile(excluded_graph));
  require(context.execute(excluded_plan.plan).ok() && source_starts == 0,
          "Whole empty static projection suppresses upstream source startup");
  auto selected_document = document;
  selected_document.nodes[0].parameters["mask"] = 1.0;
  ps::GraphContext selected_graph(selected_document);
  auto selected_plan = take(ps::Compiler(operations).compile(selected_graph));
  auto selected_result = take(context.execute(selected_plan.plan, {{binding}}));
  for (const auto& named : selected_result.results) {
    const auto ids = named.second.association();
    require(ids.size() == 1 && ids[0] == binding.result.object_id(),
            "Whole selected input grant preserves source association");
  }
  excluded_document.nodes.back().parameters["mask"] = 1.0;
  ps::GraphContext included_graph(excluded_document);
  auto included_plan = take(ps::Compiler(operations).compile(included_graph));
  auto included_failure = context.execute(included_plan.plan);
  require(
      !included_failure.ok() &&
          included_failure.status().code == ps::ErrorCode::OperationFailed &&
          source_starts == 1,
      "Whole included input starts source and propagates its failure");
  auto empty_result = take(
      context.execute_fragments(take(context.freeze(included_plan.plan)),
                                {{"values", take(ps::Footprint::none({5}))}}));
  require(
      source_starts == 1 && take(empty_result.results.at("values").descriptor())
                                .tensor_coverage(0)
                                .empty(),
      "Whole Empty suppresses selected source and publishes no samples");
  auto parameters = document.nodes[0].parameters;
  ps::ResultProgramMetadata metadata;
  metadata.inputs.resize(1);
  metadata.inputs[0].result_schema = input.result_schema;
  metadata.output.result_schema =
      std::make_shared<const ps::SchemaTemplate>(values_schema);
  auto broadened = parameters;
  broadened["mask"] = 1.0;
  require(!operations
               ->prepare_operation("manual.whole_narrow", metadata.inputs,
                                   broadened)
               .ok(),
          "specializer cannot broaden registered empty projection");
  ps::ResultProgramQuery query(metadata, parameters);
  query.prepared = compiled.plan.steps()[0].prepared;
  query.semantic_key = "manual.whole_prepared.direct";
  const auto execute_direct = [&]() {
    auto continuation = take(operations->start_result("manual.whole_prepared",
                                                      query, root.allocator()));
    ps::ResultObjectInputs objects;
    ps::ResourceVector<ps::ResultIoReply> io;
    auto allocator = root.allocator();
    ps::ResultProgramPhase phase{
        query,     objects, io,
        allocator, root,    [&](auto n) { return root.consume({n}); },
        {}};
    auto output = take(continuation.poll(phase));
    require(std::holds_alternative<ps::ResultPublication>(output),
            "Whole direct Result publication");
    auto result = std::get<ps::ResultPublication>(std::move(output)).result;
    const auto facts = take(result.descriptor());
    for (std::uint64_t at = 0; at < 5; ++at) {
      std::uint64_t bits = 0;
      require(result.read_tensor(facts, 0, {at}, &bits, sizeof(bits)).ok() &&
                  bits == UINT64_C(0x401c000000000000),
              "Whole direct prepared output bits");
    }
    return result;
  };
  require(execute_direct().valid(), "Whole direct preparation");
  auto first_call = std::async(std::launch::async, execute_direct);
  auto second_call = std::async(std::launch::async, execute_direct);
  require(first_call.get().valid() && second_call.get().valid(),
          "concurrent Whole preparation reuse");
  const auto started = counts->started.load();
  require(
      operations->start_result("manual.whole_narrow", query, root.allocator())
              .status()
              .code == ps::ErrorCode::Stale,
      "Whole key seal");
  auto changed_schema = input_schema;
  changed_schema.tensors[0].descriptor.element_type = ps::ElementType::Float32;
  metadata.inputs[0].result_schema =
      std::make_shared<const ps::SchemaTemplate>(changed_schema);
  require(!operations
               ->start_result("manual.whole_prepared", query, root.allocator())
               .ok(),
          "Whole complete Result metadata mismatch rejected");
  metadata.inputs[0].result_schema = input.result_schema;
  parameters["mask"] = -0.0;
  require(
      operations->start_result("manual.whole_prepared", query, root.allocator())
              .status()
              .code == ps::ErrorCode::Stale,
      "Whole exact parameter seal");
  require(counts->started == started, "Whole seals reject before factory");
  for (double mode : {2., 3.}) {
    parameters["mask"] = mode;
    require(!operations
                 ->prepare_operation("manual.whole_prepared", metadata.inputs,
                                     parameters)
                 .ok(),
            "invalid specialized projections rejected");
  }
  std::cout
      << "Whole Result preparation: compile reuse, static projections, tuple, "
         "concurrent direct starts and static seals passed\n";
}
struct SplitOwner {
  bool shared;
  unsigned* completed;
  SplitOwner(bool one, unsigned* count) : shared(one), completed(count) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    const auto& schema = *phase.query.output.result_schema;
    auto builder = take(ps::ResultBuilder::start(phase.resources, schema,
                                                 phase.query.semantic_key));
    require(builder
                .bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(phase.resources, 1, {})))
                .ok(),
            "split Result descriptor");
    std::shared_ptr<const ps::CpuStorage> owner;
    if (shared) {
      auto buffer = take(phase.allocator.allocate(32));
      for (unsigned i = 0; i < 4; ++i) {
        double value = i;
        std::memcpy(buffer.data() + 8 * i, &value, 8);
      }
      owner = std::move(buffer).freeze();
    }
    const auto demand = phase.query.tensor_outputs
                            ? *phase.query.tensor_outputs
                            : take(ps::Footprint::all({4}));
    for (const auto& box : demand.boxes()) {
      const auto range = box.dimensions()[0];
      for (std::uint64_t i = range.offset; i < range.offset + range.extent;
           ++i) {
        auto storage = owner;
        if (!shared) {
          auto buffer = take(phase.allocator.allocate(8));
          const double value = static_cast<double>(i);
          std::memcpy(buffer.data(), &value, 8);
          storage = std::move(buffer).freeze();
        }
        require(
            builder
                .publish_tensor(
                    0, ps::Region({{i, 1}}), {shared ? 8 * i : 0, {0}, {i}},
                    std::move(storage),
                    take(ps::ResultRelation::cartesian(phase.resources, 4, {})),
                    {true, true, true, true})
                .ok(),
            "split Result backing");
      }
    }
    auto result = take(builder.seal());
    ++*completed;
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{std::move(result), true});
  }
};
struct HugeView {
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    const auto& schema = *phase.query.output.result_schema;
    const auto extent = schema.tensors[0].sample_shape()[0];
    auto storage = take(phase.allocator.allocate(8));
    const double value = 7;
    std::memcpy(storage.data(), &value, 8);
    auto builder = take(ps::ResultBuilder::start(phase.resources, schema,
                                                 phase.query.semantic_key));
    require(builder
                .bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(phase.resources, 1, {})))
                .ok(),
            "huge Result descriptor");
    require(builder
                .publish_tensor(0, ps::Region::whole({extent}), {0, {0}},
                                std::move(storage).freeze(),
                                take(ps::ResultRelation::cartesian(
                                    phase.resources, extent, {})),
                                {true, true, true, true})
                .ok(),
            "huge zero-stride Result");
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};
struct WholeView {
  unsigned* called;
  bool broken, requested = false;
  std::optional<ps::ResultTensorInput>* retained;
  WholeView(unsigned* count, bool broken,
            std::optional<ps::ResultTensorInput>* retained)
      : called(count), broken(broken), retained(retained) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    const auto& schema = *phase.query.output.result_schema;
    const auto shape = schema.tensors[0].sample_shape();
    if (!requested) {
      requested = true;
      ps::ResultProgramNeed need;
      need.tensors.push_back({0, 0, take(ps::Footprint::all(shape)), 13});
      return ps::Result<ps::ResultProgramPoll>(std::move(need));
    }
    ++*called;
    if (broken) {
      auto denied = phase.allocator.allocate(1);
      require(!denied.ok(), "view workspace rejection");
    }
    const auto& input = phase.tensors->at({0, 0});
    if (retained)
      *retained = input;
    auto window = take(input.acquire(ps::Region::whole(shape)));
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, schema, phase.query.semantic_key, {},
        {phase.association->begin(), phase.association->end()}));
    require(builder
                .bind_descriptor_relation(take(
                    ps::ResultRelation::cartesian(phase.resources, 1, {0, 8})))
                .ok(),
            "Whole view descriptor relation");
    ps::ResultTensorViewTransform identity;
    for (std::uint32_t axis = 0; axis < shape.size(); ++axis)
      identity.source_axes.push_back(
          {static_cast<std::int32_t>(axis), 0, 1, 1});
    require(builder
                .publish_tensor_view(
                    0, ps::Region::whole(shape), window, identity,
                    take(ps::ResultRelation::identity(
                        phase.resources, take(schema.tensors[0].sample_count()),
                        0, 5, ps::ResultSupportTarget::Tensor, 0)),
                    {true, true, true, true})
                .ok(),
            "Whole Result input view");
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
};
void whole_views() {
  auto operations = std::make_shared<ps::OperationRegistry>();
  unsigned called = 0;
  unsigned sources_completed = 0;
  std::optional<ps::ResultTensorInput> retained;
  ps::OperationDefinition view;
  view.key = "manual.whole_view";
  view.traits.input_count = 1;
  view.traits.input_schema.resize(1);
  view.traits.input_schema[0].kind = ps::OperationPortKind::Result;
  view.traits.input_schema[0].result_schema_id = "manual.view.data";
  view.traits.input_schema[0].result_schema_version = 1;
  view.traits.requires_metadata_specialization = true;
  view.traits.outputs = {prepared_result_output(
      "value", prepared_tensor_schema("manual.view.data", 4))};
  auto& output = view.traits.outputs[0];
  output.continuation_bytes = sizeof(WholeView);
  output.preserve_output_views = true;
  output.requires_input_views = true;
  output.maximum_output_payload_bytes = 0;
  view.prepare_static = [](const auto& inputs, const auto&) {
    ps::OperationPreparation prepared;
    prepared.outputs.resize(1);
    prepared.outputs[0].metadata.result_schema = inputs[0].result_schema;
    return ps::Result<ps::OperationPreparation>(std::move(prepared));
  };
  view.start_result = [&](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<WholeView>(allocator, &called, false,
                                                   &retained);
  };
  auto broken = view;
  broken.key = "manual.broken_view";
  broken.start_result = [&](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<WholeView>(allocator, &called, true,
                                                   nullptr);
  };
  require(operations->register_operation(std::move(broken)).ok(),
          "register Result sticky allocation probe");
  auto automatic = view;
  automatic.key = "manual.whole_auto";
  automatic.traits.outputs[0].requires_input_views = false;
  require(operations->register_operation(std::move(view)).ok() &&
              operations->register_operation(std::move(automatic)).ok(),
          "register Whole Result view policies");
  ps::OperationDefinition split;
  split.key = "manual.split_owner";
  split.traits.input_count = 0;
  split.traits.input_schema.clear();
  split.traits.workspace_bytes = 32;
  split.traits.outputs = {prepared_result_output(
      "value", prepared_tensor_schema("manual.view.data", 4))};
  auto& split_output = split.traits.outputs[0];
  split_output.region_rule = ps::OperationRegionRule::Dependency;
  split_output.regional_atomic = true;
  split_output.continuation_bytes = sizeof(SplitOwner);
  split_output.maximum_dependency_stages = 1;
  split.start_result = [&](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<SplitOwner>(allocator, false,
                                                    &sources_completed);
  };
  auto shared_split = split;
  shared_split.key = "manual.shared_split";
  shared_split.start_result = [&](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<SplitOwner>(allocator, true,
                                                    &sources_completed);
  };
  require(operations->register_operation(std::move(shared_split)).ok(),
          "register compatible Result split owner");
  const std::uint64_t extent = UINT64_C(1) << 30;
  ps::OperationDefinition huge;
  huge.key = "manual.huge_view";
  huge.traits.input_count = 0;
  huge.traits.input_schema.clear();
  huge.traits.workspace_bytes = 8;
  huge.traits.outputs = {prepared_result_output(
      "value", prepared_tensor_schema("manual.view.data", extent))};
  auto& huge_output = huge.traits.outputs[0];
  huge_output.continuation_bytes = sizeof(HugeView);
  huge_output.maximum_dependency_stages = 1;
  huge_output.preserve_output_views = true;
  huge_output.maximum_output_payload_bytes = 8;
  huge.start_result = [](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<HugeView>(allocator);
  };
  require(operations->register_operation(std::move(split)).ok() &&
              operations->register_operation(std::move(huge)).ok() &&
              operations->freeze().ok(),
          "register split-owner and huge Result producers");
  ps::ResourceBudget root;
  ps::ResultRef external_output;
  const void* external_token = nullptr;
  {
    ps::WorkflowDocument document;
    document.nodes = {
        {1, "manual.huge_view", {}, {}},
        {2, "manual.whole_view", {ps::WorkflowNodeOutput{1, "value"}}, {}}};
    document.outputs = {{"values", 2, "value"}};
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 65536;
    config.result_cache_bytes = 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(operations, config);
    root = take(context.resource_budget());
    auto run = [&]() {
      ps::GraphContext graph(document);
      auto plan = take(ps::Compiler(operations).compile(graph));
      return context.execute(plan.plan);
    };
    auto result = take(run());
    auto window = take(result.results.at("values").acquire_tensor(
        take(result.results.at("values").descriptor()), 0,
        ps::Region::whole({extent})));
    const auto first = take(window.row_run({0}));
    require(first.sample_stride_bytes == 0 && called == 1 &&
                root.statistics().live[ps::ResourceKind::Payload] == 8,
            "Whole Result keeps huge zero-stride backing without dense copy");
    check_prepared_sample(result.results.at("values"), extent - 1);
    window = {};
    result = {};
    retained.reset();
    document.nodes[0].operation = "manual.split_owner";
    auto failed = run();
    require(!failed.ok() &&
                failed.status().message.find("ViewUnavailable") !=
                    std::string::npos &&
                called == 1,
            "strict Whole Result rejects multi-owner before computation poll");
    document.nodes[1].operation = "manual.whole_auto";
    auto collected = take(run());
    require(
        called == 2 && root.statistics().live[ps::ResourceKind::Payload] == 64,
        "Auto input collection is charged separately from zero output cap");
    const auto facts = take(collected.results.at("values").descriptor());
    require(collected.results.at("values").association().size() == 1 &&
                collected.results.at("values").association()[0] ==
                    retained->object_id(),
            "Auto preserves original Result association");
    for (std::uint64_t at = 0; at < 4; ++at) {
      double value = -1;
      require(collected.results.at("values")
                      .read_tensor(facts, 0, {at}, &value, sizeof(value))
                      .ok() &&
                  value == at,
              "Auto Result collection preserves complete values");
    }
    double outside = 0;
    const auto unauthorized = retained->read({4}, &outside, sizeof(outside));
    require(!unauthorized.ok() &&
                unauthorized.reason == ps::FailureReason::UnauthorizedRead,
            "prepared backing does not broaden its Tensor Need grant");
    auto retained_copy = *retained;
    require(
        retained_copy.read({3}, &outside, sizeof(outside)).ok() && outside == 3,
        "copied capability shares immutable prepared backing");
    retained_copy = {};
    collected = {};
    retained.reset();
    {
      auto limited_config = config;
      limited_config.maximum_live_bytes = 80;
      limited_config.managed_resources->capacity[ps::ResourceKind::Payload] =
          80;
      ps::ExecutionContext limited(operations, limited_config);
      const auto limited_root = take(limited.resource_budget());
      ps::GraphContext limited_graph(document);
      auto limited_plan = take(ps::Compiler(operations).compile(limited_graph));
      const auto before = called;
      const auto completed_before = sources_completed;
      auto exhausted = limited.execute(limited_plan.plan);
      require(
          !exhausted.ok() &&
              exhausted.status().code == ps::ErrorCode::ResourceExhausted &&
              called == before && sources_completed == completed_before + 1 &&
              limited_root.statistics().peak[ps::ResourceKind::Payload] >= 32 &&
              limited_root.statistics().live[ps::ResourceKind::Payload] == 0,
          "Auto input collection capacity failure precedes computation and "
          "rolls back");
      ps::CancellationSource cancelled;
      cancelled.cancel();
      auto denied = limited.execute(limited_plan.plan, {}, cancelled.token());
      require(
          !denied.ok() && denied.status().code == ps::ErrorCode::Cancelled &&
              called == before &&
              limited_root.statistics().live[ps::ResourceKind::Payload] == 0,
          "pre-cancelled Whole Result view has no input payload or "
          "computation");
    }
    {
      auto cached_config = config;
      cached_config.result_cache_bytes = 32768;
      ps::ExecutionContext cached(operations, cached_config);
      ps::GraphContext cached_graph(document);
      auto cached_plan = take(ps::Compiler(operations).compile(cached_graph));
      {
        auto cold = take(cached.execute(cached_plan.plan));
        require(cold.results.count("values") == 1,
                "Whole Auto result retained for completed cache");
      }
      retained.reset();
      const auto before = called;
      auto warm = take(cached.execute(cached_plan.plan));
      require(called == before && warm.diagnostics.cache_hits != 0,
              "Whole Auto cached Result replays facts without computation");
      ps::ExecutionOptions uncached;
      uncached.maximum_dependency_cache_work = 0;
      auto recomputed =
          take(cached.execute(cached_plan.plan, {}, {}, uncached));
      require(called == before + 1 &&
                  recomputed.diagnostics.dependency_cache_work == 0,
              "zero optional cache work recomputes Whole Auto normally");
      retained.reset();
    }
    document.nodes[0].operation = "manual.shared_split";
    document.nodes[1].operation = "manual.whole_view";
    auto shared_result = take(run());
    require(root.statistics().live[ps::ResourceKind::Payload] == 32,
            "same-owner affine fragments need no collection");
    auto shared_window = take(shared_result.results.at("values").acquire_tensor(
        take(shared_result.results.at("values").descriptor()), 0,
        ps::Region::whole({4})));
    require(take(shared_window.row_run({0})).sample_stride_bytes == 8,
            "same-owner Result fragments infer canonical stride");
    shared_window = {};
    shared_result = {};
    retained.reset();
    auto storage = take(root.allocator().allocate(32));
    const double expected[] = {0, 1, 2, 3};
    std::memcpy(storage.data(), expected, 32);
    auto owner = std::move(storage).freeze();
    external_token = owner.get();
    const auto schema = prepared_tensor_schema("manual.view.data", 4);
    auto builder = take(ps::ResultBuilder::start(root, schema, "external"));
    require(builder
                .bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(root, 1, {})))
                .ok(),
            "external Result descriptor");
    require(
        builder
            .publish_tensor(0, ps::Region::whole({4}), {0, {8}}, owner,
                            take(ps::ResultRelation::cartesian(root, 4, {})),
                            {true, true, true, true})
            .ok(),
        "external Result backing");
    auto external = take(builder.seal());
    ps::WorkflowInputDeclaration input;
    input.id = 1;
    input.name = "external";
    input.result_schema = std::make_shared<const ps::SchemaTemplate>(schema);
    document.inputs = {input};
    document.nodes = {
        {1, "manual.whole_view", {ps::WorkflowInputReference{1}}, {}}};
    document.outputs = {{"values", 1, "value"}};
    ps::GraphContext graph(document);
    auto plan = take(ps::Compiler(operations).compile(graph));
    ps::ExecutionBinding binding;
    binding.name = "external";
    binding.result = external;
    auto external_result = take(context.execute(plan.plan, {{binding}}));
    external_output = external_result.results.at("values");
    auto borrowed = take(external_output.acquire_tensor(
        take(external_output.descriptor()), 0, ps::Region::whole({4})));
    require(borrowed.storage_owner_token() == external_token,
            "Whole Result retains original external owner");
    document.nodes[0].operation = "manual.broken_view";
    ps::GraphContext broken_graph(document);
    auto broken_plan = take(ps::Compiler(operations).compile(broken_graph));
    auto denied = context.execute(broken_plan.plan, {{binding}});
    require(!denied.ok() &&
                denied.status().code == ps::ErrorCode::ResourceExhausted,
            "ignored Result workspace allocation rejection stays sticky");
    retained.reset();
  }
  auto window = take(external_output.acquire_tensor(
      take(external_output.descriptor()), 0, ps::Region::whole({4})));
  require(window.storage_owner_token() == external_token,
          "Result view owner survives context retirement");
  window = {};
  external_output = {};
  require(root.statistics().live[ps::ResourceKind::Payload] == 0,
          "Whole Result view owners release all payload");
  for (const auto live : root.statistics().live.values)
    require(live == 0, "Whole Result view releases all Root resources");
  std::cout
      << "Whole Result view: huge zero-stride, strict multi-owner failure, "
         "Auto input collection at zero output cap and owner lifetime passed\n";
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
