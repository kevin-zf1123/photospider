#include "photospider/numeric/sequences.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
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
const char* profile_suffix = "_strict";
void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  require(result.ok(), result.status().message);
  return result.take_value();
}

struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  std::vector<ps::Value> backing;
  unsigned other_reads = 0;

  Fixture(const std::string& key, ps::Value a, ps::Value b, std::int64_t count,
          const std::string& dtype, bool failing_other = false) {
    backing = {std::move(a), std::move(b)};
    rf::declare_sources(&document, backing);
    document.inputs[0].name = "start";
    document.inputs[1].name = "other";
    if (failing_other) {
      registry = ps::make_default_operation_registry(false);
      ps::OperationDefinition failure;
      failure.key = "manual.sequence_failure";
      failure.traits.input_count = 0;
      failure.traits.input_schema.clear();
      auto& output = failure.traits.outputs[0];
      output.output_schema.kind = ps::OperationPortKind::Result;
      output.result_schema = *document.inputs[1].result_schema;
      output.output_schema.result_schema_id = output.result_schema->id;
      output.output_schema.result_schema_version = 1;
      output.continuation_bytes = 1;
      output.maximum_dependency_stages = 1;
      output.region_rule = ps::OperationRegionRule::Whole;
      failure.start_result =
          [this](const auto&,
                 const auto&) -> ps::Result<ps::ResultContinuation> {
        ++other_reads;
        return ps::Result<ps::ResultContinuation>(
            ps::Status{ps::ErrorCode::OperationFailed,
                       "deliberately failing unneeded source"});
      };
      require(registry->register_operation(std::move(failure)).ok() &&
                  registry->freeze().ok(),
              "register Result sequence source");
    }
    std::string selected_key = key;
    if (key.size() >= 7 && key.substr(key.size() - 7) == "_strict")
      selected_key = key.substr(0, key.size() - 7) + profile_suffix;
    document.nodes = {
        {1,
         selected_key,
         {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
         {{"count", count}, {"dtype", dtype}}}};
    document.outputs = {{"values", 1, "values"}, {"axis", 1, "axis"}};
    if (failing_other) {
      document.inputs.pop_back();
      backing.pop_back();
      document.nodes[0].inputs[1] = ps::WorkflowNodeOutput{2, "value"};
      document.nodes.push_back({2, "manual.sequence_failure", {}, {}});
    }
  }

  ps::Result<ps::ExecutionResult> run(const std::string& port = {},
                                      std::uint64_t index = 0, bool one = false,
                                      bool empty = false) {
    if (!port.empty())
      document.outputs = {{port, 1, port}};
    ps::PlanningOptions planning;
    if (one)
      planning.output_regions[port] = ps::Region({{index, 1}});
    ps::GraphContext graph(document);
    auto compiled = ps::Compiler(registry).compile(graph, planning);
    if (!compiled.ok())
      return ps::Result<ps::ExecutionResult>(compiled.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 32 * 1024 * 1024;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext execution(registry, config);
    auto bindings = point_math_checks::bindings(
        take(execution.resource_budget()), backing, document);
    ps::ExecutionOptions options;
    options.dependencies.maximum_work = UINT64_C(1) << 50;
    options.maximum_dependency_work = UINT64_C(1) << 50;
    options.maximum_dependency_cache_work = 0;
    if (empty) {
      auto frozen = execution.freeze(compiled.value().plan, bindings);
      if (!frozen.ok())
        return ps::Result<ps::ExecutionResult>(frozen.status());
      const auto& step = compiled.value().plan.steps().at(
          compiled.value().plan.outputs().at(port));
      const auto shape = step.output_result_schema->tensors[0].sample_shape();
      auto demanded = execution.execute_fragments(
          frozen.value(), {{port, take(ps::Footprint::none(shape))}}, {},
          options);
      if (!demanded.ok())
        return ps::Result<ps::ExecutionResult>(demanded.status());
      auto completed = demanded.take_value();
      ps::ExecutionResult result;
      result.results = std::move(completed.results);
      result.dependencies = std::move(completed.dependencies);
      result.diagnostics = std::move(completed.diagnostics);
      return ps::Result<ps::ExecutionResult>(std::move(result));
    }
    return execution.execute(compiled.value().plan, bindings, {}, options);
  }
};
template <class T>
void exact(const ps::ResultRef& value, const std::vector<T>& expected,
           std::uint64_t first = 0) {
  require(ps::Value::element_size(
              value.schema().tensors[0].descriptor.element_type) == sizeof(T),
          "result sample width");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    T actual{};
    require(
        rf::read(value, {first + i}, &actual, sizeof(T)).ok() &&
            std::memcmp(&actual, &expected[i], sizeof(T)) == 0,
        "result bits differ at global coordinate " + std::to_string(first + i));
  }
  require(value.schema().tensors[0].facets.empty(),
          "numeric result must be generic");
}

