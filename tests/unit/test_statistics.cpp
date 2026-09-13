#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>

#include "photospider/data/statistics.hpp"
#include "photospider/plugin/statistics_operation.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  struct Case {
    std::int64_t total, count;
    double mean;
  };
  // Independent Python fractions.Fraction -> float golden binary64 values.
  const Case cases[] = {
      {0, 1, 0},
      {7, 2, 3.5},
      {9007199254740993LL, 18014398509481987LL, 0x1.0000000000000p-1},
      {9007199254740993LL, 9007199254740994LL, 0x1.fffffffffffffp-1},
      {9007199254740991LL, 9007199254740993LL, 0x1.ffffffffffffep-1},
      {1260803624606017114LL, 1358146980426967015LL, 0x1.db4d974616047p-1},
      {36141381719568500LL, 2580557587944LL, 0x1.b5aa15df87b3ap+13},
      {1205427575922608003LL, 33945187789984LL, 0x1.156e002f8ac0ap+15},
      {9007199254740993LL, 18014398509481984LL, 0x1.0000000000000p-1},
      {9007199254740995LL, 18014398509481984LL, 0x1.0000000000002p-1},
      {2305843009213693950LL, 2305843009213693951LL, 1},
      {1, INT64_MAX, 0x1.0000000000000p-63}};
  const auto old = std::fegetround();
  PS_CHECK(std::fesetround(FE_UPWARD) == 0);
  for (const auto& value : cases) {
    auto actual = statistics_mean(value.total, value.count);
    PS_CHECK(actual.ok() && actual.value() == value.mean);
    PS_CHECK(std::fegetround() == FE_UPWARD);
  }
  PS_CHECK(std::fesetround(old) == 0);
  PS_CHECK(!statistics_mean(-1, 1).ok());
  PS_CHECK(!statistics_mean(0, 0).ok());
  PS_CHECK(!statistics_mean(65536, 1).ok());
  for (auto rep : {StatisticsRepresentation::Histogram,
                   StatisticsRepresentation::Parameters,
                   StatisticsRepresentation::Graded}) {
    auto schema = statistics_schema(rep, {3, 7, 8});
    PS_CHECK(schema.ok());
    auto spec = statistics_spec(schema.value());
    PS_CHECK(spec.ok() && spec.value().height == 3 && spec.value().width == 7 &&
             spec.value().bins == 8);
    auto malformed = schema.take_value();
    malformed.metadata[0].payload.pop_back();
    PS_CHECK(!statistics_spec(malformed).ok());
  }
  PS_CHECK(
      !statistics_schema(StatisticsRepresentation::Histogram, {0, 1, 8}).ok());
  PS_CHECK(
      !statistics_schema(StatisticsRepresentation::Histogram, {1, 1, 0}).ok());
  PS_CHECK(
      !statistics_schema(StatisticsRepresentation::Graded, {UINT64_MAX, 2, 8})
           .ok());
  // Admission boundaries use required source polls, independently of data.
  for (const auto spec :
       {StatisticsSpec{2048, 2048, 65536},
        {1000000, 512, 512},
        {500000, 513, 512},
        {500000, 512, 513},
        {250000, 513, 513},
        {static_cast<std::uint64_t>((INT64_MAX - 4095) / 8), 1, 65536}}) {
    PS_CHECK(statistics_schema(StatisticsRepresentation::Histogram, spec).ok());
    const auto rejected =
        make_statistics_operation(StatisticsOperation::Histogram, spec);
    PS_CHECK(!rejected.ok());
    PS_CHECK(rejected.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(rejected.status().message ==
             "histogram required source stages exceed operation stage limit");
    PS_CHECK(
        make_statistics_operation(StatisticsOperation::Parameters, spec).ok());
    PS_CHECK(make_statistics_operation(StatisticsOperation::Grade, spec).ok());
  }
  for (const auto spec : {StatisticsSpec{999999, 512, 512},
                          {499999, 513, 512},
                          {499999, 512, 513},
                          {249999, 513, 513},
                          {200, 200, 65536}}) {
    const auto accepted =
        make_statistics_operation(StatisticsOperation::Histogram, spec);
    PS_CHECK(accepted.ok());
    PS_CHECK(accepted.value().traits.outputs[0].maximum_dependency_stages ==
             1000000);
  }
  for (const auto spec : {StatisticsSpec{0, 1, 65536},
                          {1, 0, 8},
                          {1, 1, 0},
                          {UINT64_MAX, 2, 65536}}) {
    const auto schema_error =
        statistics_schema(StatisticsRepresentation::Histogram, spec);
    const auto factory_error =
        make_statistics_operation(StatisticsOperation::Histogram, spec);
    PS_CHECK(!schema_error.ok() && !factory_error.ok());
    PS_CHECK(factory_error.status().code == schema_error.status().code &&
             factory_error.status().message == schema_error.status().message);
  }
  return 0;
}
