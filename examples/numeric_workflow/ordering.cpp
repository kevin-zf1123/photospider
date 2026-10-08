#include "photospider/numeric/ordering.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)
#include "result_fixture.hpp"     // NOLINT(build/include_subdir)

namespace {
namespace rf = numeric_result_fixture;
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
ps::Value array(ps::ElementType type, const std::vector<std::uint64_t>& shape,
                const std::vector<std::uint64_t>& bits) {
  const auto width = ps::Value::element_size(type);
  auto buffer = take(ps::BufferAllocator{}.allocate(bits.size() * width));
  for (std::size_t i = 0; i < bits.size(); ++i)
    std::memcpy(buffer.data() + i * width, &bits[i], width);
  std::vector<std::int64_t> strides(shape.size());
  std::int64_t stride = width;
  for (std::size_t j = shape.size(); j; --j) {
    strides[j - 1] = stride;
    stride *= shape[j - 1];
  }
  return take(ps::Value::from_storage({type, shape}, ps::Region::whole(shape),
                                      {0, strides},
                                      std::move(buffer).freeze()));
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  std::vector<ps::Value> backing;
  ps::ResourceBudget last_root;
  Fixture(ps::WorkflowNode node, std::vector<ps::Value> inputs, bool sorting)
      : backing(std::move(inputs)) {
    rf::declare_sources(&document, backing);
    document.outputs = {{"values", node.id, "values"}};
    if (sorting)
      document.outputs.push_back({"indices", node.id, "indices"});
    document.nodes = {std::move(node)};
  }
  ps::ExecutionBindings bind(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
  }
  ps::Result<ps::DemandResult> run(
      const ps::DemandQuery& query, bool cache = true,
      std::uint64_t proof_work = UINT64_C(64) * 1024 * 1024,
      std::uint64_t cache_bytes = 65536,
      std::uint64_t work = UINT64_C(128) * 1024 * 1024,
      const ps::CancellationToken& stop = {}) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 262144;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    config.managed_resources->maximum_work = work;
    ps::ExecutionContext context(registry, config);
    last_root = take(context.resource_budget());
    auto snapshot = context.freeze(plan.value().plan, bind(last_root));
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = work;
    options.dependencies.maximum_work = UINT64_C(16) * 1024 * 1024;
    options.maximum_dependency_cache_work = cache ? proof_work : 0;
    return context.execute_fragments(snapshot.value(), query, stop, options);
  }
};
std::vector<std::uint64_t> list(const std::string& text) {
  std::vector<std::uint64_t> result;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find(',', begin);
    result.push_back(std::stoull(text.substr(begin, end - begin)));
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return result;
}
void oracle(ps::CpuNumericProfile profile) {
  std::string operation, encoded_shape;
  unsigned source = 0, qtype = 0, destination = 0;
  std::int64_t axis = 0;
  std::uint64_t qbits = 0;
  while (std::cin >> operation >> source >> qtype >> destination >> axis >>
         encoded_shape >> std::hex >> qbits >> std::dec) {
    auto shape = list(encoded_shape);
    std::uint64_t count = 1;
    for (auto size : shape)
      count *= size;
    std::vector<std::uint64_t> bits(count);
    for (auto& value : bits)
      std::cin >> std::hex >> value >> std::dec;
    const bool sorting = operation == "sort";
    std::vector<ps::Value> inputs{
        array(static_cast<ps::ElementType>(source), shape, bits)};
    auto node = sorting
                    ? take(ps::numeric::sort_node(
                          1, ps::WorkflowInputReference{1}, axis, profile))
                    : take(ps::numeric::quantile_node(
                          1, ps::WorkflowInputReference{1},
                          ps::WorkflowInputReference{2}, axis,
                          static_cast<ps::ElementType>(destination), profile));
    if (!sorting) {
      inputs.push_back(
          array(static_cast<ps::ElementType>(qtype), {1}, {qbits}));
      shape[axis] = 1;
    }
    Fixture fixture(std::move(node), inputs, sorting);
    auto all = take(ps::Footprint::all(shape));
    ps::DemandQuery query{{"values", all}};
    if (sorting)
      query.emplace("indices", all);
    auto answer = fixture.run(query);
    if (!answer.ok()) {
      if (answer.status().message.find("InvalidQuantileProbability") !=
          std::string::npos)
        std::cout << "q\n";
      else
        throw std::runtime_error(answer.status().message);
      continue;
    }
    for (const auto* name : {"values", "indices"}) {
      if (std::string(name) == "indices") {
        if (!sorting)
          break;
        std::cout << " | ";
      }
      bool first = true;
      const auto& values = answer.value().results.at(name);
      const auto width = ps::Value::element_size(
          values.schema().tensors[0].descriptor.element_type);
      require(all.visit(
                     [&](const auto& coordinate) {
                       std::uint64_t value = 0;
                       auto status =
                           rf::read(values, coordinate, &value, width);
                       if (status.ok()) {
                         if (!first)
                           std::cout << ' ';
                         first = false;
                         std::cout << std::hex << value << std::dec;
                       }
                       return status;
                     },
                     4096)
                  .ok(),
              "oracle result read");
    }
    std::cout << '\n';
  }
}
void examples(ps::CpuNumericProfile profile) {
  Fixture sorted(take(ps::numeric::sort_node(1, ps::WorkflowInputReference{1},
                                             0, profile)),
                 {array(ps::ElementType::Int64, {4}, {3, 1, 1, 2})}, true);
  auto all = take(ps::Footprint::all({4}));
  for (bool cache : {false, true}) {
    auto answer = take(sorted.run({{"values", all}, {"indices", all}}, cache));
    const std::uint64_t values[] = {1, 1, 2, 3}, indices[] = {1, 2, 3, 0};
    for (std::uint64_t i = 0; i < 4; ++i) {
      std::uint64_t actual = 0;
      require(rf::read(answer.results.at("values"), {i}, &actual, 8).ok() &&
                  actual == values[i],
              "sort values");
      require(rf::read(answer.results.at("indices"), {i}, &actual, 8).ok() &&
                  actual == indices[i],
              "sort indices stable ties");
    }
    std::uint64_t invocations = 0;
    for (const auto& timing : answer.diagnostics.operation_timings)
      invocations += timing.invocation_count;
    require(invocations == 4,
            "one Need poll and one compute poll per sort output");
  }
  Fixture quantile(
      take(ps::numeric::quantile_node(1, ps::WorkflowInputReference{1},
                                      ps::WorkflowInputReference{2}, 0,
                                      ps::ElementType::Float64, profile)),
      {array(ps::ElementType::Int64, {4}, {0, 10, 20, 30}),
       array(ps::ElementType::Float64, {1}, {UINT64_C(0x3fd0000000000000)})},
      false);
  auto answer = take(quantile.run({{"values", take(ps::Footprint::all({1}))}}));
  std::uint64_t actual = 0;
  require(rf::read(answer.results.at("values"), {0}, &actual, 8).ok() &&
              actual == UINT64_C(0x401e000000000000),
          "quantile exact 7.5");
  std::cout << "sort [3,1,1,2]: values=[1,1,2,3], indices=[1,2,3,0]; "
               "quantile([0,10,20,30],.25)=7.5 passed\n";
}

