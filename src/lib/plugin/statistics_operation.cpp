#include "photospider/plugin/statistics_operation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"

namespace ps {
namespace {
using Op = StatisticsOperation;
using Rep = StatisticsRepresentation;
using Poll = Result<ResultProgramPoll>;
constexpr std::uint32_t kStatisticsMaximumStages = 1000000;
constexpr std::uint32_t kHistogramBlockBins = 512;
constexpr std::uint64_t kHistogramSourceBytes = 4096;
Status domain(const char* message) {
  return {ErrorCode::OperationFailed,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Domain, FailureScope::Group}};
}
Status overflow() {
  return {ErrorCode::OperationFailed,
          "integer statistics arithmetic overflow",
          FailureReason::ArithmeticOverflow,
          {FailureOrigin::Domain, FailureScope::Group}};
}
Status validate(Op op, StatisticsSpec spec,
                const std::vector<OperationMetadata>& inputs) {
  if (inputs.size() != (op == Op::Parameters ? 1U : 2U))
    return domain("integer statistics input count mismatch");
  for (unsigned i = 0; i < inputs.size(); ++i) {
    const bool object = op == Op::Parameters || (op == Op::Grade && i == 1);
    const auto& input = inputs[i];
    if (object) {
      auto expected = statistics_schema(
          op == Op::Parameters ? Rep::Histogram : Rep::Parameters, spec);
      if (!input.result_schema || !expected.ok() ||
          !input.result_schema->same_schema(expected.value()))
        return domain("integer statistics Result schema mismatch");
    } else if (input.result_schema || !input.facets.empty() ||
               input.descriptor.element_type !=
                   (i == 1 ? ElementType::UInt8 : ElementType::Int64) ||
               input.descriptor.shape !=
                   std::vector<std::uint64_t>{spec.height, spec.width}) {
      return domain("integer statistics requires facet-free Int64/UInt8 HW");
    }
  }
  return Status::success();
}
struct State {
  Op op;
  StatisticsSpec spec;
  unsigned stage = 0;
  std::uint64_t row = 0, batch = 0, rows = 0, cursor = 0, bin_first = 0;
  std::int64_t count = 0, total = 0, previous = -1;
  double gain = 0;
  ResourceVector<std::int64_t> counters;
  ResultRef input;
  ResultDescriptor descriptor;
  ResultBuilder builder;
  ResultRelation grade_relation;
  State(Op operation, StatisticsSpec specification)
      : op(operation), spec(specification) {}
  std::uint64_t size() const { return spec.height * spec.width; }
  Result<ResultRelation> relation(const ResultProgramPhase& phase,
                                  std::uint64_t outputs,
                                  bool metadata = false) {
    std::array<ResultRelation, 3> parts;
    unsigned used = 0;
    const auto add = [&](Result<ResultRelation> made) {
      if (!made.ok())
        return made.status();
      parts[used++] = made.take_value();
      return Status::success();
    };
    if (op != Op::Parameters) {
      auto status =
          add(op == Op::Grade && !metadata
                  ? ResultRelation::identity(phase.resources, outputs, 0, 5)
                  : ResultRelation::cartesian(
                        phase.resources, outputs, {0, 15, 0, size()},
                        DependencyGuarantee::Conservative));
      if (!status.ok())
        return Result<ResultRelation>(status);
    }
    const unsigned port = op == Op::Parameters ? 0 : 1;
    auto status = add(ResultRelation::cartesian(
        phase.resources, outputs,
        {port, op == Op::Histogram ? 15U : 7U, 0,
         op == Op::Histogram    ? size()
         : op == Op::Parameters ? descriptor.rows(0) * 2
                                : 4},
        DependencyGuarantee::Conservative));
    if (!status.ok())
      return Result<ResultRelation>(status);
    if (op != Op::Histogram) {
      status = add(
          ResultRelation::cartesian(phase.resources, outputs, {port, 8, 0, 1},
                                    DependencyGuarantee::Conservative));
      if (!status.ok())
        return Result<ResultRelation>(status);
    }
    auto admission = phase.resources.reserve(ResourceCapacity::host(
        used * sizeof(ResultRelation), used * sizeof(ResultRelation)));
    if (!admission.ok())
      return Result<ResultRelation>(admission.status());
    return ResultRelation::unite(
        phase.resources,
        std::vector<ResultRelation>(parts.begin(), parts.begin() + used));
  }
  Poll finish(const ResultProgramPhase& phase) {
    for (unsigned field = 0; field < builder.reference().schema().fields.size();
         ++field) {
      auto support = op == Op::Grade ? Result<ResultRelation>(grade_relation)
                                     : relation(phase, rows);
      if (!support.ok())
        return Poll(support.status());
      auto status = builder.publish(field, rows, support.take_value(),
                                    {true, true, true, true});
      if (!status.ok())
        return Poll(status);
    }
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
  Poll source(const ResultProgramPhase& phase) {
    // Histogram Value requests have their own fixed, admitted strip bound.
    // A small Result I/O window must not multiply all source scan stages.
    const auto source_bytes =
        op == Op::Histogram
            ? kHistogramSourceBytes
            : std::min<std::uint64_t>(4096, phase.query.page_bytes);
    batch = std::min(
        {size() - row, spec.width - row % spec.width, source_bytes / 8});
    if (!batch)
      return Poll(Status{ErrorCode::ResourceExhausted,
                         "statistics scalar exceeds page window"});
    auto samples = Footprint::from_regions(
        {spec.height, spec.width},
        {Region({{row / spec.width, 1}, {row % spec.width, batch}})});
    if (!samples.ok())
      return Poll(samples.status());
    ResultProgramNeed need;
    need.values.push_back({0, samples.value()});
    if (op == Op::Histogram)
      need.values.push_back({1, samples.value()});
    stage = 3;
    return Poll(std::move(need));
  }
  Poll read_histogram(const ResultProgramPhase& phase) {
    batch = std::min(descriptor.rows(0) - row,
                     std::min<std::uint64_t>(4096, phase.query.page_bytes) / 8);
    if (!batch)
      return Poll(Status{ErrorCode::ResourceExhausted,
                         "histogram row exceeds page window"});
    ResultProgramNeed need;
    for (unsigned f = 0; f < 2; ++f) {
      auto read = input.prepare_read(descriptor, f, row, batch);
      if (!read.ok())
        return Poll(read.status());
      need.io.push_back(read.take_value());
    }
    stage = 3;
    return Poll(std::move(need));
  }
  Poll poll(const ResultProgramPhase& phase) {
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Poll(domain("floating environment unavailable"));
    if (stage == 0) {
      stage = 1;
      if (op != Op::Histogram)
        return Poll(
            ResultProgramNeed{{},
                              {{op == Op::Parameters ? 0U : 1U, 0, true, 0}},
                              {}});
    }
    if (stage == 1) {
      if (op != Op::Histogram) {
        input = phase.results.at(op == Op::Parameters ? 0 : 1);
        auto facts = input.descriptor();
        if (!facts.ok())
          return Poll(facts.status());
        descriptor = facts.take_value();
        if (op == Op::Parameters && descriptor.rows(0) > spec.bins)
          return Poll(domain("too many sparse histogram bins"));
      } else {
        auto fuel = phase.consume_work(5ULL * spec.bins);
        if (!fuel.ok())
          return Poll(fuel);
        counters = ResourceVector<std::int64_t>(
            std::min<std::uint32_t>(spec.bins, kHistogramBlockBins), 0,
            ResourceAllocator<std::int64_t>(phase.resources,
                                            ResourceAllocationKind::Payload));
      }
      auto admission = phase.resources.reserve(ResourceCapacity::host(8, 8));
      if (!admission.ok())
        return Poll(admission.status());
      std::vector<std::uint64_t> association;
      if (input.valid())
        association.push_back(input.object_id());
      const auto limit = op == Op::Histogram ? spec.bins
                         : op == Op::Grade   ? size()
                                             : 1;
      auto made = ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key,
          {limit,
           op == Op::Parameters ? 32 : limit * (op == Op::Histogram ? 16 : 8)},
          std::move(association));
      if (!made.ok())
        return Poll(made.status());
      builder = made.take_value();
      auto support = relation(phase, 1, true);
      if (!support.ok())
        return Poll(support.status());
      auto bound = builder.bind_descriptor_relation(support.take_value());
      if (!bound.ok())
        return Poll(bound);
      stage = 2;
      if (op == Op::Grade) {
        auto all = relation(phase, size());
        if (!all.ok())
          return Poll(all.status());
        grade_relation = all.take_value();
        if (phase.query.page_bytes < 24)
          return Poll(Status{ErrorCode::ResourceExhausted,
                             "statistics record exceeds page window"});
        ResultProgramNeed need;
        for (unsigned f = 0; f < 2; ++f) {
          auto read = input.prepare_read(descriptor, f, 0, 1);
          if (!read.ok())
            return Poll(read.status());
          need.io.push_back(read.take_value());
        }
        stage = 6;
        return Poll(std::move(need));
      }
    }
    if (stage == 6) {
      std::array<std::int64_t, 3> record;
      double mean = 0;
      std::memcpy(record.data(),
                  std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0))
                      ->bytes()
                      .data(),
                  24);
      std::memcpy(&mean,
                  std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(1))
                      ->bytes()
                      .data(),
                  8);
      if (record[0] <= 0 || static_cast<std::uint64_t>(record[0]) > size() ||
          record[1] < 0 || record[2] != 1 ||
          record[1] / record[0] > spec.bins - 1 ||
          (record[1] / record[0] == spec.bins - 1 && record[1] % record[0]))
        return Poll(domain("grade requires nonempty valid statistics"));
      auto exact = statistics_mean(record[1], record[0]);
      if (!exact.ok() || mean != exact.value() || !(mean > 0) ||
          mean > spec.bins - 1)
        return Poll(domain("grade requires consistent positive mean"));
      gain = std::get<double>(phase.query.parameters.at("target")) / mean;
      if (!std::isfinite(gain))
        return Poll(overflow());
      stage = 2;
    }
    if (stage == 5)
      return finish(phase);
    if (stage == 4) {
      if (op == Op::Grade) {
        auto support = op == Op::Grade ? Result<ResultRelation>(grade_relation)
                                       : relation(phase, rows);
        if (!support.ok())
          return Poll(support.status());
        auto status = builder.publish(0, rows, support.take_value(),
                                      {true, true, true, true});
        if (!status.ok())
          return Poll(status);
        stage = 2;
        if (row != size())
          return Poll(ResultPublication{builder.reference(), false});
        return finish(phase);
      }
      stage = 2;
    }
    if (stage == 3) {
      auto fuel = phase.consume_work(batch);
      if (!fuel.ok())
        return Poll(fuel);
      if (op == Op::Parameters) {
        const auto& ids =
            std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0));
        const auto& counts =
            std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(1));
        for (std::uint64_t i = 0; i < batch; ++i) {
          std::int64_t id, n;
          std::memcpy(&id, ids->bytes().data() + i * 8, 8);
          std::memcpy(&n, counts->bytes().data() + i * 8, 8);
          if (id <= previous || id < 0 || id >= spec.bins || n <= 0)
            return Poll(domain("noncanonical sparse histogram"));
          previous = id;
          if (count > INT64_MAX - n || (id && n > (INT64_MAX - total) / id))
            return Poll(overflow());
          count += n;
          total += id * n;
          if (static_cast<std::uint64_t>(count) > size())
            return Poll(domain("histogram exceeds selected raster size"));
        }
      } else {
        auto memory = op == Op::Grade ? phase.allocator.allocate(batch * 8)
                                      : Result<MutableBuffer>(MutableBuffer{});
        if (!memory.ok())
          return Poll(memory.status());
        auto output = memory.take_value();
        for (std::uint64_t i = 0; i < batch; ++i) {
          const std::vector<std::uint64_t> coordinate{row / spec.width,
                                                      row % spec.width + i};
          std::int64_t value = 0;
          auto status = phase.read(0, coordinate, &value, 8);
          if (!status.ok())
            return Poll(status);
          if (op == Op::Histogram) {
            std::uint8_t mask = 0;
            status = phase.read(1, coordinate, &mask, 1);
            if (!status.ok())
              return Poll(status);
            if (mask) {
              if (value < 0 || value >= spec.bins)
                return Poll(domain("selected value outside histogram bins"));
              const auto bin = static_cast<std::uint64_t>(value);
              if (bin >= bin_first && bin - bin_first < counters.size()) {
                if (counters[bin - bin_first] == INT64_MAX)
                  return Poll(overflow());
                ++counters[bin - bin_first];
              }
            }
          } else {
            const double graded = gain * static_cast<double>(value);
            if (!std::isfinite(graded))
              return Poll(overflow());
            std::memcpy(output.data() + i * 8, &graded, 8);
          }
        }
        if (op == Op::Grade) {
          auto write =
              builder.prepare_append(0, batch, std::move(output).freeze());
          if (!write.ok())
            return Poll(write.status());
          row += batch;
          rows = row;
          stage = 4;
          return Poll(ResultProgramNeed{{}, {}, {write.take_value()}});
        }
      }
      row += batch;
      stage = 2;
    }
    if (op == Op::Grade || (op == Op::Histogram && row != size()))
      return source(phase);
    if (op == Op::Parameters) {
      if (row != descriptor.rows(0))
        return read_histogram(phase);
      if (phase.query.page_bytes < 24)
        return Poll(Status{ErrorCode::ResourceExhausted,
                           "statistics record exceeds page window"});
      const std::array<std::int64_t, 3> record{count, total, count ? 1 : 0};
      auto computed =
          count ? statistics_mean(total, count) : Result<double>(0.0);
      if (!computed.ok())
        return Poll(computed.status());
      const double mean = computed.value();
      ResultProgramNeed need;
      for (unsigned f = 0; f < 2; ++f) {
        const auto bytes = f ? 8U : 24U;
        auto memory = phase.allocator.allocate(bytes);
        if (!memory.ok())
          return Poll(memory.status());
        auto buffer = memory.take_value();
        std::memcpy(buffer.data(),
                    f ? static_cast<const void*>(&mean) : record.data(), bytes);
        auto write = builder.prepare_append(f, 1, std::move(buffer).freeze());
        if (!write.ok())
          return Poll(write.status());
        need.io.push_back(write.take_value());
      }
      rows = 1;
      stage = 5;
      return Poll(std::move(need));
    }
    // The complete selected domain has been validated before the first row.
    const auto block_bins =
        std::min<std::uint64_t>(counters.size(), spec.bins - bin_first);
    while (cursor < block_bins && !counters[cursor])
      ++cursor;
    if (cursor == block_bins) {
      bin_first += block_bins;
      if (bin_first == spec.bins)
        return finish(phase);
      std::fill(counters.begin(), counters.end(), 0);
      cursor = 0;
      row = 0;
      return source(phase);
    }
    const auto capacity =
        std::min<std::uint64_t>(4096, phase.query.page_bytes) / 8;
    if (!capacity)
      return Poll(Status{ErrorCode::ResourceExhausted,
                         "histogram row exceeds page window"});
    auto end = cursor;
    std::uint64_t populated = 0;
    while (end < block_bins && populated < capacity) {
      if (counters[end])
        ++populated;
      ++end;
    }
    auto fuel = phase.consume_work(end - cursor);
    if (!fuel.ok())
      return Poll(fuel);
    auto a = phase.allocator.allocate(populated * 8),
         b = phase.allocator.allocate(populated * 8);
    if (!a.ok() || !b.ok())
      return Poll(!a.ok() ? a.status() : b.status());
    auto ids = a.take_value(), counts = b.take_value();
    batch = 0;
    while (cursor < block_bins && batch < capacity) {
      if (counters[cursor]) {
        const auto id = static_cast<std::int64_t>(bin_first + cursor);
        std::memcpy(ids.data() + batch * 8, &id, 8);
        std::memcpy(counts.data() + batch * 8, &counters[cursor], 8);
        ++batch;
      }
      ++cursor;
    }
    auto x = builder.prepare_append(0, batch, std::move(ids).freeze());
    auto y = builder.prepare_append(1, batch, std::move(counts).freeze());
    if (!x.ok() || !y.ok())
      return Poll(!x.ok() ? x.status() : y.status());
    rows += batch;
    stage = 4;
    return Poll(ResultProgramNeed{{}, {}, {x.take_value(), y.take_value()}});
  }
};
}  // namespace
Result<OperationDefinition> make_statistics_operation(
    Op op, const StatisticsSpec& spec) {
  const auto index = static_cast<unsigned>(op);
  if (!index || index > 3)
    return Result<OperationDefinition>(domain("unknown statistics operation"));
  auto schema = statistics_schema(static_cast<Rep>(index), spec);
  if (!schema.ok())
    return Result<OperationDefinition>(schema.status());
  if (op == Op::Histogram) {
    const auto strips =
        1 + (spec.width - 1) / (kHistogramSourceBytes / sizeof(std::int64_t));
    const auto passes = 1 + (spec.bins - 1) / kHistogramBlockBins;
    // Every strip needs a poll in every pass, plus at least one final poll.
    // Divide the available stage count instead of multiplying dimensions.
    if (spec.height > (kStatisticsMaximumStages - 1) / strips / passes)
      return Result<OperationDefinition>(Status{
          ErrorCode::ResourceExhausted,
          "histogram required source stages exceed operation stage limit"});
  }
  OperationDefinition definition;
  definition.key = op == Op::Histogram    ? "statistics.histogram"
                   : op == Op::Parameters ? "statistics.parameters"
                                          : "statistics.grade";
  auto& traits = definition.traits;
  traits.input_count = op == Op::Parameters ? 1 : 2;
  traits.input_schema.resize(traits.input_count);
  traits.workspace_bytes = 16384;
  auto& out = traits.outputs[0];
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 2;
  out.continuation_bytes = sizeof(State);
  out.maximum_dependency_stages = kStatisticsMaximumStages;
  out.result_schema = schema.take_value();
  out.output_schema.kind = OperationPortKind::Result;
  out.output_schema.result_schema_id = std::string(out.result_schema->id);
  out.output_schema.result_schema_version = 1;
  for (unsigned i = 0; i < traits.input_count; ++i) {
    if (op == Op::Parameters || (op == Op::Grade && i == 1)) {
      auto input = statistics_schema(
          op == Op::Parameters ? Rep::Histogram : Rep::Parameters, spec);
      traits.input_schema[i].kind = OperationPortKind::Result;
      traits.input_schema[i].result_schema_id = std::string(input.value().id);
      traits.input_schema[i].result_schema_version = 1;
    }
  }
  if (op == Op::Grade)
    traits.parameter_schema.push_back(
        {"target", OperationParameterType::Float64, true, true, 0,
         std::numeric_limits<double>::max()});
  definition.validate_dependency = [op, spec](const auto& inputs, const auto&) {
    return validate(op, spec, inputs);
  };
  definition.start_result = [op, spec](const ResultProgramQuery& query,
                                       const BufferAllocator& allocator) {
    auto status = validate(op, spec, query.inputs);
    if (!status.ok())
      return Result<ResultContinuation>(status);
    return ResultContinuation::make<State>(allocator, op, spec);
  };
  return Result<OperationDefinition>(std::move(definition));
}
}  // namespace ps
