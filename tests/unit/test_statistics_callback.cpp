#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/plugin/statistics_operation.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
ResultRef fixture(const ResourceBudget& root, const SchemaTemplate& schema,
                  const std::vector<std::vector<std::int64_t>>& fields) {
  auto builder = take(ResultBuilder::start(root, schema, "fixture"));
  auto descriptor = take(ResultRelation::cartesian(
      root, 1, {0, 15, 0, 1}, DependencyGuarantee::Conservative));
  if (!builder.bind_descriptor_relation(std::move(descriptor)).ok())
    throw std::runtime_error("fixture descriptor");
  for (unsigned f = 0; f < fields.size(); ++f) {
    const auto rows = fields[f].size();
    auto status = builder.append(
        f, rows,
        {reinterpret_cast<const std::uint8_t*>(fields[f].data()), rows * 8});
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
// Direct public callbacks cover malformed external Result contents; the
// installed workflow separately covers the real scheduler and source path.
Result<ResultRef> parameters(const ResourceBudget& root, StatisticsSpec spec,
                             const ResultRef& histogram) {
  auto definition =
      take(make_statistics_operation(StatisticsOperation::Parameters, spec));
  ResultProgramMetadata metadata;
  metadata.inputs.resize(1);
  metadata.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(histogram.schema());
  metadata.output.result_schema = std::make_shared<const SchemaTemplate>(
      *definition.traits.outputs[0].result_schema);
  const std::map<std::string, ParameterValue> static_parameters;
  ResultProgramQuery query(metadata, static_parameters);
  query.semantic_key = "parameter-callback";
  query.page_bytes = 24;
  auto allocator = root.allocator();
  auto continuation = take(definition.start_result(query, allocator));
  ResultValueInputs values;
  ResultObjectInputs objects{{0, histogram}};
  ResourceVector<ResultIoReply> io;
  auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
  for (unsigned step = 0; step < 32; ++step) {
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
    if (auto* publication = std::get_if<ResultPublication>(&action))
      return Result<ResultRef>(publication->result);
    auto* need = std::get_if<ResultProgramNeed>(&action);
    if (!need)
      throw std::runtime_error("unexpected fixture callback publication");
    io.clear();
    for (const auto& request : need->io) {
      if (auto* read = std::get_if<ResultReadPlan>(&request)) {
        io.push_back(take(read->load(24)));
      } else {
        auto status = std::get<ResultWritePlan>(request).apply();
        if (!status.ok())
          return Result<ResultRef>(status);
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
  ResourceBudget root;
  const StatisticsSpec spec{1, 100, 8};
  const auto schema =
      take(statistics_schema(StatisticsRepresentation::Histogram, spec));
  const std::vector<std::vector<std::vector<std::int64_t>>> malformed{
      {{1, 1}, {1, 2}}, {{2, 1}, {1, 2}},  {{-1, 1}, {1, 2}}, {{0, 8}, {1, 2}},
      {{0, 1}, {0, 2}}, {{0, 1}, {1, -2}}, {{0, 1}, {99, 2}}};
  for (const auto& fields : malformed) {
    auto input = fixture(root, schema, fields);
    auto rejected = parameters(root, spec, input);
    PS_CHECK(!rejected.ok() &&
             rejected.status().reason == FailureReason::InvalidDomain);
  }
  const StatisticsSpec large{1, 1000000000000000000ULL, 65536};
  const auto large_schema =
      take(statistics_schema(StatisticsRepresentation::Histogram, large));
  auto huge = fixture(root, large_schema, {{65535}, {1000000000000000000LL}});
  PS_CHECK(parameters(root, large, huge).status().reason ==
           FailureReason::ArithmeticOverflow);
  // Valid sparse counts that exercise the double-conversion counterexample.
  const std::int64_t denominator = 18014398509481987LL,
                     numerator = 9007199254740993LL;
  const StatisticsSpec golden{1, static_cast<std::uint64_t>(denominator), 8};
  auto golden_schema =
      take(statistics_schema(StatisticsRepresentation::Histogram, golden));
  auto input = fixture(root, golden_schema,
                       {{0, 1}, {denominator - numerator, numerator}});
  auto computed = take(parameters(root, golden, input));
  auto page =
      take(take(computed.prepare_read(take(computed.descriptor()), 1, 0, 1))
               .load(8));
  double actual;
  std::memcpy(&actual, page->bytes().data(), 8);
  PS_CHECK(actual == 0.5);
  return 0;
}
