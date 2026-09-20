#include "photospider/numeric/sequences.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <atomic>
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
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
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
  ps::ExecutionBindings bindings;
  unsigned other_reads = 0;

  Fixture(const std::string& key, ps::Value a, ps::Value b, std::int64_t count,
          const std::string& dtype, bool failing_other = false) {
    const std::vector<ps::Value> inputs{a, b};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto name = i ? "other" : "start";
      const auto& value = inputs[i];
      document.inputs.push_back({i + 1,
                                 name,
                                 value.descriptor(),
                                 value.region(),
                                 value.layout(),
                                 {}});
      if (i && failing_other) {
        auto source = std::make_shared<ps::RegionalSource>();
        source->descriptor = value.descriptor();
        source->read = [this](const auto&, auto*, auto, const auto&,
                              const auto&) -> ps::Result<ps::Region> {
          ++other_reads;
          return ps::Result<ps::Region>(
              ps::Status{ps::ErrorCode::OperationFailed,
                         "deliberately failing unneeded source"});
        };
        bindings.inputs.push_back({name, {}, source});
      } else {
        bindings.inputs.push_back({name, value});
      }
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
  }

  ps::Result<ps::ExecutionResult> run(const std::string& port = {},
                                      std::uint64_t index = 0,
                                      bool one = false) {
    if (!port.empty())
      document.outputs = {{port, 1, port}};
    ps::PlanningOptions planning;
    if (one)
      planning.output_regions[port] = ps::Region({{index, 1}});
    ps::GraphContext graph(document);
    auto compiled = ps::Compiler(registry).compile(graph, planning);
    if (!compiled.ok())
      return ps::Result<ps::ExecutionResult>(compiled.status());
    ps::ExecutionContext execution(registry);
    return execution.execute(compiled.value().plan, bindings);
  }
};
template <class T>
void exact(const ps::Value& value, const std::vector<T>& expected) {
  require(value.bytes().size() == expected.size() * sizeof(T), "result size");
  require(std::memcmp(value.bytes().data(), expected.data(),
                      value.bytes().size()) == 0,
          "result bits differ from independent expected values");
  require(value.facets().empty(), "numeric result must be generic");
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
  ps::InputSnapshotStore snapshots;
  for (auto& binding : fixture.bindings.inputs) {
    binding.snapshot = std::make_shared<const ps::InputSnapshot>(
        take(snapshots.import_value(binding.value)));
    binding.value = {};
  }
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_live_bytes = 65536;
  config.result_cache_bytes = 32768;
  config.managed_resources = ps::ResourceLimits{};
  ps::Value retained;
  std::optional<ps::ResourceBudget> root;
  {
    ps::ExecutionContext execution(fixture.registry, config);
    root = take(execution.resource_budget());
    auto demand = take(execution.open_demand(compiled.plan, fixture.bindings));
    const ps::DemandQuery query{
        {"values",
         take(ps::Footprint::from_regions({256}, {ps::Region({{0, 1}})}))}};
    auto result = take(demand.request(query));
    retained = result.values.at("values").fragments().at(0);
    double first = 1;
    std::memcpy(&first, retained.bytes().data(), 8);
    require(first == 0, "first value");
    auto warm = take(demand.request(query));
    require(warm.diagnostics.cache_hits >= 1, "warm exact numeric cache");
    fixture.bindings.inputs[1].snapshot =
        std::make_shared<const ps::InputSnapshot>(
            take(snapshots.import_value(number(1.0))));
    require(demand.replace_bindings(fixture.bindings).ok(),
            "replace unused sequence binding");
    auto unchanged = take(demand.request(query));
    require(
        unchanged.diagnostics.cache_hits == 0,
        "active step edit invalidates Whole even for index-zero projection");
    double projected = 1;
    require(unchanged.values.at("values").read({0}, &projected, 8).ok() &&
                projected == 0,
            "Whole projected value");
    ps::CancellationSource cancelled;
    cancelled.cancel();
    auto stopped =
        execution.execute(compiled.plan, fixture.bindings, cancelled.token());
    require(!stopped.ok() && stopped.status().code == ps::ErrorCode::Cancelled,
            "pre-cancelled sequence");
    require(root->statistics().peak[ps::ResourceKind::Payload] >= 2048,
            "one-index request accounts full output capacity");
  }
  double first = 1;
  std::memcpy(&first, retained.bytes().data(), 8);
  require(first == 0, "escaped Whole owner");
  require(root->statistics().live[ps::ResourceKind::Payload] >= 8,
          "result retains its admitted owner");
  retained = {};
  require(root->statistics().live[ps::ResourceKind::Payload] == 0,
          "final owner must release payload");
  config.result_cache_bytes = 0;
  config.maximum_live_bytes = 32;
  ps::ExecutionContext tiny(fixture.registry, config);
  auto denied = tiny.execute(compiled.plan, fixture.bindings);
  require(
      !denied.ok() && denied.status().code == ps::ErrorCode::ResourceExhausted,
      "state capacity must be admitted before allocation");
  config.maximum_live_bytes = 65536;
  config.managed_resources->maximum_work = 20;
  ps::ExecutionContext bounded(fixture.registry, config);
  auto exhausted = bounded.execute(compiled.plan, fixture.bindings);
  require(!exhausted.ok() &&
              exhausted.status().reason == ps::FailureReason::WorkLimit,
          "numeric work must retain root WorkLimit: " +
              exhausted.status().message + " reason=" +
              std::to_string(static_cast<int>(exhausted.status().reason)));

  fenv_t saved;
  require(fegetenv(&saved) == 0, "save caller floating environment");
  require(fesetround(FE_DOWNWARD) == 0 && feraiseexcept(FE_DIVBYZERO) == 0,
          "set caller floating environment");
  const auto flags = fetestexcept(FE_ALL_EXCEPT);
  Fixture rounding("numeric.linspace_strict", number(1),
                   number(0x1.0000020000001p0), 3, "float32");
  exact<float>(take(rounding.run("values", 1, true)).values.at("values"),
               {0x1.000002p0F});
  require(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == flags,
          "caller rounding and exception flags must be restored");
  require(fesetenv(&saved) == 0, "restore caller floating environment");
  const std::vector<ps::Region> demands(2, ps::Region::whole({1}));
  std::vector<std::uint8_t> bytes(17);
  const double start = 2;
  std::memcpy(bytes.data() + 1, &start, 8);
  auto strided =
      take(ps::Value::create({ps::ElementType::Float64, {1}},
                             ps::Region::whole({1}), {1, {-8}}, bytes));
  const std::vector<ps::Value> inputs{strided, number(4)};
  const std::map<std::string, ps::ParameterValue> parameters{
      {"count", std::int64_t{3}},
      {"dtype", std::string("float64")}};
  ps::OperationInvocation call(inputs, demands, parameters, ps::Backend::Cpu,
                               {}, ps::Region::whole({3}));
  exact<double>(take(fixture.registry->invoke(
                    std::string("numeric.linspace") + profile_suffix, call)),
                {2, 3, 4});
  auto authored = take(ps::numeric::arange_node(
      2, {ps::WorkflowInputReference{1}, {ps::ElementType::Int64, {1}}},
      {ps::WorkflowInputReference{2}, {ps::ElementType::Int64, {1}}}, 4));
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
  std::vector<ps::Value> inputs{number(0), number(1)};
  std::vector<ps::Region> demands(2, ps::Region::whole({1}));
  const auto& node = fixture.document.nodes[0];
  auto traits = take(fixture.registry->resolve_traits(
      node.operation,
      {{inputs[0].descriptor(), {}}, {inputs[1].descriptor(), {}}},
      node.parameters));
  for (unsigned mode = 0; mode < 3; ++mode) {
    ps::ResourceLimits limits;
    if (mode == 0)
      limits.maximum_work = 10000;
    if (mode == 1)
      limits.capacity[ps::ResourceKind::Payload] = 65536;
    if (mode == 2)
      limits.capacity[ps::ResourceKind::Payload] =
          16384 * 8 + traits.workspace_bytes - 1;
    ps::ResourceBudget budget(limits);
    {
      ps::ResourceAllocationScope scope(budget);
      ps::OperationInvocation call(
          inputs, demands, node.parameters, ps::Backend::Cpu, {},
          ps::Region::whole({16384}), budget.allocator());
      auto result = fixture.registry->invoke(node.operation, call);
      require(
          !result.ok() &&
              result.status().code == ps::ErrorCode::ResourceExhausted,
          "Whole sequence rejects insufficient work/output/scratch capacity");
    }
    require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
            "sequence failure releases unpublished output/scratch");
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
        inputs, demands, node.parameters, ps::Backend::Cpu,
        cancellation.token(), ps::Region::whole({16384}), budget.allocator());
    status = fixture.registry->invoke(node.operation, call).status();
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
          "cancel admitted sequence arithmetic and release full storage");
}

