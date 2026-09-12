#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
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
ResourceBudget budget() {
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = 16384;
  limits.capacity[ResourceKind::Metadata] = 16384;
  limits.capacity[ResourceKind::Shared] = 16384;
  return ResourceBudget(limits);
}
int descriptors_and_lifetime() {
  auto root = budget();
  ResultRef result;
  std::shared_ptr<const CpuStorage> held;
  {
    auto producer =
        ResultBuilder::start(root, points(), "snapshot-A").take_value();
    PS_CHECK(producer
                 .bind_descriptor_relation(
                     ResultRelation::unknown(root, 1).take_value())
                 .ok());
    result = producer.reference();
    const auto id = result.object_id();
    auto relation = ResultRelation::cartesian(root, 10000, {0, 15, 0, 10000},
                                              DependencyGuarantee::Conservative)
                        .take_value();
    const std::int64_t first = 7, second = 13;
    PS_CHECK(
        producer
            .append(0, 1,
                    ByteView(reinterpret_cast<const std::uint8_t*>(&first), 8))
            .ok());
    PS_CHECK(producer.publish(0, 1, relation, {true, true, true, true}).ok());
    auto old = result.descriptor(false).take_value();
    PS_CHECK(!old.sealed() && old.rows(0) == 1 && old.object_id() == id);
    PS_CHECK(!result.descriptor().ok());
    PS_CHECK(
        producer
            .append(0, 1,
                    ByteView(reinterpret_cast<const std::uint8_t*>(&second), 8))
            .ok());
    PS_CHECK(producer.publish(0, 2, relation, {true, true, true, true}).ok());
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
  PS_CHECK(root.statistics().live[ResourceKind::Disk] > 0);
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
    PS_CHECK(producer
                 .bind_descriptor_relation(
                     ResultRelation::unknown(root, 1).take_value())
                 .ok());
    auto relation =
        ResultRelation::cartesian(root, 0, {0, 15, 0, 0}).take_value();
    PS_CHECK(producer.publish(0, 0, relation, {true, true, true, true}).ok());
    auto result = producer.seal().take_value();
    auto facts = result.descriptor().take_value();
    PS_CHECK(facts.rows(0) == 0);
    PS_CHECK(result.prepare_read(facts, 0, 0, 0).value().byte_size() == 0);
    PS_CHECK(root.statistics().live[ResourceKind::Disk] == 0);
  }
  auto producer =
      ResultBuilder::start(root, points(), "prefix", {1, 8}).take_value();
  PS_CHECK(producer
               .bind_descriptor_relation(
                   ResultRelation::unknown(root, 1).take_value())
               .ok());
  auto relation = ResultRelation::identity(root, 1).take_value();
  std::int64_t value = 19;
  auto bytes = ByteView(reinterpret_cast<const std::uint8_t*>(&value), 8);
  PS_CHECK(producer.append(0, 1, bytes).ok());
  PS_CHECK(producer.publish(0, 1, relation, {true, true, true, true}).ok());
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
int paging() {
  auto root = budget();
  auto producer = ResultBuilder::start(root, points(), "large").take_value();
  PS_CHECK(producer
               .bind_descriptor_relation(
                   ResultRelation::unknown(root, 1).take_value())
               .ok());
  auto relation = ResultRelation::cartesian(root, 8192, {0, 15, 0, 8192},
                                            DependencyGuarantee::Conservative)
                      .take_value();
  for (std::int64_t first = 0; first < 8192; first += 64) {
    std::int64_t page[64]{};
    for (std::int64_t i = 0; i < 64; ++i)
      page[i] = first + i;
    PS_CHECK(producer
                 .append(0, 64,
                         ByteView(reinterpret_cast<const std::uint8_t*>(page),
                                  sizeof(page)))
                 .ok());
    PS_CHECK(producer.publish(0, first + 64, relation, {true, true, true, true})
                 .ok());
  }
  auto result = producer.seal().take_value();
  auto facts = result.descriptor().take_value();
  PS_CHECK(facts.rows(0) == 8192);
  PS_CHECK(root.statistics().live[ResourceKind::Disk] > 16384);
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
  PS_CHECK(root.statistics().peak[ResourceKind::Host] <= 16384);
  return 0;
}
int relations() {
  auto root = budget();
  auto empty_rows =
      ResultRelation::rows(root, 1, 1, [](auto) {
        return Result<ResultRelationRow>(ResultRelationRow{0, {0, 1, 5, 0}});
      }).take_value();
  unsigned visits = 0;
  PS_CHECK(empty_rows
               .visit(0, 100,
                      [&](ResultSupport) {
                        ++visits;
                        return Status::success();
                      })
               .ok());
  PS_CHECK(visits == 0);
  PS_CHECK(!empty_rows.intersects(0, {{0, 1, 4, 2}}, 100).value().value());
  auto span = ResultRelation::cartesian(root, 1, {0, 1, 4, 2}).take_value();
  PS_CHECK(!span.intersects(0, {{0, 1, 5, 0}}, 100).value().value());
  PS_CHECK(span.intersects(0, {{0, 1, 5, 1}}, 100).value().value());
  const std::vector<ResultRelationRow> rows{{0, {0, 1, 0, 1}},
                                            {1, {0, 1, 2, 1}},
                                            {3, {0, 1, 0, 1}},
                                            {3, {0, 1, 2, 1}}};
  auto relation = ResultRelation::rows(root, 4, rows.size(), [&](auto i) {
                    return Result<ResultRelationRow>(rows[i]);
                  }).take_value();
  for (unsigned mask = 0; mask < 8; ++mask) {
    std::vector<ResultSupport> changed;
    for (unsigned i = 0; i < 3; ++i)
      if (mask & (1U << i))
        changed.push_back({0, 15, i, 1});
    for (unsigned row = 0; row < 4; ++row) {
      bool expected = false;
      for (const auto& item : rows)
        expected |= item.output == row && (mask & (1U << item.support.first));
      auto dirty = relation.intersects(row, changed, 1000);
      PS_CHECK(dirty.ok() && dirty.value().value() == expected);
    }
  }
  auto broad = ResultRelation::cartesian(root, 4, {0, 15, 0, 3},
                                         DependencyGuarantee::Conservative)
                   .take_value();
  auto combined = ResultRelation::unite(root, {relation, broad}).take_value();
  PS_CHECK(combined.guarantee() == DependencyGuarantee::Conservative);
  PS_CHECK(combined.intersects(2, {{0, 1, 1, 1}}, 100).value().value());
  auto unknown = ResultRelation::unknown(root, 4).take_value();
  PS_CHECK(!unknown.intersects(0, {}, 1).value().has_value());
  auto mixed = ResultRelation::unite(root, {relation, unknown}).take_value();
  PS_CHECK(mixed.guarantee() == DependencyGuarantee::Unknown);
  auto identity = ResultRelation::identity(root, 4).take_value();
  auto composed =
      ResultRelation::compose(root, identity, relation, 1000).take_value();
  PS_CHECK(composed.intersects(3, {{0, 1, 2, 1}}, 1000).value().value());
  PS_CHECK(!relation.intersects(0, {{0, 1, 0, 1}}, 1).ok());
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
  PS_CHECK(builder
               .bind_descriptor_relation(
                   ResultRelation::unknown(root, 1).take_value())
               .ok());
  std::int64_t id = 3;
  PS_CHECK(
      builder
          .append(0, 1, ByteView(reinterpret_cast<const std::uint8_t*>(&id), 8))
          .ok());
  auto relation = ResultRelation::identity(root, 1).take_value();
  PS_CHECK(builder.publish(0, 1, relation, {true, true, true, true}).ok());
  PS_CHECK(!builder.reference().descriptor(false).ok());
  PS_CHECK(builder.publish(1, 0, relation, {true, true, true, true}).ok());
  PS_CHECK(!builder.seal().ok());
  auto spectrum = points();
  spectrum.domain = {{ResultExtentKind::InputAxis, 1, 0, 0},
                     {ResultExtentKind::InputAxis, 1, 0, 1}};
  auto even = spectrum.resolve({{3, 4}}).take_value();
  auto odd = spectrum.resolve({{3, 5}}).take_value();
  PS_CHECK(even.canonical() != odd.canonical());
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(descriptors_and_lifetime() == 0);
  PS_CHECK(empty_and_failure() == 0);
  PS_CHECK(paging() == 0);
  PS_CHECK(relations() == 0);
  PS_CHECK(association() == 0);
  return 0;
}