ps::Value number(double x) {
  return ps::Value::from_float64(x);
}
ps::Value integer(std::int64_t x) {
  auto buffer = take(ps::MutableValue::allocate({ps::ElementType::Int64, {1}},
                                                ps::Region::whole({1}),
                                                ps::BufferAllocator{}));
  std::memcpy(buffer.data(), &x, 8);
  return take(std::move(buffer).publish());
}

void static_errors() {
  for (const auto count : {std::int64_t{0}, std::int64_t{1048577}}) {
    Fixture invalid("numeric.linspace_strict", number(0), number(1), count,
                    "float64");
    const auto status = invalid.run().status();
    require(status.code == ps::ErrorCode::InvalidArgument &&
                status.reason == ps::FailureReason::InvalidDomain &&
                status.detail.origin == ps::FailureOrigin::Schema,
            "invalid count must fail statically with schema attribution");
  }
  for (const auto& descriptor :
       {ps::ValueDescriptor{ps::ElementType::UInt8, {1}},
        ps::ValueDescriptor{ps::ElementType::Float64, {1, 1}}}) {
    auto storage = take(ps::MutableValue::allocate(
        descriptor, ps::Region::whole(descriptor.shape),
        ps::BufferAllocator{}));
    std::memset(storage.data(), 0,
                ps::Value::element_size(descriptor.element_type));
    Fixture bad_port("numeric.linspace_strict",
                     take(std::move(storage).publish()), number(1), 3,
                     "float64");
    const auto status = bad_port.run().status();
    require(status.code == ps::ErrorCode::TypeMismatch &&
                status.reason == ps::FailureReason::None &&
                status.detail.origin == ps::FailureOrigin::Schema,
            "port dtype/rank must fail with schema attribution");
  }
  Fixture mixed("numeric.arange_strict", integer(1), number(2), 3, "float64");
  const auto mixed_status = mixed.run().status();
  require(mixed_status.code == ps::ErrorCode::TypeMismatch &&
              mixed_status.reason == ps::FailureReason::None &&
              mixed_status.detail.origin == ps::FailureOrigin::Schema,
          "mixed integer/floating input must be rejected");
  Fixture unsupported("numeric.linspace_strict", number(0), number(1), 3,
                      "int64");
  const auto invalid_dtype = unsupported.run().status();
  require(invalid_dtype.code == ps::ErrorCode::TypeMismatch &&
              invalid_dtype.reason == ps::FailureReason::None &&
              invalid_dtype.detail.origin == ps::FailureOrigin::Schema,
          "unsupported output dtype must be rejected as a schema error");
  const auto invalid_helper =
      ps::numeric::linspace_node(
          1, ps::numeric::sequence_input(unsupported.document.inputs[0]),
          ps::numeric::sequence_input(unsupported.document.inputs[1]), 3,
          ps::ElementType::Int64)
          .status();
  require(invalid_helper.code == invalid_dtype.code &&
              invalid_helper.reason == invalid_dtype.reason &&
              invalid_helper.detail.origin == invalid_dtype.detail.origin,
          "helper and direct schema error classification must agree");
  Fixture missing("numeric.arange_strict", number(0), number(1), 3, "float64");
  missing.document.nodes[0].parameters.erase("count");
  require(missing.run().status().code == ps::ErrorCode::InvalidArgument,
          "missing count must be rejected");
  Fixture overflow("numeric.arange_strict", integer(INT64_MAX), integer(1), 2,
                   "int64");
  require(overflow.run("values", 0, true).status().reason ==
              ps::FailureReason::ArithmeticOverflow,
          "unrequested integer overflow fails Whole");
  require(overflow.run("values", 1, true).status().reason ==
              ps::FailureReason::ArithmeticOverflow,
          "requested integer overflow must fail");
  require(overflow.run("axis").status().reason ==
              ps::FailureReason::ArithmeticOverflow,
          "integer axis overflow must fail independently");
#if defined(__APPLE__) && defined(__aarch64__)
  const char* incompatible = "numeric.linspace_accelerated_x86_64";
#elif defined(__x86_64__)
  const char* incompatible = "numeric.linspace_accelerated_apple_silicon";
#else
  const char* incompatible = "numeric.linspace_accelerated_x86_64";
#endif
  Fixture wrong_platform(incompatible, number(0), number(1), 3, "float64");
  require(
      wrong_platform.run().status().code == ps::ErrorCode::BackendUnavailable,
      "incompatible explicit profile must fail");
}

