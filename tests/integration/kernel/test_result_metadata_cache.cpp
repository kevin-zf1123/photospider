#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::take;
namespace cache_fixture {
SchemaTemplate source_schema(const TensorSpan& value) {
  auto schema = multi_result::schema(value.descriptor().element_type,
                                     value.descriptor().shape);
  schema.tensors[0].key = "data";
  return schema;
}
ExecutionBindings bind_sources(const ResourceBudget& root,
                               const std::vector<TensorSpan>& values) {
  ExecutionBindings bindings;
  for (std::size_t i = 0; i < values.size(); ++i) {
    auto buffer = take(root.allocator().allocate(values[i].bytes().size()));
    std::memcpy(buffer.data(), values[i].bytes().data(),
                values[i].bytes().size());
    auto builder = take(
        ResultBuilder::start(root, source_schema(values[i]), "cache.source"));
    multi_result::check(builder.publish_tensor(
        0, values[i].region(), values[i].layout(), std::move(buffer).freeze()));
    bindings.inputs.push_back(
        {"input" + std::to_string(i), take(builder.seal())});
  }
  return bindings;
}
std::vector<std::uint8_t> bytes(const ResultRef& result) {
  std::vector<std::uint8_t> bytes(sizeof(double));
  multi_result::check(result.read_tensor(take(result.descriptor()), 0, {0},
                                         bytes.data(), bytes.size()));
  return bytes;
}
}  // namespace cache_fixture

