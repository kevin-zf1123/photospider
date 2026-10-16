#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "photospider/data/tensor_span.hpp"
#include "support/test_support.hpp"

/**
 * @brief Exercises Region, layout, type, bounds, and scalar paths.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
 * @note Behavioral failures otherwise return nonzero through `PS_CHECK`.
 */
int main() {
  using ps::ElementType;
  using ps::ErrorCode;
  using ps::Region;
  using ps::RegionDimension;
  using ps::StridedLayout;
  using ps::TensorDescriptor;
  using ps::TensorSpan;

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
  auto value =
      TensorSpan::create(TensorDescriptor{ElementType::UInt8, {2U, 3U}}, whole,
                         StridedLayout{0U, {3, 1}}, bytes);
  PS_CHECK(value.ok());
  PS_CHECK(value.value().bytes().size() == 6U);
  auto outside = TensorSpan::create(
      TensorDescriptor{ElementType::UInt8, {2U, 3U}}, whole,
      StridedLayout{0U, {4, 1}}, std::vector<std::uint8_t>(6U));
  PS_CHECK(!outside.ok());
  PS_CHECK(outside.status().code == ErrorCode::InvalidArgument);

  auto rank_mismatch =
      TensorSpan::create(TensorDescriptor{ElementType::UInt8, {2U, 3U}}, whole,
                         StridedLayout{0U, {1}}, std::vector<std::uint8_t>(6U));
  PS_CHECK(!rank_mismatch.ok());

  auto reverse = TensorSpan::create(
      TensorDescriptor{ElementType::UInt8, {3U}}, Region::whole({3U}),
      StridedLayout{2U, {-1}}, std::vector<std::uint8_t>{1U, 2U, 3U});
  PS_CHECK(reverse.ok());

  const std::uint64_t huge_extent =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 2U;
  auto huge_broadcast = TensorSpan::create(
      TensorDescriptor{ElementType::UInt8, {huge_extent}},
      Region::whole({huge_extent}), StridedLayout{0U, {0}}, {7U});
  PS_CHECK(huge_broadcast.ok());
  auto maximum_broadcast = TensorSpan::create(
      TensorDescriptor{ElementType::UInt8,
                       {std::numeric_limits<std::uint64_t>::max()}},
      Region::whole({std::numeric_limits<std::uint64_t>::max()}),
      StridedLayout{0U, {0}}, {8U});
  PS_CHECK(maximum_broadcast.ok());

  for (const std::int64_t nonzero_stride : {1, -1}) {
    auto huge_nonbroadcast =
        TensorSpan::create(TensorDescriptor{ElementType::UInt8, {huge_extent}},
                           Region::whole({huge_extent}),
                           StridedLayout{0U, {nonzero_stride}}, {9U});
    PS_CHECK(!huge_nonbroadcast.ok());
    PS_CHECK(huge_nonbroadcast.status().code == ErrorCode::InvalidArgument);
  }

  auto mixed_positive = TensorSpan::create(
      TensorDescriptor{ElementType::UInt8, {huge_extent, 3U}},
      Region::whole({huge_extent, 3U}), StridedLayout{0U, {0, 1}},
      {1U, 2U, 3U});
  PS_CHECK(mixed_positive.ok());
  auto mixed_negative = TensorSpan::create(
      TensorDescriptor{ElementType::UInt8, {huge_extent, 3U}},
      Region::whole({huge_extent, 3U}), StridedLayout{2U, {0, -1}},
      {1U, 2U, 3U});
  PS_CHECK(mixed_negative.ok());

  auto singleton_minimum_stride = TensorSpan::create(
      TensorDescriptor{ElementType::UInt8, {1U}}, Region::whole({1U}),
      StridedLayout{0U, {std::numeric_limits<std::int64_t>::min()}}, {10U});
  PS_CHECK(singleton_minimum_stride.ok());
  auto repeated_minimum_stride = TensorSpan::create(
      TensorDescriptor{ElementType::UInt8, {2U}}, Region::whole({2U}),
      StridedLayout{0U, {std::numeric_limits<std::int64_t>::min()}}, {10U});
  PS_CHECK(!repeated_minimum_stride.ok());
  PS_CHECK(repeated_minimum_stride.status().code == ErrorCode::InvalidArgument);

  // Borrowed addresses preserve reversed, broadcast, origin-relative and
  // singleton-stride layouts, and reject invalid pointers/ranks/coverage.
  const std::uint64_t last[] = {2};
  PS_CHECK(reverse.value().byte_address(last, 1).value() == 0);
  PS_CHECK(
      reverse.value().byte_address(std::vector<std::uint64_t>{2}).value() ==
      reverse.value().byte_address({2}).value());
  const std::uint64_t far[] = {huge_extent - 1};
  PS_CHECK(huge_broadcast.value().byte_address(far, 1).value() == 0);
  PS_CHECK(singleton_minimum_stride.value().byte_address({0}).value() == 0);
  PS_CHECK(reverse.value().byte_address(nullptr, 1).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(reverse.value().byte_address(last, 0).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(reverse.value().byte_address({3}).status().code ==
           ErrorCode::InvalidArgument);
  PS_CHECK(TensorSpan{}.byte_address(last, 1).status().code ==
           ErrorCode::InvalidArgument);
  auto origin_relative =
      TensorSpan::create({ElementType::UInt8, {10}}, Region({{7, 3}}),
                         {2, {-1}, {7}}, {11, 12, 13});
  PS_CHECK(origin_relative.ok());
  const std::uint64_t origin_end[] = {9};
  ps::TensorSpanView view(origin_relative.value());
  PS_CHECK(view.byte_address(origin_end, 1).value() == 0);
  PS_CHECK(view.byte_address({8}).value() == 1);
  PS_CHECK(view.byte_address(std::vector<std::uint64_t>{7}).value() == 2);
  const auto high_origin = UINT64_MAX - 3;
  auto high_relative = TensorSpan::create(
      {ElementType::UInt8, {UINT64_MAX}}, Region({{high_origin, 3}}),
      {2, {-1}, {high_origin}}, {21, 22, 23});
  PS_CHECK(high_relative.ok());
  const std::uint64_t high_end[] = {UINT64_MAX - 1};
  PS_CHECK(high_relative.value().byte_address(high_end, 1).value() == 0);
  PS_CHECK(high_relative.value().byte_address({high_origin}).value() == 2);

  auto overflowing_element_count =
      Region::whole({std::numeric_limits<std::uint64_t>::max(), 2U})
          .element_count();
  PS_CHECK(!overflowing_element_count.ok());
  PS_CHECK(overflowing_element_count.status().code ==
           ErrorCode::ResourceExhausted);

  const TensorSpan scalar = TensorSpan::from_float64(42.5);
  PS_CHECK(scalar.valid());
  PS_CHECK(scalar.region().rank() == 1U);
  PS_CHECK(scalar.region().dimensions().front().offset == 0U);
  PS_CHECK(scalar.region().dimensions().front().extent == 1U);
  PS_CHECK(scalar.as_float64().ok());
  PS_CHECK(scalar.as_float64().value() == 42.5);

  auto empty_scalar =
      TensorSpan::create(TensorDescriptor{ElementType::Float64, {1U}},
                         Region({RegionDimension{0U, 0U}}),
                         StridedLayout{0U, {8}}, scalar.copy_bytes());
  PS_CHECK(empty_scalar.ok());
  PS_CHECK(!empty_scalar.value().as_float64().ok());
  PS_CHECK(empty_scalar.value().as_float64().status().code ==
           ErrorCode::TypeMismatch);

  auto offset_scalar =
      TensorSpan::create(TensorDescriptor{ElementType::Float64, {1U}},
                         Region({RegionDimension{1U, 0U}}),
                         StridedLayout{0U, {8}}, scalar.copy_bytes());
  PS_CHECK(offset_scalar.ok());
  PS_CHECK(!offset_scalar.value().as_float64().ok());
  PS_CHECK(offset_scalar.value().as_float64().status().code ==
           ErrorCode::TypeMismatch);

  auto partial_vector = TensorSpan::create(
      TensorDescriptor{ElementType::Float64, {2U}},
      Region({RegionDimension{0U, 1U}}), StridedLayout{0U, {8}},
      std::vector<std::uint8_t>(2U * sizeof(double)));
  PS_CHECK(partial_vector.ok());
  PS_CHECK(!partial_vector.value().as_float64().ok());
  PS_CHECK(partial_vector.value().as_float64().status().code ==
           ErrorCode::TypeMismatch);
  PS_CHECK(!value.value().as_float64().ok());
  return 0;
}