void controls_and_ownership() {
  Fixture fixture("numeric.arange_strict", number(0), number(.5), 256,
                  "float64");
  fixture.document.outputs = {{"values", 1, "values"}};
  ps::GraphContext graph(fixture.document);
  ps::PlanningOptions planning;
  planning.output_regions["values"] = ps::Region({{0, 1}});
  auto compiled = take(ps::Compiler(fixture.registry).compile(graph, planning));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 65536;
  config.result_cache_bytes = 32768;
  config.managed_resources = ps::ResourceLimits{};
  ps::ResultRef retained;
  std::optional<ps::ResourceBudget> root;
  {
    ps::ExecutionContext execution(fixture.registry, config);
    root = take(execution.resource_budget());
    auto bindings =
        point_math_checks::bindings(*root, fixture.backing, fixture.document);
    auto demand = take(execution.open_demand(compiled.plan, bindings));
    const ps::DemandQuery query{
        {"values",
         take(ps::Footprint::from_regions({256}, {ps::Region({{0, 1}})}))}};
    auto result = take(demand.request(query));
    retained = result.results.at("values");
    double first = 1;
    require(rf::read(retained, {0}, &first, 8).ok(), "retained Result read");
    require(first == 0, "first value");
    auto shared = take(demand.request(query));
    require(shared.results.at("values").object_id() == retained.object_id(),
            "same Frozen shares the completed Result");
    auto fresh_bindings =
        point_math_checks::bindings(*root, fixture.backing, fixture.document);
    auto fresh = take(execution.freeze(compiled.plan, fresh_bindings));
    auto warm = take(execution.execute_fragments(fresh, query));
    require(warm.diagnostics.cache_hits >= 1,
            "fresh Frozen completed numeric cache hit");
    require(warm.results.at("values").association() ==
                ps::ResourceVector<std::uint64_t>{
                    fresh_bindings.inputs[0].result.object_id(),
                    fresh_bindings.inputs[1].result.object_id()},
            "cache replay associates current sources");
    bindings.inputs[1].result = point_math_checks::source(*root, number(1.0));
    require(demand.replace_bindings(bindings).ok(),
            "replace active step Result");
    auto unchanged = take(demand.request(query));
    require(
        unchanged.diagnostics.cache_hits == 0,
        "active step edit invalidates Whole even for index-zero projection");
    double projected = 1;
    require(rf::read(unchanged.results.at("values"), {0}, &projected, 8).ok() &&
                projected == 0,
            "Whole projected value");
    double last = 0;
    std::uint64_t computed = 0;
    for (const auto& timing : unchanged.diagnostics.operation_timings)
      computed += timing.computed_elements;
    require(rf::read(unchanged.results.at("values"), {255}, &last, 8).ok() &&
                last == 255 && computed == 256,
            "active step edit recomputes the entire published Result");
    ps::CancellationSource cancelled;
    cancelled.cancel();
    auto stopped =
        execution.execute(compiled.plan, bindings, cancelled.token());
    require(!stopped.ok() && stopped.status().code == ps::ErrorCode::Cancelled,
            "pre-cancelled sequence");
    require(root->statistics().peak[ps::ResourceKind::Payload] >= 2048,
            "one-index request accounts full output capacity");
  }
  double first = 1;
  require(rf::read(retained, {0}, &first, 8).ok(), "escaped Result read");
  require(first == 0, "escaped Whole owner");
  require(root->statistics().live[ps::ResourceKind::Payload] >= 2048,
          "result retains its admitted owner");
  retained = {};
  point_math_checks::released(*root);
  config.result_cache_bytes = 0;
  config.maximum_live_bytes = 32;
  ps::ExecutionContext tiny(fixture.registry, config);
  auto tiny_bindings = point_math_checks::bindings(
      take(tiny.resource_budget()), fixture.backing, fixture.document);
  auto denied = tiny.execute(compiled.plan, tiny_bindings);
  require(
      !denied.ok() && denied.status().code == ps::ErrorCode::ResourceExhausted,
      "state capacity must be admitted before allocation");

  fenv_t saved;
  require(fegetenv(&saved) == 0, "save caller floating environment");
  require(fesetround(FE_DOWNWARD) == 0 && feraiseexcept(FE_DIVBYZERO) == 0,
          "set caller floating environment");
  const auto flags = fetestexcept(FE_ALL_EXCEPT);
  Fixture rounding("numeric.linspace_strict", number(1),
                   number(0x1.0000020000001p0), 3, "float32");
  exact<float>(take(rounding.run("values", 1, true)).results.at("values"),
               {0x1.000002p0F}, 1);
  require(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == flags,
          "caller rounding and exception flags must be restored");
  require(fesetenv(&saved) == 0, "restore caller floating environment");
  std::vector<std::uint8_t> bytes(17);
  const double start = 2;
  std::memcpy(bytes.data() + 1, &start, 8);
  auto strided =
      take(ps::Value::create({ps::ElementType::Float64, {1}},
                             ps::Region::whole({1}), {1, {-8}}, bytes));
  Fixture layout("numeric.linspace_strict", strided, number(4), 3, "float64");
  auto control = std::make_shared<point_math_checks::Control>();
  control->rounding = FE_DOWNWARD;
  point_math_checks::Workflow workflow(layout.document.nodes[0],
                                       {strided, number(4)}, {}, control);
  exact<double>(take(workflow.run()).results.at("values"), {2, 3, 4});
  require(control->computation_polls > 0,
          "strided sequence uses checked worker");
  const auto int_schema = [](std::uint64_t id) {
    return ps::numeric::SequenceInput{
        ps::WorkflowInputReference{id},
        std::make_shared<ps::SchemaTemplate>(rf::source_schema(integer(0)))};
  };
  auto authored =
      take(ps::numeric::arange_node(2, int_schema(1), int_schema(2), 4));
  require(std::get<std::string>(authored.parameters.at("dtype")) == "int64" &&
              authored.operation == "numeric.arange_strict",
          "integer authoring defaults must be explicit");
  auto floating = take(ps::numeric::linspace_node(
      2, ps::numeric::sequence_input(fixture.document.inputs[0]),
      ps::numeric::sequence_input(fixture.document.inputs[1]), 3));
  require(std::get<std::string>(floating.parameters.at("dtype")) == "float64",
          "linspace authoring default");
  std::cout << "resource/work limits, cancellation, exact cache, owner "
               "lifetime, fenv, strided inputs, authoring defaults: passed\n";
}

