#include <memory>
#include <string>
#include <utility>
#pragma once

#include "support/result_numeric_math_fixture.hpp"

namespace ps::test_numeric {
struct ObservedMathPreparation {
  std::shared_ptr<const PreparedOperation> original;
};
struct ObservedMathPhase {
  ResultContinuation inner;
  std::shared_ptr<CancellationSource> stop;
  std::shared_ptr<bool> triggered;
  bool refinement, entered = false;
  uint64_t cancel_words;
  ObservedMathPhase(ResultContinuation program,
                    std::shared_ptr<CancellationSource> cancellation,
                    std::shared_ptr<bool> observed, bool cancel_refinement,
                    uint64_t words)
      : inner(std::move(program)),
        stop(std::move(cancellation)),
        triggered(std::move(observed)),
        refinement(cancel_refinement),
        cancel_words(words) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!stop)
      return inner.poll(phase);
    auto bounded = phase;
    bounded.consume_work = [&](uint64_t amount) {
      if (refinement && amount == 16)
        entered = true;
      if (refinement ? entered && amount == 1 : amount == cancel_words) {
        *triggered = true;
        stop->cancel();
      }
      return phase.consume_work(amount);
    };
    return inner.poll(bounded);
  }
};
inline OperationDefinition observed_numeric(
    const std::shared_ptr<OperationRegistry>& original, const std::string& key,
    const std::shared_ptr<unsigned>& preparations,
    std::shared_ptr<CancellationSource> stop = {},
    std::shared_ptr<bool> triggered = {}, bool refinement = false,
    uint64_t cancel_words = 640) {
  OperationDefinition definition;
  definition.key = key;
  definition.traits = take(original->find_traits(key));
  definition.traits.outputs[0].continuation_bytes += sizeof(ObservedMathPhase);
  definition.prepare_static = [original, key, preparations](
                                  const auto& inputs, const auto& parameters) {
    ++*preparations;
    auto prepared = original->prepare_operation(key, inputs, parameters);
    if (!prepared.ok())
      return Result<OperationPreparation>(prepared.status());
    OperationPreparation result;
    auto inner = prepared.take_value();
    result.state = std::make_shared<ObservedMathPreparation>(
        ObservedMathPreparation{inner});
    result.outputs.resize(1);
    result.outputs[0].metadata.result_schema = std::make_shared<SchemaTemplate>(
        *inner->traits().outputs[0].result_schema);
    return Result<OperationPreparation>(std::move(result));
  };
  definition.start_result = [original, key, stop, triggered, refinement,
                             cancel_words](const auto& query,
                                           const auto& allocator) {
    auto forwarded = query;
    const auto* observation =
        static_cast<const ObservedMathPreparation*>(query.prepared->state());
    forwarded.prepared = observation->original;
    auto inner = original->start_result(key, forwarded, allocator);
    if (!inner.ok())
      return inner;
    return ResultContinuation::make<ObservedMathPhase>(
        allocator, inner.take_value(), stop, triggered, refinement,
        cancel_words);
  };
  return definition;
}

struct CancelCurvePhase {
  ResultContinuation inner;
  std::shared_ptr<CancellationSource> stop;
  std::shared_ptr<bool> triggered;
  CancelCurvePhase(ResultContinuation program,
                   std::shared_ptr<CancellationSource> cancellation,
                   std::shared_ptr<bool> observed)
      : inner(std::move(program)),
        stop(std::move(cancellation)),
        triggered(std::move(observed)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto bounded = phase;
    bounded.consume_work = [&](uint64_t amount) {
      // ExactCurve allocates a fixed 352-limb slot after input validation.
      if (amount == 352) {
        *triggered = true;
        stop->cancel();
      }
      return phase.consume_work(amount);
    };
    return inner.poll(bounded);
  }
};
struct BoundedMetadataPhase {
  ResultContinuation inner;
  uint64_t trigger, available, occurrence;
  explicit BoundedMetadataPhase(ResultContinuation value, uint64_t trigger = 80,
                                uint64_t available = 1024,
                                uint64_t occurrence = 1)
      : inner(std::move(value)),
        trigger(trigger),
        available(available),
        occurrence(occurrence) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto bounded = phase;
    std::optional<ResourceLease> reservation;
    uint64_t hits = 0;
    bounded.consume_work = [&](uint64_t amount) {
      auto status = phase.consume_work(amount);
      if (status.ok() && amount == trigger && ++hits == occurrence &&
          !reservation) {
        const auto remaining =
            UINT64_C(1000000) -
            phase.resources.statistics().live[ResourceKind::Metadata];
        auto admitted = phase.resources.reserve(ResourceCapacity::host(
            remaining - available, remaining - available));
        if (!admitted.ok())
          return admitted.status();
        reservation = admitted.take_value();
      }
      return status;
    };
    return inner.poll(bounded);
  }
};
}  // namespace ps::test_numeric
