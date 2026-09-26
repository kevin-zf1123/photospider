#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/execution/resource_allocator.hpp"
#include "plugin/planar_exact.hpp"
#include "support/fmt_handoff.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using ps::handoff_testing::Probe;
using ps::handoff_testing::require;
using ps::handoff_testing::take;

enum class Mapping { Identity, PortOne, SwapChannels };
struct Checks {
  std::atomic<unsigned> validations{0}, writes{0}, scoped{0};
  bool sticky_failure = false;
  bool protocol_failure = false;
  bool exhaust_work = false;
};
OperationDefinition operation(const std::shared_ptr<Probe>& probe,
                              const std::shared_ptr<Checks>& checks,
                              Mapping mapping, DataMovementViewPolicy policy) {
  auto alter = [mapping, policy](OperationOutputSpecialization& output) {
    output.preserve_output_views = true;
    output.data_movement_view_policy = policy;
    auto& pieces = *output.static_dependency_pieces;
    if (mapping == Mapping::PortOne) {
      pieces[0].inputs[0].port = 1;
    } else if (mapping == Mapping::SwapChannels) {
      auto map = pieces[0].inputs[0];
      pieces.clear();
      for (std::uint64_t c : {1U, 0U}) {
        auto dims =
            Region::whole(output.metadata.descriptor.shape).dimensions();
        dims[2] = {c, 1};
        map.axes[2].observation_axis = -1;
        map.axes[2].fixed = {1 - c, 1};
        pieces.push_back(
            {take(Footprint::from_regions(output.metadata.descriptor.shape,
                                          {Region(dims)})),
             {map}});
      }
    }
  };
  auto result =
      ps::handoff_testing::probe_operation(probe, false, policy, alter);
  result.traits.input_count = 2;
  result.traits.input_schema.resize(2);
  result.traits.planar_exact_dependencies = true;
  result.planar_callback = [checks,
                            probe](const PlanarOperationInvocation& call) {
    ++probe->callbacks;
    require(call.exact_inputs && call.prepared,
            "missing exact callback inputs");
    require(call.consume_work && call.report_numeric,
            "missing exact callback work or numeric service");
    require(call.consume_work(0).ok(), "exact checkpoint failed");
    NumericDiagnostics report;
    report.profile = CpuNumericProfile::Strict;
    report.implementation[0] = 't';
    report.evaluated_values = 7;
    take(call.report_numeric(report));
    if (checks->exhaust_work) {
      // Deliberately ignore a failed admission: the host must retain it.
      static_cast<void>(call.consume_work(UINT64_MAX));
    }
    if (const auto* budget = resource_internal::metadata_budget()) {
      ++checks->scoped;
      if (checks->sticky_failure) {
        resource_internal::metadata_failure(*budget,
                                            ErrorCode::ResourceExhausted);
      }
    }
    if (probe->on_callback) {
      probe->on_callback();
    }
    if (checks->protocol_failure) {
      return Status{ErrorCode::Internal,
                    "detected protocol violation",
                    FailureReason::None,
                    {FailureOrigin::Protocol, FailureScope::Unspecified}};
    }
    if (call.validate_only) {
      ++checks->validations;
      require(!call.output.valid(), "identity validation has writable output");
      return Status::success();
    }
    ++checks->writes;
    const auto& pieces =
        *call.prepared->traits().outputs[0].static_dependency_pieces;
    const auto wanted = take(Footprint::from_regions(
        call.output_metadata.descriptor.shape, {call.output_region}));
    for (const auto& piece : pieces) {
      const auto hit = take(piece.coverage.intersect(wanted));
      const auto& map = piece.inputs[0];
      for (const auto& box : hit.boxes()) {
        // This fixture's source support coalesces to one rectangle per port.
        const auto input =
            std::find_if(call.exact_inputs->begin(), call.exact_inputs->end(),
                         [&](const auto& i) { return i.port == map.port; });
        require(input != call.exact_inputs->end(), "missing mapped source");
        auto copied = copy_planar_region(
            box, map, &input->image, nullptr, call.output,
            *call.output_metadata.planar_layout, 1, call.cancellation);
        if (!copied.ok()) {
          return copied;
        }
      }
    }
    if (probe->after_copy) {
      probe->after_copy();
    }
    return Status::success();
  };
  return result;
}
void native_operations() {
  const auto first = ps::handoff_testing::probe_image();
  auto second = take(PlanarImage::create(first.descriptor(), {}));
  std::vector<std::uint8_t> other(30);
  for (unsigned i = 0; i < other.size(); ++i) {
    other[i] = static_cast<std::uint8_t>(250 - i);
  }
  take(second.publish(Region::whole(second.descriptor().shape), other.data(),
                      other.size()));
  auto document = ps::handoff_testing::probe_document(first);
  auto declaration = document.inputs[0];
  declaration.id = 2;
  declaration.name = "other";
  document.inputs.push_back(declaration);
  document.nodes[0].inputs.push_back(WorkflowInputReference{2});
  auto bindings = ps::handoff_testing::probe_bindings(first);
  bindings.inputs.push_back(
      {"other", {}, {}, {}, std::make_shared<const PlanarImage>(second)});
  for (const auto mapping :
       {Mapping::Identity, Mapping::PortOne, Mapping::SwapChannels}) {
    for (const auto policy :
         {DataMovementViewPolicy::Auto, DataMovementViewPolicy::Materialize,
          DataMovementViewPolicy::RequireView}) {
      for (bool managed : {false, true}) {
        auto probe = std::make_shared<Probe>();
        auto checks = std::make_shared<Checks>();
        auto registry = std::make_shared<OperationRegistry>();
        take(registry->register_operation(
            operation(probe, checks, mapping, policy)));
        take(registry->freeze());
        GraphContext graph(document);
        const auto compiled = take(Compiler(registry).compile(graph));
        ExecutionContextConfig config;
        config.cpu_workers = 1;
        if (managed) {
          config.managed_resources = ResourceLimits{};
        }
        ExecutionContext execution(registry, config);
        auto result = execution.execute(compiled.plan, bindings);
        if (policy == DataMovementViewPolicy::RequireView &&
            mapping != Mapping::Identity) {
          require(!result.ok() &&
                      result.status().message.find("ViewUnavailable") !=
                          std::string::npos &&
                      probe->callbacks == 0,
                  "nonidentity map accepted as required port-0 alias");
          continue;
        }
        auto completed = take(std::move(result));
        require(completed.diagnostics.operation_timings.back()
                        .numeric.evaluated_values == 7,
                "exact callback numeric diagnostics were lost");
        auto output = completed.images.at("result");
        const bool alias = mapping == Mapping::Identity &&
                           policy != DataMovementViewPolicy::Materialize;
        require((output.owner_token() == first.owner_token()) == alias,
                "incorrect exact alias decision");
        require(checks->validations == (alias ? 1U : 0U) &&
                    checks->writes == (alias ? 0U : 1U),
                "exact callback mode mismatch");
        require(checks->scoped == (managed ? 1U : 0U),
                "worker did not receive managed scope");
        std::vector<std::uint8_t> expected(30), actual(30);
        take((mapping == Mapping::PortOne ? second : first)
                 .read(Region::whole(first.descriptor().shape), expected.data(),
                       expected.size()));
        if (mapping == Mapping::SwapChannels) {
          for (unsigned i = 0; i < expected.size(); i += 2) {
            std::swap(expected[i], expected[i + 1]);
          }
        }
        take(output.read(Region::whole(output.descriptor().shape),
                         actual.data(), actual.size()));
        require(expected == actual,
                "exact materialization selected wrong source or channel");
        if (managed) {
          checks->exhaust_work = true;
          auto exhausted = execution.execute(compiled.plan, bindings);
          require(!exhausted.ok() &&
                      exhausted.status().code == ErrorCode::ResourceExhausted,
                  "exact callback swallowed failed work admission");
          checks->exhaust_work = false;
          checks->sticky_failure = true;
          auto failed = execution.execute(compiled.plan, bindings);
          require(!failed.ok() &&
                      failed.status().code == ErrorCode::ResourceExhausted,
                  "successful callback swallowed sticky metadata failure");
          checks->sticky_failure = false;
          take(execution.execute(compiled.plan, bindings));
        }
        CancellationSource stop;
        probe->on_callback = [&] { stop.cancel(); };
        auto cancelled =
            execution.execute(compiled.plan, bindings, stop.token());
        require(
            !cancelled.ok() && cancelled.status().code == ErrorCode::Cancelled,
            "exact validation or write escaped cancellation gate");
        probe->on_callback = [&] {
          static_cast<void>(graph.replace(document));
        };
        auto stale = execution.execute(compiled.plan, bindings);
        require(!stale.ok() && stale.status().code == ErrorCode::Stale,
                "exact validation or write escaped currentness gate");
      }
    }
  }
}
void protocol_precedence() {
  const auto image = ps::handoff_testing::probe_image();
  const auto document = ps::handoff_testing::probe_document(image);
  const auto bindings = ps::handoff_testing::probe_bindings(image);
  for (bool invalidate : {false, true}) {
    auto probe = std::make_shared<Probe>();
    auto checks = std::make_shared<Checks>();
    checks->protocol_failure = true;
    auto definition = operation(probe, checks, Mapping::Identity,
                                DataMovementViewPolicy::Auto);
    definition.traits.input_count = 1;
    definition.traits.input_schema.resize(1);
    auto registry = std::make_shared<OperationRegistry>();
    take(registry->register_operation(std::move(definition)));
    take(registry->freeze());
    GraphContext graph(document);
    auto compiled = take(Compiler(registry).compile(graph));
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    ExecutionContext execution(registry, config);
    CancellationSource stop;
    probe->on_callback = [&] {
      if (invalidate) {
        static_cast<void>(graph.replace(document));
      } else {
        stop.cancel();
      }
    };
    auto result = execution.execute(compiled.plan, bindings, stop.token());
    require(!result.ok() &&
                result.status().detail.origin == FailureOrigin::Protocol,
            "detected Protocol failure lost to cancellation or stale plan");
  }
}
void cumulative_stage_budget() {
  const auto image = ps::handoff_testing::probe_image();
  const auto document = ps::handoff_testing::probe_document(image);
  const auto bindings = ps::handoff_testing::probe_bindings(image);
  for (const auto policy :
       {DataMovementViewPolicy::Auto, DataMovementViewPolicy::Materialize}) {
    for (std::uint64_t limit : {0U, 1U}) {
      auto probe = std::make_shared<Probe>();
      auto checks = std::make_shared<Checks>();
      auto definition = operation(probe, checks, Mapping::Identity, policy);
      definition.traits.input_count = 1;
      definition.traits.input_schema.resize(1);
      auto registry = std::make_shared<OperationRegistry>();
      take(registry->register_operation(std::move(definition)));
      take(registry->freeze());
      GraphContext graph(document);
      auto compiled = take(Compiler(registry).compile(graph));
      ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.managed_resources = ResourceLimits{};
      config.managed_resources->maximum_stages = limit;
      ExecutionContext execution(registry, config);
      if (limit) {
        take(execution.execute(compiled.plan, bindings));
      }
      auto denied = execution.execute(compiled.plan, bindings);
      require(
          !denied.ok() && denied.status().code == ErrorCode::ResourceExhausted,
          "exact callback bypassed cumulative stage budget");
      require(probe->callbacks == limit,
              "stage admission must fail before invoking callback");
      require(
          take(execution.resource_budget()).statistics().issued.stages == limit,
          "exact stage accounting is not cumulative across executions");
    }
  }
}
void proof_boundaries() {
  OperationMetadata input;
  input.descriptor = {ElementType::Float32, {3, 5, 2}};
  input.planar_layout = PlanarImageLayout{};
  OperationTraits traits;
  DependencyMappedNeed map;
  for (std::int32_t i = 0; i < 3; ++i) {
    DependencyAxis a;
    a.observation_axis = i;
    map.axes.push_back(a);
  }
  traits.outputs[0].static_dependency_pieces = std::vector<DependencyMapPiece>{
      {take(Footprint::all(input.descriptor.shape)), {map}}};
  auto proof = [&] {
    return input_internal::planar_exact_identity_view(traits, {input},
                                                      input.descriptor);
  };
  require(take(proof()), "identity proof rejected valid full map");
  auto& piece = traits.outputs[0].static_dependency_pieces->front();
  piece.inputs[0].roles =
      static_cast<std::uint32_t>(DependencyRole::Validation);
  require(!take(proof()), "validation-only support was mistaken for data");
  piece.inputs[0] = map;
  piece.inputs[0].axes[0].translation = 1;
  require(!take(proof()), "translation was mistaken for identity");
  piece.inputs[0] = map;
  auto changed = map;
  changed.port = 1;
  auto dims = Region::whole(input.descriptor.shape).dimensions();
  dims[2] = {0, 1};
  piece.coverage =
      take(Footprint::from_regions(input.descriptor.shape, {Region(dims)}));
  dims[2] = {1, 1};
  traits.outputs[0].static_dependency_pieces->push_back(
      {take(Footprint::from_regions(input.descriptor.shape, {Region(dims)})),
       {changed}});
  // A later piece cannot be ignored because the requested subset is identity.
  require(!take(proof()), "proof skipped a nonidentity prepared piece");
  traits.outputs[0].static_dependency_pieces->pop_back();
  traits.outputs[0].static_dependency_pieces->front().coverage =
      take(Footprint::all(input.descriptor.shape));
  FootprintLimits limits;
  limits.consume_work = [](std::uint64_t) {
    return Status{ErrorCode::ResourceExhausted, "test limit"};
  };
  auto exhausted = input_internal::planar_exact_identity_view(
      traits, {input}, input.descriptor, limits);
  require(!exhausted.ok() &&
              exhausted.status().code == ErrorCode::ResourceExhausted,
          "proof ignored work limit");
  CancellationSource cancelled;
  cancelled.cancel();
  limits.consume_work = {};
  limits.cancellation = cancelled.token();
  auto stopped = input_internal::planar_exact_identity_view(
      traits, {input}, input.descriptor, limits);
  require(!stopped.ok() && stopped.status().code == ErrorCode::Cancelled,
          "proof ignored cancellation");
}
}  // namespace
int main() try {
  proof_boundaries();
  native_operations();
  cumulative_stage_budget();
  protocol_precedence();
  std::cout << "exact identity/nonidentity/native/scope/sticky/cancellation "
               "checks passed\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
