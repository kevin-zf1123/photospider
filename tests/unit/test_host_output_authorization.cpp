#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "core/host_output_authorization.hpp"
#include "photospider/data/image_view.hpp"

namespace ps {
namespace {

static_assert(!std::is_copy_constructible_v<HostOutputBinding>);
static_assert(!std::is_copy_assignable_v<HostOutputBinding>);
static_assert(std::is_nothrow_move_constructible_v<HostOutputBinding>);
static_assert(!std::is_copy_constructible_v<HostOutputWriteGrant>);
static_assert(!std::is_copy_assignable_v<HostOutputWriteGrant>);
static_assert(std::is_nothrow_move_constructible_v<HostOutputWriteGrant>);

/**
 * @brief Builds one zero-origin UINT8 interleaved image plan for tests.
 * @param width Positive image width.
 * @param height Positive image height.
 * @param channels Positive channel count.
 * @param alignment Required Host allocation alignment.
 * @return Complete validated plan named `image`.
 * @throws Any validation or allocation exception from plan construction.
 * @note The helper creates metadata only and performs no Host allocation.
 */
DenseImageOutputPlan make_plan(std::size_t width, std::size_t height,
                               std::size_t channels,
                               std::size_t alignment = 64U) {
  DenseTensorDescriptor descriptor{{height, width, channels},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  const std::size_t row_stride = width * channels;
  StridedLayout layout{{static_cast<std::ptrdiff_t>(row_stride),
                        static_cast<std::ptrdiff_t>(channels), 1}};
  return DenseImageOutputPlan::create("image", std::move(descriptor),
                                      std::move(image), std::move(layout),
                                      row_stride * height, alignment);
}

/**
 * @brief Writes one byte value into every span of an owned grant.
 * @param grant Active grant to write and retire.
 * @param value Byte value copied across every active span.
 * @param succeeded Cross-thread completion flag set false on any exception.
 * @return Nothing.
 * @throws Nothing; all failures are converted to `succeeded=false` and failed
 * retirement is attempted before return.
 * @note Used only after the test has issued pairwise-disjoint grants.
 */
void write_and_retire(HostOutputWriteGrant grant, std::byte value,
                      std::atomic<bool>* succeeded) noexcept {
  try {
    for (std::size_t index = 0U; index < grant.span_count(); ++index) {
      std::memset(grant.data(index), std::to_integer<int>(value),
                  grant.span(index).byte_size);
    }
    grant.retire_success();
  } catch (...) {
    succeeded->store(false, std::memory_order_release);
    try {
      if (grant.active()) {
        grant.retire_failure("Concurrent test writer failed.");
      }
    } catch (...) {
    }
  }
}

TEST(HostOutputAuthorization, AllocatesAtPlannedAlignmentAndPublishesOnce) {
  HostOutputBinding binding =
      HostOutputBinding::allocate(make_plan(8U, 4U, 4U));
  const AllocationIdentity allocation = binding.allocation_identity();
  HostOutputWriteGrant grant = binding.grant_whole();
  ASSERT_TRUE(grant.active());
  ASSERT_EQ(grant.span_count(), 1U);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(grant.data(0U)) % 64U, 0U);
  std::memset(grant.data(0U), 7, grant.span(0U).byte_size);
  grant.retire_success();

  const Value published = binding.seal();
  EXPECT_EQ(published.allocation_identity(), allocation);
  EXPECT_TRUE(published.revision_id().valid());
  EXPECT_EQ(published.ready_fence().poll().state(), ReadyFenceState::Ready);
  EXPECT_THROW(binding.seal(), std::logic_error);
  EXPECT_FALSE(binding.failure().has_value());
}

/**
 * @brief Proves the frozen plan preserves signed coordinates and every rich
 *        ordinary-image interpretation record.
 * @throws Validation, overflow, length, or allocation exceptions from plan,
 *         binding, grant, Value, and ImageView construction unchanged.
 * @note The test writes only after all logical and physical facts have been
 *       frozen, then verifies seal publishes those exact facts once.
 */
TEST(HostOutputAuthorization, RetainsSignedRichImagePlanExactly) {
  DenseTensorDescriptor descriptor{{2U, 3U, 2U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  image.data_window = ImageBounds{-7, 11, -4, 13};
  image.display_window = ImageBounds{-8, 10, -3, 14};
  image.channel_schema = ChannelSchema{
      {{ChannelId{11U}, "left"}, {ChannelId{12U}, "right"}},
      {{ChannelGroupId{20U}, "pair", {ChannelId{11U}, ChannelId{12U}}}}};
  SampleDomainFacet sample_domain;
  sample_domain.encoding.kind = SampleEncodingKind::Normalized;
  sample_domain.default_domain = {SampleDomainKind::Normalized, 0.0, 1.0};
  sample_domain.per_channel = {
      {ChannelId{11U}, {SampleDomainKind::Legal, 16.0, 235.0}}};
  image.sample_domain = std::move(sample_domain);
  image.color =
      ColorFacet{1U, ChannelGroupId{20U}, ColorTransferFunction::SceneLinear,
                 ColorPrimaries::Rec709};
  StridedLayout layout{{8, 2, 1}};

  const DenseTensorDescriptor expected_descriptor = descriptor;
  const ImageFacet expected_image = image;
  const StridedLayout expected_layout = layout;
  const RegionSet expected_region =
      RegionSet::from_image_rect({image_region_domain(), -7, -4, 11, 13});
  DenseImageOutputPlan plan = DenseImageOutputPlan::create(
      "image", std::move(descriptor), std::move(image), std::move(layout), 14U,
      64U);

  EXPECT_EQ(plan.descriptor(), expected_descriptor);
  EXPECT_EQ(plan.image_facet(), expected_image);
  EXPECT_EQ(plan.layout(), expected_layout);
  EXPECT_EQ(plan.region(), expected_region);
  EXPECT_EQ(plan.storage_size(), 14U);

  HostOutputBinding binding = HostOutputBinding::allocate(std::move(plan));
  HostOutputWriteGrant grant = binding.grant_whole();
  std::memset(grant.data(0U), 0x2A, grant.span(0U).byte_size);
  grant.retire_success();
  const Value published = binding.seal();

  EXPECT_EQ(published.dense_tensor_descriptor(), expected_descriptor);
  ASSERT_TRUE(published.image_facet().has_value());
  EXPECT_EQ(*published.image_facet(), expected_image);
  EXPECT_EQ(published.strided_layout(), expected_layout);
  const ImageView view(published);
  EXPECT_EQ(view.image_facet().data_window, (ImageBounds{-7, 11, -4, 13}));
  EXPECT_EQ(*view.channel_data_at(-7, 11, 0U), std::byte{0x2A});
}

TEST(HostOutputAuthorization, ConcurrentDisjointTilesWriteOneValue) {
  HostOutputBinding binding =
      HostOutputBinding::allocate(make_plan(8U, 4U, 1U));
  HostOutputWriteGrant left =
      binding.grant_tile({image_region_domain(), 0, 4, 0, 4});
  HostOutputWriteGrant right =
      binding.grant_tile({image_region_domain(), 4, 8, 0, 4});
  ASSERT_EQ(left.span_count(), 4U);
  ASSERT_EQ(right.span_count(), 4U);

  std::atomic<bool> succeeded{true};
  std::thread left_writer(write_and_retire, std::move(left), std::byte{0x11},
                          &succeeded);
  std::thread right_writer(write_and_retire, std::move(right), std::byte{0x22},
                           &succeeded);
  left_writer.join();
  right_writer.join();
  ASSERT_TRUE(succeeded.load(std::memory_order_acquire));

  const Value published = binding.seal();
  ImageView view(published);
  for (std::size_t y = 0U; y < 4U; ++y) {
    for (std::size_t x = 0U; x < 8U; ++x) {
      EXPECT_EQ(*view.channel_data(x, y, 0U),
                x < 4U ? std::byte{0x11} : std::byte{0x22});
    }
  }
}

TEST(HostOutputAuthorization, OverlapFailsBeforeSecondPointerAndRevokesFirst) {
  HostOutputBinding binding =
      HostOutputBinding::allocate(make_plan(8U, 4U, 1U));
  HostOutputWriteGrant first =
      binding.grant_tile({image_region_domain(), 0, 5, 0, 4});
  EXPECT_THROW(binding.grant_tile({image_region_domain(), 4, 8, 0, 4}),
               std::logic_error);
  EXPECT_FALSE(first.active());
  EXPECT_THROW(first.data(0U), std::logic_error);
  EXPECT_TRUE(binding.failure().has_value());
  EXPECT_THROW(binding.seal(), std::logic_error);
}

TEST(HostOutputAuthorization, RejectsBoundsAlignmentAndOverflowBeforeWrites) {
  EXPECT_THROW(make_plan(8U, 4U, 1U, 3U), std::invalid_argument);

  DenseTensorDescriptor overflowing{
      {2U, static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()),
       4U},
      ElementSemantics::UnsignedInteger,
      StorageEncoding{8U}};
  ImageFacet overflowing_image =
      make_zero_origin_image_facet(overflowing, 1U, 0U, 2U);
  StridedLayout overflowing_layout{{4, 4, 1}};
  EXPECT_THROW(DenseImageOutputPlan::create(
                   "image", std::move(overflowing),
                   std::move(overflowing_image), std::move(overflowing_layout),
                   std::numeric_limits<std::size_t>::max(), 64U),
               std::overflow_error);

  HostOutputBinding out_of_bounds =
      HostOutputBinding::allocate(make_plan(8U, 4U, 1U));
  EXPECT_THROW(out_of_bounds.grant_tile({image_region_domain(), -1, 2, 0, 1}),
               std::invalid_argument);
  EXPECT_TRUE(out_of_bounds.failure().has_value());

  HostOutputBinding unaligned =
      HostOutputBinding::allocate(make_plan(8U, 4U, 1U));
  EXPECT_THROW(unaligned.grant_tile({image_region_domain(), 1, 3, 0, 1}, 8U),
               std::invalid_argument);
  EXPECT_TRUE(unaligned.failure().has_value());
}

TEST(HostOutputAuthorization, ActiveGrantMakesSealPermanentlyFailClosed) {
  HostOutputBinding binding =
      HostOutputBinding::allocate(make_plan(4U, 2U, 1U));
  HostOutputWriteGrant grant = binding.grant_whole();
  EXPECT_THROW(binding.seal(), std::logic_error);
  EXPECT_FALSE(grant.active());
  EXPECT_TRUE(binding.failure().has_value());
  EXPECT_THROW(binding.seal(), std::logic_error);
}

TEST(HostOutputAuthorization, DuplicateRetirementPoisonsUnpublishedBinding) {
  HostOutputBinding binding =
      HostOutputBinding::allocate(make_plan(4U, 2U, 1U));
  HostOutputWriteGrant grant = binding.grant_whole();
  grant.retire_success();
  EXPECT_THROW(grant.retire_success(), std::logic_error);
  EXPECT_TRUE(binding.failure().has_value());
  EXPECT_THROW(binding.seal(), std::logic_error);
}

TEST(HostOutputAuthorization, OmittedRetirementPoisonsUnpublishedBinding) {
  HostOutputBinding binding =
      HostOutputBinding::allocate(make_plan(4U, 2U, 1U));
  {
    HostOutputWriteGrant grant = binding.grant_whole();
    ASSERT_TRUE(grant.active());
  }
  EXPECT_TRUE(binding.failure().has_value());
  EXPECT_THROW(binding.seal(), std::logic_error);
}

TEST(HostOutputAuthorization, CancellationAndProducerFailureAreSticky) {
  HostOutputBinding cancelled =
      HostOutputBinding::allocate(make_plan(4U, 2U, 1U));
  HostOutputWriteGrant cancelled_grant = cancelled.grant_whole();
  std::memset(cancelled_grant.data(0U), 0x5A,
              cancelled_grant.span(0U).byte_size);
  cancelled.cancel("Run cancellation won before publication.");
  EXPECT_FALSE(cancelled_grant.active());
  EXPECT_EQ(
      cancelled.failure(),
      std::optional<std::string>("Run cancellation won before publication."));
  EXPECT_THROW(cancelled.seal(), std::logic_error);

  HostOutputBinding failed = HostOutputBinding::allocate(make_plan(4U, 2U, 1U));
  HostOutputWriteGrant failed_grant = failed.grant_whole();
  failed_grant.retire_failure("Operation callback threw.");
  failed.cancel("Later cancellation cannot replace the first failure.");
  EXPECT_EQ(failed.failure(),
            std::optional<std::string>("Operation callback threw."));
  EXPECT_THROW(failed.seal(), std::logic_error);
}

/**
 * @brief Proves one successful tile cannot mask a later sibling failure.
 * @return Nothing; GoogleTest reports any publication or sticky-error defect.
 * @throws Validation, allocation, grant, and retirement exceptions unchanged.
 * @note Both disjoint grants write before retirement. Failure of the second
 * grant preserves the first diagnostic and forbids publication of the partial
 * allocation even though the first grant already retired successfully.
 */
TEST(HostOutputAuthorization, SuccessfulTileCannotMaskLaterSiblingFailure) {
  HostOutputBinding binding =
      HostOutputBinding::allocate(make_plan(8U, 2U, 1U));
  HostOutputWriteGrant left =
      binding.grant_tile({image_region_domain(), 0, 4, 0, 2});
  HostOutputWriteGrant right =
      binding.grant_tile({image_region_domain(), 4, 8, 0, 2});
  for (std::size_t index = 0U; index < left.span_count(); ++index) {
    std::memset(left.data(index), 0x11, left.span(index).byte_size);
    std::memset(right.data(index), 0x22, right.span(index).byte_size);
  }
  left.retire_success();
  right.retire_failure("Sibling tile failed after another retired.");

  EXPECT_EQ(
      binding.failure(),
      std::optional<std::string>("Sibling tile failed after another retired."));
  EXPECT_THROW(binding.seal(), std::logic_error);
}

}  // namespace
}  // namespace ps