void sequences() {
  Fixture line("numeric.linspace_strict", number(0), number(1), 5, "float64");
  auto result = take(line.run());
  std::cout << "profile=" << profile_suffix << " Whole numeric counters=N/A\n";
  exact<double>(result.values.at("values"), {0, .25, .5, .75, 1});
  exact<double>(result.values.at("axis"), {0, 1, .25});
  auto traits = take(line.registry->resolve_traits(
      line.document.nodes[0].operation,
      {{number(0).descriptor(), {}}, {number(1).descriptor(), {}}},
      line.document.nodes[0].parameters));
  require(traits.outputs[1].atomic_trailing_axes == 1 &&
              traits.outputs[1].region_rule == ps::OperationRegionRule::Whole,
          "axis keeps tuple observation identity with Whole execution");
  Fixture partial_axis("numeric.linspace_strict", number(0), number(1), 5,
                       "float64");
  auto partial = take(partial_axis.run("axis", 1, true));
  exact<double>(partial.values.at("axis"), {1});
  auto changed = take(ps::Footprint::all({1}));
  auto dirty = take(partial.dependencies.potential_dirty("other", changed));
  require(dirty.at("axis") ==
              take(ps::Footprint::from_regions({3}, {ps::Region({{1, 1}})})),
          "tuple dirty mapping must restrict to actual root coverage");
  const std::vector<ps::Value> direct_inputs{number(0), number(1)};
  const std::vector<ps::Region> direct_demands(2, ps::Region::whole({1}));
  const auto parameters = line.document.nodes[0].parameters;
  ps::OperationInvocation direct(direct_inputs, direct_demands, parameters,
                                 ps::Backend::Cpu, {}, ps::Region({{1, 1}}));
  direct.output_index = 1;
  require(!line.registry->invoke(line.document.nodes[0].operation, direct).ok(),
          "direct Whole axis rejects ROI");
  ps::OperationMetadata grouped{{ps::ElementType::Float64, {2, 3, 4}},
                                {},
                                {},
                                2};
  auto component = take(ps::Footprint::from_regions(
      {2, 3, 4}, {ps::Region({{1, 1}, {2, 1}, {1, 1}})}));
  auto observation = take(ps::operation_observations(grouped, component));
  require(observation ==
              take(ps::Footprint::from_regions({2}, {ps::Region({{1, 1}})})),
          "trailing-axis observation projection");
  auto closure = take(ps::observation_samples(grouped, observation));
  require(closure == take(ps::Footprint::from_regions(
                         {2, 3, 4}, {ps::Region({{1, 1}, {0, 3}, {0, 4}})})),
          "trailing-axis complete tuple closure");
  Fixture reverse("numeric.linspace_strict", number(1), number(0), 5,
                  "float64");
  auto backwards = take(reverse.run());
  exact<double>(backwards.values.at("values"), {1, .75, .5, .25, 0});
  exact<double>(backwards.values.at("axis"), {1, 0, -.25});
  Fixture midpoint("numeric.linspace_strict", number(1),
                   number(0x1.0000020000001p0), 3, "float32");
  exact<float>(take(midpoint.run("values", 1, true)).values.at("values"),
               {0x1.000002p0F});
  for (const char* key : {"numeric.linspace_strict", "numeric.arange_strict"}) {
    Fixture singleton(key, number(-0.0), number(0), 1, "float64", true);
    auto only = take(singleton.run());
    exact<double>(only.values.at("values"), {-0.0});
    exact<double>(only.values.at("axis"), {-0.0, -0.0, 0.0});
    require(singleton.other_reads == 0, "singleton scheduled its unused input");
    Fixture endpoint(key, number(7), number(0), 5, "float64", true);
    require(!endpoint.run("values", 0, true).ok() && endpoint.other_reads > 0,
            "non-singleton Whole values require end/step even at index zero");
    Fixture zeros(key, number(-0.0), number(-0.0), 3, "float64");
    exact<double>(take(zeros.run("values")).values.at("values"),
                  {-0.0, -0.0, -0.0});
  }
  const double maximum = std::numeric_limits<double>::max();
  Fixture extremes("numeric.linspace_strict", number(-maximum), number(maximum),
                   3, "float64");
  auto extreme = take(extremes.run());
  exact<double>(extreme.values.at("values"), {-maximum, 0, maximum});
  exact<double>(extreme.values.at("axis"), {-maximum, maximum, maximum});
  Fixture axis_overflow("numeric.linspace_strict", number(-maximum),
                        number(maximum), 2, "float64");
  exact<double>(take(axis_overflow.run("values")).values.at("values"),
                {-maximum, maximum});
  auto failed = axis_overflow.run("axis");
  require(!failed.ok() &&
              failed.status().reason == ps::FailureReason::ArithmeticOverflow,
          "unrepresentable axis must fail independently");
  Fixture ints("numeric.arange_strict", integer(3), integer(-2), 4, "int64");
  auto ir = take(ints.run());
  exact<std::int64_t>(ir.values.at("values"), {3, 1, -1, -3});
  exact<std::int64_t>(ir.values.at("axis"), {3, -3, -2});
  Fixture large("numeric.arange_strict", integer(9007199254740993LL),
                integer(1), 3, "int64");
  exact<std::int64_t>(
      take(large.run("values")).values.at("values"),
      {9007199254740993LL, 9007199254740994LL, 9007199254740995LL});
  Fixture cancellation("numeric.arange_strict", integer(INT64_MIN),
                       integer(INT64_MAX), 3, "int64");
  exact<std::int64_t>(
      take(cancellation.run("values", 2, true)).values.at("values"),
      {INT64_MAX - 1});
  Fixture progression("numeric.arange_strict", number(-maximum),
                      number(maximum), 3, "float64");
  exact<double>(take(progression.run("values")).values.at("values"),
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
        const auto& value = result.value().values.at("values");
        std::uint64_t bits = 0;
        std::memcpy(&bits, value.bytes().data(), value.bytes().size());
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
