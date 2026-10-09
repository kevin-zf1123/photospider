#include "photospider/ops/numeric/reductions.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
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
template <class Answer>
void check_numeric(const Answer& result, ps::CpuNumericProfile profile,
                   std::uint64_t expected, bool executed = true) {
  std::uint64_t evaluated = 0;
  bool reported = false;
  for (const auto& timing : result.diagnostics.operation_timings) {
    const auto& numeric = timing.numeric;
    evaluated += numeric.evaluated_values;
    require(numeric.strict_fallbacks == 0 && numeric.strict_math_calls == 0 &&
                numeric.fallback_reasons == std::array<std::uint64_t, 4>{},
            "exact reduction reports no unperformed mathematical fallback");
    if (numeric.profile != ps::CpuNumericProfile::Unspecified) {
      reported = true;
      require(numeric.profile == profile && numeric.implementation[0] &&
                  !numeric.implementation.back(),
              "reduction reports selected profile and owning identity");
    }
  }
  require(evaluated == expected && (!executed || reported),
          "reduction counts accumulator inputs rather than output groups");
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
  Fixture(ps::WorkflowNode node, ps::Value value) : backing{std::move(value)} {
    rf::declare_sources(&document, backing);
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::ExecutionBindings bind(const ps::ResourceBudget& root) const {
    return point_math_checks::bindings(root, backing, document);
  }
  ps::Result<ps::DemandResult> run(const ps::Footprint& demand,
                                   const ps::CancellationToken& stop = {}) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.maximum_live_bytes = 131072;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto frozen = context.freeze(plan.value().plan,
                                 bind(take(context.resource_budget())));
    if (!frozen.ok())
      return ps::Result<ps::DemandResult>(frozen.status());
    return context.execute_fragments(frozen.value(), {{"values", demand}},
                                     stop);
  }
};
ps::WorkflowNode authored(const std::string& operation, ps::ElementType source,
                          ps::ElementType destination,
                          const std::vector<std::uint64_t>& axes,
                          std::int64_t ddof, ps::CpuNumericProfile profile) {
  const ps::WorkflowInput input = ps::WorkflowInputReference{1};
  if (operation == "sum")
    return take(ps::numeric::reduce_sum_node(1, input, axes, source,
                                             destination, profile));
  if (operation == "minimum")
    return take(ps::numeric::reduce_minimum_node(1, input, axes, profile));
  if (operation == "maximum")
    return take(ps::numeric::reduce_maximum_node(1, input, axes, profile));
  if (operation == "mean")
    return take(
        ps::numeric::reduce_mean_node(1, input, axes, destination, profile));
  if (operation == "count")
    return take(ps::numeric::reduce_count_node(1, input, axes, profile));
  if (operation == "variance")
    return take(ps::numeric::reduce_variance_node(1, input, axes, ddof,
                                                  destination, profile));
  require(operation == "std", "unknown reducer");
  return take(
      ps::numeric::reduce_std_node(1, input, axes, ddof, destination, profile));
}
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
  std::string operation, encoded_shape, encoded_axes;
  unsigned source = 0, destination = 0;
  std::int64_t ddof = 0;
  while (std::cin >> operation >> source >> destination >> ddof >>
         encoded_shape >> encoded_axes) {
    auto shape = list(encoded_shape);
    const auto axes = list(encoded_axes);
    std::uint64_t count = 1;
    for (auto size : shape)
      count *= size;
    std::vector<std::uint64_t> bits(count);
    for (auto& value : bits)
      std::cin >> std::hex >> value >> std::dec;
    const auto type = static_cast<ps::ElementType>(source);
    const auto target = static_cast<ps::ElementType>(destination);
    auto node =
        authored(operation, type, target, axes, ddof < 0 ? 0 : ddof, profile);
    if (operation == "variance" || operation == "std")
      node.parameters["ddof"] = ddof;
    Fixture fixture(std::move(node), array(type, shape, bits));
    for (auto axis : axes)
      shape[axis] = 1;
    auto all = take(ps::Footprint::all(shape));
    auto answer = fixture.run(all);
    if (!answer.ok()) {
      if (answer.status().reason == ps::FailureReason::ArithmeticOverflow)
        std::cout << "overflow\n";
      else if (answer.status().message.find("InvalidDegreesOfFreedom") !=
               std::string::npos)
        std::cout << "ddof\n";
      else
        throw std::runtime_error(answer.status().message);
      continue;
    }
    bool first = true;
    require(all.visit(
                   [&](const auto& coordinate) {
                     std::uint64_t value = 0;
                     auto status = rf::read(answer.value().results.at("values"),
                                            coordinate, &value,
                                            ps::Value::element_size(target));
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
    std::cout << '\n';
  }
}
void examples(ps::CpuNumericProfile profile) {
  fenv_t original;
  require(fegetenv(&original) == 0, "save caller fenv");
  const auto input = array(ps::ElementType::Int64, {2, 3}, {1, 2, 3, 4, 5, 6});
  const std::vector<std::string> operations = {
      "sum", "minimum", "maximum", "mean", "count", "variance", "std"};
  const std::vector<std::vector<std::uint64_t>> expected = {
      {6, 15},
      {1, 4},
      {3, 6},
      {0x4000000000000000, 0x4014000000000000},
      {3, 3},
      {0x3fe5555555555555, 0x3fe5555555555555},
      {0x3fea20bd700c2c3e, 0x3fea20bd700c2c3e}};
  for (std::size_t i = 0; i < operations.size(); ++i) {
    const auto type =
        i == 3 || i >= 5 ? ps::ElementType::Float64 : ps::ElementType::Int64;
    for (auto rounding :
         {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(fesetround(rounding) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  feraiseexcept(FE_DIVBYZERO) == 0,
              "set caller fenv");
      auto control = std::make_shared<point_math_checks::Control>();
      control->rounding = rounding;
      auto node = authored(operations[i], ps::ElementType::Int64, type, {1}, 0,
                           profile);
      point_math_checks::Workflow workflow(node, {input}, {}, control);
      auto answer = take(workflow.run());
      check_numeric(answer, profile, operations[i] == "count" ? 0 : 6);
      require(control->polls > 0 &&
                  (operations[i] == "count" || control->computation_polls > 0),
              "reduction fenv check enters actual worker continuation");
      for (std::uint64_t row = 0; row < 2; ++row) {
        std::uint64_t actual = 0;
        require(
            rf::read(answer.results.at("values"), {row, 0}, &actual, 8).ok() &&
                actual == expected[i][row],
            "public reduction example mismatch");
      }
      require(
          answer.results.at("values")
                      .schema()
                      .tensors[0]
                      .descriptor.element_type == type &&
              answer.results.at("values").schema().tensors[0].sample_shape() ==
                  std::vector<std::uint64_t>{2, 1},
          "keepdims dtype and shape");
      require(fegetround() == rounding &&
                  fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
              "preserve caller fenv");
    }
    std::cout << operations[i] << ": passed\n";
  }
  require(fesetenv(&original) == 0, "restore caller fenv");
}
struct SourceControl {
  unsigned starts = 0, polls = 0;
  std::uint64_t published = 0;
  ps::ResultRef prefix;
};
struct SourceProgram {
  std::shared_ptr<SourceControl> control;
  bool fail_tail;
  std::optional<ps::ResultBuilder> builder;
  SourceProgram(std::shared_ptr<SourceControl> state, bool fail)
      : control(std::move(state)), fail_tail(fail) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    ++control->polls;
    const auto& schema = *phase.query.output.result_schema;
    const auto shape = schema.tensors[0].sample_shape();
    if (fail_tail && control->polls > 1) {
      const auto failure =
          ps::Status{ps::ErrorCode::OperationFailed, "required tail after NaN"};
      builder->fail(failure);
      return ps::Result<ps::ResultProgramPoll>(failure);
    }
    require(phase.query.tensor_outputs &&
                *phase.query.tensor_outputs == take(ps::Footprint::all(shape)),
            "reduction producer receives complete source Need");
    builder.emplace(take(ps::ResultBuilder::start(phase.resources, schema,
                                                  phase.query.semantic_key)));
    require(builder
                ->bind_descriptor_relation(
                    take(ps::ResultRelation::cartesian(phase.resources, 1, {})))
                .ok(),
            "source descriptor relation");
    const auto count = fail_tail ? 128U : shape[0];
    const auto region = ps::Region({{0, count}});
    auto status = builder->publish_tensor_kernel(
        0, region,
        [&](const auto& writers) -> ps::Status {
          require(writers.size() == 1, "one source writer");
          for (std::uint64_t i = 0; i < count; ++i) {
            auto admitted = phase.consume_work(1);
            if (!admitted.ok())
              return admitted;
            const std::uint64_t bits =
                fail_tail ? (i ? 0 : UINT64_C(0x7ff0000000000011)) : i % 4;
            auto row = writers[0].row_run({i});
            if (!row.ok())
              return row.status();
            std::memcpy(row.value().data, &bits, 8);
          }
          return ps::Status::success();
        },
        take(ps::ResultRelation::cartesian(
            phase.resources, take(schema.tensors[0].sample_count()), {})),
        {true, true, true, true}, phase.query.cancellation);
    if (!status.ok())
      return ps::Result<ps::ResultProgramPoll>(status);
    control->published += count;
    if (fail_tail)
      control->prefix = builder->reference();
    return ps::Result<ps::ResultProgramPoll>(ps::ResultPublication{
        fail_tail ? builder->reference() : take(builder->seal()), !fail_tail});
  }
};
void source_node(Fixture* fixture,
                 const std::shared_ptr<SourceControl>& control,
                 bool fail_tail) {
  ps::OperationDefinition source;
  source.key = "manual.reduction_source";
  source.traits.input_count = 0;
  source.traits.input_schema.clear();
  auto& output = source.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.result_schema = *fixture->document.inputs[0].result_schema;
  if (fail_tail)
    output.result_schema->publication = ps::PublishPolicy::StablePrefix;
  output.output_schema.result_schema_id = output.result_schema->id;
  output.output_schema.result_schema_version = output.result_schema->version;
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.regional_atomic = true;
  output.continuation_bytes = sizeof(SourceProgram);
  output.maximum_dependency_stages = fail_tail ? 2 : 1;
  source.start_result = [control, fail_tail](const auto&,
                                             const auto& allocator) {
    ++control->starts;
    return ps::ResultContinuation::make<SourceProgram>(allocator, control,
                                                       fail_tail);
  };
  require(fixture->registry->register_operation(std::move(source)).ok(),
          "register Result producer");
  fixture->document.nodes[0].inputs = {ps::WorkflowNodeOutput{2, "value"}};
  fixture->document.nodes.push_back({2, "manual.reduction_source", {}, {}});
  fixture->document.inputs.clear();
  fixture->backing.clear();
}
void generated_groups(ps::CpuNumericProfile profile) {
  for (const std::string operation : {"sum", "mean", "variance", "std"}) {
    const auto destination =
        operation == "sum" ? ps::ElementType::Int64 : ps::ElementType::Float64;
    Fixture fixture(authored(operation, ps::ElementType::Int64, destination,
                             {0}, 0, profile),
                    array(ps::ElementType::Int64, {4096},
                          std::vector<std::uint64_t>(4096)));
    fixture.registry = ps::make_default_operation_registry(false);
    auto control = std::make_shared<SourceControl>();
    source_node(&fixture, control, false);
    require(fixture.registry->freeze().ok(), "freeze generated group registry");
    ps::ResourceBudget root;
    ps::ResultRef retained;
    const std::uint64_t expected =
        operation == "sum"        ? 6144
        : operation == "mean"     ? UINT64_C(0x3ff8000000000000)
        : operation == "variance" ? UINT64_C(0x3ff4000000000000)
                                  : UINT64_C(0x3ff1e3779b97f4a8);
    {
      ps::GraphContext graph(fixture.document);
      auto plan = take(ps::Compiler(fixture.registry).compile(graph));
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.result_cache_bytes = 0;
      config.maximum_live_bytes = 131072;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(fixture.registry, config);
      root = take(context.resource_budget());
      ps::ExecutionOptions options;
      options.dependencies.maximum_work = 128 * 1024 * 1024;
      options.maximum_dependency_work = 128 * 1024 * 1024;
      options.maximum_dependency_cache_work = 0;
      auto answer = take(context.execute(plan.plan, {}, {}, options));
      retained = answer.results.at("values");
      std::uint64_t actual = 0;
      require(rf::read(retained, {0}, &actual, 8).ok() && actual == expected &&
                  control->starts == 1 && control->polls == 1 &&
                  control->published == 4096,
              "4096-element generated Result exact reduction");
      const auto peak = root.statistics().peak[ps::ResourceKind::Payload];
      require(peak >= 4096 * 8 && peak <= 131072,
              "actual Root payload peak includes generated source and respects "
              "cap");
    }
    std::uint64_t actual = 0;
    require(rf::read(retained, {0}, &actual, 8).ok() && actual == expected &&
                root.statistics().live[ps::ResourceKind::Payload] == 8,
            "dense reduction owns eight bytes after generated source retires");
    retained = {};
    point_math_checks::released(root);
  }
  std::cout
      << "4096-element Result producer groups and bounded payload passed\n";
}
void metadata_count(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry(false);
  unsigned calls = 0;
  ps::OperationDefinition failed;
  failed.key = "manual.count_source";
  failed.traits.input_count = 0;
  failed.traits.input_schema.clear();
  auto& output = failed.traits.outputs[0];
  output.output_schema.kind = ps::OperationPortKind::Result;
  ps::SchemaTemplate schema;
  schema.id = "manual.count.source";
  ps::ResultTensorSpec member;
  member.key = "data";
  member.descriptor = {ps::ElementType::Int64,
                       {UINT64_C(1) << 20, UINT64_C(1) << 20}};
  schema.tensors.push_back(member);
  output.result_schema = schema;
  output.output_schema.result_schema_id = schema.id;
  output.output_schema.result_schema_version = schema.version;
  output.region_rule = ps::OperationRegionRule::Dependency;
  output.regional_atomic = true;
  output.continuation_bytes = 1;
  output.maximum_dependency_stages = 1;
  failed.start_result = [&](const auto&, const auto&) {
    ++calls;
    return ps::Result<ps::ResultContinuation>(ps::Status{
        ps::ErrorCode::OperationFailed, "count must not execute source"});
  };
  require(registry->register_operation(std::move(failed)).ok() &&
              registry->freeze().ok(),
          "count registry");
  ps::WorkflowDocument document;
  document.nodes = {{1, "manual.count_source", {}, {}},
                    take(ps::numeric::reduce_count_node(
                        2, ps::WorkflowNodeOutput{1, "value"}, {1}, profile))};
  document.outputs = {{"values", 2, "values"}};
  ps::GraphContext graph(document);
  auto plan = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 0;
  config.maximum_live_bytes = 4096;
  config.managed_resources = ps::ResourceLimits{};
  ps::ResourceBudget root;
  ps::ResultRef retained;
  ps::ResultTensorReadWindow window;
  {
    ps::ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    auto frozen = take(context.freeze(plan.plan, {}));
    auto answer = take(context.execute_fragments(
        frozen,
        {{"values", take(ps::Footprint::all({UINT64_C(1) << 20, 1}))}}));
    check_numeric(answer, profile, 0);
    retained = answer.results.at("values");
    const auto shape = retained.schema().tensors[0].sample_shape();
    window = take(retained.acquire_tensor(take(retained.descriptor()), 0,
                                          ps::Region::whole(shape)));
    std::int64_t first = 0, last = 0;
    require(
        rf::read(retained, {0, 0}, &first, 8).ok() &&
            rf::read(retained, {(UINT64_C(1) << 20) - 1, 0}, &last, 8).ok() &&
            first == 1048576 && last == first && calls == 0 &&
            take(window.row_run({0, 0})).data ==
                take(window.row_run({(UINT64_C(1) << 20) - 1, 0})).data &&
            root.statistics().live[ps::ResourceKind::Payload] == 8 &&
            retained.association().empty() &&
            take(answer.dependencies.source_observations()).empty(),
        "metadata count skips producer and owns eight-byte zero-stride output");
  }
  std::int64_t last = 0;
  require(rf::read(retained, {(UINT64_C(1) << 20) - 1, 0}, &last, 8).ok() &&
              last == 1048576,
          "metadata count survives context retirement");
  retained = {};
  std::memcpy(&last, take(window.row_run({0, 0})).data, 8);
  require(
      last == 1048576 && root.statistics().live[ps::ResourceKind::Payload] == 8,
      "escaped count read window pins eight-byte backing");
  window = {};
  point_math_checks::released(root);
  std::cout << "count: 2^40 logical input, 2^20 results, eight-byte owner, "
               "producer calls=0 passed\n";
}
void atom_isolation(ps::CpuNumericProfile profile) {
  Fixture fixture(
      take(ps::numeric::reduce_sum_node(1, ps::WorkflowInputReference{1}, {1},
                                        ps::ElementType::Int64, {}, profile)),
      array(ps::ElementType::Int64, {2, 2},
            {1, 2, UINT64_C(0x7fffffffffffffff), 1}));
  const auto first =
      take(ps::Footprint::from_regions({2, 1}, {ps::Region({{0, 1}, {0, 1}})}));
  auto failed = fixture.run(first);
  require(!failed.ok() &&
              failed.status().reason == ps::FailureReason::ArithmeticOverflow &&
              failed.status().detail.scope == ps::FailureScope::Run &&
              !failed.status().detail.atom,
          "unrequested group overflow fails complete Whole output");
  fixture.backing[0] = array(ps::ElementType::Int64, {2, 2}, {1, 2, 3, 4});
  auto answer = take(fixture.run(first));
  require(take(answer.dependencies.source_support()).at("input0") ==
              take(ps::Footprint::all({2, 2})),
          "Whole reducer full source support");
  std::int64_t second = 0;
  require(
      take(answer.results.at("values").descriptor()).tensor_coverage(0) ==
              take(ps::Footprint::all({2, 1})) &&
          rf::read(answer.results.at("values"), {1, 0}, &second, 8).ok() &&
          second == 7,
      "sparse reducer query retains complete output with global coordinates");
  const auto changed =
      take(ps::Footprint::from_regions({2, 2}, {ps::Region({{1, 1}, {0, 2}})}));
  require(take(answer.dependencies.potential_dirty("input0", changed))
                  .at("values") == first,
          "any group edit invalidates all recorded observations");
  std::cout << "Whole group support/dirty and Run overflow passed\n";
}

void boundaries(ps::CpuNumericProfile profile) {
  ps::ColorArrayDescriptor color;
  color.model = ps::ColorModel::Rgb;
  color.association = ps::ColorAssociation::Straight;
  color.primaries =
      take(ps::color_primary_coordinates(ps::ColorPrimaryPreset::Srgb))
          .primaries;
  color.transfer = ps::ColorTransfer{ps::ColorTransferKind::Linear, {}};
  for (const std::string operation :
       {"sum", "minimum", "maximum", "mean", "count", "variance", "std"}) {
    const auto destination = operation == "count" ? ps::ElementType::Int64
                             : operation == "minimum" || operation == "maximum"
                                 ? ps::ElementType::Float32
                                 : ps::ElementType::Float64;
    auto node = authored(operation, ps::ElementType::Float32, destination, {1},
                         0, profile);
    auto bad =
        array(ps::ElementType::Float32, {1, 2, 4},
              {0x3f800000, 0, 0, 0x40000000, 0x3f800000, 0, 0, 0x3f800000});
    Fixture fixture(node, bad);
    auto schema = rf::source_schema(bad);
    schema.tensors[0].facets = {take(ps::encode_color_array(color))};
    schema.tensors[0].atomic_trailing_axes = 1;
    fixture.document.inputs[0].result_schema =
        std::make_shared<ps::SchemaTemplate>(schema);
    const auto green = take(ps::Footprint::from_regions(
        {1, 1, 4}, {ps::Region({{0, 1}, {0, 1}, {1, 1}})}));
    auto empty = take(fixture.run(take(ps::Footprint::none({1, 1, 4}))));
    check_numeric(empty, profile, 0, false);
    require(take(empty.results.at("values").descriptor())
                .tensor_coverage(0)
                .empty(),
            "Empty reducer skips invalid typed payload");
    for (const auto& timing : empty.diagnostics.operation_timings)
      require(timing.computed_elements == 0,
              "Empty reducer performs no computation");
    for (const auto& support : take(empty.dependencies.source_support()))
      require(support.second.empty(),
              "Empty reducer has no source sample support");
    auto failed = fixture.run(green);
    if (operation == "count") {
      auto answer = take(std::move(failed));
      std::int64_t count = 0;
      require(
          rf::read(answer.results.at("values"), {0, 0, 1}, &count, 8).ok() &&
              count == 2 && answer.results.at("values").association().empty() &&
              take(answer.dependencies.source_observations()).empty(),
          "metadata-only count excludes invalid typed alpha");
    } else {
      require(!failed.ok() &&
                  failed.status().code == ps::ErrorCode::InvalidArgument &&
                  failed.status().reason == ps::FailureReason::InvalidDomain &&
                  failed.status().detail.input_id == 1,
              "numeric Whole reducer validates alpha outside requested green");
    }
    ps::CancellationSource cancelled;
    cancelled.cancel();
    require(fixture.run(green, cancelled.token()).status().code ==
                ps::ErrorCode::Cancelled,
            "pre-cancelled reducer precedes bad typed payload");
    fixture.backing[0] =
        array(ps::ElementType::Float32, {1, 2, 4},
              {0x3f800000, 0, 0, 0x3f800000, 0x3f800000, 0, 0, 0x3f800000});
    auto legal = take(fixture.run(green));
    require(take(legal.results.at("values").descriptor()).tensor_coverage(0) ==
                    take(ps::Footprint::all({1, 1, 4})) &&
                legal.results.at("values").schema().tensors[0].facets.empty(),
            "same-schema legal typed reducer publishes full untyped output");
    if (operation != "count") {
      require(take(legal.dependencies.source_support()).at("input0") ==
                  take(ps::Footprint::all({1, 2, 4})),
              "typed reducer full source support");
      const auto edit = take(ps::Footprint::from_regions(
          {1, 2, 4}, {ps::Region({{0, 1}, {1, 1}, {3, 1}})}));
      require(take(legal.dependencies.potential_dirty("input0", edit, 1))
                      .at("values") == green,
              "typed source edit invalidates observed sparse reducer query");
    }
    for (unsigned channel = 0; channel < 4; ++channel) {
      const auto expected =
          operation == "count"                            ? 2.0
          : operation == "variance" || operation == "std" ? 0.0
          : operation == "sum" ? (channel == 0 || channel == 3 ? 2.0 : 0.0)
                               : (channel == 0 || channel == 3 ? 1.0 : 0.0);
      if (destination == ps::ElementType::Float32) {
        float actual = 0;
        require(
            rf::read(legal.results.at("values"), {0, 0, channel}, &actual, 4)
                    .ok() &&
                actual == expected,
            "typed Float32 reduction positive control");
      } else if (destination == ps::ElementType::Int64) {
        std::int64_t actual = 0;
        require(
            rf::read(legal.results.at("values"), {0, 0, channel}, &actual, 8)
                    .ok() &&
                actual == expected,
            "typed count positive control");
      } else {
        double actual = 0;
        require(
            rf::read(legal.results.at("values"), {0, 0, channel}, &actual, 8)
                    .ok() &&
                actual == expected,
            "typed Float64 reduction positive control");
      }
    }
    if (operation != "count") {
      auto large = array(ps::ElementType::Float32, {16384},
                         std::vector<std::uint64_t>(16384, 0x3f800000));
      auto tested = authored(operation, ps::ElementType::Float32, destination,
                             {0}, 0, profile);
      point_math_checks::resources(tested, {large},
                                   ps::Value::element_size(destination));
    }
  }
  for (auto ddof : {-1, 2}) {
    auto node = authored("variance", ps::ElementType::Int64,
                         ps::ElementType::Float64, {1}, 0, profile);
    node.parameters["ddof"] = static_cast<std::int64_t>(ddof);
    Fixture fixture(node, array(ps::ElementType::Int64, {1, 2}, {1, 2}));
    const auto answer = fixture.run(take(ps::Footprint::all({1, 1})));
    require(!answer.ok() &&
                answer.status().code == ps::ErrorCode::InvalidArgument &&
                answer.status().detail.origin == ps::FailureOrigin::Schema,
            "invalid ddof preflight");
  }
  std::vector<std::uint64_t> bits(130);
  bits[2] = UINT64_C(0x7ff0000000000011);  // logical [0,1]
  bits[1] = UINT64_C(0xfff0000000000022);  // logical [1,0], earlier in storage
  auto packed = array(ps::ElementType::Float64, {130}, bits);
  auto strided = take(ps::Value::from_storage(
      {ps::ElementType::Float64, {2, 65}}, ps::Region::whole({2, 65}),
      {0, {8, 16}}, packed.storage()));
  for (const std::string operation :
       {"sum", "minimum", "maximum", "mean", "variance", "std"}) {
    auto node = authored(operation, ps::ElementType::Float64,
                         ps::ElementType::Float64, {1, 0}, 0, profile);
    const std::vector<ps::Value> inputs{strided};
    point_math_checks::Workflow workflow(node, inputs);
    auto answer = take(workflow.run()).results.at("values");
    std::uint64_t actual = 0;
    require(rf::read(answer, {0, 0}, &actual, 8).ok(),
            "strided NaN Result read");
    require(actual == UINT64_C(0x7ff8000000000011),
            "NaN priority follows logical order across strided windows");
  }
  for (unsigned mode = 0; mode < 3; ++mode) {
    const double data[] = {1, 2, 3, 4};
    auto buffer = take(ps::BufferAllocator{}.allocate(33));
    std::memcpy(buffer.data() + 1, data, 32);
    auto input = take(ps::Value::from_storage(
        {ps::ElementType::Float64, {2, 2}}, ps::Region::whole({2, 2}),
        {mode == 1 ? 25U : 1U,
         {mode == 1   ? -16
          : mode == 2 ? 0
                      : 16,
          mode == 1   ? -8
          : mode == 2 ? 0
                      : 8}},
        std::move(buffer).freeze()));
    auto node = authored("sum", ps::ElementType::Float64,
                         ps::ElementType::Float64, {1}, 0, profile);
    const std::vector<ps::Value> inputs{input};
    point_math_checks::Workflow workflow(node, inputs);
    auto result = take(workflow.run()).results.at("values");
    for (unsigned row = 0; row < 2; ++row) {
      double actual = 0;
      require(rf::read(result, {row, 0}, &actual, 8).ok(),
              "strided group read");
      require(actual == (mode == 2   ? 2
                         : mode == 1 ? 7 - 4 * row
                                     : 3 + 4 * row),
              "unaligned negative/zero-stride reduction groups");
    }
  }
  std::cout << "all seven reducers: typed closure, Empty, cancellation; ddof "
               "preflight and strided NaN priority passed\n";
}

void cache_and_lifetime(ps::CpuNumericProfile profile) {
  Fixture fixture(authored("sum", ps::ElementType::Int64,
                           ps::ElementType::Int64, {1}, 0, profile),
                  array(ps::ElementType::Int64, {2, 2}, {1, 2, 3, 4}));
  ps::GraphContext graph(fixture.document);
  auto plan = take(ps::Compiler(fixture.registry).compile(graph));
  const auto query =
      take(ps::Footprint::from_regions({2, 1}, {ps::Region({{0, 1}, {0, 1}})}));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 65536;
  config.managed_resources = ps::ResourceLimits{};
  ps::ResourceBudget root;
  ps::ResultRef retained;
  {
    ps::ExecutionContext context(fixture.registry, config);
    root = take(context.resource_budget());
    auto bindings = fixture.bind(root);
    auto demand = take(context.open_demand(plan.plan, bindings));
    auto cold = take(demand.request({{"values", query}}));
    check_numeric(cold, profile, 4);
    retained = cold.results.at("values");
    auto shared = take(demand.request({{"values", query}}));
    check_numeric(shared, profile, 0, false);
    require(shared.results.at("values").object_id() == retained.object_id(),
            "same Frozen shares reducer identity");
    auto fresh_bindings = fixture.bind(root);
    auto fresh = take(context.freeze(plan.plan, fresh_bindings));
    auto warm = take(context.execute_fragments(fresh, {{"values", query}}));
    check_numeric(warm, profile, 0, false);
    require(warm.diagnostics.cache_hits > 0 &&
                warm.results.at("values").association() ==
                    ps::ResourceVector<std::uint64_t>{
                        fresh_bindings.inputs[0].result.object_id()},
            "reducer completed cache replays with current source association");
    bindings.inputs[0].result = point_math_checks::source(
        root, array(ps::ElementType::Int64, {2, 2}, {1, 2, 5, 6}));
    require(demand.replace_bindings(bindings).ok(),
            "replace unrequested reducer group");
    auto unrequested = take(demand.request({{"values", query}}));
    check_numeric(unrequested, profile, 4);
    std::int64_t first = 0, second = 0;
    std::uint64_t computed = 0;
    for (const auto& timing : unrequested.diagnostics.operation_timings)
      computed += timing.computed_elements;
    require(
        unrequested.diagnostics.cache_hits == 0 && computed == 2 &&
            rf::read(unrequested.results.at("values"), {0, 0}, &first, 8)
                .ok() &&
            first == 3 &&
            rf::read(unrequested.results.at("values"), {1, 0}, &second, 8)
                .ok() &&
            second == 11,
        "unrequested group edit invalidates and recomputes complete reduction");
    bindings.inputs[0].result = point_math_checks::source(
        root, array(ps::ElementType::Int64, {2, 2}, {9, 2, 5, 6}));
    require(demand.replace_bindings(bindings).ok(),
            "replace observed reducer group");
    auto changed = take(demand.request({{"values", query}}));
    require(rf::read(changed.results.at("values"), {0, 0}, &first, 8).ok() &&
                first == 11,
            "observed reducer group replacement updates numerical result");
  }
  std::int64_t original = 0;
  require(rf::read(retained, {1, 0}, &original, 8).ok() && original == 7 &&
              root.statistics().live[ps::ResourceKind::Payload] == 16,
          "escaped dense reduction preserves original two-group output");
  retained = {};
  point_math_checks::released(root);
  std::cout << "reduction completed cache/current source association, rebind "
               "and owner lifetime passed\n";
}
void tail_failure(ps::CpuNumericProfile profile) {
  for (const std::string operation :
       {"sum", "minimum", "maximum", "mean", "variance", "std"}) {
    Fixture fixture(authored(operation, ps::ElementType::Float64,
                             ps::ElementType::Float64, {0}, 0, profile),
                    array(ps::ElementType::Float64, {129},
                          std::vector<std::uint64_t>(129)));
    fixture.registry = ps::make_default_operation_registry(false);
    auto control = std::make_shared<SourceControl>();
    auto numeric = std::make_shared<point_math_checks::Control>();
    fixture.document.nodes[0] = point_math_checks::checked_node(
        fixture.registry, fixture.document.nodes[0], numeric);
    source_node(&fixture, control, true);
    require(fixture.registry->freeze().ok(), "freeze partial source registry");
    ps::ResourceBudget root;
    {
      ps::GraphContext graph(fixture.document);
      auto plan = take(ps::Compiler(fixture.registry).compile(graph));
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.result_cache_bytes = 0;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(fixture.registry, config);
      root = take(context.resource_budget());
      auto result = context.execute(plan.plan);
      require(
          !result.ok() &&
              result.status().message == "required tail after NaN" &&
              control->starts == 1 && control->polls == 2 &&
              control->published == 128 && numeric->computation_polls == 0,
          "published NaN prefix cannot suppress required tail source failure");
    }
    {
      const auto facts = take(control->prefix.descriptor(false));
      std::uint64_t first = 0, last = 1;
      require(
          control->prefix.production_status().code ==
                  ps::ErrorCode::OperationFailed &&
              facts.tensor_coverage(0) ==
                  take(ps::Footprint::from_regions({129},
                                                   {ps::Region({{0, 128}})})) &&
              control->prefix.read_tensor(facts, 0, {0}, &first, 8).ok() &&
              first == UINT64_C(0x7ff0000000000011) &&
              control->prefix.read_tensor(facts, 0, {127}, &last, 8).ok() &&
              last == 0 &&
              !control->prefix.read_tensor(facts, 0, {128}, &last, 8).ok() &&
              root.statistics().live[ps::ResourceKind::Payload] == 1024,
          "certified NaN prefix stays readable after tail failure and context "
          "retirement");
    }
    control->prefix = {};
    point_math_checks::released(root);
  }
  std::cout << "all numeric reducers require full source after a published NaN "
               "prefix\n";
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
      generated_groups(profile);
      metadata_count(profile);
      atom_isolation(profile);
      boundaries(profile);
      cache_and_lifetime(profile);
      tail_failure(profile);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
