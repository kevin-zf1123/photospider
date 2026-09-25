#include <cstdint>
#include <memory>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"
int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(registry->load_plugin(PS_BAD_PLANAR_FIXTURE).code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(registry->load_plugin(PS_PROJECTED_PLANAR_FIXTURE).code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(!registry->find_traits("test.planar_plugin").ok());
  PS_CHECK(registry->load_plugin(PS_PLANAR_FIXTURE).ok());
  PS_CHECK(registry->freeze().ok());
  const ValueDescriptor descriptor{ElementType::Float32, {1, 1, 1}};
  const auto whole = Region::whole(descriptor.shape);
  auto image = PlanarImage::create(descriptor, {}).take_value();
  float sample = 0.75f;
  PS_CHECK(
      image.publish(whole, reinterpret_cast<const uint8_t*>(&sample), 4).ok());
  WorkflowDocument document;
  document.inputs = {
      {1, "image", descriptor, whole, {}, {}, PlanarImageLayout{}}};
  document.outputs = {{"result", 1, "values"}};
  ExecutionBindings bindings;
  bindings.inputs.push_back(
      {"image", {}, {}, {}, std::make_shared<const PlanarImage>(image)});
  ExecutionContext execution(registry, {1, false, 8, 4 * 1024 * 1024});
  for (std::int64_t test = 0; test <= 8; ++test) {
    document.nodes = {{1,
                       "test.planar_plugin",
                       {WorkflowInputReference{1}},
                       {{"case", test}}}};
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph);
    if (test >= 2 && test <= 4) {
      PS_CHECK(!plan.ok());
      continue;
    }
    PS_CHECK(plan.ok());
    auto result = execution.execute(plan.value().plan, bindings);
    if (test == 5) {
      PS_CHECK(result.status().code == ErrorCode::ResourceExhausted);
    } else if (test == 6 || test == 8) {
      PS_CHECK(result.status().code == ErrorCode::InvalidArgument);
    } else if (test == 7) {
      PS_CHECK(result.status().code == ErrorCode::Cancelled);
    } else {
      PS_CHECK(result.ok());
      float value = 0;
      PS_CHECK(result.value()
                   .images.at("result")
                   .read(whole, reinterpret_cast<uint8_t*>(&value), 4)
                   .ok());
      PS_CHECK(value == sample);
    }
  }
  // Failure paths cannot poison later calls or retain scratch allocations.
  document.nodes[0].parameters["case"] = std::int64_t{0};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value();
  CancellationSource stop;
  stop.cancel();
  PS_CHECK(execution.execute(plan.plan, bindings, stop.token()).status().code ==
           ErrorCode::Cancelled);
  PS_CHECK(execution.execute(plan.plan, bindings).ok());
  return 0;
}