void sharing_and_sparse(ps::CpuNumericProfile profile) {
  std::vector<std::uint64_t> bits(256);
  for (std::uint64_t i = 0; i < 128; ++i) {
    bits[i] = UINT64_C(0x3f800000) + (127 - i) / 2;
    bits[128 + i] = UINT64_C(0x7f800001) + i;
  }
  Fixture fixture(take(ps::numeric::sort_node(1, ps::WorkflowInputReference{1},
                                              1, profile)),
                  {array(ps::ElementType::Float32, {2, 128}, bits)}, true);
  auto row = take(
      ps::Footprint::from_regions({2, 128}, {ps::Region({{0, 1}, {0, 128}})}));
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto answer = take(fixture.run(
        {{"values", row}, {"indices", row}}, mode != 0,
        mode == 2 ? 0 : UINT64_C(64) * 1024 * 1024, mode == 3 ? 1 : 65536));
    for (std::uint64_t i = 0; i < 128; ++i) {
      std::uint64_t actual = 0;
      require(rf::read(answer.results.at("values"), {0, i}, &actual, 4).ok() &&
                  actual == UINT64_C(0x3f800000) + i / 2,
              "long sort values");
      require(rf::read(answer.results.at("indices"), {0, i}, &actual, 8).ok() &&
                  actual == 126 - 2 * (i / 2) + i % 2,
              "long stable indices");
    }
    std::uint64_t invocations = 0;
    for (const auto& timing : answer.diagnostics.operation_timings)
      invocations += timing.invocation_count;
    require(invocations == 4,
            "one Need poll and one compute poll per selected output");
    require(take(answer.dependencies.source_support()).at("input0") ==
                take(ps::Footprint::all({2, 128})),
            "selected full-line source support");
    const auto changed = take(
        ps::Footprint::from_regions({2, 128}, {ps::Region({{0, 1}, {10, 1}})}));
    auto dirty = take(answer.dependencies.potential_dirty("input0", changed));
    require(dirty.at("values") == row && dirty.at("indices") == row,
            "one source edit invalidates both sorted lines");
    const auto other = take(ps::Footprint::from_regions(
        {2, 128}, {ps::Region({{1, 1}, {0, 128}})}));
    dirty = take(answer.dependencies.potential_dirty("input0", other));
    require(dirty.at("values") == row && dirty.at("indices") == row,
            "unrequested line invalidates all recorded Whole observations");
  }
  const auto selected = take(
      ps::Footprint::from_regions({2, 128}, {ps::Region({{0, 1}, {70, 1}})}));
  for (const auto* name : {"values", "indices"}) {
    auto answer = take(fixture.run({{name, selected}}));
    require(answer.results.size() == 1 &&
                take(answer.results.at(name).descriptor()).tensor_coverage(0) ==
                    take(ps::Footprint::all({2, 128})),
            "selected named output publishes full schema coverage");
    require(fixture.last_root.statistics().live[ps::ResourceKind::Payload] ==
                (std::string(name) == "values" ? 1024U : 2048U),
            "escaped selected sort Result owns only its complete output "
            "allocation");
    require(take(answer.dependencies.source_support()).at("input0") ==
                take(ps::Footprint::all({2, 128})),
            "partial sorted position still reads full line");
  }
  std::cout << "128-element line: each output reuses its "
               "permutation; cache capacities, "
               "sparse outputs and line dirty passed\n";
}
void nonlast_axis_lines(ps::CpuNumericProfile profile) {
  std::vector<std::uint64_t> source(256);
  for (std::uint64_t row = 0; row < 128; ++row) {
    source[row * 2] = 127 - row;
    source[row * 2 + 1] = 1127 - row;
  }
  Fixture fixture(take(ps::numeric::sort_node(1, ps::WorkflowInputReference{1},
                                              0, profile)),
                  {array(ps::ElementType::Int64, {128, 2}, source)}, true);
  const auto all = take(ps::Footprint::all({128, 2}));
  const auto sparse = take(ps::Footprint::from_regions(
      {128, 2},
      {ps::Region({{1, 31}, {0, 2}}), ps::Region({{65, 62}, {0, 2}})}));
  for (const auto& q : {all, sparse}) {
    // Two lines per output fit; rebuilding 128 times per line does not.
    auto answer =
        take(fixture.run({{"values", q}, {"indices", q}}, false, 0, 0, 500000));
    require(
        q.visit(
             [&](const auto& at) {
               std::uint64_t value = 0, index = 0;
               require(
                   rf::read(answer.results.at("values"), at, &value, 8).ok() &&
                       value == at[0] + at[1] * 1000,
                   "non-last-axis stable values");
               require(
                   rf::read(answer.results.at("indices"), at, &index, 8).ok() &&
                       index == 127 - at[0],
                   "non-last-axis original indices");
               return ps::Status::success();
             },
             4096)
            .ok(),
        "non-last-axis result traversal");
  }
  std::cout << "non-last axis and disjoint boxes reuse each line within a "
               "bounded work budget\n";
}
void probability_dependencies(ps::CpuNumericProfile profile) {
  for (bool singleton : {true, false}) {
    unsigned calls = 0;
    auto registry = ps::make_default_operation_registry(false);
    ps::OperationDefinition failed;
    failed.key = "manual.quantile_failed";
    failed.traits.input_count = 0;
    failed.traits.input_schema.clear();
    auto& output = failed.traits.outputs[0];
    ps::SchemaTemplate schema;
    schema.id = "manual.quantile.failed";
    ps::ResultTensorSpec tensor;
    tensor.key = "data";
    tensor.descriptor = {
        singleton ? ps::ElementType::Float64 : ps::ElementType::Int64,
        singleton ? std::vector<std::uint64_t>{1}
                  : std::vector<std::uint64_t>{2, 2}};
    schema.tensors.push_back(tensor);
    output.output_schema.kind = ps::OperationPortKind::Result;
    output.output_schema.result_schema_id = schema.id;
    output.output_schema.result_schema_version = schema.version;
    output.result_schema = schema;
    output.region_rule = ps::OperationRegionRule::Dependency;
    output.regional_atomic = true;
    output.continuation_bytes = 1;
    output.maximum_dependency_stages = 1;
    failed.start_result = [&](const auto& query, const auto&) {
      ++calls;
      require(query.tensor_outputs &&
                  *query.tensor_outputs ==
                      take(ps::Footprint::all(tensor.sample_shape())),
              "required producer receives the complete Whole Need");
      return ps::Result<ps::ResultContinuation>(ps::Status{
          ps::ErrorCode::OperationFailed, "required quantile producer"});
    };
    require(registry->register_operation(std::move(failed)).ok() &&
                registry->freeze().ok(),
            "quantile failing Result registry");
    Fixture fixture(
        take(ps::numeric::quantile_node(1, ps::WorkflowInputReference{1},
                                        ps::WorkflowInputReference{2}, 1,
                                        ps::ElementType::Float32, profile)),
        {array(ps::ElementType::Int64,
               singleton ? std::vector<std::uint64_t>{2, 1}
                         : std::vector<std::uint64_t>{2, 2},
               singleton ? std::vector<std::uint64_t>{0x7fffffffffffffff,
                                                      0xfffffffffffffffe}
                         : std::vector<std::uint64_t>{1, 2, 3, 4}),
         array(ps::ElementType::Float64, {1}, {0x7ff8000000000011})},
        false);
    fixture.registry = registry;
    fixture.document.nodes[0].inputs[singleton ? 1 : 0] =
        ps::WorkflowNodeOutput{2, "value"};
    fixture.document.nodes.push_back({2, "manual.quantile_failed", {}, {}});
    auto answer = fixture.run({{"values", take(ps::Footprint::all({2, 1}))}});
    require(calls == (singleton ? 0 : 1),
            "singleton skips q; other Whole inputs are eagerly read");
    if (singleton) {
      auto result = take(std::move(answer));
      const std::uint64_t expected[] = {0x5f000000, 0xc0000000};
      for (std::uint64_t i = 0; i < 2; ++i) {
        std::uint64_t actual = 0;
        require(
            rf::read(result.results.at("values"), {i, 0}, &actual, 4).ok() &&
                actual == expected[i],
            "singleton exact integer conversion");
      }
      require(
          result.results.at("values").association().size() == 1 &&
              take(result.dependencies.source_support()).count("input1") == 0,
          "singleton q has no runtime association or source support");
    } else {
      require(!answer.ok() &&
                  answer.status().message == "required quantile producer",
              "source failure precedes invalid q callback validation");
      fixture.backing[1] =
          array(ps::ElementType::Float64, {1}, {0x3fe0000000000000});
      calls = 0;
      answer = fixture.run({{"values", take(ps::Footprint::all({2, 1}))}});
      require(!answer.ok() && calls == 1 &&
                  answer.status().message == "required quantile producer",
              "valid q preserves source failure");
    }
  }
  for (const auto& q : {array(ps::ElementType::Float64, {2}, {0, 0}),
                        array(ps::ElementType::Int64, {1}, {0})}) {
    Fixture fixture(
        take(ps::numeric::quantile_node(1, ps::WorkflowInputReference{1},
                                        ps::WorkflowInputReference{2}, 0,
                                        ps::ElementType::Float64, profile)),
        {array(ps::ElementType::Int64, {1}, {1}), q}, false);
    auto rejected = fixture.run({{"values", take(ps::Footprint::all({1}))}});
    require(!rejected.ok() &&
                rejected.status().code == ps::ErrorCode::TypeMismatch &&
                rejected.status().detail.origin == ps::FailureOrigin::Schema,
            "singleton projection still validates q shape and dtype");
  }
  std::cout << "Result quantile: singleton q projection, schema validation and "
               "full-source failure precedence passed\n";
}

