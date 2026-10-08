#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <atomic>
#include <chrono>
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

#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/lowpass.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)
#include "result_fixture.hpp"     // NOLINT(build/include_subdir)

namespace {
using numeric_result_fixture::read;
using numeric_result_fixture::source_schema;
using point_math_checks::source;
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
  std::vector<ps::Value> sources;
  Fixture(ps::WorkflowNode node, const std::vector<ps::Value>& inputs) {
    sources = inputs;
    numeric_result_fixture::declare_sources(&document, inputs);
    document.outputs = {{"values", node.id, "values"}};
    document.nodes = {std::move(node)};
  }
  ps::Result<ps::DemandResult> run(const ps::DemandQuery& query,
                                   bool cache = true,
                                   std::uint64_t proof_work = UINT64_C(64) *
                                                              1024 * 1024,
                                   std::uint64_t cache_bytes = 65536) {
    ps::GraphContext graph(document);
    auto plan = ps::Compiler(registry).compile(graph);
    if (!plan.ok())
      return ps::Result<ps::DemandResult>(plan.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_live_bytes = 4 * 1024 * 1024;
    config.result_cache_bytes = cache ? cache_bytes : 0;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto bindings = point_math_checks::bindings(take(context.resource_budget()),
                                                sources, document);
    auto snapshot = context.freeze(plan.value().plan, bindings);
    if (!snapshot.ok())
      return ps::Result<ps::DemandResult>(snapshot.status());
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(4096) * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(2048) * 1024 * 1024;
    options.maximum_dependency_cache_work = cache ? proof_work : 0;
    return context.execute_fragments(snapshot.value(), query, {}, options);
  }
};
std::uint64_t raw(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, 8);
  return bits;
}
ps::Value doubles(const std::vector<std::uint64_t>& shape,
                  const std::vector<double>& samples) {
  std::vector<std::uint64_t> bits;
  for (auto value : samples)
    bits.push_back(raw(value));
  return array(ps::ElementType::Float64, shape, bits);
}
ps::Footprint region(const std::vector<std::uint64_t>& shape,
                     std::vector<ps::Region> boxes) {
  return take(ps::Footprint::from_regions(shape, std::move(boxes)));
}
ps::ResultRef direct(const std::shared_ptr<ps::OperationRegistry>& registry,
                     const ps::WorkflowNode& node,
                     const std::vector<ps::Value>& inputs,
                     const ps::Footprint& outputs,
                     std::shared_ptr<point_math_checks::Control> control = {}) {
  Fixture fixture(node, inputs);
  fixture.registry = registry;
  if (control) {
    fixture.registry = ps::make_default_operation_registry(false);
    fixture.document.nodes[0] = point_math_checks::checked_node(
        fixture.registry, fixture.document.nodes[0], control);
    require(fixture.registry->freeze().ok(), "freeze checked lowpass registry");
  }
  fixture.document.outputs = {
      {"filtered", node.id,
       node.operation.find("nonuniform") != std::string::npos ? "samples"
                                                              : "values"}};
  return take(fixture.run({{"filtered", outputs}}, false))
      .results.at("filtered");
}
ps::Value reversed_unaligned(const ps::Value& value) {
  auto bytes = value.bytes();
  const auto width = ps::Value::element_size(value.descriptor().element_type);
  const auto count = bytes.size() / width;
  auto owner = take(ps::BufferAllocator{}.allocate(bytes.size() + 1));
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(owner.data() + 1 + (count - 1 - i) * width,
                bytes.data() + i * width, width);
  std::vector<std::int64_t> strides(value.descriptor().shape.size());
  std::int64_t stride = -static_cast<std::int64_t>(width);
  for (std::size_t i = strides.size(); i; --i) {
    strides[i - 1] = stride;
    stride *= value.descriptor().shape[i - 1];
  }
  return take(ps::Value::from_storage(value.descriptor(), value.region(),
                                      {1 + (count - 1) * width, strides},
                                      std::move(owner).freeze()));
}
ps::WorkflowNode authored(bool continuous, unsigned kernel,
                          ps::CpuNumericProfile profile, unsigned axis = 0) {
  using namespace ps::numeric;  // NOLINT(build/namespaces)
  if (continuous) {
    if (kernel == 3)
      return take(lowpass_nonuniform_kaiser_sinc_node(
          1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}, axis,
          .5, .25, 2, LowpassBoundary::Reflect, profile));
    const auto helper = kernel == 0   ? lowpass_nonuniform_hann_sinc_node
                        : kernel == 1 ? lowpass_nonuniform_hamming_sinc_node
                        : kernel == 2 ? lowpass_nonuniform_blackman_sinc_node
                                      : lowpass_nonuniform_gaussian_node;
    return take(helper(
        1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}, axis,
        .5, kernel == 4 ? 1. : .25, LowpassBoundary::Reflect, profile));
  }
  if (kernel == 3)
    return take(lowpass_uniform_kaiser_sinc_node(
        1, ps::WorkflowInputReference{1}, axis, 2, .25, 2,
        LowpassBoundary::Reflect, profile));
  const auto helper = kernel == 0   ? lowpass_uniform_hann_sinc_node
                      : kernel == 1 ? lowpass_uniform_hamming_sinc_node
                      : kernel == 2 ? lowpass_uniform_blackman_sinc_node
                                    : lowpass_uniform_gaussian_node;
  return take(helper(1, ps::WorkflowInputReference{1}, axis, 2,
                     kernel == 4 ? 1. : .25, LowpassBoundary::Reflect,
                     profile));
}
void layouts(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  for (bool continuous : {false, true})
    for (unsigned kernel = 0; kernel < 5; ++kernel) {
      auto node = authored(continuous, kernel, profile, 1);
      std::vector<ps::Value> dense;
      if (continuous)
        dense.push_back(doubles({3}, {0, .75, 2}));
      dense.push_back(doubles({2, 3}, {1, -2, 4, 8, 7, 6}));
      auto demand = region({2, 3}, {ps::Region({{0, 1}, {1, 1}})});
      auto baseline = direct(registry, node, dense, demand);
      std::uint64_t expected = 0;
      require(read(baseline, {0, 1}, &expected, 8).ok(),
              "baseline local signal");
      for (unsigned mask = 0; mask < (1U << dense.size()); ++mask) {
        auto inputs = dense;
        for (unsigned i = 0; i < inputs.size(); ++i)
          if (mask & (1U << i))
            inputs[i] = reversed_unaligned(inputs[i]);
        fenv_t saved;
        require(fegetenv(&saved) == 0, "save lowpass fenv");
        for (auto mode :
             {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
          require(fesetround(mode) == 0 && feclearexcept(FE_ALL_EXCEPT) == 0 &&
                      feraiseexcept(FE_DIVBYZERO) == 0,
                  "set lowpass fenv");
          auto control = std::make_shared<point_math_checks::Control>();
          control->rounding = mode;
          auto result = direct(registry, node, inputs, demand, control);
          require(control->computation_polls > 0,
                  "lowpass worker computation fenv exercised");
          std::uint64_t actual = 0;
          require(read(result, {0, 1}, &actual, 8).ok() && actual == expected,
                  "all-port negative/unaligned lowpass");
          require(fegetround() == mode &&
                      fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
                  "lowpass preserves caller fenv");
        }
        require(fesetenv(&saved) == 0, "restore lowpass fenv");
      }
      auto constant = doubles({1}, {-0.});
      dense.back() = take(ps::Value::from_storage(
          {ps::ElementType::Float64, {2, 3}}, ps::Region::whole({2, 3}),
          {0, {0, 0}}, constant.storage()));
      auto zero = direct(registry, node, dense, demand);
      std::uint64_t actual = 0;
      require(read(zero, {0, 1}, &actual, 8).ok() && actual == raw(-0.),
              "zero-stride signed constant");
    }
  std::cout << "ten lowpass families: axis-local negative/unaligned/zero "
               "strides and four floating modes passed\n";
}
void independent_axis_layout(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  for (bool continuous : {false, true})
    for (unsigned kernel = 0; kernel < 5; ++kernel) {
      auto node = authored(continuous, kernel, profile, 0);
      // Separate columns remain constant under Reflect. This independently
      // checks every output of a non-last axis, including successive maps.
      std::vector<ps::Value> inputs;
      if (continuous)
        inputs.push_back(doubles({4}, {0, .125, 1, 3}));
      inputs.push_back(doubles({4, 2}, {1, -2, 1, -2, 1, -2, 1, -2}));
      inputs.back() = reversed_unaligned(inputs.back());
      auto result =
          direct(registry, node, inputs, take(ps::Footprint::all({4, 2})));
      for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 2; ++column) {
          std::uint64_t actual = 0;
          require(read(result, {row, column}, &actual, 8).ok() &&
                      actual == raw(column ? -2 : 1),
                  "non-last-axis independent two-constant oracle");
        }
    }
}
void result_resources(const ps::WorkflowNode& node,
                      const std::vector<ps::Value>& values,
                      uint64_t output_bytes) {
  point_math_checks::resources(node, values, output_bytes);
}

