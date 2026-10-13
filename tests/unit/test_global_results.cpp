#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "photospider/data/result.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
SchemaTemplate points(PublishPolicy policy = PublishPolicy::StablePrefix) {
  SchemaTemplate schema;
  schema.id = "test.dynamic_ids";
  schema.publication = policy;
  schema.fields = {
      {"id", ElementType::Int64, {ResultExtentKind::RuntimeCount}, {}}};
  return schema;
}
ResourceBudget budget(std::uint64_t capacity = 32768) {
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = capacity;
  limits.capacity[ResourceKind::Metadata] = capacity;
  limits.capacity[ResourceKind::Shared] = capacity;
  return ResourceBudget(limits);
}
int descriptors_and_lifetime() {
  auto root = budget();
  ResultRef result;
  std::shared_ptr<const CpuStorage> held;
  {
    auto producer =
        ResultBuilder::start(root, points(), "snapshot-A").take_value();

    result = producer.reference();
    const auto id = result.object_id();

    const std::int64_t first = 7, second = 13;
    PS_CHECK(
        producer
            .append(0, 1,
                    ByteView(reinterpret_cast<const std::uint8_t*>(&first), 8))
            .ok());
    PS_CHECK(producer.publish(0, 1).ok());
    auto old = result.descriptor(false).take_value();
    PS_CHECK(!old.sealed() && old.rows(0) == 1 && old.object_id() == id);
    PS_CHECK(!result.descriptor().ok());
    PS_CHECK(
        producer
            .append(0, 1,
                    ByteView(reinterpret_cast<const std::uint8_t*>(&second), 8))
            .ok());
    PS_CHECK(producer.publish(0, 2).ok());
    PS_CHECK(!result.prepare_read(old, 0, 1, 1).ok());
    PS_CHECK(producer.seal().ok());
    auto latest = result.descriptor().take_value();
    PS_CHECK(latest.sealed() && latest.rows(0) == 2 &&
             latest.object_id() == id && latest.revision() > old.revision());
    auto read = result.prepare_read(latest, 0, 0, 2).take_value();
    PS_CHECK(!read.load(8).ok());
    held = read.load(16).take_value();
    auto other =
        ResultBuilder::start(root, points(), "snapshot-B").take_value();
    PS_CHECK(!other.reference().prepare_read(latest, 0, 0, 1).ok());
  }
  result = {};
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 32);
  std::int64_t readback[2]{};
  std::memcpy(readback, held->bytes().data(), 16);
  PS_CHECK(readback[0] == 7 && readback[1] == 13);
  held.reset();
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int empty_and_failure() {
  auto root = budget();
  for (auto policy :
       {PublishPolicy::CompleteBundle, PublishPolicy::IndependentChunks,
        PublishPolicy::StablePrefix}) {
    auto producer =
        ResultBuilder::start(root, points(policy), "empty").take_value();

    PS_CHECK(producer.publish(0, 0).ok());
    auto result = producer.seal().take_value();
    auto facts = result.descriptor().take_value();
    PS_CHECK(facts.rows(0) == 0);
    PS_CHECK(result.prepare_read(facts, 0, 0, 0).value().byte_size() == 0);
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  }
  auto producer =
      ResultBuilder::start(root, points(), "prefix", {1, 8}).take_value();

  std::int64_t value = 19;
  auto bytes = ByteView(reinterpret_cast<const std::uint8_t*>(&value), 8);
  PS_CHECK(producer.append(0, 1, bytes).ok());
  PS_CHECK(producer.publish(0, 1).ok());
  auto result = producer.reference();
  PS_CHECK(!producer.append(0, 1, bytes).ok());
  PS_CHECK(!producer.seal().ok());
  PS_CHECK(result.production_status().code == ErrorCode::ResourceExhausted);
  auto prefix = result.descriptor(false).take_value();
  PS_CHECK(result.prepare_read(prefix, 0, 0, 1).value().load(8).ok());
  auto bad_schema = points();
  bad_schema.fields[0].record_shape = {0};
  PS_CHECK(!ResultBuilder::start(root, bad_schema, "bad").ok());
  {
    auto builder =
        ResultBuilder::start(root, points(), "failed-status").take_value();
    auto reference = builder.reference();
    Status failure{ErrorCode::OperationFailed,
                   "domain prerequisite failed",
                   FailureReason::InvalidDomain,
                   {FailureOrigin::Schema, FailureScope::Association}};
    failure.detail.association = reference.object_id();
    failure.detail.node_id = 77;
    builder.fail(failure);
    builder.fail(Status{ErrorCode::Cancelled, {}});
    for (const auto& observed :
         {reference.production_status(), reference.descriptor().status(),
          builder.seal().status()}) {
      PS_CHECK(observed.reason == failure.reason &&
               observed.message == failure.message &&
               observed.detail.association == failure.detail.association &&
               observed.detail.node_id == 77);
    }
  }
  return 0;
}
int field_memory_limit() {
  ResourceLimits limits;
  limits.capacity[ResourceKind::Payload] = 16;
  ResourceBudget root(limits);
  std::shared_ptr<const CpuStorage> escaped;
  {
    auto builder =
        ResultBuilder::start(root, points(), "bounded-field").take_value();

    const std::int64_t values[]{7, 11};
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(values);
    PS_CHECK(builder.append(0, 1, {bytes, 8}).ok());
    PS_CHECK(builder.publish(0, 1).ok());
    auto result = builder.reference();
    const auto before = result.descriptor(false).take_value();
    auto old = result.prepare_read(before, 0, 0, 1).take_value();
    const auto rejected = builder.append(0, 2, {bytes, 16});
    PS_CHECK(rejected.code == ErrorCode::ResourceExhausted &&
             rejected.reason == FailureReason::CapacityLimit);
    const auto after = result.descriptor(false).take_value();
    PS_CHECK(after.rows(0) == 1 && after.revision() == before.revision());
    PS_CHECK(!result.prepare_read(after, 0, 1, 1).ok());
    escaped = old.load(8).take_value();
  }
  std::int64_t value = 0;
  std::memcpy(&value, escaped->bytes().data(), 8);
  PS_CHECK(value == 7 && root.statistics().live[ResourceKind::Payload] == 16);
  escaped.reset();
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int paging() {
  auto root = budget(131072);
  auto producer = ResultBuilder::start(root, points(), "large").take_value();

  for (std::int64_t first = 0; first < 8192; first += 64) {
    std::int64_t page[64]{};
    for (std::int64_t i = 0; i < 64; ++i)
      page[i] = first + i;
    PS_CHECK(producer
                 .append(0, 64,
                         ByteView(reinterpret_cast<const std::uint8_t*>(page),
                                  sizeof(page)))
                 .ok());
    PS_CHECK(producer.publish(0, first + 64).ok());
  }
  auto result = producer.seal().take_value();
  auto facts = result.descriptor().take_value();
  PS_CHECK(facts.rows(0) == 8192);
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 65536);
  for (std::uint64_t first = 0; first < 8192; first += 127) {
    const auto count = std::min<std::uint64_t>(127, 8192 - first);
    auto read = result.prepare_read(facts, 0, first, count)
                    .value()
                    .load(1024)
                    .take_value();
    for (std::uint64_t i = 0; i < count; ++i) {
      std::int64_t value = -1;
      std::memcpy(&value, read->bytes().data() + i * 8, 8);
      PS_CHECK(value == static_cast<std::int64_t>(first + i));
    }
  }
  PS_CHECK(root.statistics().peak[ResourceKind::Host] <= 131072);
  return 0;
}
int association() {
  auto root = budget();
  auto schema = points(PublishPolicy::CompleteBundle);
  schema.fields.push_back({"value",
                           ElementType::Float64,
                           {ResultExtentKind::FieldRows, 1, 0, 0, 0},
                           {}});
  auto builder = ResultBuilder::start(root, schema, "association").take_value();

  std::int64_t id = 3;
  PS_CHECK(
      builder
          .append(0, 1, ByteView(reinterpret_cast<const std::uint8_t*>(&id), 8))
          .ok());

  PS_CHECK(builder.publish(0, 1).ok());
  PS_CHECK(!builder.reference().descriptor(false).ok());
  PS_CHECK(builder.publish(1, 0).ok());
  PS_CHECK(!builder.seal().ok());
  auto spectrum = points();
  spectrum.domain = {{ResultExtentKind::InputAxis, 1, 0, 0},
                     {ResultExtentKind::InputAxis, 1, 0, 1}};
  auto even = spectrum.resolve({{3, 4}}).take_value();
  auto odd = spectrum.resolve({{3, 5}}).take_value();
  PS_CHECK(even.canonical() != odd.canonical());
  return 0;
}
int disjoint_tensor_publications() {
  auto root = budget(131072);
  SchemaTemplate schema;
  schema.id = "test.disjoint.tensor";
  ResultTensorSpec tensor;
  tensor.key = "data";
  tensor.descriptor = {ElementType::Float64, {3}};
  schema.tensors.push_back(std::move(tensor));
  auto builder =
      ResultBuilder::start(root, schema, "disjoint.relations").take_value();

  const double seven = 7;
  for (std::uint64_t at : {0U, 2U}) {
    PS_CHECK(builder
                 .publish_tensor(
                     0, Region({{at, 1}}),
                     ByteView(reinterpret_cast<const std::uint8_t*>(&seven),
                              sizeof(seven)))
                 .ok());
  }
  auto result = builder.seal().take_value();
  for (std::uint64_t at : {0U, 2U}) {
    double value = 0;
    PS_REQUIRE_OK(result.read_tensor(result.descriptor().take_value(), 0, {at},
                                     &value, sizeof(value)));
    PS_CHECK(value == seven);
  }
  double unread = 0;
  PS_CHECK(result
               .read_tensor(result.descriptor().take_value(), 0, {1}, &unread,
                            sizeof(unread))
               .code == ErrorCode::InvalidArgument);
  return 0;
}
int tensor_publication_frontiers() {
  auto root = budget(524288);
  SchemaTemplate schema;
  schema.id = "test.relation.frontier";
  schema.publication = PublishPolicy::StablePrefix;
  ResultTensorSpec tensor;
  tensor.key = "data";
  tensor.descriptor = {ElementType::Float64, {2, 2}};
  schema.tensors.push_back(std::move(tensor));
  auto builder = ResultBuilder::start(root, schema, "frontier").take_value();

  std::vector<ResultRef> prefixes;
  const double seven = 7;
  for (std::uint64_t point = 0; point < 4; ++point) {
    PS_CHECK(builder
                 .publish_tensor(
                     0, Region({{point / 2, 1}, {point % 2, 1}}),
                     ByteView(reinterpret_cast<const std::uint8_t*>(&seven),
                              sizeof(seven)))
                 .ok());
    prefixes.push_back(builder.reference().capture().take_value());
  }
  PS_CHECK(builder.seal().ok());

  // Later compaction must not widen an already captured publication prefix.
  for (std::size_t prefix = 0; prefix < prefixes.size(); ++prefix) {
    auto facts = prefixes[prefix].descriptor(false).take_value();
    for (std::uint64_t point = 0; point < 4; ++point) {
      double value = 0;
      auto status = prefixes[prefix].read_tensor(
          facts, 0, {point / 2, point % 2}, &value, sizeof(value));
      PS_CHECK(point <= prefix ? status.ok() && value == seven
                               : status.code == ErrorCode::InvalidArgument);
    }
  }
  return 0;
}
int batched_tensor_transaction() {
  for (unsigned action = 0; action < 6; ++action) {
    auto root = budget(UINT64_C(1) << 22);
    SchemaTemplate schema;
    schema.id = "test.batch_tensor";
    schema.publication = PublishPolicy::IndependentChunks;
    ResultTensorSpec tensor;
    tensor.key = "values";
    tensor.descriptor = {ElementType::Float32, {4, 4}};
    schema.tensors.push_back(tensor);
    auto producer =
        ResultBuilder::start(root, schema, "batch.test").take_value();

    const bool failure = action >= 1 && action <= 4;
    const float prior_value = 19;
    if (failure)
      PS_REQUIRE_OK(producer.publish_tensor(
          0, Region({{3, 1}, {0, 1}}),
          ByteView(reinterpret_cast<const std::uint8_t*>(&prior_value), 4)));
    const auto reference = producer.reference();
    const auto old = reference.descriptor(false).take_value();
    auto samples = Footprint::from_regions({4, 4}, {Region({{0, 1}, {0, 2}}),
                                                    Region({{2, 1}, {1, 3}})})
                       .take_value();
    const auto expected = samples;
    const auto before = root.statistics().live[ResourceKind::Payload];
    CancellationSource cancellation;
    unsigned callbacks = 0;
    auto status = producer.publish_tensor_kernel(
        0, samples,
        [&](const auto& windows) -> Status {
          ++callbacks;
          for (const auto& window : windows) {
            const auto& d = window.region().dimensions();
            for (std::uint64_t y = d[0].offset; y < d[0].offset + d[0].extent;
                 ++y)
              for (std::uint64_t x = d[1].offset; x < d[1].offset + d[1].extent;
                   ++x) {
                auto run = window.row_run({y, x});
                if (!run.ok())
                  return run.status();
                const float value = static_cast<float>(y * 4 + x);
                std::memcpy(run.value().data, &value, 4);
              }
          }
          if (action == 1)
            return {ErrorCode::OperationFailed, "producer test failure"};
          if (action == 2)
            throw std::runtime_error("producer test exception");
          if (action == 3)
            cancellation.cancel();
          if (action == 4) {
            auto nested = producer.publish_tensor(
                0, Region({{3, 1}, {3, 1}}),
                ByteView(reinterpret_cast<const std::uint8_t*>(&prior_value),
                         4));
            if (nested.ok())
              return {ErrorCode::OperationFailed,
                      "nested mutation was accepted"};
          }
          if (action == 5)
            samples = Footprint::none({4, 4}).take_value();
          return Status::success();
        },
        cancellation.token());
    PS_CHECK(callbacks == 1);
    if (failure) {
      PS_CHECK(!status.ok() && !producer.seal().ok());
      PS_CHECK(root.statistics().live[ResourceKind::Payload] == before);
      float value = 0;
      PS_REQUIRE_OK(reference.read_tensor(old, 0, {3, 0}, &value, 4));
      PS_CHECK(value == prior_value);
    } else {
      PS_REQUIRE_OK(status);
      auto result = producer.seal().take_value();
      auto facts = result.descriptor().take_value();
      PS_CHECK(facts.tensor_coverage(0) == expected);
      for (const auto& box : expected.boxes()) {
        auto window = result.acquire_tensor(facts, 0, box);
        PS_REQUIRE_OK(window);
        const auto& d = box.dimensions();
        for (std::uint64_t x = d[1].offset; x < d[1].offset + d[1].extent;
             ++x) {
          float value = -1;
          auto run = window.value().row_run({d[0].offset, x});
          PS_REQUIRE_OK(run);
          std::memcpy(&value, run.value().data, 4);
          PS_CHECK(value == d[0].offset * 4 + x);
        }
      }
      PS_CHECK(!result.acquire_tensor(facts, 0, Region({{0, 3}, {0, 4}})).ok());
    }
  }
  auto root = budget();
  SchemaTemplate schema;
  schema.id = "test.batch_domain";
  ResultTensorSpec tensor;
  tensor.key = "values";
  tensor.descriptor = {ElementType::Float32, {4}};
  schema.tensors.push_back(tensor);
  auto producer =
      ResultBuilder::start(root, schema, "batch.domain").take_value();

  auto wrong = Footprint::from_regions({8}, {Region({{0, 1}})}).take_value();
  bool called = false;
  auto status = producer.publish_tensor_kernel(0, wrong, [&](const auto&) {
    called = true;
    return Status::success();
  });
  PS_CHECK(!status.ok() && !called);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(batched_tensor_transaction() == 0);
  PS_CHECK(descriptors_and_lifetime() == 0);
  PS_CHECK(empty_and_failure() == 0);
  PS_CHECK(paging() == 0);
  PS_CHECK(field_memory_limit() == 0);
  PS_CHECK(association() == 0);
  PS_CHECK(disjoint_tensor_publications() == 0);
  PS_CHECK(tensor_publication_frontiers() == 0);
  return 0;
}