void whole_budgets() {
  Fixture fixture("numeric.linspace_strict", number(0), number(1), 16384,
                  "float64");
  point_math_checks::resources(fixture.document.nodes[0],
                               {number(0), number(1)});
  std::cout << "Result callback-work/capacity/cancellation and all-Root "
               "release: passed\n";
}

void sequences() {
  Fixture line("numeric.linspace_strict", number(0), number(1), 5, "float64");
  auto result = take(line.run());
  std::cout << "profile=" << profile_suffix << " Whole numeric counters=N/A\n";
  exact<double>(result.results.at("values"), {0, .25, .5, .75, 1});
  exact<double>(result.results.at("axis"), {0, 1, .25});
  require(
      result.results.at("axis").schema().tensors[0].atomic_trailing_axes == 1,
      "axis keeps atomic tuple observation identity");
  Fixture partial_axis("numeric.linspace_strict", number(0), number(1), 5,
                       "float64");
  auto partial = take(partial_axis.run("axis", 1, true));
  exact<double>(partial.results.at("axis"), {0, 1, .25});
  auto changed = take(ps::Footprint::all({1}));
  auto dirty = take(partial.dependencies.potential_dirty("other", changed));
  require(dirty.at("axis") == take(ps::Footprint::all({3})),
          "tuple dirty mapping covers the complete atomic observation");
  ps::ResultTensorSpec grouped;
  grouped.key = "grouped";
  grouped.descriptor = {ps::ElementType::Float64, {2, 3, 4}};
  grouped.atomic_trailing_axes = 2;
  auto component = take(ps::Footprint::from_regions(
      {2, 3, 4}, {ps::Region({{1, 1}, {2, 1}, {1, 1}})}));
  require(grouped.observation_shape() == std::vector<std::uint64_t>{2},
          "trailing axes form one observation per leading coordinate");
  auto closure = take(grouped.close_samples(component));
  require(closure == take(ps::Footprint::from_regions(
                         {2, 3, 4}, {ps::Region({{1, 1}, {0, 3}, {0, 4}})})),
          "Result tensor trailing-axis complete tuple closure");
  Fixture reverse("numeric.linspace_strict", number(1), number(0), 5,
                  "float64");
  auto backwards = take(reverse.run());
  exact<double>(backwards.results.at("values"), {1, .75, .5, .25, 0});
  exact<double>(backwards.results.at("axis"), {1, 0, -.25});
  Fixture midpoint("numeric.linspace_strict", number(1),
                   number(0x1.0000020000001p0), 3, "float32");
  exact<float>(take(midpoint.run("values", 1, true)).results.at("values"),
               {0x1.000002p0F}, 1);
  for (const char* key : {"numeric.linspace_strict", "numeric.arange_strict"}) {
    Fixture singleton(key, number(-0.0), number(0), 1, "float64", true);
    auto only = take(singleton.run());
    exact<double>(only.results.at("values"), {-0.0});
    exact<double>(only.results.at("axis"), {-0.0, -0.0, 0.0});
    require(singleton.other_reads == 0, "singleton scheduled its unused input");
    Fixture endpoint(key, number(7), number(0), 5, "float64", true);
    auto required = endpoint.run("values", 0, true);
    require(
        !required.ok() && endpoint.other_reads == 1 &&
            required.status().message == "deliberately failing unneeded source",
        "non-singleton Whole values require end/step even at index zero");
    Fixture empty(key, number(0), number(1), 5, "float64", true);
    for (const char* port : {"values", "axis"}) {
      auto result = take(empty.run(port, 0, false, true));
      require(
          empty.other_reads == 0 && take(result.results.at(port).descriptor())
                                        .tensor_coverage(0)
                                        .empty(),
          "Empty has no producer start or published samples");
      for (const auto& timing : result.diagnostics.operation_timings)
        require(timing.computed_elements == 0, "Empty has no computation");
      for (const auto& input : take(result.dependencies.source_support()))
        require(input.second.empty(), "Empty has no input sample support");
    }
    Fixture static_other(key, number(0), integer(1), 1, "float64", true);
    auto schema_error = static_other.run();
    require(!schema_error.ok() &&
                schema_error.status().code == ps::ErrorCode::TypeMismatch &&
                static_other.other_reads == 0,
            "excluded second input retains static dtype validation");
    Fixture zeros(key, number(-0.0), number(-0.0), 3, "float64");
    exact<double>(take(zeros.run("values")).results.at("values"),
                  {-0.0, -0.0, -0.0});
  }
  const double maximum = std::numeric_limits<double>::max();
  Fixture extremes("numeric.linspace_strict", number(-maximum), number(maximum),
                   3, "float64");
  auto extreme = take(extremes.run());
  exact<double>(extreme.results.at("values"), {-maximum, 0, maximum});
  exact<double>(extreme.results.at("axis"), {-maximum, maximum, maximum});
  Fixture axis_overflow("numeric.linspace_strict", number(-maximum),
                        number(maximum), 2, "float64");
  exact<double>(take(axis_overflow.run("values")).results.at("values"),
                {-maximum, maximum});
  auto failed = axis_overflow.run("axis");
  require(!failed.ok() &&
              failed.status().reason == ps::FailureReason::ArithmeticOverflow,
          "unrepresentable axis must fail independently");
  Fixture ints("numeric.arange_strict", integer(3), integer(-2), 4, "int64");
  auto ir = take(ints.run());
  exact<std::int64_t>(ir.results.at("values"), {3, 1, -1, -3});
  exact<std::int64_t>(ir.results.at("axis"), {3, -3, -2});
  Fixture large("numeric.arange_strict", integer(9007199254740993LL),
                integer(1), 3, "int64");
  exact<std::int64_t>(
      take(large.run("values")).results.at("values"),
      {9007199254740993LL, 9007199254740994LL, 9007199254740995LL});
  Fixture cancellation("numeric.arange_strict", integer(INT64_MIN),
                       integer(INT64_MAX), 3, "int64");
  exact<std::int64_t>(
      take(cancellation.run("values", 2, true)).results.at("values"),
      {INT64_MAX - 1}, 2);
  Fixture progression("numeric.arange_strict", number(-maximum),
                      number(maximum), 3, "float64");
  exact<double>(take(progression.run("values")).results.at("values"),
                {-maximum, 0, maximum});
  std::cout << "NUM-02: values=[0,0.25,0.5,0.75,1] axis=[0,1,0.25]\n"
               "direct Float32 rounding, endpoint isolation, signed zero, "
               "integer exactness, extreme cancellation: passed\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    bool oracle = false;
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--oracle")
        oracle = true;
      else if (argument == "apple_silicon")
        profile_suffix = "_accelerated_apple_silicon";
      else if (argument == "x86_64")
        profile_suffix = "_accelerated_x86_64";
      else if (argument != "strict")
        throw std::runtime_error(
            "expected strict, apple_silicon, x86_64, or --oracle");
    }
    if (oracle) {
      std::string kind, dtype;
      std::uint64_t a = 0, b = 0, count = 0, index = 0;
      while (std::cin >> kind >> dtype >> std::hex >> a >> b >> std::dec >>
             count >> index) {
        double start = 0, other = 0;
        std::memcpy(&start, &a, 8);
        std::memcpy(&other, &b, 8);
        Fixture fixture("numeric." + kind + "_strict", number(start),
                        number(other), static_cast<std::int64_t>(count), dtype);
        auto result = fixture.run("values", index, true);
        if (!result.ok()) {
          std::cout << "error " << static_cast<int>(result.status().reason)
                    << '\n';
          continue;
        }
        const auto& value = result.value().results.at("values");
        std::uint64_t bits = 0;
        require(
            rf::read(value, {index}, &bits, dtype == "float32" ? 4 : 8).ok(),
            "oracle Result read");
        std::cout << std::hex << bits << std::dec << '\n';
      }
      return 0;
    }
    sequences();
    whole_budgets();
    static_errors();
    controls_and_ownership();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
