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
int generic_snapshots() {
  using namespace ps;  // NOLINT(build/namespaces)
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    for (std::size_t rank = 1; rank <= 8; ++rank) {
      const auto width = Value::element_size(type);
      const std::vector<std::uint64_t> shape(rank, 2);
      const auto count = std::uint64_t{1} << rank;
      auto writer = MutableValue::allocate({type, shape}, Region::whole(shape),
                                           BufferAllocator{})
                        .take_value();
      for (std::size_t i = 0; i < writer.size(); ++i)
        writer.data()[i] = static_cast<std::uint8_t>((i * 7 + 13) % 256);
      auto source = std::move(writer).publish().take_value();
      InputSnapshotStore store({count * width * 2, 1});
      auto base = store.import_value(source);
      PS_CHECK(base.ok() && store.live_bytes() == count * width);
      std::vector<std::uint8_t> bytes(source.bytes().size());
      PS_CHECK(
          base.value().read(source.region(), bytes.data(), bytes.size()).ok());
      PS_CHECK(bytes == source.copy_bytes());
      auto dimensions = source.region().dimensions();
      for (auto& d : dimensions)
        d = {1, 1};
      Region patch_region(dimensions);
      auto patch_writer =
          MutableValue::allocate(source.descriptor(), patch_region,
                                 BufferAllocator{})
              .take_value();
      std::memset(patch_writer.data(), 0x42, width);
      auto patch = std::move(patch_writer).publish().take_value();
      auto next = store.patch(base.value(), patch);
      PS_CHECK(next.ok() && store.live_bytes() == (count + 1) * width);
      PS_CHECK(
          next.value().read(source.region(), bytes.data(), bytes.size()).ok());
      auto expected = source.copy_bytes();
      std::memset(expected.data() + expected.size() - width, 0x42, width);
      PS_CHECK(bytes == expected);
      PS_CHECK(
          base.value().read(source.region(), bytes.data(), bytes.size()).ok());
      PS_CHECK(bytes == source.copy_bytes());
      for (auto& d : dimensions)
        d = {0, 1};
      Region untouched(dimensions);
      PS_CHECK(next.value().content_identity(untouched).value() ==
               base.value().content_identity(untouched).value());
      PS_CHECK(next.value().content_identity(patch_region).value() !=
               base.value().content_identity(patch_region).value());
      InputSnapshotStore other_geometry({count * width, 2});
      PS_CHECK(other_geometry.import_value(source)
                   .value()
                   .content_identity(source.region())
                   .value() ==
               base.value().content_identity(source.region()).value());
      SnapshotAccessOptions limited;
      limited.maximum_samples = count - 1;
      PS_CHECK(!store.import_value(source, limited).ok());
      PS_CHECK(base.value()
                   .read(source.region(), bytes.data(), bytes.size(), limited)
                   .code == ErrorCode::ResourceExhausted);
      CancellationSource cancellation;
      cancellation.cancel();
      limited.cancellation = cancellation.token();
      PS_CHECK(base.value()
                   .content_identity(source.region(), limited)
                   .status()
                   .code == ErrorCode::Cancelled);
      PS_CHECK(store.patch(base.value(), patch, limited).status().code ==
               ErrorCode::Cancelled);
    }
  }
  // Dtype belongs to identity even when all payload bytes match.
  InputSnapshotStore store;
  auto f64 = Value::create({ElementType::Float64, {2}}, Region::whole({2}),
                           {0, {8}}, std::vector<std::uint8_t>(16))
                 .take_value();
  auto i64 = Value::create({ElementType::Int64, {2}}, Region::whole({2}),
                           {0, {8}}, std::vector<std::uint8_t>(16))
                 .take_value();
  auto floating = store.import_value(f64).take_value();
  auto integer = store.import_value(i64).take_value();
  PS_CHECK(floating.content_identity(f64.region()).value() !=
           integer.content_identity(i64.region()).value());
  PS_CHECK(store.patch(floating, i64).status().code == ErrorCode::TypeMismatch);
  auto reversed = Value::create({ElementType::UInt8, {4}}, Region::whole({4}),
                                {3, {-1}}, {1, 2, 3, 4})
                      .take_value();
  auto snapshot = store.import_value(reversed).take_value();
  std::vector<std::uint8_t> read(4);
  PS_CHECK(snapshot.read(reversed.region(), read.data(), 4).ok());
  PS_CHECK(read == std::vector<std::uint8_t>({4, 3, 2, 1}));
  InputSnapshotStore bounded({16, 1, 2});
  PS_CHECK(bounded.import_value(reversed).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(bounded.live_bytes() == 0);
  // Public compile/execute and frozen capture consume generic snapshots.
  auto registry = make_default_operation_registry();
  WorkflowDocument document;
  document.inputs = {
      {1, "input", f64.descriptor(), f64.region(), f64.layout(), {}}};
  document.nodes = {{1, "numeric.mean", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  Compiler compiler(registry);
  auto plan = compiler.compile(graph).take_value().plan;
  ExecutionContext execution(registry, {1, false, 8, 65536, 8192});
  auto frozen =
      execution
          .freeze(
              plan,
              {{{"input", {}, {}, std::make_shared<InputSnapshot>(floating)}}})
          .take_value();
  auto result = execution.execute(frozen);
  PS_CHECK(result.ok() &&
           result.value().values.at("result").as_float64().value() == 0);
  return 0;
}
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
  PS_CHECK(store.import_value(valid).status().code == ErrorCode::TypeMismatch);
  PS_CHECK(std::fesetenv(FE_DFL_DISABLE_DENORMS_ENV) == 0);
  const bool direct_invalid = validate_semantic_value(rgba, invalid).ok();
  const bool direct_valid = validate_semantic_value(rgba, valid).ok();
  const bool accepted_invalid = store.import_value(invalid).ok();
  const bool accepted_valid = store.import_value(valid).ok();
  // The caller's flush mode must still be active after nested validation.
  volatile float tiny = std::numeric_limits<float>::denorm_min();
  volatile double converted = tiny;
  const bool restored = converted == 0;
  std::fesetenv(&original);
  PS_CHECK(!direct_invalid && direct_valid);
  PS_CHECK(!accepted_invalid);
  PS_CHECK(!accepted_valid);
  PS_CHECK(restored);
#endif
  return 0;
}
int typed_snapshots() {
  using namespace ps;  // NOLINT(build/namespaces)
  for (const auto& semantic : typed_images::descriptions()) {
    auto original = typed_images::value(semantic);
    InputSnapshotStore store({4096, 2});
    PS_CHECK(store.import_value(original).status().code ==
             ErrorCode::TypeMismatch);
    PS_CHECK(store.live_bytes() == 0);
  }
  return 0;
}
}  // namespace

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(generic_snapshots() == 0);
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
  PS_CHECK(store.import_value(full).status().code == ErrorCode::TypeMismatch);
  PS_CHECK(store.live_bytes() == 0);
  return 0;
}