struct MetadataControl {
  bool admitted = false;
};
struct MetadataProgram {
  ps::ResultContinuation inner;
  std::shared_ptr<MetadataControl> control;
  MetadataProgram(ps::ResultContinuation original,
                  std::shared_ptr<MetadataControl> state)
      : inner(std::move(original)), control(std::move(state)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    auto bounded = phase;
    std::optional<ps::ResourceLease> reservation;
    bounded.consume_work = [&](std::uint64_t amount) {
      auto status = phase.consume_work(amount);
      // The 64-element ordering workspace charges count + 16 before its
      // two 512-byte vectors. Keep setup intact and reject the second vector.
      if (status.ok() && phase.tensors && !phase.tensors->empty() &&
          amount == 80 && !reservation) {
        const auto remaining =
            UINT64_C(1000000) -
            phase.resources.statistics().live[ps::ResourceKind::Metadata];
        require(remaining > 1024, "metadata fixture leaves room for setup");
        auto admitted = phase.resources.reserve(
            ps::ResourceCapacity::host(remaining - 1024, remaining - 1024));
        if (!admitted.ok())
          return admitted.status();
        reservation = admitted.take_value();
        control->admitted = true;
      }
      return status;
    };
    return inner.poll(bounded);
  }
};
void metadata_limit(const ps::WorkflowNode& node) {
  Fixture fixture(
      node,
      {array(ps::ElementType::Int64, {64}, std::vector<std::uint64_t>(64, 1))},
      true);
  fixture.registry = ps::make_default_operation_registry(false);
  const std::weak_ptr<ps::OperationRegistry> owner = fixture.registry;
  auto control = std::make_shared<MetadataControl>();
  ps::OperationDefinition adapter;
  adapter.key = "manual.metadata." + node.operation;
  adapter.traits = take(fixture.registry->find_traits(node.operation));
  adapter.traits.cacheable = false;
  for (auto& output : adapter.traits.outputs)
    output.continuation_bytes += sizeof(MetadataProgram);
  const auto key = node.operation;
  adapter.prepare_static =
      [owner, key](
          const auto& inputs,
          const auto& parameters) -> ps::Result<ps::OperationPreparation> {
    auto registry = owner.lock();
    if (!registry)
      return ps::Result<ps::OperationPreparation>(
          ps::Status{ps::ErrorCode::Stale, {}});
    auto original = registry->prepare_operation(key, inputs, parameters);
    if (!original.ok())
      return ps::Result<ps::OperationPreparation>(original.status());
    ps::OperationPreparation prepared;
    for (const auto& output : original.value()->traits().outputs) {
      ps::OperationOutputSpecialization item;
      item.metadata.result_schema =
          std::make_shared<ps::SchemaTemplate>(*output.result_schema);
      item.input_indices = output.input_indices;
      prepared.outputs.push_back(std::move(item));
    }
    prepared.state = original.take_value();
    return ps::Result<ps::OperationPreparation>(std::move(prepared));
  };
  adapter.start_result = [owner, key, control](const auto& query,
                                               const auto& allocator) {
    auto registry = owner.lock();
    if (!registry)
      return ps::Result<ps::ResultContinuation>(
          ps::Status{ps::ErrorCode::Stale, {}});
    auto forwarded = query;
    forwarded.prepared = std::shared_ptr<const ps::PreparedOperation>(
        query.prepared,
        static_cast<const ps::PreparedOperation*>(query.prepared->state()));
    auto original = registry->start_result(key, forwarded, allocator);
    if (!original.ok())
      return original;
    return ps::ResultContinuation::make<MetadataProgram>(
        allocator, original.take_value(), control);
  };
  fixture.document.nodes[0].operation = adapter.key;
  require(fixture.registry->register_operation(std::move(adapter)).ok(),
          "register metadata-limited ordering");
  require(fixture.registry->freeze().ok(), "freeze metadata-limited ordering");
  ps::ResourceBudget root;
  {
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = ps::ResourceLimits{};
    config.managed_resources->capacity[ps::ResourceKind::Metadata] = 1000000;
    ps::ExecutionContext execution(fixture.registry, config);
    root = take(execution.resource_budget());
    auto frozen = take(execution.freeze(plan.plan, fixture.bind(root)));
    auto result = execution.execute(frozen);
    require(!result.ok() &&
                result.status().code == ps::ErrorCode::ResourceExhausted &&
                result.status().reason == ps::FailureReason::CapacityLimit &&
                control->admitted,
            "ordering fails inside admitted per-line metadata vectors");
  }
  point_math_checks::released(root);
}
void failure_and_schema(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  auto node = take(
      ps::numeric::sort_node(1, ps::WorkflowInputReference{1}, 0, profile));
  std::vector<std::uint64_t> descending(4096);
  for (unsigned i = 0; i < 4096; ++i)
    descending[i] = 4095 - i;
  auto large = array(ps::ElementType::Int64, {4096}, descending);
  point_math_checks::resources(node, {large}, 32768);
  point_math_checks::resources(node, {large}, 32768, 1);
  auto quantile = take(ps::numeric::quantile_node(
      1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}, 0,
      ps::ElementType::Float64, profile));
  auto half = array(ps::ElementType::Float64, {1}, {0x3fe0000000000000});
  point_math_checks::resources(quantile, {large, half}, 8);
  metadata_limit(node);
  Fixture invalid(
      take(ps::numeric::quantile_node(1, ps::WorkflowInputReference{1},
                                      ps::WorkflowInputReference{2}, 0,
                                      ps::ElementType::Float64, profile)),
      {array(ps::ElementType::Int64, {2}, {1, 2}),
       array(ps::ElementType::Float64, {2}, {0, 0})},
      false);
  auto result = invalid.run({{"values", take(ps::Footprint::all({1}))}});
  require(!result.ok() && result.status().code == ps::ErrorCode::TypeMismatch &&
              result.status().reason == ps::FailureReason::None &&
              result.status().detail.origin == ps::FailureOrigin::Schema,
          "q shape mismatch classification");
  ps::SchemaTemplate oversized_schema;
  oversized_schema.id = "manual.ordering.oversized";
  ps::ResultTensorSpec tensor;
  tensor.key = "data";
  tensor.descriptor = {ps::ElementType::UInt8, {(UINT64_C(1) << 40) + 1}};
  oversized_schema.tensors.push_back(tensor);
  auto oversized = registry->resolve_traits(
      node.operation,
      {ps::OperationMetadata{
          {},
          {},
          std::make_shared<ps::SchemaTemplate>(oversized_schema)}},
      node.parameters);
  require(!oversized.ok() &&
              oversized.status().code == ps::ErrorCode::TypeMismatch &&
              oversized.status().reason == ps::FailureReason::None &&
              oversized.status().detail.origin == ps::FailureOrigin::Schema,
          "oversized descriptor classification");
  std::cout << "sorting WorkLimit/cancel attempts and cleanup, descriptor/q "
               "shape classification passed\n";
}

