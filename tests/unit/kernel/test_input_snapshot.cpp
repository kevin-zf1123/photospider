#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/content_digest.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
int narrow_snapshot_identity() {
  using namespace ps;  // NOLINT(build/namespaces)
  const std::uint16_t samples[] = {0xfedc, 0xabcd, 0xffff, 0x0080};
  const char* expected[] = {
      "81686325b8c8b926132958dc19e310c2ab9ec8a74e1b1bcaf767977e4f634b5b",
      "dfda3858c2d7b952c6b68f27e2d5114a606215a60231624de56b66da96a492d4"};
  unsigned index = 0;
  for (auto type : {ElementType::UInt16, ElementType::Int16}) {
    std::vector<std::uint8_t> bytes(sizeof(samples));
    std::memcpy(bytes.data(), samples, sizeof(samples));
    auto source = TensorSpan::create({type, {4}}, Region::whole({4}), {0, {2}},
                                     std::move(bytes))
                      .take_value();
    InputSnapshotStore store({64, 4});
    auto snapshot =
        store
            .import_tensor(Tensor::create(source.descriptor(),
                                          Footprint::all({4}).take_value(),
                                          {source})
                               .take_value())
            .take_value();
    // Independent SHA-256 vector for one sample: neighboring samples are not
    // part of the requested region, even when they share a storage block.
    PS_CHECK(snapshot.content_identity(Region({{0, 1}})).value() ==
             expected[index++]);
  }
  return 0;
}
int exact_patch_versions() {
  using namespace ps;  // NOLINT(build/namespaces)
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    const auto width = TensorSpan::element_size(type);
    const TensorDescriptor descriptor{type, {1, 5}};
    std::array<InputSnapshot, 3> versions;
    std::array<std::vector<std::uint8_t>, 3> expected;
    expected[0].assign(5 * width, 0x11);
    {
      InputSnapshotStore store({256, 2});
      auto span = TensorSpan::create(descriptor, Region::whole({1, 5}),
                                     {0,
                                      {static_cast<std::int64_t>(5 * width),
                                       static_cast<std::int64_t>(width)}},
                                     expected[0]);
      PS_REQUIRE_OK(span);
      auto initial = Tensor::create(
          descriptor, Footprint::all({1, 5}).take_value(), {span.value()});
      PS_REQUIRE_OK(initial);
      auto imported = store.import_tensor(initial.value());
      PS_REQUIRE_OK(imported);
      versions[0] = imported.take_value();
      for (unsigned step = 1; step <= 2; ++step) {
        const std::uint64_t column = step == 1 ? 0 : 4;
        const Region roi({{0, 1}, {column, 1}});
        auto allocated =
            TensorWriter::allocate(descriptor, roi, BufferAllocator{});
        PS_REQUIRE_OK(allocated);
        auto writer = allocated.take_value();
        // These are opaque sample bits, including nonfinite float payloads.
        std::memset(writer.data(), 0xff, width);
        auto patch_span = std::move(writer).publish();
        PS_REQUIRE_OK(patch_span);
        auto coverage = Footprint::from_regions({1, 5}, {roi});
        PS_REQUIRE_OK(coverage);
        auto patch =
            Tensor::create(descriptor, coverage.value(), {patch_span.value()});
        PS_REQUIRE_OK(patch);
        auto changed = store.patch(versions[step - 1], patch.value());
        PS_REQUIRE_OK(changed);
        versions[step] = changed.take_value();
        expected[step] = expected[step - 1];
        std::memset(expected[step].data() + column * width, 0xff, width);
      }
    }
    // Every version keeps its own bytes after the store and source retire.
    for (unsigned step = 0; step < versions.size(); ++step) {
      std::vector<std::uint8_t> observed(5 * width);
      PS_REQUIRE_OK(versions[step].read(Region::whole({1, 5}), observed.data(),
                                        observed.size()));
      PS_CHECK(observed == expected[step]);
    }
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(narrow_snapshot_identity() == 0);
  PS_CHECK(exact_patch_versions() == 0);
  return 0;
}
