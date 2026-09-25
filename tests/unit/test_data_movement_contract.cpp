#include <cstring>
#include <memory>
#include <string>
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
      const auto& out = result.images.at("result");
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
  require(result.images.at("result").owner_token() == image.owner_token(),
          "unordered valid pieces lost their affine alias");
  std::vector<std::uint8_t> expected(30), actual(30);
  take(image.read(Region::whole(image.descriptor().shape), expected.data(),
                  expected.size()));
  take(result.images.at("result").read(Region::whole(image.descriptor().shape),
                                       actual.data(), actual.size()));
  require(actual == expected,
          "unordered pieces silently reordered alias channels");
  return 0;
}
