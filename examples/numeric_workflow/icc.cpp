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
#include "photospider/numeric/unary.hpp"
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

struct ProfileOutput {
  ps::Result<ps::DependencyPoll> poll(const ps::DependencyPhase& phase) {
    const auto& query = phase.query;
    auto allocated = ps::MutableValue::allocate(query.output.descriptor,
                                                query.outputs.boxes().front(),
                                                phase.allocator);
    if (!allocated.ok())
      return ps::Result<ps::DependencyPoll>(allocated.status());
    auto writer = allocated.take_value();
    std::memset(writer.data(), 0, writer.size());
    auto value =
        std::move(writer).publish(query.output.facets, query.resources);
    if (!value.ok())
      return ps::Result<ps::DependencyPoll>(value.status());
    auto result = ps::ValueFragments::create(
        query.output.descriptor, query.output.facets, query.outputs,
        {value.take_value()}, phase.sets, query.resources);
    return result.ok() ? ps::Result<ps::DependencyPoll>(result.take_value())
                       : ps::Result<ps::DependencyPoll>(result.status());
  }
};
struct StructuredProfileOutput {
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    auto writer = take(ps::MutableValue::allocate(
        phase.query.output.descriptor,
        phase.query.value_outputs->boxes().front(), phase.allocator));
    std::memset(writer.data(), 0, writer.size());
    auto value = take(std::move(writer).publish(phase.query.output.facets,
                                                phase.query.resources));
    auto fragments = take(ps::ValueFragments::create(
        phase.query.output.descriptor, phase.query.output.facets,
        *phase.query.value_outputs, {value}, {}, phase.query.resources));
    auto relation =
        take(ps::ResultRelation::cartesian(phase.resources, 4, {0, 1, 0, 0}));
    return ps::Result<ps::ResultProgramPoll>(
        ps::ResultValuePublication{std::move(fragments), std::move(relation)});
  }
};
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
  ps::InputSnapshotStore store;
  auto snapshot = take(store.import_value(dense));
  auto patched = take(store.patch(snapshot, b));
  require(take(patched.resources().icc_profile(profile.identity())).bytes() ==
              view(bytes),
          "snapshot patch retains frozen profile bytes");
  auto registry = ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  document.inputs = {
      {1, "inks", descriptor, dense.region(), dense.layout(), {facet}}};
  document.nodes = {{1, "core.identity", {ps::WorkflowInputReference{1}}, {}}};
  document.outputs = {{"inks", 1, "value"}};
  ps::GraphContext graph(document);
  ps::Compiler compiler(registry);
  require(!compiler.compile(graph).ok(),
          "compiler rejects unresolved static input identity");
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
  auto executed = take(context.execute(compiled.plan, {{{"inks", dense}}}));
  require(take(executed.values.at("inks").resources().icc_profile(
                   profile.identity()))
                  .bytes() == view(bytes),
          "legacy callback execution retains profile");
  auto frozen = take(context.freeze(
      compiled.plan, {{{"inks",
                        {},
                        {},
                        std::make_shared<const ps::InputSnapshot>(patched)}}}));
  auto from_snapshot = take(context.execute(frozen));
  require(from_snapshot.values.at("inks").resources().size() == 1,
          "snapshot regional source publication retains profile");
  const ps::Region legacy_partial({{0, 1}, {2, 1}});
  const auto legacy_full =
      take(ps::Footprint::from_regions({2, 4}, {ps::Region({{0, 1}, {0, 4}})}));
  ps::PlanningOptions legacy_options;
  legacy_options.output_regions = {{"inks", legacy_partial}};
  auto legacy_roi = take(compiler.compile(graph, legacy_options, bindings));
  require(take(ps::Footprint::from_regions(
              {2, 4}, {legacy_roi.plan.output_regions().at("inks")})) ==
              legacy_full,
          "ordinary compiler output ROI expands ColorArray channels");
  const std::vector<ps::Value> legacy_values{dense};
  const std::vector<ps::Region> legacy_demands{dense.region()};
  const std::map<std::string, ps::ParameterValue> legacy_parameters;
  ps::OperationInvocation legacy_call(legacy_values, legacy_demands,
                                      legacy_parameters);
  legacy_call.output_region = legacy_partial;
  require(registry->invoke("core.identity", legacy_call).ok(),
          "ordinary direct callback accepts partial color demand");
  const auto image_facet = take(ps::encode_semantic(ps::rgba_semantics()));
  require(!ps::operation_observations(
               {{ps::ElementType::Float32, {1, 1, 4}}, {image_facet}},
               take(ps::Footprint::from_regions(
                   {1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {2, 1}})})))
               .ok(),
          "legacy Image observation still rejects partial channels");
  document.nodes = {
      take(ps::numeric::abs_node(1, ps::WorkflowInputReference{1}))};
  document.outputs = {{"absolute", 1, "values"}};
  ps::GraphContext numeric_graph(document);
  auto numeric_plan = take(compiler.compile(numeric_graph, {}, bindings));
  frozen = take(context.freeze(numeric_plan.plan, {{{"inks", dense}}}));
  auto numeric = take(context.execute_fragments(
      frozen, {{"absolute", take(ps::Footprint::all({2, 4}))}}));
  require(numeric.values.at("absolute").resources().size() == 0,
          "generic numeric output drops consumed ICC owner");

  auto custom = std::make_shared<ps::OperationRegistry>();
  ps::OperationDefinition operation;
  operation.key = "manual.profile_output";
  operation.traits.input_count = 0;
  operation.traits.deterministic = true;
  operation.traits.side_effect_free = true;
  operation.traits.cacheable = true;
  operation.traits.input_schema.clear();
  auto& output = operation.traits.outputs[0];
  output.shape_rule = ps::OperationShapeRule::Fixed;
  output.fixed_output_shape = {1, 4};
  output.output_element_type = ps::ElementType::Float64;
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.dependency_version = 1;
  output.continuation_bytes = sizeof(ProfileOutput);
  output.maximum_dependency_stages = 2;
  output.atomic_trailing_axes = 1;
  output.failure_delivery = ps::FailureDelivery::PerAtomOutcome;
  output.output_semantic_rule = ps::OperationSemanticRule::Establish;
  output.output_facets = {facet};
  unsigned starts = 0;
  operation.start_dependency = [&starts](const auto&, const auto& allocator) {
    ++starts;
    return ps::DependencyContinuation::make<ProfileOutput>(allocator);
  };
  auto structured_operation = operation;
  structured_operation.key = "manual.structured_profile_output";
  structured_operation.traits.outputs[0].dependency_version = 2;
  structured_operation.traits.outputs[0].atomic_trailing_axes = 0;
  structured_operation.traits.outputs[0].failure_delivery =
      ps::FailureDelivery::RequestFailureOnly;
  structured_operation.traits.outputs[0].continuation_bytes =
      sizeof(StructuredProfileOutput);
  structured_operation.start_dependency = {};
  structured_operation.start_result = [](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<StructuredProfileOutput>(allocator);
  };
  require(
      custom->register_operation(std::move(operation)).ok() &&
          custom->register_operation(std::move(structured_operation)).ok() &&
          custom->freeze().ok(),
      "register profile output probes");
  ps::WorkflowDocument generated;
  generated.nodes = {{1, "manual.profile_output", {}, {}}};
  generated.outputs = {{"inks", 1, "value"}};
  ps::GraphContext generated_graph(generated);
  require(!ps::Compiler(custom).compile(generated_graph).ok(),
          "compiler rejects unresolved inferred output identity");
  auto generated_plan =
      take(ps::Compiler(custom).compile(generated_graph, {}, bindings));
  ps::DependencyRequest request;
  request.outputs = take(ps::Footprint::none({1, 4}));
  request.snapshot_identity = "profile-empty";
  require(!custom->start_dependency("manual.profile_output", request).ok() &&
              starts == 0,
          "Empty start resolves profile before callbacks");
  request.resources = bindings;
  auto session =
      take(custom->start_dependency("manual.profile_output", request));
  auto progress = take(session->poll());
  require(
      std::get<ps::DependencyResult>(progress).value.resources().size() == 1,
      "Empty dependency result retains queryable ICC owner");
  // Borrowed invocation vectors must have a live owner.
  const std::vector<ps::Value> no_inputs;
  const std::vector<ps::Region> no_demands;
  const std::map<std::string, ps::ParameterValue> no_parameters;
  ps::OperationInvocation direct(no_inputs, no_demands, no_parameters);
  direct.resources = bindings;
  auto direct_value = take(custom->invoke("manual.profile_output", direct));
  require(direct_value.resources().size() == 1,
          "direct dependency invocation publishes explicit output resources");
  const ps::Region partial_region({{0, 1}, {2, 1}});
  const auto partial =
      take(ps::Footprint::from_regions({1, 4}, {partial_region}));
  const auto full_color = take(ps::Footprint::all({1, 4}));
  direct.output_region = partial_region;
  auto partial_direct = take(custom->invoke("manual.profile_output", direct));
  require(take(ps::Footprint::from_regions(
              {1, 4}, {partial_direct.region()})) == full_color,
          "direct collector expands a channel to full color");
  request.outputs = partial;
  auto partial_session =
      take(custom->start_dependency("manual.profile_output", request));
  auto partial_progress = take(partial_session->poll());
  const auto& partial_result = std::get<ps::DependencyResult>(partial_progress);
  require(partial_result.value.coverage() == full_color &&
              partial_result.original_outputs == full_color,
          "direct session uses complete color observation");
  ps::ResourceBudget direct_root;
  ps::Value retained_direct;
  {
    ps::ResourceAllocationScope scope(direct_root);
    direct.allocator = direct_root.allocator();
    retained_direct = take(custom->invoke("manual.profile_output", direct));
  }
  require(direct_root.statistics().live[ps::ResourceKind::Referenced] ==
              bytes.size(),
          "direct collector retains active root ICC admission");
  retained_direct = {};
  require(direct_root.statistics().live[ps::ResourceKind::Referenced] == 0,
          "direct collector last release returns ICC reference capacity");
  ps::ResourceLimits direct_limits;
  direct_limits.capacity[ps::ResourceKind::Referenced] = bytes.size() - 1;
  ps::ResourceBudget direct_small(direct_limits);
  {
    ps::ResourceAllocationScope scope(direct_small);
    direct.allocator = direct_small.allocator();
    const auto old_starts = starts;
    auto denied = custom->invoke("manual.profile_output", direct);
    require(!denied.ok() &&
                denied.status().code == ps::ErrorCode::ResourceExhausted &&
                starts == old_starts,
            "direct profile admission precedes callback");
    const std::vector<ps::Value> values{dense};
    const std::vector<ps::Region> demands{dense.region()};
    ps::OperationInvocation legacy(values, demands, no_parameters);
    legacy.allocator = direct_small.allocator();
    auto legacy_denied = registry->invoke("core.identity", legacy);
    require(!legacy_denied.ok() &&
                legacy_denied.status().code == ps::ErrorCode::ResourceExhausted,
            "ordinary direct callback honors active ICC reference budget");
  }
  ps::ExecutionContext generated_context(custom, config);
  auto generated_frozen =
      take(generated_context.freeze(generated_plan.plan, {}));
  auto output_value = take(generated_context.execute(generated_frozen));
  require(output_value.values.at("inks").resources().size() == 1,
          "compiled dependency output retains admitted resources");
  require(generated_context.cache_statistics().retained_bytes == 0 &&
              generated_context.cache_statistics().entries == 0,
          "sample-only optional cache does not retain unaccounted ICC owner");
  auto partial_result_run = take(generated_context.execute_fragments(
      generated_frozen, {{"inks", partial}}));
  require(partial_result_run.values.at("inks").coverage() == full_color,
          "named sparse request returns full color");
  auto atoms = take(generated_context.execute_atoms(generated_plan.plan, {},
                                                    {{"inks", partial}}));
  require(atoms.atoms.size() == 1 && atoms.atoms[0].outcome.ok() &&
              atoms.atoms[0].outcome.value().coverage() == full_color,
          "per-Atom partial request is one complete color");
  auto handle = take(generated_context.open_demand(generated_plan.plan, {}));
  require(
      take(handle.request({{"inks", partial}})).values.at("inks").coverage() ==
              full_color &&
          handle.release({{"inks", partial}}).ok(),
      "demand handle canonical request/release uses same full color");
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
          "compiler ROI and tile normalize color channels");
  auto empty = take(generated_context.execute_fragments(
      generated_frozen, {{"inks", take(ps::Footprint::none({1, 4}))}}));
  require(empty.values.at("inks").resources().size() == 1,
          "Empty compiled demand retains static output resources");
  config.managed_resources->capacity[ps::ResourceKind::Referenced] =
      bytes.size() - 1;
  ps::ExecutionContext limited(custom, config);
  const auto before = starts;
  auto failed = limited.execute_fragments(
      generated_frozen, {{"inks", take(ps::Footprint::none({1, 4}))}});
  require(!failed.ok() &&
              failed.status().code == ps::ErrorCode::ResourceExhausted &&
              starts == before,
          "Empty run admits ICC capacity before callbacks");
  generated.nodes[0].operation = "manual.structured_profile_output";
  ps::GraphContext structured_graph(generated);
  auto structured_plan =
      take(ps::Compiler(custom).compile(structured_graph, {}, bindings));
  auto structured_frozen =
      take(generated_context.freeze(structured_plan.plan, {}));
  auto structured_output = take(generated_context.execute(structured_frozen));
  require(structured_output.values.at("inks").resources().size() == 1,
          "structured query gives zero-input producer accepted ICC owner");
  auto structured_partial = take(generated_context.execute_fragments(
      structured_frozen, {{"inks", partial}}));
  require(structured_partial.values.at("inks").coverage() == full_color,
          "structured root closes partial color output");
  ps::WorkflowDocument bridge_document;
  bridge_document.nodes = {{1, "manual.profile_output", {}, {}},
                           {2, "manual.structured_profile_output", {}, {}}};
  bridge_document.outputs = {{"inks", 1, "value"}, {"structured", 2, "value"}};
  ps::GraphContext bridge_graph(bridge_document);
  auto bridge_plan =
      take(ps::Compiler(custom).compile(bridge_graph, {}, bindings));
  auto bridge_frozen = take(generated_context.freeze(bridge_plan.plan, {}));
  require(take(generated_context.execute_fragments(bridge_frozen,
                                                   {{"inks", partial}}))
                  .values.at("inks")
                  .coverage() == full_color,
          "structured v1 bridge returns closed complete-color output");
  ps::ResultProgramMetadata metadata;
  metadata.output = {ps::ValueDescriptor{ps::ElementType::Float64, {1, 4}},
                     {facet},
                     {},
                     1};
  ps::ResultProgramQuery structured_query(metadata, no_parameters);
  structured_query.value_outputs = partial;
  structured_query.semantic_key = "icc-structured-direct";
  require(!custom
               ->start_result("manual.structured_profile_output",
                              structured_query, root.allocator())
               .ok(),
          "direct structured start rejects unresolved output profile");
  structured_query.resources = bindings;
  auto structured_state = take(custom->start_result(
      "manual.structured_profile_output", structured_query, root.allocator()));
  structured_query.resources = {};
  ps::ResultValueInputs no_values;
  ps::ResultObjectInputs no_results;
  ps::ResourceVector<ps::ResultIoReply> no_io;
  auto allocator = root.allocator();
  ps::ResultProgramPhase phase{
      structured_query,
      no_values,
      no_results,
      no_io,
      allocator,
      root,
      [](auto) { return ps::Status::success(); },
      std::make_shared<std::atomic<ps::ErrorCode>>(ps::ErrorCode::Ok)};
  auto structured_polled = take(structured_state.poll(phase));
  require(
      std::get<ps::ResultValuePublication>(structured_polled)
              .value.resources()
              .size() == 1,
      "structured continuation owns resources independently of query handle");
  std::cout
      << "ICC propagation: fragments, snapshots, compiler, callback, "
         "dependency/v2, partial-color closure, Empty and capacity PASS\n";
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