void typed_layout_environment(ps::CpuNumericProfile profile) {
  ps::ColorArrayDescriptor color;
  color.model = ps::ColorModel::Rgb;
  color.association = ps::ColorAssociation::Straight;
  color.primaries =
      take(ps::color_primary_coordinates(ps::ColorPrimaryPreset::Srgb))
          .primaries;
  color.transfer = ps::ColorTransfer{ps::ColorTransferKind::Linear, {}};
  for (unsigned kind = 0; kind < 3; ++kind) {
    const bool quantile = kind == 2;
    auto node = quantile ? take(ps::numeric::quantile_node(
                               1, ps::WorkflowInputReference{1},
                               ps::WorkflowInputReference{2}, 1,
                               ps::ElementType::Float64, profile))
                         : take(ps::numeric::sort_node(
                               1, ps::WorkflowInputReference{1}, 1, profile));
    std::vector<ps::Value> inputs{array(ps::ElementType::Float32, {1, 1, 4},
                                        {0x3f800000, 0, 0, 0x40000000})};
    if (quantile)
      inputs.push_back(
          array(ps::ElementType::Float64, {1}, {0x7ff0000000000001}));
    Fixture fixture(node, inputs, !quantile);
    auto schema = rf::source_schema(inputs[0]);
    schema.tensors[0].facets = {take(ps::encode_color_array(color))};
    schema.tensors[0].atomic_trailing_axes = 1;
    fixture.document.inputs[0].result_schema =
        std::make_shared<ps::SchemaTemplate>(schema);
    const auto name = kind == 1 ? "indices" : "values";
    const auto green = take(ps::Footprint::from_regions(
        {1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
    auto empty =
        take(fixture.run({{name, take(ps::Footprint::none({1, 1, 4}))}}));
    require(
        take(empty.results.at(name).descriptor()).tensor_coverage(0).empty(),
        "Empty ordering skips invalid typed input and q");
    for (const auto& timing : empty.diagnostics.operation_timings)
      require(timing.computed_elements == 0,
              "Empty ordering has no arithmetic");
    for (const auto& entry : take(empty.dependencies.source_support()))
      require(entry.second.empty(), "Empty ordering has no source samples");
    auto failed = fixture.run({{name, green}});
    require(!failed.ok() &&
                failed.status().code == ps::ErrorCode::InvalidArgument &&
                failed.status().reason == ps::FailureReason::InvalidDomain &&
                failed.status().detail.input_id == 1,
            "values, indices and singleton quantile validate alpha outside "
            "green Q");
    ps::CancellationSource stop;
    stop.cancel();
    require(fixture.run({{name, green}}, false, 0, 0, 128 * 1024 * 1024,
                        stop.token())
                    .status()
                    .code == ps::ErrorCode::Cancelled,
            "pre-cancelled Whole ordering");
    fixture.backing[0] = array(ps::ElementType::Float32, {1, 1, 4},
                               {0x3f800000, 0, 0, 0x3f800000});
    auto good = take(fixture.run({{name, green}}));
    const auto& result = good.results.at(name);
    require(result.schema().tensors[0].facets.empty() &&
                take(result.descriptor()).tensor_coverage(0) ==
                    take(ps::Footprint::all({1, 1, 4})) &&
                take(good.dependencies.source_support()).at("input0") ==
                    take(ps::Footprint::all({1, 1, 4})),
            "same-schema legal input publishes full untyped Whole output");
    for (unsigned channel = 0; channel < 4; ++channel) {
      std::uint64_t actual = 0;
      const auto expected =
          kind == 1 ? UINT64_C(0)
          : channel == 0 || channel == 3
              ? (quantile ? UINT64_C(0x3ff0000000000000) : UINT64_C(0x3f800000))
              : UINT64_C(0);
      require(rf::read(result, {0, 0, channel}, &actual, kind ? 8 : 4).ok() &&
                  actual == expected,
              "typed ordering positive control");
    }
    if (quantile)
      require(result.association().size() == 1 &&
                  take(good.dependencies.source_support()).count("input1") == 0,
              "singleton skips signaling NaN q without runtime provenance");
  }
  const auto packed =
      array(ps::ElementType::Float32, {6},
            {0x7f800011, 0x80000000, 0, 0x40000000, 0xbf800000, 0xff800022});
  const auto strided = take(ps::Value::from_storage(
      {ps::ElementType::Float32, {2, 3}}, ps::Region::whole({2, 3}),
      {8, {12, -4}}, packed.storage()));
  fenv_t saved;
  require(fegetenv(&saved) == 0, "save fenv");
  for (unsigned kind = 0; kind < 3; ++kind) {
    const bool quantile = kind == 2;
    auto node = quantile ? take(ps::numeric::quantile_node(
                               1, ps::WorkflowInputReference{1},
                               ps::WorkflowInputReference{2}, 1,
                               ps::ElementType::Float32, profile))
                         : take(ps::numeric::sort_node(
                               1, ps::WorkflowInputReference{1}, 1, profile));
    std::vector<ps::Value> inputs{strided};
    if (quantile)
      inputs.push_back(
          array(ps::ElementType::Float64, {1}, {0x3fe0000000000000}));
    const std::vector<std::uint64_t> expected =
        kind == 0
            ? std::vector<std::uint64_t>{0,          0x80000000, 0x7f800011,
                                         0xbf800000, 0x40000000, 0xff800022}
        : kind == 1 ? std::vector<std::uint64_t>{0, 1, 2, 1, 2, 0}
                    : std::vector<std::uint64_t>{0x7fc00011, 0xffc00022};
    for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "prepare caller fenv");
      auto control = std::make_shared<point_math_checks::Control>();
      control->rounding = mode;
      point_math_checks::Workflow workflow(node, inputs, {}, control,
                                           kind == 1 ? 1 : 0);
      auto answer = take(workflow.run());
      require(fegetround() == mode &&
                  fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO &&
                  control->computation_polls == 1,
              "Result ordering preserves caller and actual worker fenv");
      const auto bytes = rf::bytes(answer.results.at("values"));
      const auto width = kind == 1 ? 8U : 4U;
      for (std::size_t j = 0; j < expected.size(); ++j) {
        std::uint64_t actual = 0;
        std::memcpy(&actual, bytes.data() + j * width, width);
        require(actual == expected[j],
                "negative-stride raw sort, stable indices and first NaN");
      }
    }
  }
  require(fesetenv(&saved) == 0, "restore fenv");
  std::cout << "Result ordering: typed closure, Empty, cancellation, negative "
               "strides and four caller/worker fenv modes passed\n";
}

ps::SchemaTemplate probe_schema(ps::ElementType type, std::uint64_t extent,
                                const char* id) {
  ps::SchemaTemplate schema;
  schema.id = id;
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {type, {extent}};
  schema.tensors.push_back(tensor);
  return schema;
}
ps::ResultRef probe_state(const ps::ResourceBudget& root, std::uint64_t bits) {
  const auto schema =
      probe_schema(ps::ElementType::Int64, 1, "manual.block.state");
  auto builder = take(ps::ResultBuilder::start(root, schema, "block.state"));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(root, 1, {})))
              .ok(),
          "probe state descriptor");
  require(builder
              .publish_tensor(0, ps::Region::whole({1}),
                              {reinterpret_cast<const std::uint8_t*>(&bits), 8},
                              take(ps::ResultRelation::cartesian(root, 1, {})),
                              {true, true, true, true})
              .ok(),
          "probe state publication");
  return take(builder.seal());
}
struct BlockProbeControl {
  unsigned computed = 0;
};
struct BlockProbeState {
  bool requested = false;
  std::shared_ptr<BlockProbeControl> control;
  explicit BlockProbeState(std::shared_ptr<BlockProbeControl> observed)
      : control(std::move(observed)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    using Answer = ps::Result<ps::ResultProgramPoll>;
    const auto atom = take(ps::result_atom_key(phase.query));
    const auto coordinate = atom.coordinate[0];
    const auto roles = phase.query.output_index ? 2U : 1U;
    if (!requested) {
      requested = true;
      ps::ResultProgramNeed need;
      need.tensors.push_back({0, 0, *phase.query.tensor_outputs, roles});
      return Answer(std::move(need));
    }
    auto state = probe_state(phase.resources, 0);
    auto computed =
        phase.block(1, 0, 1, 1, state, [&]() -> ps::Result<ps::ResultRef> {
          ++control->computed;
          std::uint64_t bits = 0;
          auto status = phase.read_tensor(0, 0, {coordinate}, &bits, 1);
          return status.ok() ? ps::Result<ps::ResultRef>(
                                   probe_state(phase.resources, bits))
                             : ps::Result<ps::ResultRef>(status);
        });
    if (!computed.ok())
      return Answer(computed.status());
    require(computed.value().association().empty(),
            "block state does not import old source identity");
    std::uint64_t bits = 0;
    auto status = rf::read(computed.value(), {0}, &bits, 8);
    if (!status.ok())
      return Answer(status);
    auto builder = take(ps::ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    require(builder
                .bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(phase.resources, 1, {})))
                .ok(),
            "probe output descriptor");
    const auto width = phase.query.output_index ? 8U : 1U;
    status = builder.publish_tensor(
        0, ps::Region({{coordinate, 1}}),
        {reinterpret_cast<const std::uint8_t*>(&bits), width},
        take(ps::ResultRelation::cartesian(
            phase.resources, 2,
            {0, roles, coordinate, 1, ps::ResultSupportTarget::Tensor, 0})),
        {true, true, true, true});
    if (!status.ok())
      return Answer(status);
    return Answer(ps::ResultPublication{take(builder.seal()), true});
  }
};
ps::OperationDefinition block_probe(
    bool shared, const std::shared_ptr<BlockProbeControl>& control) {
  ps::OperationDefinition definition;
  definition.key = "manual.block_probe";
  auto& traits = definition.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].kind = ps::OperationPortKind::Result;
  traits.input_schema[0].result_schema_id = "manual.lowpass.input";
  traits.input_schema[0].result_schema_version = 1;
  traits.share_blocks_across_outputs = shared;
  traits.outputs.resize(2);
  for (unsigned i = 0; i < 2; ++i) {
    auto& output = traits.outputs[i];
    output.key = i ? "indices" : "values";
    output.output_schema.kind = ps::OperationPortKind::Result;
    output.result_schema =
        probe_schema(i ? ps::ElementType::Int64 : ps::ElementType::UInt8, 2,
                     i ? "manual.block.indices" : "manual.block.values");
    output.output_schema.result_schema_id = output.result_schema->id;
    output.output_schema.result_schema_version = output.result_schema->version;
    output.region_rule = ps::OperationRegionRule::Dependency;
    output.continuation_bytes = sizeof(BlockProbeState);
    output.maximum_dependency_stages = 2;
  }
  definition.start_result = [control](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<BlockProbeState>(allocator, control);
  };
  return definition;
}
void block_contracts() {
  for (bool shared : {false, true}) {
    for (unsigned mode = 0; mode < 5; ++mode) {
      auto control = std::make_shared<BlockProbeControl>();
      auto registry = std::make_shared<ps::OperationRegistry>();
      const auto registration =
          registry->register_operation(block_probe(shared, control));
      if (!registration.ok())
        throw std::runtime_error("register Result block probe: " +
                                 registration.message);
      require(registry->freeze().ok(), "freeze Result shared-block probe");
      Fixture fixture(
          {1, "manual.block_probe", {ps::WorkflowInputReference{1}}, {}},
          {array(ps::ElementType::UInt8, {2}, {4, 4})}, true);
      fixture.registry = registry;
      ps::GraphContext graph(fixture.document);
      auto plan = take(ps::Compiler(registry).compile(graph)).plan;
      ps::ResourceBudget root;
      {
        ps::ExecutionContextConfig config;
        config.cpu_workers = 1;
        config.result_cache_bytes = mode == 1 ? 0 : mode == 4 ? 1 : 65536;
        config.managed_resources = ps::ResourceLimits{};
        ps::ExecutionContext context(registry, config);
        root = take(context.resource_budget());
        auto bindings = fixture.bind(root);
        auto demand = take(context.open_demand(plan, bindings));
        ps::ExecutionOptions options;
        options.maximum_dependency_cache_work = mode == 2   ? 0
                                                : mode == 3 ? 64
                                                            : 64 * 1024 * 1024;
        for (unsigned serial = 0; serial < 4; ++serial) {
          const auto coordinate = serial == 2 ? 1U : 0U;
          const auto name = serial == 0 ? "values" : "indices";
          const auto selected = take(ps::Footprint::from_regions(
              {2}, {ps::Region({{coordinate, 1}})}));
          if (serial == 3) {
            fixture.backing[0] = array(ps::ElementType::UInt8, {2}, {5, 4});
            bindings = fixture.bind(root);
            require(demand.replace_bindings(bindings).ok(),
                    "replace block probe current Result");
          }
          auto answer = take(demand.request({{name, selected}}, {}, options));
          std::uint64_t actual = 0;
          const auto& result = answer.results.at(name);
          require(
              rf::read(result, {coordinate}, &actual, serial == 0 ? 1 : 8)
                      .ok() &&
                  actual == (serial == 3 ? 5U : 4U) &&
                  result.association() ==
                      ps::ResourceVector<std::uint64_t>{
                          bindings.inputs[0].result.object_id()},
              "block output uses current bits, dtype and source association");
          if (shared && mode == 3)
            require(answer.diagnostics.dependency_cache_work > 0 &&
                        answer.diagnostics.dependency_cache_work <= 64,
                    "exhausted shared contract hash accounts optional work at "
                    "the Root");
          const auto expected_roles = serial == 0 ? 1U : 2U;
          unsigned roles = 0;
          for (const auto& observation :
               take(answer.dependencies.source_observations()))
            roles |= observation.roles & 3U;
          require(roles == expected_roles,
                  "shared state never imports old Data/Control witness");
          require(take(answer.dependencies.potential_dirty("input0", selected,
                                                           expected_roles))
                              .at(name) == selected &&
                      take(answer.dependencies.potential_dirty(
                               "input0", selected, expected_roles == 1 ? 2 : 1))
                          .at(name)
                          .empty(),
                  "only current output role invalidates observed point");
        }
      }
      point_math_checks::released(root);
    }
  }
  for (unsigned invalid = 0; invalid < 4; ++invalid) {
    auto definition = block_probe(true, std::make_shared<BlockProbeControl>());
    if (invalid == 0) {
      definition.traits.deterministic = false;
      definition.traits.cacheable = false;
    } else if (invalid == 1) {
      definition.traits.outputs[0].regional_atomic = true;
    } else if (invalid == 2) {
      definition.traits.outputs[0].observation_kind =
          ps::ObservationKind::RequestRecord;
    } else {
      definition.traits.side_effect_free = false;
      definition.traits.cacheable = false;
    }
    ps::OperationRegistry registry;
    require(registry.register_operation(std::move(definition)).code ==
                ps::ErrorCode::InvalidArgument,
            "reject incompatible Result shared-block contract");
  }
  std::cout << "public Result block sharing: differing dtype/roles, "
               "content/coordinate "
               "keys, cache/proof fallback and invalid contracts passed\n";
}

