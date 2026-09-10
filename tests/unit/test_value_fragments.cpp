#include <cstring>
#include <memory>
#include <vector>

#include "photospider/data/value_fragments.hpp"
#include "support/test_support.hpp"

int main() {
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
