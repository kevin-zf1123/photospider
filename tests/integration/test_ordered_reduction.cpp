#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
Value input(const std::vector<std::uint64_t>& shape,
            const std::vector<double>& numbers, bool fp32 = false) {
  const ValueDescriptor descriptor{
      fp32 ? ElementType::Float32 : ElementType::Float64, shape};
  auto writer = MutableValue::allocate(descriptor, Region::whole(shape),
                                       BufferAllocator{})
                    .take_value();
  for (std::size_t i = 0; i < numbers.size(); ++i) {
    if (fp32) {
      const float value = static_cast<float>(numbers[i]);
      std::memcpy(writer.data() + i * 4, &value, 4);
    } else {
      std::memcpy(writer.data() + i * 8, &numbers[i], 8);
    }
  }
  return std::move(writer).publish().take_value();
}
WorkflowDocument document(const Value& source, const std::string& key,
                          std::int64_t block) {
  WorkflowDocument doc;
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "x";
  input.result_schema = std::make_shared<SchemaTemplate>(
      numeric_result_fixture::source_schema(source));
  doc.inputs = {input};
  doc.nodes = {{1, key, {WorkflowInputReference{1}}, {{"block_size", block}}}};
  doc.outputs = {{"result", 1, "value"}};
  return doc;
}
struct Generator {
  ValueDescriptor descriptor;
  std::vector<ValueFacet> facets;
  std::function<Result<Region>(const Region&, uint8_t*, uint64_t,
                               const BufferAllocator&,
                               const CancellationToken&)>
      read;
  uint64_t bytes = 0, maximum_payload = 0;
};
struct GeneratorProgram {
  std::shared_ptr<Generator> source;
  explicit GeneratorProgram(std::shared_ptr<Generator> value)
      : source(std::move(value)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto builder =
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key)
            .take_value();
    auto status = builder.bind_descriptor_relation(
        ResultRelation::cartesian(phase.resources, 1, {}).take_value());
    if (!status.ok())
      return Result<ResultProgramPoll>(status);
    const auto width = Value::element_size(source->descriptor.element_type);
    for (const auto& region : phase.query.tensor_outputs->boxes()) {
      const auto size = region.element_count().take_value() * width;
      auto allocated = phase.resources.allocator().allocate(size);
      if (!allocated.ok())
        return Result<ResultProgramPoll>(allocated.status());
      auto buffer = allocated.take_value();
      auto read =
          source->read(region, buffer.data(), size, phase.resources.allocator(),
                       phase.query.cancellation);
      if (!read.ok())
        return Result<ResultProgramPoll>(read.status());
      bool exact = region.rank() == read.value().rank();
      for (size_t axis = 0; exact && axis < region.rank(); ++axis)
        exact = region.dimensions()[axis].offset ==
                    read.value().dimensions()[axis].offset &&
                region.dimensions()[axis].extent ==
                    read.value().dimensions()[axis].extent;
      if (!exact)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::TypeMismatch, "generator coverage"});
      source->bytes += size;
      std::vector<int64_t> strides(region.rank());
      uint64_t stride = width;
      for (size_t axis = strides.size(); axis--;) {
        strides[axis] = stride;
        stride *= region.dimensions()[axis].extent;
      }
      std::vector<uint64_t> origin;
      for (const auto& dimension : region.dimensions())
        origin.push_back(dimension.offset);
      status = builder.publish_tensor(
          0, region, {0, strides, origin}, std::move(buffer).freeze(),
          ResultRelation::cartesian(phase.resources,
                                    Region::whole(source->descriptor.shape)
                                        .element_count()
                                        .take_value(),
                                    {})
              .take_value(),
          {true, true, true, true}, phase.query.cancellation);
      if (!status.ok())
        return Result<ResultProgramPoll>(status);
    }
    source->maximum_payload =
        std::max(source->maximum_payload,
                 phase.resources.statistics().live[ResourceKind::Payload]);
    auto sealed = builder.seal();
    return sealed.ok() ? Result<ResultProgramPoll>(
                             ResultPublication{sealed.take_value(), true})
                       : Result<ResultProgramPoll>(sealed.status());
  }
};
WorkflowDocument generator_document(
    const std::shared_ptr<OperationRegistry>& registry,
    const std::shared_ptr<Generator>& source, const Value& declaration,
    const std::string& operation, int64_t block) {
  auto schema = numeric_result_fixture::source_schema(declaration);
  schema.tensors[0].facets = source->facets;
  for (const auto& facet : source->facets)
    if (facet.key == "photospider.color-array")
      schema.tensors[0].atomic_trailing_axes = 1;
  OperationDefinition producer;
  producer.key = "test.ordered.generator";
  producer.traits.input_count = 0;
  auto& output = producer.traits.outputs[0];
  output.output_schema.kind = OperationPortKind::Result;
  output.result_schema = schema;
  output.output_schema.result_schema_id = std::string(schema.id);
  output.output_schema.result_schema_version = schema.version;
  output.region_rule = OperationRegionRule::Dependency;
  output.continuation_bytes = sizeof(GeneratorProgram);
  output.maximum_dependency_stages = 2;
  producer.start_result = [source](const auto&, const auto& allocator) {
    return ResultContinuation::make<GeneratorProgram>(allocator, source);
  };
  numeric_result_fixture::require(
      registry->register_operation(std::move(producer)).ok(),
      "generator registration");
  numeric_result_fixture::require(registry->freeze().ok(), "generator freeze");
  auto doc = document(declaration, operation, block);
  doc.inputs.clear();
  doc.nodes[0].inputs = {WorkflowNodeOutput{99, "value"}};
  doc.nodes.insert(doc.nodes.begin(), {99, "test.ordered.generator", {}, {}});
  return doc;
}
ExecutionContextConfig managed(uint64_t payload = 4096, uint64_t cache = 0) {
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = cache;
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Payload] = payload;
  return config;
}
ExecutionBindings bindings(ExecutionContext& context, const Value& value) {
  auto root = context.resource_budget().take_value();
  return {{{"x", numeric_result_fixture::source(root, value)}}};
}
double number(const ResultRef& result) {
  double value = 0;
  numeric_result_fixture::require(
      result.read_tensor(result.descriptor().take_value(), 0, {0}, &value, 8)
          .ok(),
      "ordered Result scalar read");
  return value;
}

