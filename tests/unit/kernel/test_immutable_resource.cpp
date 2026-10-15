#include <array>

#include "photospider/data/immutable_resource.hpp"
#include "photospider/data/resource_set.hpp"
#include "support/test_support.hpp"

int main() {
  ps::ResourceBudget root;
  std::array<std::uint8_t, 3> source{'a', 'b', 'c'};
  auto first =
      ps::ImmutableResource::import({source.data(), source.size()}, root);
  PS_CHECK(first.ok());
  const std::array<std::uint8_t, 32> expected{
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  PS_CHECK(first.value().identity().sha256 == expected);
  PS_CHECK(first.value().identity().byte_length == 3);
  source[0] = 'x';
  PS_CHECK(first.value().bytes()[0] == 'a');
  auto second =
      ps::ImmutableResource::from_storage(first.value().storage(), root);
  PS_CHECK(second.ok() &&
           second.value().identity() == first.value().identity());
  ps::ResourceBudget other;
  auto retained = first.value().reference(other);
  PS_CHECK(retained.ok() && retained.value().bytes() == first.value().bytes());
  PS_CHECK(other.statistics().live[ps::ResourceKind::Referenced] == 3);
  auto duplicate = first.value().reference(other);
  PS_CHECK(duplicate.ok() &&
           other.statistics().live[ps::ResourceKind::Referenced] == 3);
  PS_CHECK(!ps::ImmutableResource::import({nullptr, 1}, root).ok());
  PS_CHECK(!ps::ImmutableResource::from_storage({}, root).ok());
  PS_CHECK(!ps::ImmutableResource{}.reference(root).ok());
  auto limited = ps::ImmutableResource::import({source.data(), source.size()},
                                               root, {}, 5);
  PS_CHECK(!limited.ok() &&
           limited.status().reason == ps::FailureReason::WorkLimit);
  auto empty = ps::ImmutableResource::import({nullptr, 0}, root);
  PS_CHECK(!empty.ok() &&
           empty.status().code == ps::ErrorCode::InvalidArgument);
  auto set = ps::ResourceSet::create({first.value(), second.value()}, root);
  PS_CHECK(set.ok() && set.value().size() == 1);
  PS_CHECK(set.value().find(first.value().identity()).ok());
  PS_CHECK(!set.value().at(1).ok());
  auto selected =
      set.value().select({first.value().identity(), first.value().identity()});
  PS_CHECK(selected.ok() && selected.value().size() == 1);
  auto missing = first.value().identity();
  ++missing.byte_length;
  PS_CHECK(set.value().select({missing}).status().code ==
           ps::ErrorCode::NotFound);
  PS_CHECK(set.value().select({}).value().size() == 0);
  PS_CHECK(set.value().unite(set.value()).value().size() == 1);
  auto changed =
      ps::ImmutableResource::import({source.data(), source.size()}, root);
  PS_CHECK(changed.ok() &&
           !(changed.value().identity() == first.value().identity()));
  auto changed_set = ps::ResourceSet::create({changed.value()}, root);
  PS_CHECK(changed_set.ok() &&
           set.value().unite(changed_set.value()).value().size() == 2);
  return 0;
}
