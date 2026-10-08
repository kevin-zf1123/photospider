#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/fmt_handoff.hpp"

int main() {
  using namespace ps;                   // NOLINT(build/namespaces)
  using namespace ps::handoff_testing;  // NOLINT(build/namespaces)
  const auto image = probe_image();
  const auto document = probe_document(image);
  std::string ordinary_digest, movement_digest;
  for (bool movement : {false, true}) {
    for (const auto policy :
         {DataMovementViewPolicy::Auto, DataMovementViewPolicy::Materialize,
          DataMovementViewPolicy::RequireView}) {
      auto probe = std::make_shared<Probe>();
      auto registry = std::make_shared<OperationRegistry>();
      take(registry->register_operation(
          probe_operation(probe, movement, policy)));
      take(registry->freeze());
      GraphContext graph(document);
      auto compiled = take(Compiler(registry).compile(graph));
      if (policy == DataMovementViewPolicy::Auto) {
        (movement ? movement_digest : ordinary_digest) =
            compiled.semantic.digest().value;
      }
      require(compiled.semantic.nodes()[0].traits.outputs[0].data_movement ==
                  (movement ? DataMovementKind::BitwiseMapped
                            : DataMovementKind::None),
              "movement relation missing from semantic IR");
      ExecutionContext execution(registry);
      auto result =
          take(execution.execute(compiled.plan, probe_bindings(image)));
      require(probe->callbacks == (movement ? 0U : 1U),
              "executor inferred bitwise behavior from dependencies or name");
      std::vector<std::uint8_t> expected(30), actual(30);
      take(image.read(Region::whole(image.descriptor().shape), expected.data(),
                      expected.size()));
      const auto& out = result.tensors.at("result");
      take(out.read(Region::whole(out.descriptor().shape), actual.data(),
                    actual.size()));
      require(actual == expected, "declared copy changed bits");
      const bool alias =
          movement && policy != DataMovementViewPolicy::Materialize;
      require((out.owner_token() == image.owner_token()) == alias,
              "explicit movement view policy was ignored");
    }
  }
  require(ordinary_digest != movement_digest,
          "movement relation absent from canonical identity");

  // Reject malformed capabilities at compile time, before any sample callback.
  for (unsigned bad = 0; bad < 8; ++bad) {
    auto probe = std::make_shared<Probe>();
    auto registry = std::make_shared<OperationRegistry>();
    auto corrupt = [bad](OperationOutputSpecialization& out) {
      switch (bad) {
        case 0:
          out.data_movement = static_cast<DataMovementKind>(99);
          break;
        case 1:
          out.data_movement_view_policy =
              static_cast<DataMovementViewPolicy>(99);
          break;
        case 2:
          out.static_dependency_pieces.reset();
          break;
        case 3:
          out.metadata.descriptor.element_type = ElementType::Float32;
          break;
        case 4:
          out.static_dependency_pieces->front().inputs.push_back(
              out.static_dependency_pieces->front().inputs.front());
          break;
        case 5:
          out.static_dependency_pieces->front()
              .inputs[0]
              .axes[0]
              .observation_axis = 1;
          break;
        case 6:
          out.metadata.planar_layout.reset();
          break;
        case 7:
          out.static_dependency_pieces->front().inputs[0].roles =
              static_cast<std::uint32_t>(DependencyRole::Control);
          break;
      }
    };
    take(registry->register_operation(
        probe_operation(probe, true, DataMovementViewPolicy::Auto, corrupt)));
    take(registry->freeze());
    GraphContext graph(document);
    require(!Compiler(registry).compile(graph).ok() && probe->callbacks == 0,
            "malformed movement declaration accepted");
  }
  // Exact planar computation is an explicit, sealed capability. Merely having
  // dependency pieces must not opt an unrelated operation into this path.
  for (unsigned bad = 0; bad < 4; ++bad) {
    auto exact_probe = std::make_shared<Probe>();
    OperationRegistry exact_registry;
    auto op = probe_operation(exact_probe);
    op.traits.planar_exact_dependencies = true;
    if (bad == 0)
      op.traits.planar_storage_capable = false;
    if (bad == 1)
      op.planar_callback = {};
    if (bad == 2)
      op.start_dependency = {};
    if (bad == 3)
      op.prepare_static = {};
    require(!exact_registry.register_operation(std::move(op)).ok() &&
                exact_probe->callbacks == 0,
            "incomplete exact planar capability accepted");
  }
  for (unsigned bad = 0; bad < 3; ++bad) {
    auto exact_probe = std::make_shared<Probe>();
    auto exact_registry = std::make_shared<OperationRegistry>();
    auto corrupt = [bad](OperationOutputSpecialization& out) {
      if (bad == 0)
        out.static_dependency_pieces.reset();
      if (bad == 1)
        out.regional_atomic = false;
      if (bad == 2)
        out.data_movement = DataMovementKind::BitwiseMapped;
    };
    auto op = probe_operation(exact_probe, false, DataMovementViewPolicy::Auto,
                              corrupt);
    op.traits.planar_exact_dependencies = true;
    take(exact_registry->register_operation(std::move(op)));
    take(exact_registry->freeze());
    GraphContext exact_graph(document);
    require(!Compiler(exact_registry).compile(exact_graph).ok() &&
                exact_probe->callbacks == 0,
            "malformed prepared exact planar contract accepted");
  }
  {
    auto exact_probe = std::make_shared<Probe>();
    auto exact_registry = std::make_shared<OperationRegistry>();
    auto op = probe_operation(exact_probe);
    op.traits.planar_exact_dependencies = true;
    take(exact_registry->register_operation(std::move(op)));
    take(exact_registry->freeze());
    GraphContext exact_graph(document);
    const auto exact_compiled =
        take(Compiler(exact_registry).compile(exact_graph));
    require(exact_compiled.semantic.digest().value != ordinary_digest,
            "exact planar capability absent from canonical identity");
  }
  // Unsorted disjoint pieces must not silently reverse a zero-copy result.
  auto probe = std::make_shared<Probe>();
  auto registry = std::make_shared<OperationRegistry>();
  auto unsorted = [](OperationOutputSpecialization& out) {
    const auto map = out.static_dependency_pieces->front().inputs;
    out.static_dependency_pieces->clear();
    for (auto channel : {1U, 0U}) {
      auto dims = Region::whole(out.metadata.descriptor.shape).dimensions();
      dims[2] = {channel, 1};
      out.static_dependency_pieces->push_back(
          {take(Footprint::from_regions(out.metadata.descriptor.shape,
                                        {Region(dims)})),
           map});
    }
  };
  take(registry->register_operation(probe_operation(
      probe, true, DataMovementViewPolicy::RequireView, unsorted)));
  take(registry->freeze());
  GraphContext graph(document);
  auto compiled = take(Compiler(registry).compile(graph));
  ExecutionContext execution(registry);
  auto result = take(execution.execute(compiled.plan, probe_bindings(image)));
  require(result.tensors.at("result").owner_token() == image.owner_token(),
          "unordered valid pieces lost their affine alias");
  std::vector<std::uint8_t> expected(30), actual(30);
  take(image.read(Region::whole(image.descriptor().shape), expected.data(),
                  expected.size()));
  take(result.tensors.at("result").read(Region::whole(image.descriptor().shape),
                                       actual.data(), actual.size()));
  require(actual == expected,
          "unordered pieces silently reordered alias channels");
  return 0;
}
