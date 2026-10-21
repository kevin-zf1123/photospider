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
SchemaTemplate points() {
  SchemaTemplate schema;
  schema.id = "test.ids";
  ResultTensorSpec tensor;
  tensor.key = "id";
  tensor.descriptor = {ElementType::Int64, {2}};
  schema.tensors.push_back(std::move(tensor));
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
  ResultTensorReadWindow held;
  {
    auto producer =
        ResultBuilder::start(root, points(), "snapshot-A").take_value();
    result = producer.reference();
    const auto id = result.object_id();
    const std::int64_t first = 7, second = 13;
    PS_REQUIRE_OK(producer.publish_tensor(
        0, Region({{0, 1}}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&first), 8)));
    auto old = result.descriptor(false).take_value();
    PS_CHECK(!old.sealed() && old.tensor_coverage(0).contains({0}) &&
             old.object_id() == id);
    PS_CHECK(!result.descriptor().ok());
    PS_REQUIRE_OK(producer.publish_tensor(
        0, Region({{1, 1}}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&second), 8)));
    PS_CHECK(!result.acquire_tensor(old, 0, Region({{1, 1}})).ok());
    PS_CHECK(producer.seal().ok());
    auto latest = result.descriptor().take_value();
    PS_CHECK(latest.sealed() && latest.tensor_coverage(0).contains({1}) &&
             latest.object_id() == id && latest.revision() > old.revision());
    held = result.acquire_tensor(latest, 0, Region::whole({2})).take_value();
    auto other =
        ResultBuilder::start(root, points(), "snapshot-B").take_value();
    PS_CHECK(
        !other.reference().acquire_tensor(latest, 0, Region({{0, 1}})).ok());
  }
  result = {};
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 16);
  for (std::uint64_t i = 0; i < 2; ++i) {
    std::int64_t value = 0;
    auto row = held.row_run({i}).take_value();
    std::memcpy(&value, row.data, 8);
    PS_CHECK(value == (i ? 13 : 7));
  }
  held = {};
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int empty_and_failure() {
  auto root = budget();
  {
    auto producer = ResultBuilder::start(root, points(), "empty").take_value();
    auto result = producer.seal().take_value();
    auto facts = result.descriptor().take_value();
    PS_CHECK(facts.sealed() && facts.tensor_coverage(0).empty());
    PS_CHECK(!result.acquire_tensor(facts, 0, Region::whole({2})).ok());
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  }
  auto bad_schema = points();
  bad_schema.tensors[0].descriptor.shape = {0};
  PS_CHECK(!ResultBuilder::start(root, bad_schema, "bad").ok());
  {
    auto builder =
        ResultBuilder::start(root, points(), "failed-status").take_value();
    auto reference = builder.reference();
    Status failure{ErrorCode::OperationFailed,
                   "domain prerequisite failed",
                   FailureReason::InvalidDomain,
                   {FailureOrigin::Schema, FailureScope::Group}};
    failure.detail.node_id = 77;
    builder.fail(failure);
    builder.fail(Status{ErrorCode::Cancelled, {}});
    for (const auto& observed :
         {reference.production_status(), reference.descriptor().status(),
          builder.seal().status()}) {
      PS_CHECK(observed.reason == failure.reason &&
               observed.message == failure.message &&
               observed.detail.origin == failure.detail.origin &&
               observed.detail.scope == failure.detail.scope &&
               observed.detail.node_id == 77);
    }
  }
  return 0;
}
int tensor_memory_limit() {
  ResourceLimits limits;
  limits.capacity[ResourceKind::Payload] = 8;
  ResourceBudget root(limits);
  ResultTensorReadWindow escaped;
  {
    auto builder =
        ResultBuilder::start(root, points(), "bounded-tensor").take_value();
    const std::int64_t value = 7;
    const auto bytes =
        ByteView(reinterpret_cast<const std::uint8_t*>(&value), 8);
    PS_REQUIRE_OK(builder.publish_tensor(0, Region({{0, 1}}), bytes));
    auto result = builder.reference();
    const auto before = result.descriptor(false).take_value();
    escaped = result.acquire_tensor(before, 0, Region({{0, 1}})).take_value();
    const auto rejected = builder.publish_tensor(0, Region({{1, 1}}), bytes);
    PS_CHECK(rejected.code == ErrorCode::ResourceExhausted &&
             rejected.reason == FailureReason::CapacityLimit);
    const auto after = result.descriptor(false).take_value();
    PS_CHECK(after.tensor_coverage(0) == before.tensor_coverage(0) &&
             after.revision() == before.revision());
    PS_CHECK(!result.acquire_tensor(after, 0, Region({{1, 1}})).ok());
  }
  std::int64_t value = 0;
  std::memcpy(&value, escaped.row_run({0}).value().data, 8);
  PS_CHECK(value == 7 && root.statistics().live[ResourceKind::Payload] == 8);
  escaped = {};
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int window_admission_and_lifetime() {
  auto root = budget(1048576);
  ResultTensorReadWindow retained;
  {
    auto schema = points();
    schema.tensors[0].descriptor.shape = {65};
    auto started = ResultBuilder::start(root, schema, "fragmented.window");
    PS_REQUIRE_OK(started);
    auto builder = started.take_value();
    for (std::uint64_t i = 0; i < 65; ++i) {
      const std::int64_t value = i + 2;
      PS_REQUIRE_OK(builder.publish_tensor(
          0, Region({{i, 1}}),
          ByteView(reinterpret_cast<const std::uint8_t*>(&value), 8)));
    }
    auto sealed = builder.seal();
    PS_REQUIRE_OK(sealed);
    auto result = sealed.take_value();
    auto descriptor = result.descriptor();
    PS_REQUIRE_OK(descriptor);
    auto facts = descriptor.take_value();
    const auto before = root.statistics().live.values;
    CancellationSource cancellation;
    cancellation.cancel();
    {
      auto denied = result.acquire_tensor(facts, 0, Region::whole({65}),
                                          cancellation.token());
      PS_CHECK(denied.status().code == ErrorCode::Cancelled);
    }
    PS_CHECK(root.statistics().live.values == before);
    {
      auto fill = root.reserve(
          ResourceCapacity::host(root.available_capacity()[ResourceKind::Host] -
                                 ResourceBudget::lease_metadata_bytes()));
      PS_REQUIRE_OK(fill);
      auto denied = result.acquire_tensor(facts, 0, Region::whole({65}));
      PS_CHECK(denied.status().code == ErrorCode::ResourceExhausted);
    }
    PS_CHECK(root.statistics().live.values == before);
    auto recovered = result.acquire_tensor(facts, 0, Region::whole({65}));
    PS_REQUIRE_OK(recovered);
    retained = recovered.take_value();
  }
  // The window owns all fragments after the producer, Result and facts retire.
  for (std::uint64_t next = 0; next < 65;) {
    auto row = retained.row_run({next});
    PS_REQUIRE_OK(row);
    PS_CHECK(row.value().samples && row.value().samples <= 65 - next);
    for (std::uint64_t i = 0; i < row.value().samples; ++i) {
      std::int64_t value = 0;
      std::memcpy(&value,
                  row.value().data + i * row.value().sample_stride_bytes, 8);
      PS_CHECK(value == static_cast<std::int64_t>(next + i + 2));
    }
    next += row.value().samples;
  }
  retained = {};
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int batch_admission_rollback() {
  ResourceLimits limits;
  limits.capacity[ResourceKind::Payload] = 12;
  ResourceBudget root(limits);
  {
    auto schema = points();
    auto& tensor = schema.tensors[0];
    tensor.descriptor = {ElementType::Float32, {1, 1}};
    tensor.batch_axes = {3, 1};
    tensor.layout.spatial = true;
    tensor.layout.channel_axis.reset();
    auto started = ResultBuilder::start(root, schema, "batch.admission");
    PS_REQUIRE_OK(started);
    auto builder = started.take_value();
    const float prior = 4;
    PS_REQUIRE_OK(builder.publish_tensor(
        0, Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&prior), 4)));
    auto captured = builder.reference().capture();
    PS_REQUIRE_OK(captured);
    auto old = captured.take_value();
    auto old_facts = old.descriptor(false);
    PS_REQUIRE_OK(old_facts);
    ResourceCapacity occupied;
    occupied[ResourceKind::Payload] = 4;
    auto blocker = root.reserve(occupied);
    PS_REQUIRE_OK(blocker);
    const auto before = root.statistics().live[ResourceKind::Payload];
    unsigned callbacks = 0;
    auto status = builder.publish_tensor_kernel(
        0, Region({{1, 2}, {0, 1}, {0, 1}, {0, 1}}), [&](const auto&) {
          ++callbacks;
          return Status::success();
        });
    PS_CHECK(status.code == ErrorCode::ResourceExhausted && callbacks == 0);
    PS_CHECK(root.statistics().live[ResourceKind::Payload] == before);
    auto after = builder.reference().descriptor(false);
    PS_REQUIRE_OK(after);
    PS_CHECK(after.value().revision() == old_facts.value().revision());
    PS_CHECK(after.value().tensor_coverage(0) ==
             old_facts.value().tensor_coverage(0));
    float observed = 0;
    PS_REQUIRE_OK(
        old.read_tensor(old_facts.value(), 0, {0, 0, 0, 0}, &observed, 4));
    PS_CHECK(observed == prior);
    PS_CHECK(builder.seal().status().code == ErrorCode::ResourceExhausted);
  }
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
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
  PS_CHECK(tensor_memory_limit() == 0);
  PS_CHECK(window_admission_and_lifetime() == 0);
  PS_CHECK(batch_admission_rollback() == 0);
  PS_CHECK(disjoint_tensor_publications() == 0);
  PS_CHECK(tensor_publication_frontiers() == 0);
  return 0;
}