void resources(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  for (bool continuous : {false, true}) {
    auto node = authored(continuous, 4, profile);
    std::vector<ps::Value> inputs;
    if (continuous)
      inputs.push_back(doubles({3}, {0, .75, 2}));
    inputs.push_back(doubles({3}, {1, -2, 4}));
    Fixture fixture(node, inputs);
    const auto key = continuous ? "samples" : "values";
    fixture.document.outputs = {{"filtered", 1, key}};
    auto empty = take(
        fixture.run({{"filtered", take(ps::Footprint::none({3}))}}, false));
    require(take(empty.dependencies.source_support()).empty() &&
                take(empty.results.at("filtered").descriptor())
                    .tensor_coverage(0)
                    .empty(),
            "Empty lowpass reads none");
    result_resources(node, inputs, 24);
    std::vector<ps::OperationMetadata> metadata;
    for (const auto& input : inputs) {
      ps::OperationMetadata m;
      m.result_schema =
          std::make_shared<ps::SchemaTemplate>(source_schema(input));
      metadata.push_back(std::move(m));
    }
    for (unsigned kind = 0; kind < 5; ++kind) {
      auto parameters = node.parameters;
      auto ports = metadata;
      if (kind == 0)
        parameters["axis"] = std::int64_t{-1};
      if (kind == 1)
        parameters["sigma"] = 0.;
      if (kind == 2)
        parameters["unused"] = true;
      if (kind == 3) {
        auto schema =
            std::make_shared<ps::SchemaTemplate>(*ports.back().result_schema);
        schema->tensors[0].descriptor.element_type = ps::ElementType::Int64;
        ports.back().result_schema = std::move(schema);
      }
      if (kind == 4)
        parameters["boundary"] = std::string("mirror");
      require(!registry->resolve_traits(node.operation, ports, parameters).ok(),
              "lowpass schema rejection");
    }
  }
  std::cout << "Empty/schema, Whole work/output/workspace budgets, active "
               "cancellation and release passed\n";
}
struct FailedSource final {
  unsigned* calls;
  explicit FailedSource(unsigned* counter) : calls(counter) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase&) {
    ++*calls;
    return ps::Result<ps::ResultProgramPoll>(
        ps::Status{ps::ErrorCode::OperationFailed, "required lowpass source"});
  }
};
void cache_validation_and_owners(ps::CpuNumericProfile profile) {
  for (bool continuous : {false, true}) {
    auto node = authored(continuous, 4, profile);
    std::vector<ps::Value> inputs;
    if (continuous)
      inputs.push_back(doubles({5}, {0, .75, 2, 3, 4}));
    inputs.push_back(doubles({5}, {1, -2, 4, 6, 8}));
    Fixture fixture(node, inputs);
    fixture.document.outputs = {
        {"filtered", 1, continuous ? "samples" : "values"}};
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    const auto prepared = plan.plan.steps().back().prepared;
    require(prepared != nullptr, "immutable lowpass static preparation");
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 1048576;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(fixture.registry, config);
    auto root = take(context.resource_budget());
    auto bindings = point_math_checks::bindings(root, inputs, fixture.document);
    auto demand = take(context.open_demand(plan.plan, bindings));
    ps::DemandQuery query{{"filtered", region({5}, {ps::Region({{2, 1}})})}};
    ps::ExecutionOptions options;
    options.maximum_dependency_work = UINT64_C(4096) * 1024 * 1024;
    options.dependencies.maximum_work = UINT64_C(2048) * 1024 * 1024;
    options.maximum_dependency_cache_work = 128 * 1024 * 1024;
    auto original = take(demand.request(query, {}, options));
    auto repeated = take(demand.request(query, {}, options));
    std::uint64_t repeat = 0, first = 0;
    require(read(repeated.results.at("filtered"), {2}, &repeat, 8).ok() &&
                read(original.results.at("filtered"), {2}, &first, 8).ok() &&
                repeat == first &&
                repeated.results.at("filtered").object_id() ==
                    original.results.at("filtered").object_id(),
            "repeated lowpass demand preserves bits");
    require(
        take(original.results.at("filtered").descriptor()).tensor_coverage(0) ==
            take(ps::Footprint::all({5})),
        "sparse request publishes complete Whole coverage");
    auto fresh_bindings =
        point_math_checks::bindings(root, inputs, fixture.document);
    auto frozen = take(context.freeze(plan.plan, fresh_bindings));
    auto hit = take(context.execute_fragments(frozen, query, {}, options));
    require(
        hit.diagnostics.cache_hits == 1 &&
            numeric_result_fixture::bytes(hit.results.at("filtered")) ==
                numeric_result_fixture::bytes(original.results.at("filtered")),
        "fresh identical sources hit actual lowpass cache");
    const auto ids = hit.results.at("filtered").association();
    for (std::size_t i = 0; i < fresh_bindings.inputs.size(); ++i) {
      require(
          std::find(ids.begin(), ids.end(),
                    fresh_bindings.inputs[i].result.object_id()) != ids.end() &&
              std::find(ids.begin(), ids.end(),
                        bindings.inputs[i].result.object_id()) == ids.end(),
          "cached output associates current source Result identities");
    }
    bindings.inputs.back().result =
        source(root, doubles({5}, {1, -2, 5, 6, 8}));
    require(demand.replace_bindings(bindings).ok(), "replace lowpass values");
    auto changed = take(demand.request(query, {}, options));
    require(plan.plan.steps().back().prepared == prepared,
            "values rebind reuses immutable static preparation");
    auto fresh_inputs = inputs;
    fresh_inputs.back() = doubles({5}, {1, -2, 5, 6, 8});
    Fixture fresh(node, fresh_inputs);
    fresh.document.outputs = fixture.document.outputs;
    auto uncached = take(fresh.run(query, false));
    std::uint64_t a = 0, b = 0, old = 0;
    require(read(changed.results.at("filtered"), {2}, &a, 8).ok() &&
                read(uncached.results.at("filtered"), {2}, &b, 8).ok() &&
                a == b &&
                read(original.results.at("filtered"), {2}, &old, 8).ok() &&
                old != a,
            "cached/uncached replacement bits");
    if (continuous) {
      bindings.inputs[0].result = source(root, doubles({5}, {0, .75, 2, 3, 3}));
      require(demand.replace_bindings(bindings).ok(),
              "replace remote positions");
      auto invalid = demand.request(query, {}, options);
      require(plan.plan.steps().back().prepared == prepared,
              "positions rebind reuses immutable static preparation");
      require(!invalid.ok() &&
                  invalid.status().reason == ps::FailureReason::InvalidDomain,
              "remote topology invalidates warm lowpass");
    }
    auto typed =
        array(ps::ElementType::Float32, {3, 1}, {0, 0x3f000000, 0x40000000});
    const auto coverage = take(ps::encode_semantic(ps::coverage_semantics()));
    std::vector<ps::Value> typed_inputs;
    if (continuous)
      typed_inputs.push_back(doubles({3}, {0, .75, 2}));
    typed_inputs.push_back(typed);
    Fixture invalid(node, typed_inputs);
    invalid.document.outputs = fixture.document.outputs;
    auto typed_schema = std::make_shared<ps::SchemaTemplate>(
        *invalid.document.inputs.back().result_schema);
    typed_schema->tensors[0].facets = {coverage};
    invalid.document.inputs.back().result_schema = std::move(typed_schema);
    auto failure = invalid.run(
        {{"filtered", region({3, 1}, {ps::Region({{1, 1}, {0, 1}})})}}, false);
    require(
        !failure.ok() &&
            failure.status().code == ps::ErrorCode::InvalidArgument &&
            failure.status().detail.input_id == (continuous ? 2U : 1U) &&
            failure.status().message == "sample violates typed semantic domain",
        "generic lowpass preserves typed payload validation");

    auto registry = ps::make_default_operation_registry(false);
    unsigned calls = 0;
    ps::OperationDefinition producer;
    producer.key = "manual.lowpass_source";
    producer.traits.input_count = 0;
    producer.traits.input_schema.clear();
    auto& output = producer.traits.outputs[0];
    output.key = "value";
    output.output_schema.kind = ps::OperationPortKind::Result;
    output.output_schema.result_schema_id = "manual.lowpass.input";
    output.output_schema.result_schema_version = 1;
    output.result_schema = source_schema(inputs.back());
    output.dependency_version = 2;
    output.region_rule = ps::OperationRegionRule::Whole;
    output.continuation_bytes = sizeof(FailedSource);
    output.maximum_dependency_stages = 1;
    producer.start_result = [&](const auto&, const auto& allocator) {
      return ps::ResultContinuation::make<FailedSource>(allocator, &calls);
    };
    require(registry->register_operation(std::move(producer)).ok() &&
                registry->freeze().ok(),
            "lowpass source registration");
    Fixture upstream(node, inputs);
    upstream.registry = registry;
    upstream.document.outputs = fixture.document.outputs;
    upstream.document.inputs.pop_back();
    upstream.sources.pop_back();
    upstream.document.nodes[0].inputs.back() =
        ps::WorkflowNodeOutput{2, "value"};
    upstream.document.nodes.push_back({2, "manual.lowpass_source", {}, {}});
    auto empty_upstream = take(
        upstream.run({{"filtered", take(ps::Footprint::none({5}))}}, false));
    require(
        calls == 0 && take(empty_upstream.results.at("filtered").descriptor())
                          .tensor_coverage(0)
                          .empty(),
        "Empty lowpass does not poll failing source continuation");
    auto rejected = upstream.run(query, false);
    require(!rejected.ok() &&
                rejected.status().message == "required lowpass source" &&
                calls == 1,
            "preserve lowpass upstream failure");

    for (auto dtype : {ps::ElementType::Float32, ps::ElementType::Float64}) {
      ps::ResourceBudget budget;
      std::optional<ps::ResultRef> escaped;
      std::optional<ps::ResultTensorReadWindow> window;
      std::vector<std::weak_ptr<const ps::CpuStorage>> owners;
      const auto width = ps::Value::element_size(dtype);
      {
        std::vector<ps::Value> backing;
        if (continuous)
          backing.push_back(doubles({5}, {0, .75, 2, 3, 4}));
        backing.push_back(array(
            dtype, {5},
            dtype == ps::ElementType::Float32
                ? std::vector<std::uint64_t>{0x3f800000, 0xc0000000, 0x40800000,
                                             0x40c00000, 0x41000000}
                : std::vector<std::uint64_t>{raw(1), raw(-2), raw(4), raw(6),
                                             raw(8)}));
        for (const auto& value : backing)
          owners.push_back(value.storage());
        Fixture owner_fixture(node, backing);
        owner_fixture.document.outputs = fixture.document.outputs;
        ps::GraphContext owner_graph(owner_fixture.document);
        auto owner_plan =
            take(ps::Compiler(owner_fixture.registry).compile(owner_graph));
        ps::ExecutionContextConfig owner_config;
        owner_config.cpu_workers = 1;
        owner_config.managed_resources = ps::ResourceLimits{};
        ps::ExecutionContext owner_context(owner_fixture.registry,
                                           owner_config);
        budget = take(owner_context.resource_budget());
        auto owner_bindings = point_math_checks::bindings(
            budget, owner_fixture.sources, owner_fixture.document);
        require(budget.statistics().live[ps::ResourceKind::Payload] == 0 &&
                    budget.statistics().live[ps::ResourceKind::Referenced] > 0,
                "immutable source backing is Referenced without payload copy");
        auto frozen =
            take(owner_context.freeze(owner_plan.plan, owner_bindings));
        auto result =
            take(owner_context.execute_fragments(frozen, query, {}, options));
        escaped = result.results.at("filtered");
        auto descriptor = take(escaped->descriptor());
        window =
            take(escaped->acquire_tensor(descriptor, 0, ps::Region({{2, 1}})));
      }
      require(std::all_of(owners.begin(), owners.end(),
                          [](const auto& owner) { return owner.expired(); }),
              "source owners retire independently from dense lowpass output");
      require(budget.statistics().live[ps::ResourceKind::Payload] == 5 * width,
              "escaped lowpass retains exact full-output payload");
      std::uint64_t value = 0;
      require(read(*escaped, {2}, &value, width).ok(),
              "escaped lowpass readable");
      escaped.reset();
      auto row = take(window->row_run({2}));
      std::uint64_t after = 0;
      std::memcpy(&after, row.data, width);
      require(
          after == value &&
              budget.statistics().live[ps::ResourceKind::Payload] == 5 * width,
          "authorized read window retains output after Result release");
      window.reset();
      point_math_checks::released(budget);
    }
  }
  std::cout << "cache replacement, typed/upstream failures and escaped "
               "payload/metadata lifetime passed\n";
}

