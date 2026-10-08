#pragma once

#include <cstring>
#include <utility>
#include <vector>

#include "01-numeric/numeric_tensor_program.hpp"

namespace ps::plugin_internal::numeric_ops {
inline Result<ResultRef> ordered_state(const ResultProgramPhase& phase,
                                       const std::vector<double>& numbers) {
  auto schema = numeric_tensor_schema(ElementType::Float64, {numbers.size()});
  auto made =
      ResultBuilder::start(phase.resources, schema, "numeric.ordered.state");
  if (!made.ok())
    return Result<ResultRef>(made.status());
  auto builder = made.take_value();
  auto relation = ResultRelation::cartesian(phase.resources, 1, {});
  if (!relation.ok())
    return Result<ResultRef>(relation.status());
  auto status = builder.bind_descriptor_relation(relation.take_value());
  if (!status.ok())
    return Result<ResultRef>(status);
  relation = ResultRelation::cartesian(phase.resources, numbers.size(), {});
  if (!relation.ok())
    return Result<ResultRef>(relation.status());
  status = builder.publish_tensor(
      0, Region::whole({numbers.size()}),
      {reinterpret_cast<const std::uint8_t*>(numbers.data()),
       numbers.size() * sizeof(double)},
      relation.take_value(), {true, true, true, true},
      phase.query.cancellation);
  return status.ok() ? builder.seal() : Result<ResultRef>(status);
}
inline Result<double> ordered_read_state(const ResultRef& state,
                                         std::uint64_t index) {
  auto facts = state.descriptor();
  if (!facts.ok())
    return Result<double>(facts.status());
  double number = 0;
  auto status = state.read_tensor(facts.value(), 0, {index}, &number, 8);
  return status.ok() ? Result<double>(number) : Result<double>(status);
}
inline FootprintLimits ordered_limits(const ResultProgramPhase& phase) {
  FootprintLimits limits;
  limits.cancellation = phase.query.cancellation;
  limits.consume_work = phase.consume_work;
  return limits;
}
inline Result<ResultProgramPoll> ordered_empty_output(
    const ResultProgramPhase& phase) {
  auto made =
      ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                           phase.query.semantic_key);
  if (!made.ok())
    return Result<ResultProgramPoll>(made.status());
  auto builder = made.take_value();
  auto relation = ResultRelation::cartesian(phase.resources, 1, {});
  if (!relation.ok())
    return Result<ResultProgramPoll>(relation.status());
  auto status = builder.bind_descriptor_relation(relation.take_value());
  if (!status.ok())
    return Result<ResultProgramPoll>(status);
  auto sealed = builder.seal();
  return sealed.ok() ? Result<ResultProgramPoll>(
                           ResultPublication{sealed.take_value(), true})
                     : Result<ResultProgramPoll>(sealed.status());
}
inline Result<ResultProgramPoll> ordered_output(const ResultProgramPhase& phase,
                                                const Region& region,
                                                double number,
                                                ResultRelation relation) {
  auto made =
      ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                           phase.query.semantic_key);
  if (!made.ok())
    return Result<ResultProgramPoll>(made.status());
  auto builder = made.take_value();
  auto descriptors = ResultRelation::cartesian(
      phase.resources, 1, {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0});
  if (!descriptors.ok())
    return Result<ResultProgramPoll>(descriptors.status());
  auto status = builder.bind_descriptor_relation(descriptors.take_value());
  if (!status.ok())
    return Result<ResultProgramPoll>(status);
  status = builder.publish_tensor(
      0, region, {reinterpret_cast<const std::uint8_t*>(&number), 8},
      std::move(relation), {true, true, true, true}, phase.query.cancellation);
  if (!status.ok())
    return Result<ResultProgramPoll>(status);
  auto output = builder.seal();
  if (!output.ok())
    return Result<ResultProgramPoll>(output.status());
  return Result<ResultProgramPoll>(
      ResultPublication{output.take_value(), true});
}
}  // namespace ps::plugin_internal::numeric_ops
