#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
  doc.inputs = {{1, "x", source.descriptor(), source.region(), source.layout(),
                 source.facets()}};
  doc.nodes = {{1, key, {WorkflowInputReference{1}}, {{"block_size", block}}}};
  doc.outputs = {{"result", 1, "value"}};
  return doc;
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
          auto plan = Compiler(registry).compile(graph).take_value().plan;
          InputSnapshotStore store({4096, 3});
          auto snapshot = std::make_shared<const InputSnapshot>(
              store.import_value(source).take_value());
          ExecutionContext context(registry, {1, false, 8, 4096, 128});
          auto demand = context.open_demand(plan, {{{"x", {}, {}, snapshot}}})
                            .take_value();
          const auto q = Footprint::all({1}).take_value();
          const double expected = oracle(numbers, variance, fp32);
          for (unsigned warm = 0; warm < 2; ++warm) {
            auto result = demand.request({{"result", q}});
            if (!result.ok())
              std::cerr << "block=" << block << " rank=" << shape.size() << ": "
                        << result.status().message << '\n';
            PS_CHECK(result.ok());
            double actual = 0;
            PS_CHECK(
                result.value().values.at("result").read({0}, &actual, 8).ok());
            PS_CHECK(std::memcmp(&actual, &expected, 8) == 0);
            PS_CHECK(result.value().diagnostics.cache_hits == warm);
            PS_CHECK(result.value().dependencies.source_support().value().at(
                         "x") == Footprint::all(shape).take_value());
            PS_CHECK(
                result.value()
                    .dependencies
                    .potential_dirty("x", Footprint::all(shape).take_value())
                    .value()
                    .at("result") == q);
          }
        }
  return 0;
}
int bounded_source() {
  auto registry = make_default_operation_registry();
  for (bool opaque : {false, true})
    for (bool variance : {false, true}) {
      auto declaration = input({4096}, std::vector<double>(4096, 1));
      if (opaque)
        declaration =
            Value::create(declaration.descriptor(), declaration.region(),
                          declaration.layout(), declaration.copy_bytes(),
                          {{"vendor.proof", 1, {42}}})
                .take_value();
      GraphContext graph(document(
          declaration, variance ? "numeric.variance" : "numeric.mean", 64));
      auto plan = Compiler(registry).compile(graph).take_value().plan;
      auto source = std::make_shared<RegionalSource>();
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
      ExecutionContext context(registry, {1, false, 8, 1024});
      auto result = context.execute(plan, {{{"x", {}, source}}});
      if (!result.ok())
        std::cerr << result.status().message << '\n';
      PS_CHECK(result.ok());
      // Uniform repetitions of 0,1,2,3: mean=1.5, population variance=1.25.
      PS_CHECK(result.value().values.at("result").as_float64().value() ==
               (variance ? 1.25 : 1.5));
      PS_CHECK(reads == (variance ? 128U : 64U) && next == 0);
      PS_CHECK(result.value().diagnostics.peak_live_bytes <= 1024);
      PS_CHECK(result.value().diagnostics.source_read_bytes ==
               32768 * (variance ? 2U : 1U));
    }
  return 0;
}
int exact_rank_eight_reads() {
  auto registry = make_default_operation_registry();
  const std::vector<std::uint64_t> shape{2, 2, 2, 2, 2, 2, 2, 32};
  const auto declaration = input(shape, std::vector<double>(4096, 1));
  GraphContext graph(document(declaration, "numeric.variance", 17));
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto source = std::make_shared<RegionalSource>();
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
  ExecutionContext context(registry, {1, false, 8, 1024});
  auto result = context.execute(plan, {{{"x", {}, source}}});
  if (!result.ok())
    std::cerr << result.status().message << '\n';
  PS_CHECK(result.ok() &&
           result.value().values.at("result").as_float64().value() == 1.25);
  PS_CHECK(visits == 8192 && next == 0 &&
           result.value().diagnostics.source_read_bytes == 65536);
  return 0;
}
int typed_channels_and_cancellation() {
  auto registry = make_default_operation_registry();
  std::vector<double> numbers;
  for (unsigned i = 0; i < 6; ++i)
    numbers.insert(numbers.end(), {-1, 2, 0, 1});
  auto plain = input({2, 3, 4}, numbers, true);
  auto image = Value::create(plain.descriptor(), plain.region(), plain.layout(),
                             plain.copy_bytes(),
                             {encode_semantic(rgba_semantics()).take_value()})
                   .take_value();
  for (auto block : {1, 5, 7}) {
    auto doc = document(image, "numeric.variance", block);
    GraphContext graph(doc);
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    auto source = std::make_shared<RegionalSource>();
    source->descriptor = image.descriptor();
    source->facets = image.facets();
    unsigned reads = 0;
    bool bad_tail = false;
    source->read = [&](const Region& region, std::uint8_t* destination,
                       std::uint64_t bytes, const BufferAllocator&,
                       const CancellationToken&) {
      if (region.dimensions()[2].offset || region.dimensions()[2].extent != 4)
        return Result<Region>(
            Status{ErrorCode::OperationFailed, "partial image channel read"});
      ++reads;
      for (std::uint64_t i = 0; i < bytes / 4; ++i) {
        float value = static_cast<float>(numbers[i % 4]);
        if (bad_tail && region.dimensions()[0].offset == 1 && i % 4 == 3)
          value = 2;
        std::memcpy(destination + i * 4, &value, 4);
      }
      return Result<Region>(region);
    };
    ExecutionContext context(registry, {1, false, 8, 1024});
    auto result = context.execute(plan, {{{"x", {}, source}}});
    PS_CHECK(result.ok() &&
             result.value().values.at("result").as_float64().value() == 1.25);
    const auto successful_reads = reads;
    reads = 0;
    bad_tail = true;
    PS_CHECK(context.execute(plan, {{{"x", {}, source}}}).status().code ==
             ErrorCode::OperationFailed);
    // Failure happens in global typed validation, before either arithmetic
    // pass.
    PS_CHECK(reads <= successful_reads / 3);
  }
  const auto value = input({256}, std::vector<double>(256, 1));
  GraphContext graph(document(value, "numeric.variance", 64));
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  auto source = std::make_shared<RegionalSource>();
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
  ExecutionContext context(registry, {1, false, 8, 1024});
  PS_CHECK(context.execute(plan, {{{"x", {}, source}}}, cancel.token())
               .status()
               .code == ErrorCode::Cancelled);
  PS_CHECK(reads == 5);
  // The cancelled state and stage buffers retire; the same context recovers.
  PS_CHECK(context.execute(plan, {{{"x", {}, source}}})
               .value()
               .values.at("result")
               .as_float64()
               .value() == 0);
  return 0;
}
int edited_block_cache() {
  auto registry = make_default_operation_registry();
  for (bool variance : {false, true}) {
    std::vector<double> numbers{1, 2, 3, 4};
    auto source = input({4}, numbers);
    GraphContext graph(
        document(source, variance ? "numeric.variance" : "numeric.mean", 2));
    auto plan = Compiler(registry).compile(graph).take_value().plan;
    ExecutionContext context(registry, {1, false, 8, 4096, 512});
    auto demand = context.open_demand(plan, {{{"x", source}}}).take_value();
    DemandQuery q{{"result", Footprint::all({1}).take_value()}};
    auto initial = demand.request(q);
    PS_CHECK(initial.ok() && initial.value().diagnostics.block_cache_misses ==
                                 (variance ? 4 : 2));
    // The unchanged first block retains its incoming sum. Variance pass two
    // must miss even that block because its fixed mean changes from 2.5 to 3.5.
    numbers[3] = 8;
    PS_CHECK(demand.replace_bindings({{{"x", input({4}, numbers)}}}).ok());
    auto changed = demand.request(q);
    PS_CHECK(
        changed.ok() && changed.value().diagnostics.block_cache_hits == 1 &&
        changed.value().diagnostics.block_cache_misses == (variance ? 3 : 1));
    double actual = 0, expected = oracle(numbers, variance, false);
    PS_CHECK(changed.value().values.at("result").read({0}, &actual, 8).ok());
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
      auto plan = Compiler(registry).compile(graph).take_value().plan;
      ExecutionContext context(registry, {1, false, 8, 4096, 64});
      auto demand = context.open_demand(plan, {{{"x", source}}}).take_value();
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
  auto plan = Compiler(registry).compile(graph).take_value().plan;
  const auto prior = std::fegetround();
  PS_CHECK(std::fesetround(FE_UPWARD) == 0);
  // New workers inherit the nondefault environment; every poll must restore
  // strict arithmetic instead of relying on worker startup defaults.
  ExecutionContext context(registry);
  auto result = context.execute(plan, {{{"x", finite}}});
  const auto restored = std::fegetround();
  std::fesetround(prior);
  PS_CHECK(result.ok() &&
           result.value().values.at("result").as_float64().value() == 1 &&
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
  PS_CHECK(edited_block_cache() == 0);
  PS_CHECK(bounded_source() == 0);
  PS_CHECK(failures_and_environment() == 0);
  PS_CHECK(typed_channels_and_cancellation() == 0);
  PS_CHECK(exact_rank_eight_reads() == 0);
  return 0;
}