template <class T>
TensorSpan array(const std::vector<T>& samples,
                 std::vector<std::uint64_t> shape = {}) {
  if (shape.empty())
    shape = {samples.size()};
  const auto type =
      std::is_same_v<T, double> ? ElementType::Float64 : ElementType::Float32;
  auto value = take(TensorWriter::allocate({type, shape}, Region::whole(shape),
                                           BufferAllocator{}));
  std::memcpy(value.data(), samples.data(), samples.size() * sizeof(T));
  return take(std::move(value).publish());
}
WorkflowDocument document(const std::vector<TensorSpan>& inputs,
                          const std::vector<WorkflowNode>& nodes) {
  WorkflowDocument result;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    result.inputs.push_back(
        multi_result::declaration(i + 1, "input" + std::to_string(i),
                                  cache_fixture::source_schema(inputs[i])));
  result.nodes = nodes;
  result.outputs = {{"result", nodes.back().id, "value"}};
  return result;
}
struct MetadataIdentityProgram {
  bool requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    using multi_result::check;
    using multi_result::take;
    using Poll = Result<ResultProgramPoll>;
    auto scratch =
        take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    const auto& input = phase.query.inputs[0].result_schema->tensors[0];
    const auto shape = input.sample_shape();
    const bool empty =
        phase.query.tensor_outputs && phase.query.tensor_outputs->empty();
    if (!empty && !requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, 0, false,
           take(TileSet::range(
               phase.resources, 0,
               phase.query.input_tile_grids[0][0].tile_count()))});
      return Poll(std::move(need));
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    if (!empty) {
      check(phase.consume_work(shape.size() + 1));
      double number = std::get<double>(phase.query.parameters.at("offset")) +
                      static_cast<double>(shape.size() * 100 + shape[0] +
                                          static_cast<std::uint32_t>(
                                              input.descriptor.element_type));
      if (input.descriptor.element_type == ElementType::Float64) {
        double first = 0;
        check(phase.read_tensor(
            0, 0, std::vector<std::uint64_t>(shape.size(), 0), &first, 8));
        if (std::signbit(first))
          number += 7;
      }
      check(builder.publish_tensor(
          0, Region::whole({1}),
          ByteView(reinterpret_cast<const std::uint8_t*>(&number), 8)));
    }
    return Poll(ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
int compact_identity() {
  auto registry = std::make_shared<OperationRegistry>();
  std::atomic<unsigned> calls{0};
  OperationDefinition metadata;
  metadata.key = "fixture.metadata";
  metadata.traits.parameter_schema = {
      {"offset", OperationParameterType::Float64}};
  metadata.traits.input_count = 1;
  metadata.traits.input_schema.resize(1);

  metadata.traits.input_schema[0].tensor_key = "data";
  metadata.traits.outputs[0] = multi_result::output("value");
  metadata.start_result = [&](const ResultProgramQuery&,
                              const BufferAllocator& allocator) {
    ++calls;
    return ResultContinuation::make<MetadataIdentityProgram>(allocator);
  };
  PS_CHECK(registry->register_operation(metadata).ok());
  OperationDefinition view;
  view.key = "fixture.partial";
  view.traits.input_count = 1;
  view.traits.input_schema = metadata.traits.input_schema;
  view.traits.outputs[0] = multi_result::output(
      "value", cache_fixture::source_schema(array<double>({0})));
  view.traits.requires_metadata_specialization = true;
  view.specialize_metadata = [](const auto& inputs, const auto&) {
    OperationOutputSpecialization output;
    output.metadata.result_schema = inputs[0].result_schema;
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  view.start_result = [&](const ResultProgramQuery&,
                          const BufferAllocator& allocator) {
    ++calls;
    return ResultContinuation::make<multi_result::Program>(allocator, 0);
  };
  PS_CHECK(registry->register_operation(view).ok());
  PS_CHECK(registry->freeze().ok());
  ExecutionContext execution(registry, {2, false, 16, 1024 * 1024, 65536});
  auto root = execution.resource_budget().take_value();
  auto check = [&](const TensorSpan& v, double expected, bool cacheable = true,
                   std::uint64_t cache_work = 0, double offset = 0) {
    GraphContext graph(document({v}, {{1,
                                       "fixture.metadata",
                                       {WorkflowInputReference{1}},
                                       {{"offset", offset}}}}));
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    const auto before = calls.load();
    std::vector<std::uint8_t> observed;
    unsigned publications = 0;
    ExecutionOptions options;
    options.maximum_dependency_cache_work = cacheable ? 1048576 : cache_work;
    options.result_publication = [&](ValueRef ref, const ResultRef& result) {
      if (ref != ValueRef{1, 0})
        return Status{ErrorCode::InvalidArgument, "wrong publication target"};
      observed = cache_fixture::bytes(result);
      ++publications;
      return Status::success();
    };
    auto first_binding = cache_fixture::bind_sources(root, {v});
    auto second_binding = cache_fixture::bind_sources(root, {v});
    auto a = execution.execute(plan, first_binding, {}, options);
    const auto first = observed;
    observed.clear();
    auto b = execution.execute(plan, second_binding, {}, options);
    if (!a.ok() || !b.ok())
      return false;
    const auto& first_result = a.value().results.at("result");
    const auto& second_result = b.value().results.at("result");
    const auto record = b.value().dependencies.tile_record(
        {plan.output("result").record_index, 0, 0});
    if (!record.ok() || !record.value() || record.value()->inputs.size() != 1)
      return false;
    const auto& dependency = record.value()->inputs[0];
    const auto* source = std::get_if<PlanWorkflowInput>(&dependency.producer);
    return source && source->declaration_index == 0 &&
           dependency.tensor_slot == 0 &&
           dependency.tiles == take(TileSet::range(root, 0,
                                                   plan.steps()[0]
                                                       .structured_metadata
                                                       ->input_tile_grids[0][0]
                                                       .tile_count())) &&
           a.value().diagnostics.dependency_cache_work <=
               options.maximum_dependency_cache_work &&
           b.value().diagnostics.dependency_cache_work <=
               options.maximum_dependency_cache_work &&
           publications == 2 && first.size() == 8 && first == observed &&
           multi_result::number(first_result) == expected &&
           multi_result::number(second_result) == expected &&
           first_result.object_id() != second_result.object_id() &&
           b.value().diagnostics.cache_hits == (cacheable ? 1U : 0U) &&
           calls == before + (cacheable ? 1U : 2U);
  };
  PS_CHECK(check(array<double>(std::vector<double>(256)), 359));
  PS_CHECK(check(array<double>(std::vector<double>(257)), 360));
  const auto integers =
      TensorSpan::create({ElementType::Int64, {256}}, Region::whole({256}),
                         {0, {8}}, std::vector<std::uint8_t>(2048))
          .take_value();
  PS_CHECK(check(integers, 358));
  PS_CHECK(check(array<float>({0}), 105));
  PS_CHECK(check(array<double>(std::vector<double>(256), {128, 2}), 331));
  PS_CHECK(check(array<double>({0.}), 104));
  PS_CHECK(check(array<double>({-0.}), 111));
  PS_CHECK(check(array<double>({0.}), 105, true, 0, 1));
  PS_CHECK(check(array<double>({0.}), 106, true, 0, 2));
  auto bytes =
      TensorSpan::create({ElementType::UInt8, {2048}}, Region::whole({2048}),
                         {0, {1}}, std::vector<std::uint8_t>(2048))
          .take_value();
  PS_CHECK(check(bytes, 2149));
  auto oversized =
      TensorSpan::create({ElementType::UInt8, {2049}}, Region::whole({2049}),
                         {0, {1}}, std::vector<std::uint8_t>(2049))
          .take_value();
  PS_CHECK(check(oversized, 2150));
  PS_CHECK(check(array<double>(std::vector<double>(257)), 360, false));
  PS_CHECK(check(oversized, 2150, false));
  PS_CHECK(check(oversized, 2150, false, 1));
  const auto input = array<double>({1, 2, 3, 4});
  GraphContext graph(document(
      {input}, {{1, "fixture.partial", {WorkflowInputReference{1}}, {}}}));
  PlanningOptions options;
  options.output_requests["result"] = {{{0, take(TileSet::range(root, 0, 1))}}};
  auto partial = Compiler(registry).compile(graph, options).take_value().plan;
  const auto before = calls.load();
  auto first =
      execution.execute(partial, cache_fixture::bind_sources(root, {input}));
  auto second =
      execution.execute(partial, cache_fixture::bind_sources(root, {input}));
  PS_CHECK(first.ok() && second.ok() && calls == before + 1 &&
           second.value().diagnostics.cache_hits == 1);
  const auto& result = second.value().results.at("result");
  PS_CHECK(multi_result::number(result, {1}) == 2 &&
           multi_result::number(result, {2}) == 3);
  double inaccessible = 0;
  PS_CHECK(
      result.read_tensor(result.descriptor().value(), 0, {0}, &inaccessible, 8)
          .ok() &&
      inaccessible == 1);
  const auto dirty = second.value().dependencies.dirty_units(
      {{PlanWorkflowInput{0}, 0, 0, take(TileSet::from_indices(root, {0})),
        false}});
  PS_REQUIRE_OK(dirty);
  PS_CHECK(dirty.value().size() == 2 &&
           dirty.value()[0] == (TileUnit{0, 0, 0}) &&
           dirty.value()[1] ==
               (TileUnit{partial.output("result").record_index, 0, 0}));
  ResultRef retained;
  ResourceBudget retained_root;
  {
    ExecutionContext transient(registry);
    retained_root = transient.resource_budget().take_value();
    auto published = transient.execute(
        partial, cache_fixture::bind_sources(retained_root, {input}));
    PS_CHECK(published.ok());
    retained = published.value().results.at("result");
  }
  PS_CHECK(multi_result::number(retained, {1}) == 2 &&
           multi_result::number(retained, {2}) == 3);
  PS_CHECK(retained_root.statistics().live[ResourceKind::Payload] == 32);
  retained = {};
  PS_CHECK(retained_root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
}

}  // namespace
int main() {
  return compact_identity();
}
