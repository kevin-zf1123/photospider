#include <cstring>
#include <memory>
#include <vector>

#include "photospider/data/value_fragments.hpp"
#include "support/test_support.hpp"

namespace {
int packed_collection() {
  using namespace ps;  // NOLINT(build/namespaces)
  for (const auto type : {ElementType::UInt8, ElementType::Int64,
                          ElementType::Float32, ElementType::Float64}) {
    const auto width = Value::element_size(type);
    const ValueDescriptor descriptor{type, {300, 1, 150}};
    const Region region({{10, 257}, {0, 1}, {9, 129}});
    const auto count = 257 * 129;
    std::vector<std::uint8_t> bytes(3 + count * width + 5);
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<std::uint8_t>(i * 37 + 11);
    auto source = Value::create(descriptor, region,
                                {3 + 129 * width,
                                 {static_cast<std::int64_t>(129 * width), -999,
                                  static_cast<std::int64_t>(width)},
                                 {11, 0, 9}},
                                bytes)
                      .take_value();
    auto fragments =
        ValueFragments::create(
            descriptor, {},
            Footprint::from_regions(descriptor.shape, {region}).take_value(),
            {source})
            .take_value();
    auto allocator = BufferAllocator{}.limited(count * width);
    auto output = fragments.collect(region, allocator);
    PS_CHECK(output.ok() && output.value().region().rank() == region.rank());
    for (std::size_t axis = 0; axis < region.rank(); ++axis) {
      PS_CHECK(output.value().region().dimensions()[axis].offset ==
               region.dimensions()[axis].offset);
      PS_CHECK(output.value().region().dimensions()[axis].extent ==
               region.dimensions()[axis].extent);
    }
    PS_CHECK(output.value().storage() != source.storage() &&
             allocator.owns_allocation(*output.value().storage()));
    PS_CHECK(output.value().copy_bytes() ==
             std::vector<std::uint8_t>(bytes.begin() + 3,
                                       bytes.begin() + 3 + count * width));
    PS_CHECK(output.value().layout().origin ==
             std::vector<std::uint64_t>({10, 0, 9}));
    const Region middle({{12, 3}, {0, 1}, {9, 129}});
    auto subset = fragments.collect(middle, BufferAllocator{});
    PS_CHECK(subset.ok() && subset.value().copy_bytes() ==
                                std::vector<std::uint8_t>(
                                    bytes.begin() + 3 + 2 * 129 * width,
                                    bytes.begin() + 3 + 5 * 129 * width));
    // Logical work limits apply even when copies are batched.
    FootprintLimits work;
    work.maximum_work = count - 1;
    PS_CHECK(fragments.collect(region, BufferAllocator{}, work).status().code ==
             ErrorCode::ResourceExhausted);
    work.maximum_work = count;
    PS_CHECK(fragments.collect(region, BufferAllocator{}, work).ok());
    PS_CHECK(
        fragments.collect(region, BufferAllocator{}.limited(count * width - 1))
            .status()
            .code == ErrorCode::ResourceExhausted);
    // Cancellation after payload reservation must release the unpublished copy.
    CancellationSource cancellation;
    bool released = false;
    BufferAllocator cancel_on_allocate([&](std::uint64_t) {
      cancellation.cancel();
      return Result<std::shared_ptr<void>>(
          std::shared_ptr<void>(new int(0), [&](void* p) {
            delete static_cast<int*>(p);
            released = true;
          }));
    });
    FootprintLimits limits;
    limits.cancellation = cancellation.token();
    PS_CHECK(
        fragments.collect(region, cancel_on_allocate, limits).status().code ==
        ErrorCode::Cancelled);
    PS_CHECK(released);
    // Packed collection cannot expand the authorization carried by a fragment.
    auto narrow =
        fragments
            .restrict(Footprint::from_regions(descriptor.shape, {middle})
                          .take_value())
            .take_value();
    PS_CHECK(narrow.collect(region, BufferAllocator{}).status().code ==
             ErrorCode::NotFound);
    source = Value{};
    fragments = ValueFragments{};
    narrow = ValueFragments{};
    PS_CHECK(output.value().copy_bytes()[0] == bytes[3]);
  }
  // Padded/reversed rows and broadcast elements keep their logical order.
  const ValueDescriptor d{ElementType::UInt8, {2, 3}};
  for (const auto& layout :
       std::vector<StridedLayout>{{1, {4, 1}}, {7, {-4, -1}}, {2, {0, 0}}}) {
    auto v = Value::create(d, Region::whole(d.shape), layout,
                           {0, 1, 2, 3, 4, 5, 6, 7, 8})
                 .take_value();
    auto f =
        ValueFragments::create(d, {}, Footprint::all(d.shape).take_value(), {v})
            .take_value();
    const auto collected =
        f.collect(v.region(), BufferAllocator{}).take_value();
    std::vector<std::uint8_t> expected;
    for (std::uint64_t row = 0; row < 2; ++row)
      for (std::uint64_t col = 0; col < 3; ++col)
        expected.push_back(v.bytes().data()[layout.byte_offset +
                                            static_cast<std::int64_t>(row) *
                                                layout.byte_strides[0] +
                                            static_cast<std::int64_t>(col) *
                                                layout.byte_strides[1]]);
    PS_CHECK(collected.copy_bytes() == expected);
  }
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(packed_collection() == 0);
  using namespace ps;  // NOLINT(build/namespaces)
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    const auto width = Value::element_size(type);
    const ValueDescriptor descriptor{type, {1000000000}};
    const auto bytes = std::vector<std::uint8_t>(width * 2, 0x3f);
    auto left =
        Value::create(descriptor, Region({{0, 2}}),
                      {width, {-static_cast<std::int64_t>(width)}, {0}}, bytes)
            .take_value();
    auto right =
        Value::create(descriptor, Region({{999999998, 2}}),
                      {0, {static_cast<std::int64_t>(width)}, {999999998}},
                      bytes)
            .take_value();
    auto wanted = Footprint::from_regions(descriptor.shape,
                                          {left.region(), right.region()})
                      .take_value();
    auto sparse = ValueFragments::create(descriptor, {}, wanted, {left, right});
    PS_CHECK(sparse.ok() && sparse.value().fragments().size() == 2);
    std::vector<std::uint8_t> sample(width, 0);
    PS_CHECK(sparse.value().read({1}, sample.data(), width).ok());
    PS_CHECK(sample == std::vector<std::uint8_t>(width, 0x3f));
    PS_CHECK(sparse.value().read({999999999}, sample.data(), width).ok());
    PS_CHECK(sparse.value().read({500000000}, sample.data(), width).code ==
             ErrorCode::InvalidArgument);
    PS_CHECK(sparse.value().read({0}, sample.data(), 0).code ==
             ErrorCode::TypeMismatch);
    PS_CHECK(!sparse.value()
                  .collect(Region({{0, 1000000000}}), BufferAllocator{})
                  .ok());
    PS_CHECK(sparse.value()
                 .collect(left.region(), BufferAllocator{})
                 .value()
                 .copy_bytes() == bytes);
    PS_CHECK(sparse.value().retained_bytes().value() ==
             left.storage()->capacity() + right.storage()->capacity());
    auto one = Footprint::from_regions(descriptor.shape, {Region({{1, 1}})})
                   .take_value();
    auto restricted = sparse.value().restrict(one);
    PS_CHECK(restricted.ok() && restricted.value().fragments().size() == 1);
    PS_CHECK(!restricted.value().read({0}, sample.data(), width).ok());
    PS_CHECK(!restricted.value().restrict(wanted).ok());
    PS_CHECK(
        ValueFragments::create(descriptor, {}, wanted, {left}).status().code ==
        ErrorCode::NotFound);
    // Retained owner survives the original inputs and the full sparse carrier.
    std::weak_ptr<const CpuStorage> owner = left.storage();
    sparse = Result<ValueFragments>(ValueFragments{});
    left = Value{};
    PS_CHECK(!owner.expired());
    restricted = Result<ValueFragments>(ValueFragments{});
    PS_CHECK(owner.expired());
  }
  auto value = Value::create({ElementType::UInt8, {4}}, Region({{0, 4}}),
                             {0, {1}}, {1, 2, 3, 4})
                   .take_value();
  const auto all = Footprint::all({4}).take_value();
  auto duplicate = ValueFragments::create(
      value.descriptor(), {}, all,
      {value, value.view(Region({{1, 2}})).take_value()});
  PS_CHECK(duplicate.ok());
  PS_CHECK(duplicate.value().retained_bytes().value() ==
           value.storage()->capacity());
  auto explicit_origin = Value::from_storage(value.descriptor(), value.region(),
                                             {0, {1}, {0}}, value.storage())
                             .take_value();
  auto shifted_origin =
      Value::from_storage(value.descriptor(), Region({{1, 2}}), {1, {1}, {1}},
                          value.storage())
          .take_value();
  auto aliases = ValueFragments::create(
      value.descriptor(), {}, all, {value, explicit_origin, shifted_origin});
  PS_CHECK(aliases.ok() && aliases.value().retained_bytes().value() ==
                               value.storage()->capacity());
  auto other = Value::create(value.descriptor(), value.region(), value.layout(),
                             {4, 3, 2, 1})
                   .take_value();
  PS_CHECK(!ValueFragments::create(value.descriptor(), {}, all, {value, other})
                .ok());
  // A shared parameter-sized table stays compact even with atomic publication.
  const ValueDescriptor table_descriptor{ElementType::UInt8, {129, 129}};
  auto table = Value::create(table_descriptor, Region::whole({129, 129}),
                             {0, {129, 1}}, std::vector<std::uint8_t>(16641, 7))
                   .take_value();
  std::vector<Value> atoms;
  for (std::uint64_t y = 0; y < 129; ++y)
    for (std::uint64_t x = 0; x < 129; ++x)
      atoms.push_back(table.view(Region({{y, 1}, {x, 1}})).take_value());
  auto compact = ValueFragments::create(
      table_descriptor, {}, Footprint::all({129, 129}).take_value(), atoms);
  PS_CHECK(compact.ok() && compact.value().fragments().size() == 1);
  PS_CHECK(compact.value().retained_bytes().value() ==
           table.storage()->capacity());
  std::uint8_t last = 0;
  PS_CHECK(compact.value().read({128, 128}, &last, 1).ok() && last == 7);
  // Adjacent logical regions with a different mapping must not be coalesced.
  auto reversed_tail = Value::from_storage(value.descriptor(), Region({{2, 2}}),
                                           {3, {-1}, {2}}, value.storage())
                           .take_value();
  auto mixed = ValueFragments::create(
      value.descriptor(), {}, all,
      {value.view(Region({{0, 2}})).take_value(), reversed_tail});
  PS_CHECK(mixed.ok() && mixed.value().fragments().size() == 2);
  PS_CHECK(mixed.value().read({2}, &last, 1).ok() && last == 4);
  PS_CHECK(mixed.value()
               .collect(value.region(), BufferAllocator{})
               .value()
               .copy_bytes() == std::vector<std::uint8_t>({1, 2, 4, 3}));
  // A reversed contiguous mapping may coalesce without changing its samples.
  auto reverse = Value::from_storage(value.descriptor(), value.region(),
                                     {3, {-1}, {0}}, value.storage())
                     .take_value();
  auto reverse_parts =
      ValueFragments::create(value.descriptor(), {}, all,
                             {reverse.view(Region({{0, 2}})).take_value(),
                              reverse.view(Region({{2, 2}})).take_value()});
  PS_CHECK(reverse_parts.ok() && reverse_parts.value().fragments().size() == 1);
  PS_CHECK(reverse_parts.value().read({3}, &last, 1).ok() && last == 1);
  // Generic rank-three data may select channels; typed image-v2 may not.
  const ValueDescriptor image{ElementType::Float32, {1, 1, 4}};
  const auto channel =
      Footprint::from_regions(image.shape, {Region({{0, 1}, {0, 1}, {3, 1}})})
          .take_value();
  auto generic = Value::create(image, Region::whole(image.shape),
                               {0, {16, 16, 4}}, std::vector<std::uint8_t>(16))
                     .take_value();
  PS_CHECK(ValueFragments::create(image, {}, channel, {generic}).ok());
  const auto facet = encode_semantic(rgba_semantics()).take_value();
  auto typed = Value::create(image, generic.region(), generic.layout(),
                             generic.copy_bytes(), {facet})
                   .take_value();
  PS_CHECK(!ValueFragments::create(image, {facet}, channel, {typed}).ok());
  auto head = typed.view(Region({{0, 1}, {0, 1}, {0, 2}})).take_value();
  auto tail = typed.view(Region({{0, 1}, {0, 1}, {2, 2}})).take_value();
  PS_CHECK(!ValueFragments::create(image, {facet},
                                   Footprint::all(image.shape).take_value(),
                                   {head, tail})
                .ok());

  return 0;
}
