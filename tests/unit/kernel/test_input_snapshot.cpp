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
}  // namespace
int main() {
  PS_CHECK(narrow_snapshot_identity() == 0);
  return 0;
}
