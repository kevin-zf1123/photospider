#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "data/content_digest.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  content_internal::Sha256 empty;
  PS_CHECK(empty.finish() ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  content_internal::Sha256 abc;
  abc.bytes("abc", 3);
  PS_CHECK(abc.finish() ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  content_internal::Sha256 million;
  const std::string chunk(1000, 'a');
  for (int i = 0; i < 1000; ++i)
    million.bytes(chunk.data(), chunk.size());
  PS_CHECK(million.finish() ==
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  const std::vector<std::uint64_t> shape{3, 5};
  auto full = Value::create({ElementType::Float32, shape}, Region::whole(shape),
                            {0, {20, 4}}, std::vector<std::uint8_t>(60))
                  .take_value();
  InputSnapshotStore store({76, 2});
  auto base = store.import_value(full);
  PS_CHECK(base.ok() && store.live_bytes() == 60);
  std::vector<std::uint8_t> bytes(4);
  float one = 1;
  std::memcpy(bytes.data(), &one, 4);
  auto patch = Value::create(full.descriptor(), Region({{0, 1}, {0, 1}}),
                             {0, {4, 4}, {0, 0}}, bytes)
                   .take_value();
  auto next = store.patch(base.value(), patch);
  PS_CHECK(next.ok() && store.live_bytes() == 76);
  auto unrelated = Region({{0, 1}, {1, 1}});
  PS_CHECK(base.value().content_identity(unrelated).value() ==
           next.value().content_identity(unrelated).value());
  PS_CHECK(base.value().content_identity(patch.region()).value() !=
           next.value().content_identity(patch.region()).value());
  PS_CHECK(!store.patch(next.value(), patch).ok() && store.live_bytes() == 76);
  std::vector<std::uint8_t> old_bytes(60), new_bytes(60);
  PS_CHECK(base.value().read(full.region(), old_bytes.data(), 60).ok());
  PS_CHECK(next.value().read(full.region(), new_bytes.data(), 60).ok());
  PS_CHECK(old_bytes == full.copy_bytes() && new_bytes[2] == 128 &&
           new_bytes[3] == 63);
  PS_CHECK(!next.value().read(full.region(), new_bytes.data(), 59).ok());
  InputSnapshotStore foreign;
  PS_CHECK(!foreign.patch(base.value(), patch).ok());
  auto read = std::async(std::launch::async, [&] {
    return next.value().content_identity(full.region());
  });
  PS_CHECK(read.get().value() ==
           next.value().content_identity(full.region()).value());
  next = Result<InputSnapshot>(InputSnapshot{});
  PS_CHECK(store.live_bytes() == 60);
  base = Result<InputSnapshot>(InputSnapshot{});
  PS_CHECK(store.live_bytes() == 0);
  InputSnapshotStore short_store({59, 2});
  PS_CHECK(short_store.import_value(full).status().code ==
           ErrorCode::ResourceExhausted);
  float bad = 2;
  std::memcpy(bytes.data(), &bad, 4);
  auto invalid =
      Value::create(full.descriptor(), patch.region(), patch.layout(), bytes)
          .take_value();
  PS_CHECK(!store.patch(store.import_value(full).value(), invalid).ok());
  auto retained = [&] {
    InputSnapshotStore temporary({60, 1});
    return temporary.import_value(full).take_value();
  }();
  PS_CHECK(
      retained.content_identity(full.region()).value() ==
      store.import_value(full).value().content_identity(full.region()).value());
  auto registry = std::make_shared<OperationRegistry>();
  OperationTraits traits;
  traits.input_count = 1;
  traits.output_element_type = ElementType::Float32;
  traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.region_rule = OperationRegionRule::Elementwise;
  traits.input_schema = {{OperationPortKind::Float32Mask, 0, 0}};
  traits.output_schema = traits.input_schema[0];
  PS_CHECK(registry
               ->register_operation({"identity", traits,
                                     [](const OperationInvocation& invocation) {
                                       return Result<Value>(
                                           invocation.inputs[0]);
                                     }})
               .ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {
      {1, "mask", full.descriptor(), full.region(), full.layout(), {}}};
  document.nodes = {{1, "identity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"mask", 1, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto compiled = compiler.compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry);
  ExecutionBindings bindings{
      {{"mask", {}, {}, std::make_shared<InputSnapshot>(retained)}}};
  auto output = execution.execute(compiled.value().plan, bindings);
  PS_CHECK(output.ok() &&
           output.value().values.at("mask").copy_bytes() == full.copy_bytes());
  bindings.inputs[0].value = full;
  PS_CHECK(execution.execute(compiled.value().plan, bindings).status().code ==
           ErrorCode::InvalidArgument);
  return 0;
}