void sparse_large_and_partition_cancel(ps::CpuNumericProfile profile) {
  const auto huge = UINT64_C(1) << 39;
  for (bool continuous : {false, true}) {
    std::vector<ps::Value> inputs;
    if (continuous)
      inputs.push_back(doubles({2}, {0, 1}));
    inputs.push_back(doubles({1}, {1.5}));
    const auto shape = continuous ? std::vector<std::uint64_t>{huge, 2}
                                  : std::vector<std::uint64_t>{huge * 2};
    Fixture fixture(authored(continuous, 4, profile, continuous ? 1 : 0),
                    inputs);
    fixture.document.nodes[0].inputs.back() =
        ps::WorkflowNodeOutput{2, "values"};
    fixture.document.nodes.push_back(take(ps::numeric::constant_node(
        2, ps::WorkflowInputReference{continuous ? 2U : 1U}, shape,
        ps::numeric::ArrayLayout::View, profile)));
    fixture.document.outputs = {
        {"filtered", 1, continuous ? "samples" : "values"}};
    const auto wanted =
        continuous ? region(shape, {ps::Region({{huge - 1, 1}, {1, 1}})})
                   : region(shape, {ps::Region({{huge * 2 - 1, 1}})});
    auto result = fixture.run({{"filtered", wanted}}, false);
    require(!result.ok() &&
                result.status().code == ps::ErrorCode::ResourceExhausted &&
                result.status().reason == ps::FailureReason::CapacityLimit &&
                result.status().detail.node_id == 1,
            "2^40 Whole output rejects small payload budget");
  }
  auto node = authored(true, 4, profile);
  node.parameters["support_radius"] = 1.;
  node.parameters["boundary"] = std::string("wrap");
  std::vector<ps::Value> inputs{
      array(ps::ElementType::Float64, {2}, {raw(1), raw(1) + 1}),
      doubles({2}, {0, 1})};
  result_resources(node, inputs, 16);
  Fixture extreme(
      authored(true, 4, profile),
      {array(ps::ElementType::Float64, {3},
             {UINT64_C(0xffefffffffffffff), 0, UINT64_C(0x7fefffffffffffff)}),
       doubles({3}, {0, 0, 0})});
  double radius;
  auto radius_bits = UINT64_C(0x7fefffffffffffff);
  std::memcpy(&radius, &radius_bits, 8);
  extreme.document.nodes[0].parameters["support_radius"] = radius;
  extreme.document.outputs = {{"filtered", 1, "samples"}};
  auto result = take(
      extreme.run({{"filtered", region({3}, {ps::Region({{1, 1}})})}}, false));
  std::uint64_t value = 1;
  require(
      read(result.results.at("filtered"), {1}, &value, 8).ok() && value == 0,
      "exact extreme-width constant cancellation");
  std::cout << "2^40-element Whole budget rejection, huge-period "
               "cancellation and extreme-width exact constant passed\n";
}

}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "profile must be strict, apple, or x86");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    layouts(profile);
    independent_axis_layout(profile);
    resources(profile);
    cache_validation_and_owners(profile);
    sparse_large_and_partition_cancel(profile);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
