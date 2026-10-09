#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "icc_fixture.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/color_array.hpp"
#include "photospider/data/icc_profile.hpp"
#include "photospider/data/resource_bindings.hpp"
#include "photospider/ops/numeric/unary.hpp"
#include "photospider/photospider.hpp"

namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
using numeric_fixture::fixture;
using numeric_fixture::key;
using numeric_fixture::word;
ps::ByteView view(const std::vector<std::uint8_t>& bytes) {
  return {bytes.data(), bytes.size()};
}
void ownership() {
  auto bytes = fixture();
  const auto original = bytes;
  ps::IccProfile escaped;
  {
    ps::ResourceBudget temporary;
    escaped = take(ps::IccProfile::import(view(bytes), temporary));
    require(
        temporary.statistics().live[ps::ResourceKind::Payload] == bytes.size(),
        "profile allocation charged");
  }
  bytes[100] = 9;
  require(escaped.bytes() == view(original),
          "profile frozen beyond caller/context lifetime");
  ps::ResourceBudget root;
  auto first = take(escaped.reference(root));
  auto second = take(escaped.reference(root));
  require(first.storage().get() == escaped.storage().get() &&
              second.storage().get() == escaped.storage().get(),
          "reference preserves actual allocation identity");
  require(
      root.statistics().live[ps::ResourceKind::Referenced] == original.size(),
      "same allocation references charged once");
  first = {};
  require(
      root.statistics().live[ps::ResourceKind::Referenced] == original.size(),
      "second reference keeps lease");
  second = {};
  require(root.statistics().live[ps::ResourceKind::Referenced] == 0 &&
              root.statistics().live[ps::ResourceKind::Host] == 0,
          "last resource owner releases bytes and metadata");
  auto invalid = ps::IccProfile::import(view(bytes), root);
  require(!invalid.ok() && root.statistics().live[ps::ResourceKind::Host] == 0,
          "malformed bytes clean up");
  ps::CancellationSource cancellation;
  cancellation.cancel();
  require(ps::IccProfile::import(view(original), root, cancellation.token())
                  .status()
                  .code == ps::ErrorCode::Cancelled,
          "profile pre-cancel");
  require(ps::IccProfile::import(view(original), root, {}, original.size() + 1)
                      .status()
                      .code == ps::ErrorCode::ResourceExhausted &&
              root.statistics().live[ps::ResourceKind::Host] == 0,
          "profile work failure cleans up copied bytes");
  ps::ResourceLimits limits;
  limits.capacity[ps::ResourceKind::Payload] = original.size() - 1;
  ps::ResourceBudget small(limits);
  require(ps::IccProfile::import(view(original), small).status().code ==
                  ps::ErrorCode::ResourceExhausted &&
              small.statistics().live[ps::ResourceKind::Host] == 0,
          "profile payload admission");
  // Every surrogate pair crosses a 1024-byte boundary. Two mluc records share
  // the string; desc/cprt share the complete tag. Each reference is parsed and
  // charged, so a scanner cannot bypass work by jumping over modulo
  // checkpoints.
  constexpr unsigned string_size = 4098;
  std::vector<std::uint8_t> localized(40 + string_size);
  key(&localized, 0, "mluc");
  word(&localized, 8, 2);
  word(&localized, 12, 12);
  for (unsigned record = 0; record < 2; ++record) {
    key(&localized, 16 + 12 * record, "enUS");
    word(&localized, 20 + 12 * record, string_size);
    word(&localized, 24 + 12 * record, 40);
  }
  for (unsigned at = 0; at < string_size; at += 2)
    word(&localized, 40 + at, 'x', 2);
  for (unsigned at = 1022; at + 4 <= string_size; at += 1024) {
    word(&localized, 40 + at, 0xd83d, 2);
    word(&localized, 42 + at, 0xde00, 2);
  }
  auto unicode_bytes = fixture(localized);
  ps::ResourceBudget unicode_root;
  auto unicode_profile =
      take(ps::IccProfile::import(view(unicode_bytes), unicode_root));
  require(
      unicode_root.statistics().issued.work >=
          2 * unicode_bytes.size() + 4 * string_size,
      "all shared Unicode code units consume work across surrogate boundaries");
  ps::ResourceBudget limited_unicode;
  auto exhausted =
      ps::IccProfile::import(view(unicode_bytes), limited_unicode, {},
                             2 * unicode_bytes.size() + string_size);
  require(!exhausted.ok() &&
              exhausted.status().code == ps::ErrorCode::ResourceExhausted &&
              limited_unicode.statistics().live[ps::ResourceKind::Host] == 0,
          "repeated Unicode references respect work cap and release capacity");
  ps::ResourceBudget bindings_root;
  auto independently_loaded =
      take(ps::IccProfile::import(view(original), root));
  auto bindings = take(ps::ResourceBindings::create(
      {escaped, independently_loaded, escaped}, bindings_root));
  require(bindings.size() == 1 &&
              take(bindings.icc_profile(escaped.identity())).bytes() ==
                  view(original),
          "binding duplicates compare actual bytes and retain one owner");
  require(bindings_root.statistics().live[ps::ResourceKind::Referenced] ==
              original.size(),
          "canonical binding charges one profile allocation");
  ps::ColorArrayDescriptor cmyk;
  cmyk.model = ps::ColorModel::Cmyk;
  cmyk.reference = ps::ColorReference::ProfileRelative;
  cmyk.white.reset();
  cmyk.profile = escaped.identity();
  auto facet = take(ps::encode_color_array(cmyk));
  auto subset = take(bindings.select({facet}));
  require(subset.size() == 1 &&
              take(subset.icc_profile(escaped.identity())).bytes() ==
                  view(original),
          "explicit ColorArray resource selection");
  auto missing_value = ps::Value::create(
      {ps::ElementType::Float64, {1, 4}}, ps::Region::whole({1, 4}),
      {0, {32, 8}}, std::vector<std::uint8_t>(32), {facet});
  require(!missing_value.ok(),
          "CMYK Value cannot publish digest without owner");
  auto owned_value = take(ps::Value::create(
      {ps::ElementType::Float64, {1, 4}}, ps::Region::whole({1, 4}),
      {0, {32, 8}}, std::vector<std::uint8_t>(32), {facet}, bindings));
  auto read_view = take(owned_value.view(ps::Region::whole({1, 4})));
  owned_value = {};
  require(take(read_view.resources().icc_profile(escaped.identity())).bytes() ==
              view(original),
          "CMYK read view retains queryable accepted bytes");
  auto untyped = take(ps::Value::from_storage(
      read_view.descriptor(), read_view.region(), read_view.layout(),
      read_view.storage(), {}, bindings));
  require(untyped.resources().size() == 0,
          "dropping interpretation releases unrelated resources");
  require(take(bindings.select({})).size() == 0 &&
              !ps::ResourceBindings{}.select({facet}).ok(),
          "empty selection drops owners; missing profile fails admission");
  cmyk.profile->sha256[0] ^= 1;
  require(!bindings.select({take(ps::encode_color_array(cmyk))}).ok(),
          "unknown identity rejected");
  bindings = {};
  subset = {};
  require(bindings_root.statistics().live[ps::ResourceKind::Referenced] ==
              original.size(),
          "last Value read view keeps ICC reference alive");
  read_view = {};
  require(
      bindings_root.statistics().live[ps::ResourceKind::Host] == 0 &&
          bindings_root.statistics().live[ps::ResourceKind::Referenced] == 0,
      "last binding releases set metadata and profile ancestry");
  std::cout << "ICC public import: frozen CMYK profile, lifetime, reference "
               "dedup, work/cancel/admission cleanup PASS\n";
}

