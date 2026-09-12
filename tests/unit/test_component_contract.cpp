#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
ResultRef fixture(const ResourceBudget& root, SchemaTemplate schema,
                  const std::vector<std::vector<std::int64_t>>& fields,
                  std::vector<std::uint64_t> association = {}) {
  auto builder = take(ResultBuilder::start(root, schema, "fixture", {},
                                           std::move(association)));
  auto status = builder.bind_descriptor_relation(take(ResultRelation::cartesian(
      root, 1, {0, 15, 0, 1}, DependencyGuarantee::Conservative)));
  if (!status.ok())
    throw std::runtime_error(status.message);
  for (unsigned f = 0; f < fields.size(); ++f) {
    const auto row_bytes = take(schema.row_bytes(f));
    const auto rows = fields[f].size() * 8 / row_bytes;
    status =
        builder.append(f, rows,
                       {reinterpret_cast<const std::uint8_t*>(fields[f].data()),
                        fields[f].size() * 8});
    if (!status.ok())
      throw std::runtime_error(status.message);
    status = builder.publish(
        f, rows,
        take(ResultRelation::cartesian(root, rows, {0, 15, 0, 1},
                                       DependencyGuarantee::Conservative)),
        {true, true, true, true});
    if (!status.ok())
      throw std::runtime_error(status.message);
  }
  return take(builder.seal());
}
// Direct public registry/callback driving injects malformed external indices.
// The separate installed workflow verifies actual scheduling, sources and BFS.
Result<ResultRef> filter(const ResourceBudget& root, const ComponentsSpec& spec,
                         const ResultRef& labels, const ResultRef& index,
                         std::int64_t threshold, unsigned* reads) {
  OperationRegistry registry;
  auto status = registry.register_operation(
      take(make_component_operation(ComponentOperation::Filter, spec)));
  if (!status.ok())
    return Result<ResultRef>(status);
  if (!registry.freeze().ok())
    throw std::runtime_error("freeze");
  ResultProgramMetadata metadata;
  metadata.inputs.resize(2);
  metadata.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(labels.schema());
  metadata.inputs[1].result_schema =
      std::make_shared<const SchemaTemplate>(index.schema());
  metadata.output.result_schema = std::make_shared<const SchemaTemplate>(
      take(component_filter_schema(spec)));
  const std::map<std::string, ParameterValue> parameters{
      {"minimum_area", threshold}};
  ResultProgramQuery query(metadata, parameters);
  query.semantic_key = "filter-callback";
  query.page_bytes = 48;
  auto allocator = root.allocator();
  auto started = registry.start_result("components4.filter", query, allocator);
  if (!started.ok())
    return Result<ResultRef>(started.status());
  auto continuation = started.take_value();
  ResultValueInputs values;
  ResultObjectInputs objects{{0, labels}, {1, index}};
  ResourceVector<ResultIoReply> io;
  auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
  for (unsigned step = 0; step < 128; ++step) {
    ResultProgramPhase phase{
        query,
        values,
        objects,
        io,
        allocator,
        root,
        [&](std::uint64_t work) { return root.consume({work}); },
        failure};
    auto polled = continuation.poll(phase);
    if (!polled.ok())
      return Result<ResultRef>(polled.status());
    auto action = polled.take_value();
    if (auto* result = std::get_if<ResultPublication>(&action))
      return Result<ResultRef>(result->result);
    auto* need = std::get_if<ResultProgramNeed>(&action);
    if (!need)
      throw std::runtime_error("unexpected filter publication");
    io.clear();
    for (const auto& request : need->io) {
      if (auto* read = std::get_if<ResultReadPlan>(&request)) {
        ++*reads;
        io.push_back(take(read->load(48)));
      } else {
        auto written = std::get<ResultWritePlan>(request).apply();
        if (!written.ok())
          return Result<ResultRef>(written);
        io.push_back(std::monostate{});
      }
    }
  }
  return Result<ResultRef>(
      Status{ErrorCode::ResourceExhausted, "test stage cap"});
}
}  // namespace
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(
      !make_component_operation(ComponentOperation::Labels, {0, 1, 1}).ok());
  PS_CHECK(!component_area_schema({UINT64_MAX, 2, 1}).ok());
  PS_CHECK(
      !component_filter_schema({1, 1, 1, ComponentIdScheme::CompactMinOrder})
           .ok());
  const ComponentsSpec spec{1, 5, 2};
  ResourceBudget root;
  auto over_count =
      fixture(root, take(components_schema({1, 1, 0})), {{1}, {1, 1, 0}});
  auto semantic_limit = validate_representation(over_count, root, 24);
  PS_CHECK(semantic_limit.code == ErrorCode::OperationFailed &&
           semantic_limit.reason == FailureReason::InvalidDomain &&
           semantic_limit.detail.origin == FailureOrigin::Domain &&
           semantic_limit.detail.scope == FailureScope::Association &&
           semantic_limit.detail.association == over_count.object_id());
  const auto label_schema = take(components_schema(spec)),
             index_schema = take(component_area_schema(spec));
  auto labels =
      fixture(root, label_schema, {{1, 1, 0, 4, 4}, {1, 2, 0, 4, 2, 3}});
  for (const auto& data :
       std::vector<std::vector<std::int64_t>>{{1, 2},
                                              {1, 2, 5, 2},
                                              {1, 0, 4, 2},
                                              {4, 2, 1, 2},
                                              {1, 2, 4, 3}}) {
    auto index = fixture(root, index_schema, {data}, {labels.object_id()});
    unsigned reads = 0;
    auto rejected = filter(root, spec, labels, index, 2, &reads);
    PS_CHECK(!rejected.ok() &&
             rejected.status().reason == FailureReason::InvalidAssociation);
    PS_CHECK(rejected.status().detail.scope == FailureScope::Association &&
             rejected.status().detail.association == index.object_id());
    if (data.size() == 2)
      PS_CHECK(reads == 0);
  }
  auto index =
      fixture(root, index_schema, {{1, 2, 4, 2}}, {labels.object_id()});
  auto identical_other =
      fixture(root, label_schema, {{1, 1, 0, 4, 4}, {1, 2, 0, 4, 2, 3}});
  unsigned reads = 0;
  auto foreign = filter(root, spec, identical_other, index, 2, &reads);
  PS_CHECK(!foreign.ok() &&
           foreign.status().reason == FailureReason::InvalidAssociation &&
           reads == 0);
  auto output = take(filter(root, spec, labels, index, 2, &reads));
  auto page = take(
      take(output.prepare_read(take(output.descriptor()), 0, 0, 5)).load(5));
  const std::uint8_t expected[]{1, 1, 0, 1, 1};
  PS_CHECK(std::memcmp(page->bytes().data(), expected, 5) == 0);
  reads = 0;
  auto zero = filter(root, spec, labels, index, 0, &reads);
  PS_CHECK(!zero.ok() && zero.status().reason == FailureReason::InvalidDomain &&
           reads == 0);
  output = take(filter(root, spec, labels, index, INT64_MAX, &reads));
  page = take(
      take(output.prepare_read(take(output.descriptor()), 0, 0, 5)).load(5));
  const std::uint8_t empty[]{0, 0, 0, 0, 0};
  PS_CHECK(std::memcmp(page->bytes().data(), empty, 5) == 0);
  const ComponentsSpec empty_spec{1, 5, 0};
  auto background =
      fixture(root, take(components_schema(empty_spec)), {{0, 0, 0, 0, 0}, {}});
  auto empty_index = fixture(root, take(component_area_schema(empty_spec)),
                             {{}}, {background.object_id()});
  output = take(filter(root, empty_spec, background, empty_index, 1, &reads));
  page = take(
      take(output.prepare_read(take(output.descriptor()), 0, 0, 5)).load(5));
  PS_CHECK(std::memcmp(page->bytes().data(), empty, 5) == 0);
  return 0;
}
