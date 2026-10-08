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
  auto root = budget(16384);
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
  {
    auto repeated = combined;
    for (unsigned i = 0; i < 128; ++i) {
      auto next = ResultRelation::unite(root, {repeated, relation, broad});
      PS_CHECK(next.ok());
      repeated = next.take_value();
    }
    PS_CHECK(repeated.guarantee() == DependencyGuarantee::Conservative);
    for (unsigned row = 0; row < 4; ++row)
      for (unsigned changed = 0; changed < 5; ++changed) {
        const std::vector<ResultSupport> edit{{0, 15, changed, 1}};
        auto actual = repeated.intersects(row, edit, 100);
        auto expected = combined.intersects(row, edit, 100);
        PS_CHECK(actual.ok() && expected.ok() &&
                 actual.value() == expected.value());
      }

    PS_CHECK(repeated.intersects(2, {{0, 1, 1, 1}}, 100).value().value());
    PS_CHECK(!repeated.intersects(2, {{0, 1, 4, 1}}, 100).value().value());
    auto projected = Footprint::none({3}).take_value();
    auto projection = ResultRelation::unite(root, {broad, broad}).take_value();
    PS_CHECK(projection
                 .project(Footprint::all({4}).take_value(),
                          [&](ResultSupport support, const Footprint* samples) {
                            auto requested =
                                samples ? Result<Footprint>(*samples)
                                        : Footprint::from_regions(
                                              {3}, {Region({{support.first,
                                                             support.count}})});
                            if (!requested.ok())
                              return requested.status();
                            auto joined = projected.unite(requested.value());
                            if (!joined.ok())
                              return joined.status();
                            projected = joined.take_value();
                            return Status::success();
                          })
                 .ok());
    PS_CHECK(projected == Footprint::all({3}).take_value());
  }
  auto identity = ResultRelation::identity(root, 4).take_value();
  auto composed =
      ResultRelation::compose(root, identity, relation, 1000).take_value();
  PS_CHECK(composed.intersects(3, {{0, 1, 2, 1}}, 1000).value().value());
  PS_CHECK(!relation.intersects(0, {{0, 1, 0, 1}}, 1).ok());
  return 0;
}
int repeated_cartesian() {
  auto root = budget();
  ResultRelation combined;
  for (unsigned pass = 0; pass < 512; ++pass) {
    auto fresh = ResultRelation::cartesian(
        root, 65537, {0, 2, 0, 1, ResultSupportTarget::Tensor, 0});
    PS_CHECK(fresh.ok());
    auto next = combined.valid()
                    ? ResultRelation::unite(root, {combined, fresh.value()})
                    : fresh;
    PS_CHECK(next.ok());
    combined = next.take_value();
  }
  auto different =
      ResultRelation::cartesian(root, 65537,
                                {0, 4, 1, 1, ResultSupportTarget::Tensor, 0},
                                DependencyGuarantee::Conservative)
          .take_value();
  combined = ResultRelation::unite(root, {combined, different}).take_value();
  PS_CHECK(combined.guarantee() == DependencyGuarantee::Conservative);
  PS_CHECK(
      combined
          .intersects(65536, {{0, 2, 0, 1, ResultSupportTarget::Tensor, 0}}, 32)
          .value()
          .value());
  PS_CHECK(
      combined
          .intersects(65536, {{0, 4, 1, 1, ResultSupportTarget::Tensor, 0}}, 32)
          .value()
          .value());
  PS_CHECK(!combined
                .intersects(65536,
                            {{0, 2, 1, 1, ResultSupportTarget::Tensor, 0}}, 32)
                .value()
                .value());
  PS_CHECK(root.statistics().peak[ResourceKind::Metadata] <= 32768);
  return 0;
}
int identity_projection() {
  auto root = budget();
  const auto identity = ResultRelation::identity(root, 1000000, 0, 5,
                                                 ResultSupportTarget::Tensor, 2)
                            .take_value();
  const auto requested =
      Footprint::from_regions({700000},
                              {Region({{100, 500000}}), Region({{699999, 1}})})
          .take_value();
  FootprintLimits limits;
  limits.maximum_work = 4;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
  auto projected = identity.project(
      requested,
      [&](ResultSupport support, const Footprint* mapped) {
        if (mapped || support.target != ResultSupportTarget::Tensor ||
            support.slot != 2 || support.roles != 5)
          return Status{ErrorCode::InvalidArgument, "identity support"};
        spans.emplace_back(support.first, support.count);
        return Status::success();
      },
      limits);
  PS_CHECK(projected.ok());
  PS_CHECK((spans ==
            std::vector<std::pair<std::uint64_t, std::uint64_t>>{{100, 500000},
                                                                 {699999, 1}}));
  const auto beyond =
      Footprint::from_regions({1000001}, {Region({{1000000, 1}})}).take_value();
  PS_CHECK(
      identity
          .project(
              beyond,
              [](ResultSupport, const Footprint*) { return Status::success(); },
              limits)
          .code == ErrorCode::InvalidArgument);
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
int disjoint_tensor_relations(bool unknown = false) {
  auto root = budget(131072);
  SchemaTemplate schema;
  schema.id = "test.disjoint.tensor";
  ResultTensorSpec tensor;
  tensor.key = "data";
  tensor.descriptor = {ElementType::Float64, {3}};
  schema.tensors.push_back(std::move(tensor));
  auto builder =
      ResultBuilder::start(root, schema, "disjoint.relations").take_value();
  PS_CHECK(builder
               .bind_descriptor_relation(
                   ResultRelation::cartesian(root, 1, {}).take_value())
               .ok());
  const double seven = 7;
  for (std::uint64_t at : {0U, 2U}) {
    const ResultSupport support{
        static_cast<std::uint32_t>(at / 2),
        at ? 4U : 1U,
        at,
        1,
        at ? ResultSupportTarget::Field : ResultSupportTarget::Tensor,
        0};
    auto relation =
        ResultRelation::cartesian(root, 3, support,
                                  unknown && !at ? DependencyGuarantee::Unknown
                                                 : DependencyGuarantee::Exact)
            .take_value();
    PS_CHECK(builder
                 .publish_tensor(
                     0, Region({{at, 1}}),
                     ByteView(reinterpret_cast<const std::uint8_t*>(&seven),
                              sizeof(seven)),
                     std::move(relation), {true, true, true, true})
                 .ok());
  }
  auto result = builder.seal().take_value();
  auto relation = result.tensor_relation(0).take_value();
  for (std::uint64_t at : {0U, 2U}) {
    unsigned visits = 0;
    const auto check_span = [&](ResultSupport observed) {
      ++visits;
      return observed.input == at / 2 && observed.roles == (at ? 4U : 1U) &&
                     observed.first == at && observed.count == 1 &&
                     observed.target == (at ? ResultSupportTarget::Field
                                            : ResultSupportTarget::Tensor)
                 ? Status::success()
                 : Status{ErrorCode::InvalidArgument,
                          "support escaped its publication"};
    };
    auto visited = unknown ? relation.visit_declared(at, 32, check_span)
                           : relation.visit(at, 32, check_span);
    PS_CHECK(visited.ok());
    PS_CHECK(visits == 1);
    auto selected =
        Footprint::from_regions({3}, {Region({{at, 1}})}).take_value();
    unsigned projected = 0;
    PS_CHECK(relation
                 .project(selected,
                          [&](ResultSupport observed, const Footprint*) {
                            ++projected;
                            return observed.input == at / 2
                                       ? Status::success()
                                       : Status{ErrorCode::InvalidArgument,
                                                "projected foreign support"};
                          })
                 .ok());
    PS_CHECK(projected == 1);
  }
  auto hole = Footprint::from_regions({3}, {Region({{1, 1}})}).take_value();
  PS_CHECK(relation.certify(hole).code == ErrorCode::NotFound);
  PS_CHECK(relation
               .visit_declared(1, 32,
                               [](ResultSupport) { return Status::success(); })
               .code == ErrorCode::NotFound);
  auto all = Footprint::all({3}).take_value();
  auto changed = Footprint::from_regions({3}, {Region({{2, 1}})}).take_value();
  auto dirty = relation.preimage(
      all, {1, 4, 0, 0, ResultSupportTarget::Field, 0}, changed);
  PS_CHECK(dirty.ok() && dirty.value() == changed);
  if (unknown) {
    PS_CHECK(relation.guarantee() == DependencyGuarantee::Unknown);
    auto unresolved = relation.intersects(
        2, {{1, 4, 2, 1, ResultSupportTarget::Field, 0}}, 32);
    PS_CHECK(unresolved.ok() && !unresolved.value());
  }
  double unread = 0;
  PS_CHECK(result
               .read_tensor(result.descriptor().take_value(), 0, {1}, &unread,
                            sizeof(unread))
               .code == ErrorCode::InvalidArgument);
  return 0;
}
int tensor_relation_shape_admission() {
  auto root = budget(131072);
  SchemaTemplate schema;
  schema.id = "test.relation.shape";
  ResultTensorSpec tensor;
  tensor.key = "data";
  tensor.descriptor = {ElementType::Float64, {2, 3}};
  schema.tensors.push_back(std::move(tensor));
  const double seven = 7;
  for (bool union_relation : {false, true}) {
    auto builder =
        ResultBuilder::start(root, schema, "original.shape").take_value();
    PS_CHECK(builder
                 .bind_descriptor_relation(
                     ResultRelation::cartesian(root, 1, {}).take_value())
                 .ok());
    for (unsigned point = 0; point < (union_relation ? 2U : 1U); ++point) {
      auto relation = ResultRelation::cartesian(root, 6, {point, 1, point, 1},
                                                DependencyGuarantee::Unknown)
                          .take_value();
      PS_CHECK(builder
                   .publish_tensor(
                       0, Region({{1, 1}, {point, 1}}),
                       ByteView(reinterpret_cast<const std::uint8_t*>(&seven),
                                sizeof(seven)),
                       relation, {true, true, true, true})
                   .ok());
    }
    auto relation = builder.seal().take_value().tensor_relation(0).take_value();
    auto composed = ResultRelation::compose(
        root, ResultRelation::prefix(root, 6).take_value(),
        ResultRelation::cartesian(root, 6, {0, 1, 0, 1}).take_value(), 64);
    PS_CHECK(composed.ok());
    PS_CHECK(ResultRelation::unite(root, {relation, composed.value()})
                 .status()
                 .code == ErrorCode::InvalidArgument);
    auto wrong = Footprint::all({3, 2}).take_value();
    PS_CHECK(relation.certify(wrong).code == ErrorCode::InvalidArgument);
    PS_CHECK(relation.preimage(wrong, {0, 1, 0, 1}, wrong).status().code ==
             ErrorCode::InvalidArgument);
    auto reshaped = schema;
    reshaped.tensors[0].descriptor.shape = {3, 2};
    auto output =
        ResultBuilder::start(root, reshaped, "wrong.shape").take_value();
    PS_CHECK(output
                 .bind_descriptor_relation(
                     ResultRelation::cartesian(root, 1, {}).take_value())
                 .ok());
    const double values[6] = {};
    PS_CHECK(output
                 .publish_tensor(
                     0, Region::whole({3, 2}),
                     ByteView(reinterpret_cast<const std::uint8_t*>(values),
                              sizeof(values)),
                     relation, {true, true, true, true})
                 .code == ErrorCode::InvalidArgument);
  }
  return 0;
}
int tensor_relation_frontiers() {
  auto root = budget(524288);
  SchemaTemplate schema;
  schema.id = "test.relation.frontier";
  schema.publication = PublishPolicy::StablePrefix;
  ResultTensorSpec tensor;
  tensor.key = "data";
  tensor.descriptor = {ElementType::Float64, {2, 2}};
  schema.tensors.push_back(std::move(tensor));
  auto builder = ResultBuilder::start(root, schema, "frontier").take_value();
  PS_CHECK(builder
               .bind_descriptor_relation(
                   ResultRelation::cartesian(root, 1, {}).take_value())
               .ok());
  auto witness = ResultRelation::cartesian(root, 4, {0, 1, 0, 1}).take_value();
  const double seven = 7;
  for (std::uint64_t point = 0; point < 4; ++point) {
    PS_CHECK(builder
                 .publish_tensor(
                     0, Region({{point / 2, 1}, {point % 2, 1}}),
                     ByteView(reinterpret_cast<const std::uint8_t*>(&seven),
                              sizeof(seven)),
                     witness, {true, true, true, true})
                 .ok());
    auto relation = builder.reference().tensor_relation(0).take_value();
    for (std::uint64_t at = 0; at < 4; ++at) {
      auto status = relation.visit(at, 32, [](ResultSupport support) {
        return support.input == 0 && support.first == 0 && support.count == 1
                   ? Status::success()
                   : Status{ErrorCode::InvalidArgument, "changed witness"};
      });
      PS_CHECK(at <= point ? status.ok() : status.code == ErrorCode::NotFound);
    }
  }
  PS_CHECK(builder.seal().ok());

  schema.tensors[0].descriptor.shape = {80};
  auto sparse = ResultBuilder::start(root, schema, "nested.union").take_value();
  PS_CHECK(sparse
               .bind_descriptor_relation(
                   ResultRelation::cartesian(root, 1, {}).take_value())
               .ok());
  for (std::uint64_t point = 0; point < 80; point += 2) {
    auto relation = ResultRelation::cartesian(root, 80, {0, 1, point, 1},
                                              DependencyGuarantee::Unknown)
                        .take_value();
    PS_CHECK(sparse
                 .publish_tensor(
                     0, Region({{point, 1}}),
                     ByteView(reinterpret_cast<const std::uint8_t*>(&seven),
                              sizeof(seven)),
                     relation, {true, true, true, true})
                 .ok());
  }
  auto relation = sparse.seal().take_value().tensor_relation(0).take_value();
  for (std::uint64_t point = 0; point < 80; ++point) {
    unsigned visits = 0;
    auto status = relation.visit_declared(point, 256, [&](ResultSupport span) {
      ++visits;
      return span.first == point && span.count == 1
                 ? Status::success()
                 : Status{ErrorCode::InvalidArgument, "foreign point support"};
    });
    PS_CHECK(point % 2 ? status.code == ErrorCode::NotFound : status.ok());
    PS_CHECK(visits == (point % 2 ? 0U : 1U));
  }
  auto wrapper =
      ResultBuilder::start(root, schema, "wrapped.union").take_value();
  PS_CHECK(wrapper
               .bind_descriptor_relation(
                   ResultRelation::cartesian(root, 1, {}).take_value())
               .ok());
  PS_CHECK(wrapper
               .publish_tensor(
                   0, Region({{1, 1}}),
                   ByteView(reinterpret_cast<const std::uint8_t*>(&seven),
                            sizeof(seven)),
                   relation, {true, true, true, true})
               .ok());
  auto wrapped = wrapper.seal().take_value().tensor_relation(0).take_value();
  const auto ignore = [](ResultSupport) { return Status::success(); };
  PS_CHECK(wrapped.visit_declared(1, 1, ignore).code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(wrapped.visit_declared(1, 256, ignore).code == ErrorCode::NotFound);
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(descriptors_and_lifetime() == 0);
  PS_CHECK(empty_and_failure() == 0);
  PS_CHECK(paging() == 0);
  PS_CHECK(relations() == 0);
  PS_CHECK(association() == 0);
  PS_CHECK(repeated_cartesian() == 0);
  PS_CHECK(identity_projection() == 0);
  PS_CHECK(disjoint_tensor_relations() == 0);
  PS_CHECK(disjoint_tensor_relations(true) == 0);
  PS_CHECK(tensor_relation_shape_admission() == 0);
  PS_CHECK(tensor_relation_frontiers() == 0);
  return 0;
}
