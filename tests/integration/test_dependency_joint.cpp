#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Counts {
  unsigned starts = 0, polls = 0, members = 0, destroys = 0;
  bool quality = false;
};
struct Joint {
  std::shared_ptr<Counts> counts;
  explicit Joint(std::shared_ptr<Counts> counts) : counts(std::move(counts)) {}
  ~Joint() noexcept { ++counts->destroys; }
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ++counts->polls;
    ResourceVector<ResultJointOutcome> outcomes;
    for (const auto* member : phase.members) {
      ++counts->members;
      const auto key = take(result_atom_key(member->query));
      auto builder = take(ResultBuilder::start(
          member->resources, *member->query.output.result_schema,
          member->query.semantic_key));
      check(builder.bind_descriptor_relation(
          take(ResultRelation::cartesian(member->resources, 1, {}))));
      const double value = 10 + key.output_index;
      const auto count =
          take(member->query.output.result_schema->tensors[0].sample_count());
      check(builder.publish_tensor(
          0, member->query.tensor_outputs->boxes()[0],
          {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)},
          take(ResultRelation::cartesian(member->resources, count, {})),
          {true, true, true, true}));
      std::optional<QualityReport> quality;
      if (counts->quality && key.output_index == 0)
        quality = take(QualityReport::measured_residual("direct-c1", 1, 0,
                                                        phase.allocator));
      outcomes.push_back({key,
                          Result<ResultProgramPoll>(
                              ResultPublication{take(builder.seal()), true}),
                          std::move(quality)});
    }
    std::reverse(outcomes.begin(), outcomes.end());
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
OperationDefinition definition(const std::shared_ptr<Counts>& counts) {
  OperationDefinition op;
  op.key = "test.distinct_result_outputs";
  op.traits.outputs.clear();
  for (unsigned i = 0; i < 64; ++i)
    op.traits.outputs.push_back(multi_result::output(
        "out" + std::to_string(i),
        multi_result::schema(ElementType::Float64, {i + 1})));
  op.traits.joint_contract = 1;
  op.traits.joint_continuation_bytes = sizeof(Joint);
  op.traits.joint_workspace_bytes = 4096;
  op.start_result = [](const auto&, const auto&) {
    return Result<ResultContinuation>(
        Status{ErrorCode::Internal, "unexpected singleton"});
  };
  op.start_result_joint = [counts](const auto&, const auto& allocator) {
    ++counts->starts;
    return ResultJointContinuation::make<Joint>(allocator, counts);
  };
  return op;
}
int distinct_outputs(int cancelled, bool before_start, bool no_work = false,
                     bool quality = false) {
  ResourceLimits limits;
  if (no_work)
    limits.maximum_work = 100000;
  ResourceBudget root(limits);
  ResourceAllocationScope scope(root);
  auto counts = std::make_shared<Counts>();
  counts->quality = quality;
  OperationRegistry registry;
  check(registry.register_operation(definition(counts)));
  check(registry.freeze());
  auto outputs = take(infer_operation_outputs(
      take(registry.find_traits("test.distinct_result_outputs")), {}, {}));
  std::vector<ResultProgramMetadata> metadata;
  metadata.reserve(64);
  std::map<std::string, ParameterValue> parameters;
  ResourceVector<ResultProgramQuery> queries;
  queries.reserve(64);
  std::array<std::string, 64> keys;
  CancellationSource cancellation;
  if (before_start)
    cancellation.cancel();
  for (unsigned i = 0; i < 64; ++i) {
    metadata.push_back({{}, outputs[i]});
    queries.emplace_back(metadata.back(), parameters);
    auto& query = queries.back();
    query.output_index = i;
    keys[i] = "result-output-" + std::to_string(i);
    query.semantic_key = keys[i];
    query.snapshot_identity = "distinct-outputs";
    query.tensor_outputs =
        take(Footprint::from_regions({i + 1}, {Region({{i, 1}})}));
    if (static_cast<int>(i) == cancelled)
      query.cancellation = cancellation.token();
  }
  for (unsigned malformed = 0; malformed < 3; ++malformed) {
    auto changed = queries;
    if (malformed == 0)
      changed.back().output_index = 0;
    else if (malformed == 1)
      changed.back().snapshot_identity = "different";
    else
      changed.push_back(queries.back());
    auto rejected = registry.start_result_joint("test.distinct_result_outputs",
                                                changed, root);
    PS_CHECK(!rejected.ok() && counts->starts == 0);
  }
  auto state = take(registry.start_result_joint("test.distinct_result_outputs",
                                                queries, root));
  if (!before_start && cancelled >= 0)
    cancellation.cancel();
  auto allocator = root.allocator();
  auto work = [&](std::uint64_t n) { return root.consume({n}); };
  ResultObjectInputs inputs;
  ResourceVector<ResultIoReply> io;
  ResourceVector<ResultProgramPhase> phases;
  phases.reserve(64);
  for (const auto& query : queries)
    phases.push_back({query, inputs, io, allocator, root, work, {}});
  ResourceVector<const ResultProgramPhase*> ready;
  for (const auto& phase : phases)
    ready.push_back(&phase);
  if (no_work)
    check(root.consume({limits.maximum_work - root.statistics().issued.work}));
  auto response = state.poll({ready, allocator, work});
  if (no_work) {
    PS_CHECK(!response.ok() &&
             response.status().code == ErrorCode::ResourceExhausted &&
             counts->polls == 0 && counts->members == 0);
  } else if (quality) {
    PS_CHECK(!response.ok() &&
             response.status().detail.origin == FailureOrigin::Protocol &&
             counts->polls == 1);
  } else {
    PS_CHECK(response.ok() && response.value().size() == 64 &&
             counts->polls == 1 &&
             counts->members == (cancelled < 0 ? 64u : 63u));
    std::array<bool, 64> seen{};
    for (const auto& outcome : response.value()) {
      const auto index = outcome.key.output_index;
      PS_CHECK(index < 64 && !seen[index] &&
               outcome.key.coordinate[0] == index);
      seen[index] = true;
      if (static_cast<int>(index) == cancelled) {
        PS_CHECK(!outcome.outcome.ok() &&
                 outcome.outcome.status().code == ErrorCode::Cancelled &&
                 !outcome.quality);
      } else {
        PS_CHECK(outcome.outcome.ok());
        const auto& result =
            std::get<ResultPublication>(outcome.outcome.value()).result;
        PS_CHECK(multi_result::number(result, {index}) == 10 + index);
        if (index) {
          double unavailable = 0;
          PS_CHECK(!result
                        .read_tensor(take(result.descriptor()), 0, {0},
                                     &unavailable, 8)
                        .ok());
        }
      }
    }
    auto repeated = state.poll({ready, allocator, work});
    PS_CHECK(!repeated.ok() && counts->polls == 1);
  }
  state = {};
  PS_CHECK(counts->starts == 1 && counts->destroys == 1);
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(distinct_outputs(-1, false) == 0);
  for (int cancelled : {0, 31, 63}) {
    PS_CHECK(distinct_outputs(cancelled, false) == 0);
    PS_CHECK(distinct_outputs(cancelled, true) == 0);
  }
  PS_CHECK(distinct_outputs(-1, false, true) == 0);
  PS_CHECK(distinct_outputs(-1, false, false, true) == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