void check(ps::Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
ps::SchemaTemplate color_schema(const ps::ValueFacet& facet,
                                unsigned colors = 1) {
  ps::SchemaTemplate schema;
  schema.id = "photospider.tensor";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ps::ElementType::Float64, {colors, 4}};
  tensor.facets = {facet};
  tensor.atomic_trailing_axes = 1;
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
ps::ResultRef source(const ps::ResourceBudget& root,
                     const ps::SchemaTemplate& schema,
                     const ps::Value& backing) {
  auto builder = take(ps::ResultBuilder::start(
      root, schema, "icc.input", {}, {}, 128, 128, backing.resources()));
  check(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {}))));
  auto storage = take(root.allocator().allocate(backing.bytes().size()));
  std::memcpy(storage.data(), backing.bytes().data(), backing.bytes().size());
  check(builder.publish_tensor(
      0, backing.region(), backing.layout(), std::move(storage).freeze(),
      take(ps::ResultRelation::cartesian(
          root, take(schema.tensors[0].sample_count()), {})),
      {true, true, true, true}));
  return take(builder.seal());
}
ps::Footprint coverage(const ps::ResultRef& result) {
  return take(result.descriptor()).tensor_coverage(0);
}
struct ProfileOutput {
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    const auto& schema = *phase.query.output.result_schema;
    const auto& tensor = schema.tensors[0];
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, schema, phase.query.semantic_key, {}, {},
        phase.query.tile_height, phase.query.tile_width,
        phase.query.resources));
    check(builder.bind_descriptor_relation(
        take(ps::ResultRelation::cartesian(phase.resources, 1, {}))));
    auto demand = phase.query.tensor_outputs
                      ? *phase.query.tensor_outputs
                      : take(ps::Footprint::all(tensor.sample_shape()));
    demand = take(tensor.close_samples(demand));
    auto relation = take(ps::ResultRelation::cartesian(
        phase.resources, take(tensor.sample_count()), {}));
    for (const auto& box : demand.boxes()) {
      std::vector<double> values(take(box.element_count()));
      check(builder.publish_tensor(
          0, box,
          {reinterpret_cast<const std::uint8_t*>(values.data()),
           values.size() * sizeof(double)},
          relation, {true, true, true, true}));
    }
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultPublication{take(builder.seal()), true});
  }
  ps::Result<ps::ResourceVector<ps::ResultJointOutcome>> poll(
      const ps::ResultJointPhase& phase) {
    ps::ResourceVector<ps::ResultJointOutcome> replies;
    for (const auto* member : phase.members)
      replies.push_back(
          {take(ps::result_atom_key(member->query)), poll(*member)});
    return ps::Result<ps::ResourceVector<ps::ResultJointOutcome>>(
        std::move(replies));
  }
};
ps::OperationDefinition profile_output(const ps::ValueFacet& facet,
                                       unsigned* starts) {
  ps::OperationDefinition operation;
  operation.key = "manual.profile_output";
  auto& traits = operation.traits;
  auto& output = traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = "photospider.tensor";
  output.output_schema.result_schema_version = 1;
  output.output_schema.tensor_key = "samples";
  output.result_schema = color_schema(facet);
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.continuation_bytes = sizeof(ProfileOutput);
  output.maximum_dependency_stages = 2;
  output.failure_delivery = ps::FailureDelivery::PerAtomOutcome;
  traits.joint_contract = 2;
  traits.joint_continuation_bytes = sizeof(ProfileOutput);
  operation.start_result = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ps::ResultContinuation::make<ProfileOutput>(allocator);
  };
  operation.start_result_joint = [starts](const auto&, const auto& allocator) {
    ++*starts;
    return ps::ResultJointContinuation::make<ProfileOutput>(allocator);
  };
  return operation;
}
ps::Result<ps::ResultRef> direct_result(const ps::OperationRegistry& registry,
                                        const ps::ResourceBudget& root,
                                        const ps::SchemaTemplate& schema,
                                        const ps::ResourceBindings& resources,
                                        const ps::Footprint& samples,
                                        bool late_cancel = false) {
  ps::ResourceAllocationScope scope(root);
  ps::ResultProgramMetadata metadata;
  metadata.output.result_schema =
      std::make_shared<const ps::SchemaTemplate>(schema);
  const std::map<std::string, ps::ParameterValue> parameters;
  ps::ResultProgramQuery query(metadata, parameters);
  query.tensor_outputs = samples;
  query.semantic_key = "icc.direct";
  query.resources = resources;
  ps::CancellationSource cancelled;
  query.cancellation = cancelled.token();
  auto state = take(
      registry.start_result("manual.profile_output", query, root.allocator()));
  query.resources = {};
  if (late_cancel)
    cancelled.cancel();

  ps::ResultObjectInputs no_results;
  ps::ResourceVector<ps::ResultIoReply> no_io;
  auto allocator = root.allocator();
  ps::ResultProgramPhase phase{
      query,
      no_results,
      no_io,
      allocator,
      root,
      [root](auto work) { return root.consume({work}); },
      std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok)};
  auto result = state.poll(phase);
  return result.ok()
             ? ps::Result<ps::ResultRef>(
                   std::get<ps::ResultPublication>(result.value()).result)
             : ps::Result<ps::ResultRef>(result.status());
}
void propagation() {
  auto bytes = fixture();
  ps::ResourceBudget root;
  auto profile = take(ps::IccProfile::import(view(bytes), root));
  auto bindings = take(ps::ResourceBindings::create({profile}, root));
  ps::ColorArrayDescriptor description;
  description.model = ps::ColorModel::Cmyk;
  description.reference = ps::ColorReference::ProfileRelative;
  description.white.reset();
  description.profile = profile.identity();
  const auto facet = take(ps::encode_color_array(description));
  const ps::ValueDescriptor descriptor{ps::ElementType::Float64, {2, 4}};
  auto second_profile = take(ps::IccProfile::import(view(bytes), root));
  auto second_bindings =
      take(ps::ResourceBindings::create({second_profile}, root));
  auto a = take(ps::Value::create(descriptor, ps::Region({{0, 1}, {0, 4}}),
                                  {0, {32, 8}}, std::vector<std::uint8_t>(32),
                                  {facet}, bindings));
  auto b = take(ps::Value::create(
      descriptor, ps::Region({{1, 1}, {0, 4}}), {0, {32, 8}, {1, 0}},
      std::vector<std::uint8_t>(32), {facet}, second_bindings));
  auto fragments = take(ps::ValueFragments::create(
      descriptor, {facet}, take(ps::Footprint::all({2, 4})), {a, b}));
  require(fragments.fragments().size() == 2 &&
              take(fragments.retained_bytes()) ==
                  a.storage()->capacity() + b.storage()->capacity() +
                      profile.storage()->capacity(),
          "fragment profile ancestry matches actual retained capacity");
  for (const auto& fragment : fragments.fragments())
    require(take(fragment.resources().icc_profile(profile.identity()))
                    .storage()
                    .get() == profile.storage().get(),
            "equal-content fragments share canonical profile allocation");
  auto dense =
      take(fragments.collect(ps::Region::whole({2, 4}), ps::BufferAllocator{}));
  auto packed = take(ps::ValueFragments::create(
      descriptor, {facet}, take(ps::Footprint::all({2, 4})), {dense}));
  auto recollected = take(packed.collect(dense.region(), root.allocator()));
  require(recollected.facets().size() == 1 &&
              recollected.facets()[0].payload == facet.payload &&
              recollected.storage() != dense.storage() &&
              take(recollected.resources().icc_profile(profile.identity()))
                      .storage() == profile.storage(),
          "packed collect copies payload and retains typed ICC owner");
  auto registry = ps::make_default_operation_registry();
  const auto input_schema = color_schema(facet, 2);
  ps::WorkflowDocument document;
  ps::WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "inks";
  input.result_schema =
      std::make_shared<const ps::SchemaTemplate>(input_schema);
  document.inputs = {input};
  document.nodes = {{1, "core.identity", {ps::WorkflowInputReference{1}}, {}}};
  document.outputs = {{"inks", 1, "value"}};
  ps::GraphContext graph(document);
  ps::Compiler compiler(registry);
  require(!compiler.compile(graph).ok(),
          "compiler rejects unresolved static input profile");
  auto compiled = take(compiler.compile(graph, {}, bindings));
  require(compiled.semantic.resources().size() == 1 &&
              compiled.optimized.resources().size() == 1 &&
              compiled.plan.resources().size() == 1,
          "compiler stages retain accepted profiles");
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 64;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(registry, config);
  auto execution_root = take(context.resource_budget());
  auto original = source(execution_root, input_schema, dense);
  ps::ExecutionBindings input_bindings{{{"inks", original}}};
  auto executed = take(context.execute(compiled.plan, input_bindings));
  require(take(executed.results.at("inks").resources().icc_profile(
                   profile.identity()))
                  .bytes() == view(bytes),
          "Result identity output retains profile");
  auto frozen = take(context.freeze(compiled.plan, input_bindings));
  std::vector<std::uint8_t> changed(64);
  const double replacement = .25;
  std::memcpy(changed.data() + 7 * sizeof(double), &replacement,
              sizeof(replacement));
  auto patched_backing = take(
      ps::Value::create(descriptor, ps::Region::whole({2, 4}), {0, {32, 8}},
                        std::move(changed), {facet}, bindings));
  auto patched = source(execution_root, input_schema, patched_backing);
  auto handle = take(context.open_demand(compiled.plan, input_bindings));
  check(handle.replace_bindings({{{"inks", patched}}}).status());
  auto from_original = take(context.execute(frozen));
  auto from_replacement = take(context.execute(take(handle.freeze())));
  double before = 1, after = 0;
  check(from_original.results.at("inks").read_tensor(
      take(from_original.results.at("inks").descriptor()), 0, {1, 3}, &before,
      sizeof(before)));
  check(from_replacement.results.at("inks").read_tensor(
      take(from_replacement.results.at("inks").descriptor()), 0, {1, 3}, &after,
      sizeof(after)));
  require(before == 0 && after == .25 &&
              from_replacement.results.at("inks").resources().size() == 1,
          "immutable Result binding replacement retains ICC and old capture");
  const ps::Region input_partial({{0, 1}, {2, 1}});
  const auto input_full =
      take(ps::Footprint::from_regions({2, 4}, {ps::Region({{0, 1}, {0, 4}})}));
  ps::PlanningOptions input_planning;
  input_planning.output_regions = {{"inks", input_partial}};
  auto input_roi = take(compiler.compile(graph, input_planning, bindings));
  require(
      take(ps::Footprint::from_regions(
          {2, 4}, {input_roi.plan.output_regions().at("inks")})) == input_full,
      "Result compiler ROI expands ColorArray channels");
  require(coverage(take(context.execute_fragments(
                            frozen, {{"inks", take(ps::Footprint::from_regions(
                                                  {2, 4}, {input_partial}))}}))
                       .results.at("inks")) == input_full,
          "Result identity sparse output closes complete color");
  document.nodes = {
      take(ps::numeric::abs_node(1, ps::WorkflowInputReference{1}))};
  document.outputs = {{"absolute", 1, "values"}};
  ps::GraphContext numeric_graph(document);
  auto numeric_plan = take(compiler.compile(numeric_graph, {}, bindings));
  auto numeric = take(context.execute(numeric_plan.plan, input_bindings));
  require(numeric.results.at("absolute").resources().size() == 0,
          "generic numeric Result drops unreferenced ICC owner");

  auto custom = std::make_shared<ps::OperationRegistry>();
  unsigned starts = 0;
  check(custom->register_operation(profile_output(facet, &starts)));
  check(custom->freeze());
  ps::WorkflowDocument generated;
  generated.nodes = {{1, "manual.profile_output", {}, {}}};
  generated.outputs = {{"inks", 1, "value"}};
  ps::GraphContext generated_graph(generated);
  require(!ps::Compiler(custom).compile(generated_graph).ok(),
          "compiler rejects unresolved inferred output profile");
  auto generated_plan =
      take(ps::Compiler(custom).compile(generated_graph, {}, bindings));
  const auto schema = color_schema(facet);
  const auto empty_samples = take(ps::Footprint::none({1, 4}));
  const ps::Region partial_region({{0, 1}, {2, 1}});
  const auto partial =
      take(ps::Footprint::from_regions({1, 4}, {partial_region}));
  const auto full_color = take(ps::Footprint::all({1, 4}));
  ps::ResultProgramMetadata metadata;
  metadata.output.result_schema =
      std::make_shared<const ps::SchemaTemplate>(schema);
  const std::map<std::string, ps::ParameterValue> parameters;
  ps::ResultProgramQuery query(metadata, parameters);
  query.tensor_outputs = empty_samples;
  query.semantic_key = "icc.empty";
  const auto old_starts = starts;
  require(
      !custom->start_result("manual.profile_output", query, root.allocator())
              .ok() &&
          starts == old_starts,
      "Empty direct start resolves profile before callbacks");
  auto direct_empty =
      take(direct_result(*custom, root, schema, bindings, empty_samples));
  require(direct_empty.resources().size() == 1 &&
              coverage(direct_empty).empty() && starts == old_starts,
          "Empty direct Result owns profile without operator callback");
  auto direct = take(direct_result(*custom, root, schema, bindings, partial));
  require(coverage(direct) == full_color && direct.resources().size() == 1,
          "direct Result publication closes a complete color and owns ICC");
  ps::ResourceBudget direct_root;
  auto retained_direct =
      take(direct_result(*custom, direct_root, schema, bindings, full_color));
  require(direct_root.statistics().live[ps::ResourceKind::Referenced] ==
              bytes.size(),
          "direct Result retains active Root ICC admission");
  retained_direct = {};
  require(direct_root.statistics().live[ps::ResourceKind::Referenced] == 0 &&
              direct_root.statistics().live[ps::ResourceKind::Host] == 0,
          "last direct Result releases profile and metadata capacity");
  ps::ResourceBudget cancelled_root;
  const auto before_cancel = starts;
  auto cancelled = direct_result(*custom, cancelled_root, schema, bindings,
                                 empty_samples, true);
  require(!cancelled.ok() &&
              cancelled.status().code == ps::ErrorCode::Cancelled &&
              starts == before_cancel,
          "late cancellation skips Empty C2 publication and callbacks");
  for (auto live : cancelled_root.statistics().live.values)
    require(live == 0, "cancelled direct Empty releases all Root resources");
  ps::ResourceLimits direct_limits;
  direct_limits.capacity[ps::ResourceKind::Referenced] = bytes.size() - 1;
  ps::ResourceBudget direct_small(direct_limits);
  {
    ps::ResourceAllocationScope scope(direct_small);
    query.resources = bindings;
    const auto prior = starts;
    auto denied = custom->start_result("manual.profile_output", query,
                                       direct_small.allocator());
    require(!denied.ok() &&
                denied.status().code == ps::ErrorCode::ResourceExhausted &&
                starts == prior,
            "Empty direct profile admission precedes callbacks");
    query.tensor_outputs = partial;
    denied = custom->start_result("manual.profile_output", query,
                                  direct_small.allocator());
    require(!denied.ok() &&
                denied.status().code == ps::ErrorCode::ResourceExhausted &&
                starts == prior,
            "nonempty direct profile admission precedes callbacks");
  }
  ps::ExecutionContext generated_context(custom, config);
  auto generated_frozen =
      take(generated_context.freeze(generated_plan.plan, {}));
  auto output = take(generated_context.execute(generated_frozen));
  require(output.results.at("inks").resources().size() == 1,
          "compiled Result output retains admitted profile");
  require(generated_context.cache_statistics().retained_bytes == 0 &&
              generated_context.cache_statistics().entries == 0,
          "sample-only optional cache skips profile-bearing Results");
  auto sparse = take(generated_context.execute_fragments(generated_frozen,
                                                         {{"inks", partial}}));
  require(coverage(sparse.results.at("inks")) == full_color,
          "named sparse Result request returns a full color");
  auto atoms = take(generated_context.execute_atoms(generated_plan.plan, {},
                                                    {{"inks", partial}}));
  require(atoms.atoms.size() == 1 && atoms.atoms[0].outcome.ok() &&
              coverage(atoms.atoms[0].outcome.value()) == full_color,
          "partial Atom request is one complete Result color");
  auto generated_handle =
      take(generated_context.open_demand(generated_plan.plan, {}));
  require(coverage(take(generated_handle.request({{"inks", partial}}))
                       .results.at("inks")) == full_color,
          "Result demand handle closes a partial color");
  check(generated_handle.release({{"inks", partial}}));
  ps::PlanningOptions planning;
  planning.output_regions = {{"inks", partial_region}};
  auto narrow_plan =
      take(ps::Compiler(custom).compile(generated_graph, planning, bindings));
  require(take(ps::Footprint::from_regions(
              {1, 4}, {narrow_plan.plan.output_regions().at("inks")})) ==
                  full_color &&
              take(ps::Footprint::from_regions(
                  {1, 4},
                  {take(generated_plan.plan.tile_plan("inks", partial_region))
                       .output_regions()
                       .at("inks")})) == full_color,
          "Result compiler ROI and tile normalize color channels");
  const auto before_empty = starts;
  auto empty = take(generated_context.execute_fragments(
      generated_frozen, {{"inks", empty_samples}}));
  require(empty.results.at("inks").resources().size() == 1 &&
              coverage(empty.results.at("inks")).empty() &&
              starts == before_empty,
          "Empty C2 root Result retains profile without operator callback");
  config.managed_resources->capacity[ps::ResourceKind::Referenced] =
      bytes.size() - 1;
  ps::ExecutionContext limited(custom, config);
  const auto before_limited = starts;
  for (const auto& demand : {empty_samples, full_color}) {
    auto failed =
        limited.execute_fragments(generated_frozen, {{"inks", demand}});
    require(!failed.ok() &&
                failed.status().code == ps::ErrorCode::ResourceExhausted &&
                starts == before_limited,
            "compiled ICC admission precedes Empty and nonempty callbacks");
  }
  std::cout << "ICC Result propagation: typed backing, immutable bindings, "
               "compiler/direct/C2 ownership, partial-color closure, Empty "
               "and pre-callback capacity PASS\n";
}

}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--inspect") {
      std::ifstream stream(argv[2], std::ios::binary | std::ios::ate);
      require(stream.good(), "open explicit profile file");
      const auto length = stream.tellg();
      require(length >= 0 && length <= 64 * 1024 * 1024,
              "manual file loader bounds");
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
      stream.seekg(0);
      stream.read(reinterpret_cast<char*>(bytes.data()), length);
      require(stream.good(), "read complete profile bytes");
      ps::ResourceBudget root;
      auto imported = ps::IccProfile::import(view(bytes), root);
      if (!imported.ok()) {
        std::cout << "ERR " << static_cast<int>(imported.status().code) << ' '
                  << imported.status().message << '\n';
      } else {
        std::cout << "OK " << imported.value().identity().byte_length << ' ';
        for (auto byte : imported.value().identity().sha256)
          std::cout << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<unsigned>(byte);
        std::cout << '\n';
      }
      return 0;
    }
    ownership();
    propagation();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