double oracle(const std::vector<double>& samples, bool variance, bool fp32) {
  // Volatile intermediates establish explicit binary64 left-fold operations,
  // independent of implementation blocks, descriptors and fragment helpers.
  volatile double sum = 0;
  for (const double number : samples) {
    const double sample =
        fp32 ? static_cast<double>(static_cast<float>(number)) : number;
    sum = sum + sample;
  }
  volatile double mean = sum / static_cast<double>(samples.size());
  if (!variance)
    return mean;
  sum = 0;
  for (const double number : samples) {
    const double sample =
        fp32 ? static_cast<double>(static_cast<float>(number)) : number;
    volatile double difference = sample - mean;
    volatile double square = difference * difference;
    sum = sum + square;
  }
  return sum / static_cast<double>(samples.size());
}
int order_and_cache() {
  auto registry = make_default_operation_registry();
  std::vector<double> numbers;
  for (unsigned i = 0; i < 30; ++i)
    numbers.insert(numbers.end(), {1e16, 1, -1e16, 4});
  for (bool fp32 : {false, true})
    for (bool variance : {false, true})
      for (const auto& shape :
           std::vector<std::vector<std::uint64_t>>{{120},
                                                   {2, 3, 4, 5},
                                                   {1, 2, 1, 3, 1, 4, 1, 5}})
        for (const auto block : {1, 2, 7, 64, 65536}) {
          const auto source = input(shape, numbers, fp32);
          GraphContext graph(document(
              source, variance ? "numeric.variance" : "numeric.mean", block));
          auto plan =
              numeric_result_fixture::take(Compiler(registry).compile(graph))
                  .plan;
          ExecutionContext context(registry, managed(4096, 128));
          auto demand =
              context.open_demand(plan, bindings(context, source)).take_value();
          const auto q = Footprint::all({1}).take_value();
          const double expected = oracle(numbers, variance, fp32);
          for (unsigned warm = 0; warm < 2; ++warm) {
            auto result = demand.request({{"result", q}});
            if (!result.ok())
              std::cerr << "block=" << block << " rank=" << shape.size() << ": "
                        << result.status().message << '\n';
            PS_CHECK(result.ok());
            double actual = 0;
            PS_CHECK(result.value()
                         .results.at("result")
                         .read_tensor(result.value()
                                          .results.at("result")
                                          .descriptor()
                                          .take_value(),
                                      0, {0}, &actual, 8)
                         .ok());
            PS_CHECK(std::memcmp(&actual, &expected, 8) == 0);
            PS_CHECK(context.cache_statistics().retained_bytes <= 128);
            if (warm && block >= 64) {
              // A complete Result can bypass state-transition lookup.
              PS_CHECK(result.value().diagnostics.shared_computations > 0 ||
                       result.value().diagnostics.cache_hits > 0);
            }
            PS_CHECK(result.value().dependencies.source_support().value().at(
                         "x") == Footprint::all(shape).take_value());
            PS_CHECK(result.value()
                         .dependencies
                         .potential_dirty("x",
                                          Footprint::all(shape).take_value(), 7,
                                          {}, ResultSupportTarget::Tensor, 0)
                         .value()
                         .at("result") == q);
          }
        }
  return 0;
}
int bounded_source() {
  for (bool opaque : {false, true})
    for (bool variance : {false, true}) {
      auto registry = make_default_operation_registry(false);
      auto declaration = input({4096}, std::vector<double>(4096, 1));
      if (opaque)
        declaration =
            Value::create(declaration.descriptor(), declaration.region(),
                          declaration.layout(), declaration.copy_bytes(),
                          {{"vendor.proof", 1, {42}}})
                .take_value();
      auto source = std::make_shared<Generator>();
      source->descriptor = declaration.descriptor();
      source->facets = declaration.facets();
      std::uint64_t reads = 0, next = 0;
      source->read = [&](const Region& region, std::uint8_t* destination,
                         std::uint64_t bytes, const BufferAllocator&,
                         const CancellationToken&) {
        if (bytes != 512 || region.dimensions()[0].offset != next ||
            region.dimensions()[0].extent != 64)
          return Result<Region>(
              Status{ErrorCode::OperationFailed, "unexpected block read"});
        ++reads;
        next = (next + 64) % 4096;
        for (unsigned i = 0; i < 64; ++i) {
          const double value =
              static_cast<double>((region.dimensions()[0].offset + i) % 4);
          std::memcpy(destination + 8 * i, &value, 8);
        }
        return Result<Region>(region);
      };
      GraphContext graph(generator_document(
          registry, source, declaration,
          variance ? "numeric.variance" : "numeric.mean", 64));
      auto plan =
          numeric_result_fixture::take(Compiler(registry).compile(graph)).plan;
      ExecutionContext context(registry, managed(1024));
      auto result = context.execute(plan);
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      // Uniform repetitions of 0,1,2,3: mean=1.5, population variance=1.25.
      PS_CHECK(number(result.value().results.at("result")) ==
               (variance ? 1.25 : 1.5));
      PS_CHECK(reads == (variance ? 128U : 64U) && next == 0);
      PS_CHECK(source->maximum_payload <= 1024);
      PS_CHECK(source->bytes == 32768 * (variance ? 2U : 1U));
    }
  return 0;
}
int exact_rank_eight_reads() {
  auto registry = make_default_operation_registry(false);
  const std::vector<std::uint64_t> shape{2, 2, 2, 2, 2, 2, 2, 32};
  const auto declaration = input(shape, std::vector<double>(4096, 1));
  auto source = std::make_shared<Generator>();
  source->descriptor = declaration.descriptor();
  std::uint64_t next = 0, visits = 0;
  source->read = [&](const Region& region, std::uint8_t* destination,
                     std::uint64_t bytes, const BufferAllocator&,
                     const CancellationToken&) {
    if (bytes > 17 * 8 || bytes % 8)
      return Result<Region>(
          Status{ErrorCode::OperationFailed, "oversized rank-eight read"});
    std::vector<std::uint64_t> at;
    for (const auto& dim : region.dimensions())
      at.push_back(dim.offset);
    for (std::uint64_t i = 0; i < bytes / 8; ++i) {
      std::uint64_t flat = 0;
      for (std::size_t axis = 0; axis < shape.size(); ++axis)
        flat = flat * shape[axis] + at[axis];
      if (flat != next)
        return Result<Region>(Status{ErrorCode::OperationFailed,
                                     "gap or duplicate in source traversal"});
      const double sample = static_cast<double>(flat % 4);
      std::memcpy(destination + i * 8, &sample, 8);
      next = (next + 1) % 4096;
      ++visits;
      for (std::size_t axis = shape.size(); axis; --axis) {
        const auto dim = region.dimensions()[axis - 1];
        if (++at[axis - 1] < dim.offset + dim.extent)
          break;
        at[axis - 1] = dim.offset;
      }
    }
    return Result<Region>(region);
  };
  GraphContext graph(generator_document(registry, source, declaration,
                                        "numeric.variance", 17));
  auto plan =
      numeric_result_fixture::take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, managed(1024));
  // This traversal deliberately creates hundreds of rank-eight query records.
  // Bound its bookkeeping separately from its 1 KiB payload acceptance gate.
  ExecutionOptions options;
  options.maximum_dependency_work = 4 * 1048576;
  auto result = context.execute(plan, {}, {}, options);
  if (!result.ok())
    std::cerr << result.status().message << " visits=" << visits
              << " root_work="
              << context.resource_budget().value().statistics().issued.work
              << '\n';
  PS_REQUIRE_OK(result);
  PS_CHECK(number(result.value().results.at("result")) == 1.25);
  PS_CHECK(visits == 8192 && next == 0 && source->bytes == 65536 &&
           source->maximum_payload <= 1024);
  return 0;
}
int typed_channels_and_cancellation() {
  auto registry = make_default_operation_registry();
  std::vector<double> numbers;
  for (unsigned i = 0; i < 6; ++i)
    numbers.insert(numbers.end(), {1, 2, 3});
  auto plain = input({2, 3, 3}, numbers, true);
  ColorArrayDescriptor xyz;

  registry = make_default_operation_registry(false);
  auto tuples = std::make_shared<Generator>();
  tuples->descriptor = plain.descriptor();
  tuples->facets = {encode_color_array(xyz).take_value()};
  unsigned tuple_reads = 0;
  tuples->read = [&](const Region& region, uint8_t* destination, uint64_t bytes,
                     const BufferAllocator&, const CancellationToken&) {
    if (bytes != 12 || region.dimensions().back().extent != 3)
      return Result<Region>(
          Status{ErrorCode::OperationFailed, "partial typed tuple"});
    const float data[] = {1, 2, 3};
    std::memcpy(destination, data, 12);
    ++tuple_reads;
    return Result<Region>(region);
  };
  {
    GraphContext graph(
        generator_document(registry, tuples, plain, "numeric.variance", 1));
    auto plan =
        numeric_result_fixture::take(Compiler(registry).compile(graph)).plan;
    ExecutionContext context(registry, managed(1024));
    auto result = context.execute(plan);
    PS_CHECK(result.ok() &&
             std::abs(number(result.value().results.at("result")) - 2. / 3) <
                 1e-15);
    PS_CHECK(tuple_reads == 18 && tuples->bytes == 216 &&
             tuples->maximum_payload <= 1024);
  }
  const auto value = input({256}, std::vector<double>(256, 1));
  auto source = std::make_shared<Generator>();
  source->descriptor = value.descriptor();
  CancellationSource cancel;
  unsigned reads = 0;
  source->read = [&](const Region& region, std::uint8_t* destination,
                     std::uint64_t bytes, const BufferAllocator&,
                     const CancellationToken&) {
    ++reads;
    for (std::uint64_t i = 0; i < bytes / 8; ++i) {
      const double sample = 1;
      std::memcpy(destination + 8 * i, &sample, 8);
    }
    if (reads == 5)
      cancel.cancel();
    return Result<Region>(region);
  };
  registry = make_default_operation_registry(false);
  GraphContext graph(
      generator_document(registry, source, value, "numeric.variance", 64));
  auto plan =
      numeric_result_fixture::take(Compiler(registry).compile(graph)).plan;
  ExecutionContext context(registry, managed(1024));
  PS_CHECK(context.execute(plan, {}, cancel.token()).status().code ==
           ErrorCode::Cancelled);
  PS_CHECK(reads == 5);
  // The cancelled state and stage buffers retire; the same context recovers.
  PS_CHECK(number(context.execute(plan).value().results.at("result")) == 0);
  return 0;
}
int result_and_block_reuse() {
  auto registry = make_default_operation_registry();
  const std::vector<double> numbers{1, 2, 3, 4};
  for (bool variance : {false, true}) {
    auto source = input({4}, numbers);
    GraphContext graph(
        document(source, variance ? "numeric.variance" : "numeric.mean", 2));
    auto plan =
        numeric_result_fixture::take(Compiler(registry).compile(graph)).plan;
    const double expected = oracle(numbers, variance, false);
    for (bool blocks_only : {false, true}) {
      auto config = managed(4096, 512);
      if (blocks_only)
        config.maximum_dependency_cache_metadata = 1;
      ExecutionContext context(registry, config);
      const auto bound = bindings(context, source);
      if (blocks_only) {
        {
          auto cold = context.execute(plan, bound);
          PS_REQUIRE_OK(cold);
          PS_CHECK(number(cold.value().results.at("result")) == expected);
        }
        auto warm = context.execute(plan, bound);
        PS_REQUIRE_OK(warm);
        PS_CHECK(number(warm.value().results.at("result")) == expected);
        PS_CHECK(warm.value().diagnostics.cache_hits == 0);
      } else {
        auto cold = context.execute(plan, bound);
        PS_REQUIRE_OK(cold);
        auto warm = context.execute(plan, bound);
        PS_REQUIRE_OK(warm);
        PS_CHECK(number(warm.value().results.at("result")) == expected);
        PS_CHECK((warm.value().diagnostics.shared_computations > 0 ||
                  warm.value().diagnostics.cache_hits > 0) &&
                 warm.value().diagnostics.operation_timings.empty());
      }
    }
  }
  return 0;
}
int edited_block_cache() {
  auto registry = make_default_operation_registry();
  for (bool variance : {false, true}) {
    std::vector<double> numbers{1, 2, 3, 4};
    auto source = input({4}, numbers);
    GraphContext graph(
        document(source, variance ? "numeric.variance" : "numeric.mean", 2));
    auto plan =
        numeric_result_fixture::take(Compiler(registry).compile(graph)).plan;
    ExecutionContext context(registry, managed(4096, 512));
    auto demand =
        context.open_demand(plan, bindings(context, source)).take_value();
    DemandQuery q{{"result", Footprint::all({1}).take_value()}};
    auto initial = demand.request(q);
    PS_CHECK(initial.ok());
    // The unchanged first block retains its incoming sum. Variance pass two
    // must miss even that block because its fixed mean changes from 2.5 to 3.5.
    numbers[3] = 8;
    PS_CHECK(
        demand.replace_bindings(bindings(context, input({4}, numbers))).ok());
    auto changed = demand.request(q);
    PS_CHECK(changed.ok());
    double actual = 0, expected = oracle(numbers, variance, false);
    actual = number(changed.value().results.at("result"));
    PS_CHECK(std::memcmp(&actual, &expected, 8) == 0);
    PS_CHECK(changed.value().dependencies.source_support().value().at("x") ==
             Footprint::all({4}).value());
  }
  return 0;
}
int failures_and_environment() {
  auto registry = make_default_operation_registry();
  const double maximum = std::numeric_limits<double>::max();
  const auto q = Footprint::all({1}).take_value();
  for (const auto block : {1, 2, 64})
    for (const auto& scenario :
         std::vector<std::pair<std::vector<double>, std::string>>{
             {{maximum, maximum, -maximum},
              "reduction sum overflow at sample 1"},
             {{1, std::numeric_limits<double>::infinity()},
              "reduction input is nonfinite at sample 1"},
             {{maximum, -maximum}, "variance overflow at sample 0"}}) {
      const auto source = input({scenario.first.size()}, scenario.first);
      GraphContext graph(document(source, "numeric.variance", block));
      auto plan =
          numeric_result_fixture::take(Compiler(registry).compile(graph)).plan;
      ExecutionContext context(registry, managed(4096, 64));
      auto demand =
          context.open_demand(plan, bindings(context, source)).take_value();
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        auto failed = demand.request({{"result", q}});
        PS_CHECK(failed.status().code == ErrorCode::OperationFailed &&
                 failed.status().message == scenario.second);
      }
      PS_CHECK(
          demand.request({{"result", Footprint::none({1}).take_value()}}).ok());
      CancellationSource cancelled;
      cancelled.cancel();
      PS_CHECK(
          demand.request({{"result", q}}, cancelled.token()).status().code ==
          ErrorCode::Cancelled);
    }
  const auto finite = input({4}, {1e16, 1, -1e16, 4});
  GraphContext graph(document(finite, "numeric.mean", 1));
  auto plan =
      numeric_result_fixture::take(Compiler(registry).compile(graph)).plan;
  const auto prior = std::fegetround();
  PS_CHECK(std::fesetround(FE_UPWARD) == 0);
  // New workers inherit the nondefault environment; every poll must restore
  // strict arithmetic instead of relying on worker startup defaults.
  ExecutionContext context(registry, managed());
  auto result = context.execute(plan, bindings(context, finite));
  const auto restored = std::fegetround();
  std::fesetround(prior);
  PS_CHECK(result.ok() && number(result.value().results.at("result")) == 1 &&
           restored == FE_UPWARD);
  for (const auto block : {0, 65537}) {
    GraphContext bad(document(finite, "numeric.mean", block));
    PS_CHECK(Compiler(registry).compile(bad).status().code ==
             ErrorCode::InvalidArgument);
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(order_and_cache() == 0);
  PS_CHECK(result_and_block_reuse() == 0);
  PS_CHECK(edited_block_cache() == 0);
  PS_CHECK(bounded_source() == 0);
  PS_CHECK(failures_and_environment() == 0);
  PS_CHECK(typed_channels_and_cancellation() == 0);
  PS_CHECK(exact_rank_eight_reads() == 0);
  return 0;
}
