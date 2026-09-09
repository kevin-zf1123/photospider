#include <cfenv>  // NOLINT(build/c++11)
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "data/content_digest.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"
#include "support/typed_images.hpp"

namespace {
int numeric_environment() {
#if defined(__APPLE__) && defined(__aarch64__)
  using namespace ps;  // NOLINT(build/namespaces)
  std::fenv_t original;
  PS_CHECK(std::fegetenv(&original) == 0);
  const auto rgba = rgba_semantics();
  auto make = [&](float red, float alpha) {
    std::vector<std::uint8_t> bytes(16);
    std::memcpy(bytes.data(), &red, 4);
    std::memcpy(bytes.data() + 12, &alpha, 4);
    return Value::create({ElementType::Float32, {1, 1, 4}},
                         Region::whole({1, 1, 4}), {0, {16, 16, 4}}, bytes,
                         {encode_semantic(rgba).take_value()})
        .take_value();
  };
  const auto invalid = make(std::numeric_limits<float>::denorm_min(), 0);
  const auto valid = make(1, std::numeric_limits<float>::denorm_min());
  InputSnapshotStore store;
  const auto base = store.import_value(valid).take_value();
  PS_CHECK(std::fesetenv(FE_DFL_DISABLE_DENORMS_ENV) == 0);
  const bool direct_invalid = validate_semantic_value(rgba, invalid).ok();
  const bool direct_valid = validate_semantic_value(rgba, valid).ok();
  const bool accepted_invalid = store.import_value(invalid).ok();
  const bool accepted_valid = store.import_value(valid).ok();
  const bool patched_invalid = store.patch(base, invalid).ok();
  const bool patched_valid = store.patch(base, valid).ok();
  // The caller's flush mode must still be active after nested validation.
  volatile float tiny = std::numeric_limits<float>::denorm_min();
  volatile double converted = tiny;
  const bool restored = converted == 0;
  std::fesetenv(&original);
  PS_CHECK(!direct_invalid && direct_valid);
  PS_CHECK(!accepted_invalid);
  PS_CHECK(accepted_valid);
  PS_CHECK(!patched_invalid);
  PS_CHECK(patched_valid);
  PS_CHECK(restored);
#endif
  return 0;
}
int typed_snapshots() {
  using namespace ps;  // NOLINT(build/namespaces)
  for (const auto& semantic : typed_images::descriptions()) {
    auto original = typed_images::value(semantic);
    const auto channels = original.descriptor().shape[2];
    InputSnapshotStore store({4096, 2});
    auto base = store.import_value(original);
    PS_CHECK(base.ok());
    PS_CHECK(store.live_bytes() == 3 * 5 * channels * 4);
    const Region region({{1, 2}, {1, 3}, {0, channels}});
    auto writer =
        MutableValue::allocate(original.descriptor(), region, BufferAllocator{})
            .take_value();
    const float replacement[] = {-3.F, 8.F, .25F, 1.F};
    for (std::size_t i = 0; i < writer.size(); i += 4)
      std::memcpy(writer.data() + i, &replacement[(i / 4) % channels], 4);
    auto patch = std::move(writer).publish(original.facets()).take_value();
    auto next = store.patch(base.value(), patch);
    PS_CHECK(next.ok());
    auto expected = original.copy_bytes();
    for (std::uint64_t y = 1; y < 3; ++y)
      for (std::uint64_t x = 1; x < 4; ++x)
        std::memcpy(expected.data() + (y * 5 + x) * channels * 4, replacement,
                    channels * 4);
    std::vector<std::uint8_t> read(expected.size());
    PS_CHECK(
        next.value().read(original.region(), read.data(), read.size()).ok());
    PS_CHECK(read == expected);
    PS_CHECK(
        base.value().read(original.region(), read.data(), read.size()).ok());
    PS_CHECK(read == original.copy_bytes());
    PS_CHECK(next.value().content_identity(region).value() !=
             base.value().content_identity(region).value());
    const Region untouched({{0, 1}, {0, 5}, {0, channels}});
    PS_CHECK(next.value().content_identity(untouched).value() ==
             base.value().content_identity(untouched).value());
    const Region incomplete({{0, 1}, {0, 1}, {1, channels - 1}});
    PS_CHECK(
        !base.value().read(incomplete, read.data(), (channels - 1) * 4).ok());
    PS_CHECK(!store.patch(base.value(), original.view(incomplete).take_value())
                  .ok());
    auto changed = semantic;
    changed.reference = "display";
    auto different = typed_images::value(changed);
    auto other = store.import_value(different);
    PS_CHECK(other.ok());
    PS_CHECK(base.value().content_identity(original.region()).value() !=
             other.value().content_identity(original.region()).value());
    PS_CHECK(store.patch(base.value(), different).status().code ==
             ErrorCode::TypeMismatch);

    // Freeze copies the snapshot handle and preserves the old semantic/pixels.
    auto operations = make_default_operation_registry();
    Compiler compiler(operations);
    ExecutionContext execution(operations, {1, false, 8, 65536, 8192});
    GraphContext graph(typed_images::document(original));
    auto plan = compiler.compile(graph).take_value().plan;
    auto handle = std::make_shared<InputSnapshot>(base.value());
    auto frozen =
        execution.freeze(plan, {{{"image", {}, {}, handle}}}).take_value();
    *handle = next.value();
    graph.replace(typed_images::document(different));
    auto result = execution.execute(frozen);
    PS_CHECK(result.ok() &&
             typed_images::same(result.value().values.at("result"), original));
    auto hit = execution.execute(frozen);
    PS_CHECK(hit.ok() && hit.value().diagnostics.cache_hits > 0 &&
             typed_images::same(hit.value().values.at("result"), original));
  }
  return 0;
}
}  // namespace

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(numeric_environment() == 0);
  PS_CHECK(typed_snapshots() == 0);
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
  auto full =
      Value::create({ElementType::Float32, shape}, Region::whole(shape),
                    {0, {20, 4}}, std::vector<std::uint8_t>(60),
                    {encode_semantic(coverage_semantics()).take_value()})
          .take_value();
  InputSnapshotStore store({76, 2});
  auto base = store.import_value(full);
  PS_CHECK(base.ok() && store.live_bytes() == 60);
  std::vector<std::uint8_t> bytes(4);
  float one = 1;
  std::memcpy(bytes.data(), &one, 4);
  auto patch = Value::create(full.descriptor(), Region({{0, 1}, {0, 1}}),
                             {0, {4, 4}, {0, 0}}, bytes, full.facets())
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
  auto invalid = Value::create(full.descriptor(), patch.region(),
                               patch.layout(), bytes, full.facets())
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
  document.inputs = {{1, "mask", full.descriptor(), full.region(),
                      full.layout(), full.facets()}};
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
