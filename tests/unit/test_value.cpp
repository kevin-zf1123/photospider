#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "photospider/data/value.hpp"
#include "support/test_support.hpp"

/**
 * @brief Exercises Region, layout, facet, type, bounds, and scalar paths.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
 * @note Behavioral failures otherwise return nonzero through `PS_CHECK`.
 */
int main() {
  using namespace ps;

  const Region whole = Region::whole({2U, 3U});
  PS_CHECK(whole.rank() == 2U);
  PS_CHECK(!whole.empty());
  PS_CHECK(whole.element_count().ok());
  PS_CHECK(whole.element_count().value() == 6U);

  const Region empty({RegionDimension{0U, 2U}, RegionDimension{1U, 0U}});
  PS_CHECK(empty.empty());
  PS_CHECK(empty.element_count().value() == 0U);
  PS_CHECK(!Region({RegionDimension{2U, 2U}}).validate({3U}).ok());

  bool overflow_rejected = false;
  try {
    static_cast<void>(Region(
        {RegionDimension{std::numeric_limits<std::uint64_t>::max(), 1U}}));
  } catch (const std::invalid_argument&) {
    overflow_rejected = true;
  }
  PS_CHECK(overflow_rejected);

  std::vector<std::uint8_t> bytes(6U, 9U);
  auto value = Value::create(
      ValueDescriptor{ElementType::UInt8, {2U, 3U}}, whole,
      StridedLayout{0U, {3, 1}}, bytes,
      {ValueFacet{"zeta", 2U, {9U}}, ValueFacet{"alpha", 1U, {1U, 2U}}});
  PS_CHECK(value.ok());
  PS_CHECK(value.value().bytes().size() == 6U);
  PS_CHECK(value.value().facets().size() == 2U);
  PS_CHECK(value.value().facets()[0].key == "alpha");
  PS_CHECK(value.value().facets()[1].key == "zeta");

  auto duplicate_facet =
      Value::create(ValueDescriptor{ElementType::UInt8, {1U}},
                    Region::whole({1U}), StridedLayout{0U, {1}}, {1U},
                    {ValueFacet{"same", 1U, {}}, ValueFacet{"same", 2U, {}}});
  PS_CHECK(!duplicate_facet.ok());
  PS_CHECK(duplicate_facet.status().code == ErrorCode::InvalidArgument);

  auto malformed_facet = Value::create(
      ValueDescriptor{ElementType::UInt8, {1U}}, Region::whole({1U}),
      StridedLayout{0U, {1}}, {1U}, {ValueFacet{"bad key", 0U, {}}});
  PS_CHECK(!malformed_facet.ok());

  auto oversized_facet = Value::create(
      ValueDescriptor{ElementType::UInt8, {1U}}, Region::whole({1U}),
      StridedLayout{0U, {1}}, {1U},
      {ValueFacet{"large", 1U, std::vector<std::uint8_t>(65537U)}});
  PS_CHECK(!oversized_facet.ok());
  PS_CHECK(oversized_facet.status().code == ErrorCode::ResourceExhausted);

  auto outside =
      Value::create(ValueDescriptor{ElementType::UInt8, {2U, 3U}}, whole,
                    StridedLayout{0U, {4, 1}}, std::vector<std::uint8_t>(6U));
  PS_CHECK(!outside.ok());
  PS_CHECK(outside.status().code == ErrorCode::InvalidArgument);

  auto rank_mismatch =
      Value::create(ValueDescriptor{ElementType::UInt8, {2U, 3U}}, whole,
                    StridedLayout{0U, {1}}, std::vector<std::uint8_t>(6U));
  PS_CHECK(!rank_mismatch.ok());

  auto reverse = Value::create(ValueDescriptor{ElementType::UInt8, {3U}},
                               Region::whole({3U}), StridedLayout{2U, {-1}},
                               std::vector<std::uint8_t>{1U, 2U, 3U});
  PS_CHECK(reverse.ok());

  const Value scalar = Value::from_float64(42.5);
  PS_CHECK(scalar.valid());
  PS_CHECK(scalar.as_float64().ok());
  PS_CHECK(scalar.as_float64().value() == 42.5);
  PS_CHECK(!value.value().as_float64().ok());
  return 0;
}