std::uint64_t computed(const ps::DemandResult& answer) {
  std::uint64_t count = 0;
  for (const auto& timing : answer.diagnostics.operation_timings)
    count += timing.computed_elements;
  return count;
}
void changed_probability_and_source(ps::CpuNumericProfile profile) {
  Fixture fixture(
      take(ps::numeric::quantile_node(1, ps::WorkflowInputReference{1},
                                      ps::WorkflowInputReference{2}, 0,
                                      ps::ElementType::Float64, profile)),
      {array(ps::ElementType::Int64, {4}, {0, 10, 20, 30}),
       array(ps::ElementType::Float64, {1}, {0x3fd0000000000000})},
      false);
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  ps::ResourceBudget root;
  ps::ResultRef retained;
  {
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 65536;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(fixture.registry, config);
    root = take(context.resource_budget());
    auto bindings = fixture.bind(root);
    auto demand = take(context.open_demand(plan.plan, bindings));
    const ps::DemandQuery query{{"values", take(ps::Footprint::all({1}))}};
    retained = take(demand.request(query)).results.at("values");
    auto shared = take(demand.request(query));
    require(shared.results.at("values").object_id() == retained.object_id(),
            "same frozen demand shares Result identity");
    auto fresh_bindings = fixture.bind(root);
    auto fresh = take(context.freeze(plan.plan, fresh_bindings));
    auto warm = take(context.execute_fragments(fresh, query));
    require(warm.diagnostics.cache_hits > 0 &&
                warm.results.at("values").association() ==
                    ps::ResourceVector<std::uint64_t>{
                        fresh_bindings.inputs[0].result.object_id(),
                        fresh_bindings.inputs[1].result.object_id()},
            "completed cache rebound uses current source and q association");
    fixture.backing[1] =
        array(ps::ElementType::Float64, {1}, {0x3fe8000000000000});
    bindings = fixture.bind(root);
    require(demand.replace_bindings(bindings).ok(), "replace Result q");
    auto changed = take(demand.request(query));
    std::uint64_t bits = 0;
    require(rf::read(changed.results.at("values"), {0}, &bits, 8).ok() &&
                bits == 0x4036800000000000 &&
                changed.diagnostics.cache_hits == 0 && computed(changed) == 1,
            "changed q recomputes exact Whole quantile 22.5");
    require(changed.results.at("values").association().size() == 2 &&
                changed.results.at("values").association()[0] ==
                    bindings.inputs[0].result.object_id() &&
                changed.results.at("values").association()[1] ==
                    bindings.inputs[1].result.object_id(),
            "q replacement publication has current Result association");
    require(take(changed.dependencies.potential_dirty(
                     "input1", take(ps::Footprint::all({1}))))
                    .at("values") == query.at("values"),
            "current q dirty witness");
    fixture.backing[0] = array(ps::ElementType::Int64, {4}, {30, 10, 20, 0});
    bindings = fixture.bind(root);
    require(demand.replace_bindings(bindings).ok(), "replace Result source");
    changed = take(demand.request(query));
    require(rf::read(changed.results.at("values"), {0}, &bits, 8).ok() &&
                bits == 0x4036800000000000 &&
                changed.diagnostics.cache_hits == 0 && computed(changed) == 1,
            "changed source invalidates permutation despite identical sorted "
            "values");
  }
  std::uint64_t bits = 0;
  require(rf::read(retained, {0}, &bits, 8).ok() &&
              bits == 0x401e000000000000 &&
              root.statistics().live[ps::ResourceKind::Payload] == 8,
          "escaped Result quantile retains exactly eight owned payload bytes");
  retained = {};
  point_math_checks::released(root);
  std::cout << "completed Result cache, q/source replacement, current "
               "association and owner retirement passed\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "profile must be strict, apple or x86");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "oracle") {
      oracle(profile);
    } else {
      examples(profile);
      sharing_and_sparse(profile);
      nonlast_axis_lines(profile);
      probability_dependencies(profile);
      failure_and_schema(profile);
      typed_layout_environment(profile);
      block_contracts();
      changed_probability_and_source(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
