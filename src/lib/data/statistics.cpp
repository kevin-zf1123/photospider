#include "photospider/data/statistics.hpp"

#include <cmath>
#include <limits>
#include <utility>

#include "data/input_validation.hpp"

namespace ps {
namespace {
Status invalid() {
  return Status{ErrorCode::InvalidArgument,
                "invalid integer statistics schema/domain",
                FailureReason::InvalidDomain,
                {FailureOrigin::Schema, FailureScope::Group}};
}
const char* name(StatisticsRepresentation representation) {
  switch (representation) {
    case StatisticsRepresentation::Histogram:
      return "photospider.integer_histogram";
    case StatisticsRepresentation::Parameters:
      return "photospider.integer_statistics";
    case StatisticsRepresentation::Graded:
      return "photospider.graded_scalar";
  }
  return nullptr;
}
std::uint64_t word(const ResultFacet& facet, unsigned index) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= static_cast<std::uint64_t>(facet.payload[index * 8 + i])
             << (i * 8);
  return value;
}
}  // namespace
Result<SchemaTemplate> statistics_schema(
    StatisticsRepresentation representation, const StatisticsSpec& spec) {
  const auto* id = name(representation);
  constexpr auto maximum = (static_cast<std::uint64_t>(INT64_MAX) - 4095) / 8;
  if (!id || !spec.height || !spec.width ||
      spec.height > maximum / spec.width || !spec.bins || spec.bins > 65536)
    return Result<SchemaTemplate>(invalid());
  SchemaTemplate schema;
  schema.id = id;
  ResultFacet facet;
  facet.key = "integer_statistics_v1";
  for (std::uint64_t value : {std::uint64_t{1}, spec.height, spec.width,
                              static_cast<std::uint64_t>(spec.bins)})
    for (unsigned i = 0; i < 8; ++i)
      facet.payload.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
  schema.metadata.push_back(std::move(facet));
  if (representation == StatisticsRepresentation::Histogram) {
    schema.fields = {
        {"bin", ElementType::Int64, {ResultExtentKind::RuntimeCount}, {}},
        {"count",
         ElementType::Int64,
         {ResultExtentKind::FieldRows, 1, 0, 0, 0},
         {}}};
  } else if (representation == StatisticsRepresentation::Parameters) {
    schema.fields = {{"count_total_valid", ElementType::Int64, {}, {3}},
                     {"mean", ElementType::Float64, {}, {}}};
  } else {
    schema.publication = PublishPolicy::StablePrefix;
    schema.domain = {{ResultExtentKind::Fixed, spec.height},
                     {ResultExtentKind::Fixed, spec.width}};
    schema.fields = {{"pixels",
                      ElementType::Float64,
                      {ResultExtentKind::Fixed, spec.height * spec.width},
                      {}}};
  }
  auto checked = schema.validate(true);
  return checked.ok() ? Result<SchemaTemplate>(std::move(schema))
                      : Result<SchemaTemplate>(checked);
}
Result<StatisticsSpec> statistics_spec(const SchemaTemplate& schema) {
  if (schema.version != 1 || schema.metadata.size() != 1 ||
      schema.metadata[0].key != "integer_statistics_v1" ||
      schema.metadata[0].version != 1 ||
      schema.metadata[0].payload.size() != 32 ||
      word(schema.metadata[0], 0) != 1 || word(schema.metadata[0], 3) > 65536)
    return Result<StatisticsSpec>(invalid());
  StatisticsSpec spec{word(schema.metadata[0], 1), word(schema.metadata[0], 2),
                      static_cast<std::uint32_t>(word(schema.metadata[0], 3))};
  for (auto representation : {StatisticsRepresentation::Histogram,
                              StatisticsRepresentation::Parameters,
                              StatisticsRepresentation::Graded}) {
    if (schema.id != name(representation))
      continue;
    auto expected = statistics_schema(representation, spec);
    if (expected.ok() && schema.same_schema(expected.value()))
      return Result<StatisticsSpec>(spec);
  }
  return Result<StatisticsSpec>(invalid());
}
Result<double> statistics_mean(std::int64_t total, std::int64_t count) {
  if (total < 0 || count <= 0 || total / count >= 65536)
    return Result<double>(invalid());
  if (!total)
    return Result<double>(0.0);
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<double>(
        Status{ErrorCode::OperationFailed, "floating environment unavailable"});
  const auto denominator = static_cast<std::uint64_t>(count);
  auto remainder = static_cast<std::uint64_t>(total) % denominator;
  auto significant = static_cast<std::uint64_t>(total) / denominator;
  const auto bit = [&]() {
    const auto complement = denominator - remainder;
    if (remainder >= complement) {
      remainder -= complement;
      return std::uint64_t{1};
    }
    remainder *= 2;
    return std::uint64_t{0};
  };
  int exponent = -1;
  unsigned bits = 0;
  if (significant) {
    auto copy = significant;
    while (copy) {
      ++bits;
      copy >>= 1;
    }
    exponent = static_cast<int>(bits) - 1;
  } else {
    do {
      significant = bit();
      if (!significant)
        --exponent;
    } while (!significant);
    bits = 1;
  }
  while (bits++ < 53)
    significant = (significant << 1) | bit();
  const auto complement = denominator - remainder;
  if (remainder > complement || (remainder == complement && (significant & 1)))
    ++significant;
  return Result<double>(
      std::ldexp(static_cast<double>(significant), exponent - 52));
}
}  // namespace ps
