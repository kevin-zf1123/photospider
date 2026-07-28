#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "compute/compute_cache_policy.hpp"
#include "compute/compute_result_committer.hpp"
#include "compute/dirty_node_executor.hpp"
#include "compute/dirty_region_planner.hpp"
#include "compute/dirty_write_buffers.hpp"
#include "compute/node_executor.hpp"
#include "core/cpu_dense_image_operation.hpp"
#include "core/ops.hpp"
#include "core/value_image_adapter.hpp"
#include "graph/graph_cache_service.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"
#include "graph/node.hpp"  // NOLINT(build/include_subdir)
#include "graph/roi_propagation_service.hpp"
#include "photospider/core/compute_intent.hpp"
#include "photospider/core/graph_error.hpp"
#include "photospider/core/image_buffer.hpp"
#include "photospider/data/image_view.hpp"
#include "runtime/graph_event_service.hpp"
#include "support/fake_cache_metadata_codec.hpp"
#include "support/fake_image_artifact_codec.hpp"

namespace ps {
namespace {

static_assert(std::is_nothrow_copy_constructible_v<DenseTensorView>);
static_assert(std::is_nothrow_copy_assignable_v<DenseTensorView>);
static_assert(std::is_nothrow_move_constructible_v<DenseTensorView>);
static_assert(std::is_nothrow_move_assignable_v<DenseTensorView>);
static_assert(std::is_nothrow_copy_constructible_v<ImageView>);
static_assert(std::is_nothrow_copy_assignable_v<ImageView>);
static_assert(std::is_nothrow_move_constructible_v<ImageView>);
static_assert(std::is_nothrow_move_assignable_v<ImageView>);
static_assert(std::is_nothrow_copy_constructible_v<BufferHandle>);
static_assert(std::is_nothrow_copy_assignable_v<BufferHandle>);
static_assert(!std::is_copy_constructible_v<WriteLease>);
static_assert(!std::is_copy_assignable_v<WriteLease>);
static_assert(std::is_nothrow_move_constructible_v<WriteLease>);
static_assert(std::is_nothrow_move_assignable_v<WriteLease>);
static_assert(!std::is_copy_constructible_v<ValueBuilder>);
static_assert(std::is_nothrow_move_constructible_v<ValueBuilder>);

/**
 * @brief Creates one valid padded unsigned-8 HWC Value for test inspection.
 *
 * @param width Positive image width.
 * @param height Positive image height.
 * @param channels Positive channel count.
 * @param row_stride Positive row stride at least width times channels.
 * @return Immutable Value whose active bytes increase from one.
 * @throws std::invalid_argument from Value validation for invalid arguments.
 * @throws std::bad_alloc when shape, layout, or storage allocation fails.
 * @note Inter-row padding is initialized to 0xA5 and is never an active
 *       element.
 */
Value make_unsigned8_value(std::size_t width, std::size_t height,
                           std::size_t channels, std::size_t row_stride) {
  const std::size_t row_bytes = width * channels;
  const std::size_t storage_size = (height - 1U) * row_stride + row_bytes;
  std::vector<std::byte> storage(storage_size, std::byte{0xA5});
  std::uint8_t next = 1U;
  for (std::size_t y = 0U; y < height; ++y) {
    for (std::size_t x = 0U; x < width; ++x) {
      for (std::size_t channel = 0U; channel < channels; ++channel) {
        storage[y * row_stride + x * channels + channel] = std::byte{next++};
      }
    }
  }

  DenseTensorDescriptor descriptor{{height, width, channels},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image;
  image.x_axis = 1U;
  image.y_axis = 0U;
  image.channel_axis = 2U;
  StridedLayout layout{{static_cast<std::ptrdiff_t>(row_stride),
                        static_cast<std::ptrdiff_t>(channels), 1}};
  return Value::from_cpu_dense_tensor(std::move(descriptor), image,
                                      std::move(layout), std::move(storage));
}

/**
 * @brief Creates one padded rank-four unsigned-8 image Value.
 *
 * @param width Positive x-axis extent.
 * @param height Positive y-axis extent.
 * @param channels Positive channel-axis extent.
 * @param row_stride Positive padded y-axis stride.
 * @param first First active byte before row-major incrementing.
 * @return Immutable Value with shape `[1,height,width,channels]`.
 * @throws std::invalid_argument from Value validation for invalid arguments.
 * @throws std::bad_alloc when descriptor, layout, or storage allocation fails.
 * @note Axis zero is an explicit singleton non-image dimension. Active bytes
 *       increase from one; padding is initialized to 0xA5.
 */
Value make_unsigned8_rank4_value(std::size_t width, std::size_t height,
                                 std::size_t channels, std::size_t row_stride,
                                 std::uint8_t first = 1U) {
  const std::size_t row_bytes = width * channels;
  const std::size_t storage_size = (height - 1U) * row_stride + row_bytes;
  std::vector<std::byte> storage(storage_size, std::byte{0xA5});
  std::uint8_t next = first;
  for (std::size_t y = 0U; y < height; ++y) {
    for (std::size_t x = 0U; x < width; ++x) {
      for (std::size_t channel = 0U; channel < channels; ++channel) {
        storage[y * row_stride + x * channels + channel] = std::byte{next++};
      }
    }
  }

  DenseTensorDescriptor descriptor{{1U, height, width, channels},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image;
  image.x_axis = 2U;
  image.y_axis = 1U;
  image.channel_axis = 3U;
  StridedLayout layout{{1, static_cast<std::ptrdiff_t>(row_stride),
                        static_cast<std::ptrdiff_t>(channels), 1}};
  return Value::from_cpu_dense_tensor(std::move(descriptor), image,
                                      std::move(layout), std::move(storage));
}

/**
 * @brief Returns the complete validity Region for one rank-four test output.
 *
 * @return Exact `[1,3,4,3]` dense tensor Region.
 * @throws std::bad_alloc when Region storage cannot allocate.
 * @note All rank-four fixtures in this translation unit use this shape.
 */
RegionSet full_rank4_region() {
  return RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 3U}, {0U, 4U}, {0U, 3U}}});
}

/**
 * @brief Fills active bytes of a current ImageBuffer with a known sequence.
 *
 * @param buffer Valid writable unsigned-8 CPU buffer.
 * @return Flat active byte sequence in row-major interleaved order.
 * @throws std::bad_alloc when expected-byte storage allocation fails.
 * @note Every allocation byte is first set to 0xA5 so padding differs from all
 *       expected inverted active bytes.
 */
std::vector<std::uint8_t> fill_unsigned8_image(ImageBuffer* buffer) {
  std::memset(buffer->data.get(), 0xA5,
              buffer->step * static_cast<std::size_t>(buffer->height));
  std::vector<std::uint8_t> expected;
  expected.reserve(static_cast<std::size_t>(buffer->width) *
                   static_cast<std::size_t>(buffer->height) *
                   static_cast<std::size_t>(buffer->channels));
  std::uint8_t next = 0U;
  auto* base = static_cast<std::byte*>(buffer->data.get());
  for (int y = 0; y < buffer->height; ++y) {
    for (int x = 0; x < buffer->width; ++x) {
      for (int channel = 0; channel < buffer->channels; ++channel) {
        base[static_cast<std::size_t>(y) * buffer->step +
             static_cast<std::size_t>(x * buffer->channels + channel)] =
            std::byte{next};
        expected.push_back(next);
        next = static_cast<std::uint8_t>(next + 17U);
      }
    }
  }
  return expected;
}

/**
 * @brief Reads active unsigned-8 bytes from a validated current ImageBuffer.
 *
 * @param buffer Valid CPU UINT8 image.
 * @return Flat active byte sequence in row-major interleaved order.
 * @throws std::bad_alloc when result allocation fails.
 * @note Row padding is skipped through image_buffer_row_data and row_bytes.
 */
std::vector<std::uint8_t> read_unsigned8_image(const ImageBuffer& buffer) {
  std::vector<std::uint8_t> result;
  const std::size_t row_bytes = image_buffer_row_bytes(buffer);
  result.reserve(row_bytes * static_cast<std::size_t>(buffer.height));
  for (int y = 0; y < buffer.height; ++y) {
    const std::byte* row = image_buffer_row_data(buffer, y);
    for (std::size_t index = 0U; index < row_bytes; ++index) {
      result.push_back(std::to_integer<std::uint8_t>(row[index]));
    }
  }
  return result;
}

/**
 * @brief Owns one unique temporary directory for disk-cache identity tests.
 *
 * @throws std::filesystem::filesystem_error when construction cannot create the
 * directory.
 * @note Destruction performs best-effort removal with error_code so assertion
 * unwinding cannot leak issue-specific test artifacts or throw.
 */
class ScopedTestDirectory final {
 public:
  /**
   * @brief Creates one process-unique directory below the system temp root.
   *
   * @param label Stable diagnostic prefix.
   * @throws std::filesystem::filesystem_error when path discovery or directory
   * creation fails.
   */
  explicit ScopedTestDirectory(const std::string& label) {
    const auto token =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            (label + "-" + std::to_string(token));
    std::filesystem::create_directories(path_);
  }

  /** @brief Copy construction is forbidden for unique directory ownership. */
  ScopedTestDirectory(const ScopedTestDirectory&) = delete;

  /** @brief Copy assignment is forbidden for unique directory ownership. */
  ScopedTestDirectory& operator=(const ScopedTestDirectory&) = delete;

  /**
   * @brief Removes the complete owned temporary directory tree.
   *
   * @throws Nothing.
   */
  ~ScopedTestDirectory() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Returns the owned directory path.
   *
   * @return Borrowed path valid for this object's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Unique temporary directory removed at destruction. */
  std::filesystem::path path_;
};

TEST(CpuDenseTensorImageOperation,
     ValueRejectsMalformedFacetStrideAndEnvelope) {
  DenseTensorDescriptor descriptor{{2U, 3U, 2U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet image;
  image.x_axis = 1U;
  image.y_axis = 0U;
  image.channel_axis = 2U;

  StridedLayout zero_stride{{8, 2, 0}};
  EXPECT_THROW(
      Value::from_cpu_dense_tensor(descriptor, image, zero_stride,
                                   std::vector<std::byte>(14U, std::byte{0})),
      std::invalid_argument);

  StridedLayout padded{{8, 2, 1}};
  EXPECT_THROW(
      Value::from_cpu_dense_tensor(descriptor, image, padded,
                                   std::vector<std::byte>(13U, std::byte{0})),
      std::invalid_argument);

  ImageFacet duplicate_axis = image;
  duplicate_axis.channel_axis = duplicate_axis.x_axis;
  EXPECT_THROW(
      Value::from_cpu_dense_tensor(descriptor, duplicate_axis, padded,
                                   std::vector<std::byte>(14U, std::byte{0})),
      std::invalid_argument);

  const DenseTensorDescriptor unsigned16_line{{2U},
                                              ElementSemantics::UnsignedInteger,
                                              StorageEncoding{16U}};
  EXPECT_THROW(ValueBuilder::allocate_cpu_dense_tensor(
                   unsigned16_line, std::nullopt, StridedLayout{{1}}, 3U),
               std::invalid_argument);

  const DenseTensorDescriptor cross_axis_collision{
      {2U, 3U},
      ElementSemantics::UnsignedInteger,
      StorageEncoding{8U}};
  EXPECT_THROW(
      ValueBuilder::allocate_cpu_dense_tensor(
          cross_axis_collision, std::nullopt, StridedLayout{{2, 1}}, 5U),
      std::invalid_argument);

  const DenseTensorDescriptor unsigned16_matrix{
      {2U, 2U},
      ElementSemantics::UnsignedInteger,
      StorageEncoding{16U}};
  ValueBuilder padded_builder = ValueBuilder::allocate_cpu_dense_tensor(
      unsigned16_matrix, std::nullopt, StridedLayout{{8, 2}}, 12U);
  EXPECT_TRUE(padded_builder.seal().valid());

  const DenseTensorDescriptor transposed_descriptor{
      {2U, 3U},
      ElementSemantics::UnsignedInteger,
      StorageEncoding{16U}};
  ValueBuilder transposed_builder = ValueBuilder::allocate_cpu_dense_tensor(
      transposed_descriptor, std::nullopt, StridedLayout{{2, 4}}, 12U);
  EXPECT_TRUE(transposed_builder.seal().valid());

  const DenseTensorDescriptor singleton_axis{{1U, 3U},
                                             ElementSemantics::UnsignedInteger,
                                             StorageEncoding{16U}};
  ValueBuilder singleton_builder = ValueBuilder::allocate_cpu_dense_tensor(
      singleton_axis, std::nullopt, StridedLayout{{1, 2}}, 6U);
  EXPECT_TRUE(singleton_builder.seal().valid());

  const DenseTensorDescriptor zero_extent{{0U, 3U},
                                          ElementSemantics::UnsignedInteger,
                                          StorageEncoding{8U}};
  EXPECT_THROW(ValueBuilder::allocate_cpu_dense_tensor(
                   zero_extent, std::nullopt, StridedLayout{{3, 1}}, 1U),
               std::invalid_argument);

  const DenseTensorDescriptor overflow_boundary{
      {3U},
      ElementSemantics::UnsignedInteger,
      StorageEncoding{16U}};
  EXPECT_THROW(ValueBuilder::allocate_cpu_dense_tensor(
                   overflow_boundary, std::nullopt,
                   StridedLayout{{
                       std::numeric_limits<std::ptrdiff_t>::max(),
                   }},
                   1U),
               std::overflow_error);
}

TEST(CpuDenseTensorImageOperation,
     BuilderScopesWriteAuthorityAndReadLeaseLifetime) {
  DenseTensorDescriptor descriptor{{4U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      descriptor, std::nullopt, StridedLayout{{1}}, 4U);
  {
    WriteLease write = builder.acquire_write();
    ASSERT_TRUE(write.valid());
    ASSERT_EQ(write.size(), 4U);
    write.data()[0] = std::byte{10U};
    write.data()[1] = std::byte{20U};
    write.data()[2] = std::byte{30U};
    write.data()[3] = std::byte{40U};
    EXPECT_THROW(builder.acquire_write(), std::logic_error);
    EXPECT_THROW(builder.seal(), std::logic_error);
  }

  Value value = builder.seal();
  ASSERT_TRUE(builder.sealed());
  ASSERT_TRUE(value.valid());
  EXPECT_TRUE(value.allocation_identity().valid());
  EXPECT_TRUE(value.revision_id().valid());
  EXPECT_THROW(builder.acquire_write(), std::logic_error);
  EXPECT_THROW(builder.seal(), std::logic_error);

  const BufferHandle subrange = value.buffer_handle().subrange(1U, 2U);
  EXPECT_EQ(subrange.allocation_identity(), value.allocation_identity());
  EXPECT_EQ(subrange.allocation_offset(),
            value.buffer_handle().allocation_offset() + 1U);
  EXPECT_EQ(subrange.size(), 2U);
  EXPECT_THROW(value.buffer_handle().subrange(4U, 1U), std::out_of_range);
  EXPECT_THROW(value.buffer_handle().subrange(0U, 0U), std::invalid_argument);

  ReadLease retained = value.buffer_handle().acquire_read();
  const AllocationIdentity allocation = value.allocation_identity();
  value = Value{};
  EXPECT_TRUE(retained.valid());
  EXPECT_EQ(retained.allocation_identity(), allocation);
  EXPECT_EQ(std::to_integer<std::uint8_t>(retained.data()[0]), 10U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(retained.data()[3]), 40U);
}

/**
 * @brief Proves AllocationIdentity validity is token state, not liveness.
 */
TEST(CpuDenseTensorImageOperation,
     AllocationIdentityValidityDoesNotQueryAllocationLiveness) {
  AllocationIdentity issued;
  {
    const Value value = make_unsigned8_value(1U, 1U, 1U, 1U);
    issued = value.allocation_identity();
    ASSERT_TRUE(issued.valid());
    ASSERT_NE(issued.value(), 0U);
  }

  EXPECT_TRUE(issued.valid());
  EXPECT_NE(issued.value(), 0U);
}

TEST(CpuDenseTensorImageOperation,
     ImmutableSignedOffsetViewsShareAllocationAndMintRevisions) {
  DenseTensorDescriptor descriptor{{4U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  const Value base = Value::from_cpu_dense_tensor(
      descriptor, std::nullopt, StridedLayout{{1}},
      {std::byte{1U}, std::byte{2U}, std::byte{3U}, std::byte{4U}});

  StridedLayout reverse_layout{{-1}, 3U};
  const Value reverse = Value::from_cpu_dense_tensor(
      descriptor, std::nullopt, reverse_layout, base.buffer_handle());
  EXPECT_EQ(reverse.allocation_identity(), base.allocation_identity());
  EXPECT_NE(reverse.revision_id(), base.revision_id());
  DenseTensorView reverse_view(reverse);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*reverse_view.element_data({0U})),
            4U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*reverse_view.element_data({3U})),
            1U);

  StridedLayout broadcast_layout{{0}, 1U};
  const Value broadcast = Value::from_cpu_dense_tensor(
      descriptor, std::nullopt, broadcast_layout, base.buffer_handle());
  EXPECT_EQ(broadcast.allocation_identity(), base.allocation_identity());
  EXPECT_NE(broadcast.revision_id(), reverse.revision_id());
  DenseTensorView broadcast_view(broadcast);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*broadcast_view.element_data({0U})),
            2U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*broadcast_view.element_data({3U})),
            2U);

  const BufferHandle middle = base.buffer_handle().subrange(1U, 2U);
  const Value middle_view = Value::from_cpu_dense_tensor(
      DenseTensorDescriptor{{2U},
                            ElementSemantics::UnsignedInteger,
                            StorageEncoding{8U}},
      std::nullopt, StridedLayout{{1}}, middle);
  EXPECT_EQ(middle_view.allocation_identity(), base.allocation_identity());
  EXPECT_NE(middle_view.revision_id(), base.revision_id());
  DenseTensorView middle_tensor(middle_view);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*middle_tensor.element_data({0U})),
            2U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*middle_tensor.element_data({1U})),
            3U);

  EXPECT_THROW(Value::from_cpu_dense_tensor(descriptor, std::nullopt,
                                            StridedLayout{{-1}, 2U},
                                            base.buffer_handle()),
               std::out_of_range);
  EXPECT_THROW(Value::from_cpu_dense_tensor(descriptor, std::nullopt,
                                            StridedLayout{{2}, 0U},
                                            base.buffer_handle()),
               std::out_of_range);
}

TEST(CpuDenseTensorImageOperation,
     ValueCopiesShareBytesAndViewsRetainLifetime) {
  Value original = make_unsigned8_value(3U, 2U, 2U, 8U);
  Value shared = original;
  EXPECT_EQ(original.allocation_identity(), shared.allocation_identity());
  EXPECT_EQ(original.revision_id(), shared.revision_id());
  const ReadLease original_read = original.buffer_handle().acquire_read();
  const ReadLease shared_read = shared.buffer_handle().acquire_read();
  EXPECT_EQ(original_read.data(), shared_read.data());
  EXPECT_EQ(original.storage_size(), 14U);

  ImageView image = [&original]() {
    ImageView retaining(original);
    return retaining;
  }();
  original = Value{};
  shared = Value{};

  EXPECT_EQ(image.width(), 3U);
  EXPECT_EQ(image.height(), 2U);
  EXPECT_EQ(image.channels(), 2U);
  EXPECT_EQ(image.row_stride(), 8);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*image.channel_data(2U, 1U, 1U)),
            12U);
  EXPECT_THROW(image.channel_data(3U, 0U, 0U), std::out_of_range);
}

TEST(CpuDenseTensorImageOperation,
     DenseTensorViewMovesPreserveSourceAndReplaceDestination) {
  DenseTensorView source(make_unsigned8_value(3U, 2U, 2U, 8U));
  const Value expected = source.value();
  const ReadLease expected_read = expected.buffer_handle().acquire_read();
  const std::byte* const expected_data =
      expected_read.data() + expected.strided_layout().byte_offset;
  const std::vector<std::size_t> coordinate{1U, 2U, 1U};

  DenseTensorView constructed(std::move(source));

  ASSERT_TRUE(source.value().valid());
  EXPECT_EQ(source.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(source.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(source.storage_size(), expected.storage_size());
  EXPECT_EQ(source.data(), expected_data);
  EXPECT_EQ(source.element_data(coordinate), expected_data + 13U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*source.element_data(coordinate)),
            12U);

  ASSERT_TRUE(constructed.value().valid());
  EXPECT_EQ(constructed.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(constructed.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(constructed.storage_size(), expected.storage_size());
  EXPECT_EQ(constructed.data(), expected_data);
  EXPECT_EQ(constructed.element_data(coordinate), expected_data + 13U);
  EXPECT_EQ(
      std::to_integer<std::uint8_t>(*constructed.element_data(coordinate)),
      12U);

  DenseTensorView assigned(make_unsigned8_value(1U, 1U, 1U, 1U));
  const Value displaced = assigned.value();
  ASSERT_NE(displaced.allocation_identity(), expected.allocation_identity());

  assigned = std::move(constructed);

  ASSERT_TRUE(constructed.value().valid());
  EXPECT_EQ(constructed.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(constructed.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(constructed.storage_size(), expected.storage_size());
  EXPECT_EQ(constructed.data(), expected_data);
  EXPECT_EQ(constructed.element_data(coordinate), expected_data + 13U);
  EXPECT_EQ(
      std::to_integer<std::uint8_t>(*constructed.element_data(coordinate)),
      12U);

  ASSERT_TRUE(assigned.value().valid());
  EXPECT_EQ(assigned.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(assigned.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(assigned.storage_size(), expected.storage_size());
  EXPECT_EQ(assigned.data(), expected_data);
  EXPECT_EQ(assigned.element_data(coordinate), expected_data + 13U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*assigned.element_data(coordinate)),
            12U);
  EXPECT_TRUE(displaced.valid());
  EXPECT_EQ(displaced.storage_size(), 1U);
  EXPECT_NE(assigned.value().allocation_identity(),
            displaced.allocation_identity());
}

TEST(CpuDenseTensorImageOperation,
     ImageViewMovesPreserveSourceAndReplaceDestination) {
  ImageView source(make_unsigned8_value(3U, 2U, 2U, 8U));
  const Value expected = source.value();
  const ReadLease expected_read = expected.buffer_handle().acquire_read();
  const std::byte* const expected_data =
      expected_read.data() + expected.strided_layout().byte_offset;
  ASSERT_TRUE(expected.image_facet().has_value());

  ImageView constructed(std::move(source));

  ASSERT_TRUE(source.value().valid());
  EXPECT_EQ(source.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(source.image_facet(), *expected.image_facet());
  EXPECT_EQ(source.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(source.value().storage_size(), expected.storage_size());
  EXPECT_EQ(source.value().allocation_identity(),
            expected.allocation_identity());
  EXPECT_EQ(source.width(), 3U);
  EXPECT_EQ(source.height(), 2U);
  EXPECT_EQ(source.channels(), 2U);
  EXPECT_EQ(source.element_bytes(), 1U);
  EXPECT_EQ(source.row_stride(), 8);
  EXPECT_EQ(source.channel_data(2U, 1U, 1U), expected_data + 13U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*source.channel_data(2U, 1U, 1U)),
            12U);

  ASSERT_TRUE(constructed.value().valid());
  EXPECT_EQ(constructed.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(constructed.image_facet(), *expected.image_facet());
  EXPECT_EQ(constructed.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(constructed.value().storage_size(), expected.storage_size());
  EXPECT_EQ(constructed.value().revision_id(), expected.revision_id());
  EXPECT_EQ(constructed.width(), 3U);
  EXPECT_EQ(constructed.height(), 2U);
  EXPECT_EQ(constructed.channels(), 2U);
  EXPECT_EQ(constructed.element_bytes(), 1U);
  EXPECT_EQ(constructed.row_stride(), 8);
  EXPECT_EQ(constructed.channel_data(2U, 1U, 1U), expected_data + 13U);
  EXPECT_EQ(
      std::to_integer<std::uint8_t>(*constructed.channel_data(2U, 1U, 1U)),
      12U);

  ImageView assigned(make_unsigned8_value(1U, 1U, 1U, 1U));
  const Value displaced = assigned.value();
  ASSERT_NE(displaced.allocation_identity(), expected.allocation_identity());

  assigned = std::move(constructed);

  ASSERT_TRUE(constructed.value().valid());
  EXPECT_EQ(constructed.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(constructed.image_facet(), *expected.image_facet());
  EXPECT_EQ(constructed.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(constructed.value().storage_size(), expected.storage_size());
  EXPECT_EQ(constructed.value().revision_id(), expected.revision_id());
  EXPECT_EQ(constructed.width(), 3U);
  EXPECT_EQ(constructed.height(), 2U);
  EXPECT_EQ(constructed.channels(), 2U);
  EXPECT_EQ(constructed.element_bytes(), 1U);
  EXPECT_EQ(constructed.row_stride(), 8);
  EXPECT_EQ(constructed.channel_data(2U, 1U, 1U), expected_data + 13U);
  EXPECT_EQ(
      std::to_integer<std::uint8_t>(*constructed.channel_data(2U, 1U, 1U)),
      12U);

  ASSERT_TRUE(assigned.value().valid());
  EXPECT_EQ(assigned.descriptor(), expected.dense_tensor_descriptor());
  EXPECT_EQ(assigned.image_facet(), *expected.image_facet());
  EXPECT_EQ(assigned.layout().byte_strides,
            expected.strided_layout().byte_strides);
  EXPECT_EQ(assigned.value().storage_size(), expected.storage_size());
  EXPECT_EQ(assigned.value().revision_id(), expected.revision_id());
  EXPECT_EQ(assigned.width(), 3U);
  EXPECT_EQ(assigned.height(), 2U);
  EXPECT_EQ(assigned.channels(), 2U);
  EXPECT_EQ(assigned.element_bytes(), 1U);
  EXPECT_EQ(assigned.row_stride(), 8);
  EXPECT_EQ(assigned.channel_data(2U, 1U, 1U), expected_data + 13U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*assigned.channel_data(2U, 1U, 1U)),
            12U);
  EXPECT_TRUE(displaced.valid());
  EXPECT_EQ(displaced.storage_size(), 1U);
  EXPECT_NE(assigned.value().allocation_identity(),
            displaced.allocation_identity());
}

TEST(CpuDenseTensorImageOperation,
     ValueDeepCopiesLvaluePayloadShapeAndStrides) {
  DenseTensorDescriptor descriptor{{2U, 3U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  StridedLayout layout{{4, 1}};
  std::vector<std::byte> storage{std::byte{1U}, std::byte{2U}, std::byte{3U},
                                 std::byte{0U}, std::byte{4U}, std::byte{5U},
                                 std::byte{6U}};

  const Value value =
      Value::from_cpu_dense_tensor(descriptor, std::nullopt, layout, storage);
  ASSERT_NE(value.dense_tensor_descriptor().shape.data(),
            descriptor.shape.data());
  ASSERT_NE(value.strided_layout().byte_strides.data(),
            layout.byte_strides.data());
  const ReadLease read = value.buffer_handle().acquire_read();
  ASSERT_NE(read.data(), storage.data());

  descriptor.shape[0] = 9U;
  layout.byte_strides[0] = 9;
  storage[0] = std::byte{9U};

  EXPECT_EQ(value.dense_tensor_descriptor().shape,
            (std::vector<std::size_t>{2U, 3U}));
  EXPECT_EQ(value.strided_layout().byte_strides,
            (std::vector<std::ptrdiff_t>{4, 1}));
  EXPECT_EQ(std::to_integer<std::uint8_t>(read.data()[0]), 1U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(read.data()[6]), 6U);

  NodeOutput tensor_output;
  tensor_output.image_value = value;
  EXPECT_EQ(value_image_adapter::full_node_output_region(tensor_output),
            RegionSet::from_tensor_slice(
                {dense_tensor_region_domain(), {{0U, 2U}, {0U, 3U}}}));
}

/**
 * @brief Proves rvalue construction copies payload instead of adopting it.
 *
 * @note The source address is converted to an integer while its vector still
 *       owns the allocation. The test never reads, writes, or compares a
 *       dangling pointer after the moved parameter is destroyed.
 */
TEST(CpuDenseTensorImageOperation,
     ValueDeepCopiesRvaluePayloadBeforeSourceOwnerRetires) {
  DenseTensorDescriptor descriptor{{2U, 3U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  StridedLayout layout{{4, 1}};
  std::vector<std::byte> storage{std::byte{1U}, std::byte{2U}, std::byte{3U},
                                 std::byte{0U}, std::byte{4U}, std::byte{5U},
                                 std::byte{6U}};
  const std::uintptr_t source_storage_address =
      reinterpret_cast<std::uintptr_t>(storage.data());

  const Value value =
      Value::from_cpu_dense_tensor(std::move(descriptor), std::nullopt,
                                   std::move(layout), std::move(storage));
  const ReadLease read = value.buffer_handle().acquire_read();
  EXPECT_NE(reinterpret_cast<std::uintptr_t>(read.data()),
            source_storage_address);

  descriptor.shape = {9U};
  layout.byte_strides = {9};
  storage.assign(9U, std::byte{9U});

  EXPECT_EQ(value.dense_tensor_descriptor().shape,
            (std::vector<std::size_t>{2U, 3U}));
  EXPECT_EQ(value.strided_layout().byte_strides,
            (std::vector<std::ptrdiff_t>{4, 1}));
  EXPECT_EQ(std::to_integer<std::uint8_t>(read.data()[0]), 1U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(read.data()[6]), 6U);
}

TEST(CpuDenseTensorImageOperation,
     FormalHpCachePreservesAliasesAndResealsDirtyAndReplacementBytes) {
  GraphModel graph("cache/value-identity");
  Node node;
  node.id = 80;
  node.name = "value_identity";
  node.type = "image_process";
  node.subtype = "invert_dense";
  graph.add_node(node);

  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache(image_codec, metadata_codec);
  std::mutex graph_mutex;
  const std::string cache_precision = "int8";
  compute::ComputeResultCommitter committer(cache, graph_mutex,
                                            cache_precision);

  std::vector<std::optional<NodeOutput>> results(1U);
  results[0].emplace();
  results[0]->image_buffer =
      make_aligned_cpu_image_buffer(3, 2, 1, DataType::UINT8);
  (void)fill_unsigned8_image(&results[0]->image_buffer);
  ASSERT_FALSE(results[0]->image_value.valid());
  committer.commit(graph, {80}, results);

  ASSERT_TRUE(graph.node(80).cached_output_high_precision.has_value());
  ASSERT_TRUE(graph.node(80).hp_region.has_value());
  EXPECT_EQ(*graph.node(80).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 3, 0, 2}));
  const Value first = graph.node(80).cached_output_high_precision->image_value;
  ASSERT_TRUE(first.valid());
  const ReadLease first_read = first.buffer_handle().acquire_read();
  const NodeOutput immutable_alias =
      *graph.node(80).cached_output_high_precision;
  EXPECT_EQ(immutable_alias.image_value.allocation_identity(),
            first.allocation_identity());
  EXPECT_EQ(immutable_alias.image_value.revision_id(), first.revision_id());

  compute::HighPrecisionDirtyWriteBuffer dirty;
  NodeOutput& staged = dirty.ensure_output(graph.node(80));
  ASSERT_FALSE(staged.image_value.valid());
  static_cast<std::byte*>(staged.image_buffer.data.get())[0] = std::byte{99U};
  (void)dirty.mark_updated(
      graph.node(80),
      RegionSet::from_image_rect({image_region_domain(), 0, 1, 0, 1}), false,
      0U);
  dirty.commit_to_graph(graph);

  const Value dirty_value =
      graph.node(80).cached_output_high_precision->image_value;
  ASSERT_TRUE(dirty_value.valid());
  ASSERT_TRUE(graph.node(80).hp_region.has_value());
  EXPECT_EQ(*graph.node(80).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 3, 0, 2}));
  EXPECT_NE(dirty_value.allocation_identity(), first.allocation_identity());
  EXPECT_NE(dirty_value.revision_id(), first.revision_id());
  EXPECT_EQ(std::to_integer<std::uint8_t>(first_read.data()[0]), 0U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(
                *ImageView(dirty_value).channel_data(0U, 0U, 0U)),
            99U);

  results[0] = NodeOutput{};
  results[0]->image_buffer =
      make_aligned_cpu_image_buffer(3, 2, 1, DataType::UINT8);
  (void)fill_unsigned8_image(&results[0]->image_buffer);
  committer.commit(graph, {80}, results);

  const Value replacement =
      graph.node(80).cached_output_high_precision->image_value;
  ASSERT_TRUE(replacement.valid());
  EXPECT_NE(replacement.allocation_identity(),
            dirty_value.allocation_identity());
  EXPECT_NE(replacement.revision_id(), dirty_value.revision_id());
  EXPECT_EQ(graph.node(80).hp_version, 3);
  ASSERT_TRUE(graph.node(80).hp_region.has_value());
  EXPECT_EQ(*graph.node(80).hp_region,
            RegionSet::from_image_rect({image_region_domain(), 0, 3, 0, 2}));

  compute::clear_planned_high_precision_caches(graph, graph_mutex, {80});
  EXPECT_FALSE(graph.node(80).cached_output_high_precision.has_value());
  EXPECT_FALSE(graph.node(80).hp_region.has_value());
  EXPECT_EQ(graph.node(80).hp_version, 3);
}

/**
 * @brief Proves request staging publishes TensorSlice validity with fresh
 * immutable Value identities only at commit.
 *
 * @return Nothing; GoogleTest reports premature publication, identity reuse,
 * Region loss, version, or generation failures.
 * @throws Allocation, Value, Region, Graph, or staging exceptions unchanged.
 * @note The buffer disables existing-output seeding, so old full validity must
 * not leak into the fresh rank-general partial publication.
 */
TEST(CpuDenseTensorImageOperation,
     TensorDirtyStagingPublishesFreshIdentityAndExactRegionAtCommit) {
  GraphModel graph("cache/tensor-region-staging");
  Node node;
  node.id = 86;
  node.name = "tensor_region_staging";
  node.type = "image_process";
  node.subtype = "invert_dense";
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->image_value =
      make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  node.hp_version = 5;
  node.hp_region = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 3U}, {0U, 4U}, {0U, 3U}}});
  graph.add_node(node);

  const Value original =
      graph.node(86).cached_output_high_precision->image_value;
  const RegionSet update = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});
  compute::HighPrecisionDirtyWriteBuffer staging(false);
  NodeOutput& staged = staging.ensure_output(graph.node(86));
  staged.image_value = make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  const Value staged_value = staged.image_value;
  (void)staging.mark_updated(graph.node(86), update, true, 91U);

  EXPECT_EQ(
      graph.node(86).cached_output_high_precision->image_value.revision_id(),
      original.revision_id());
  EXPECT_EQ(graph.node(86).hp_version, 5);
  ASSERT_TRUE(graph.node(86).hp_region.has_value());
  EXPECT_FALSE(*graph.node(86).hp_region == update);
  EXPECT_FALSE(graph.dirty_source_hp_commit_generation.count(86));

  staging.commit_to_graph(graph);

  const Value committed =
      graph.node(86).cached_output_high_precision->image_value;
  EXPECT_EQ(committed.allocation_identity(),
            staged_value.allocation_identity());
  EXPECT_EQ(committed.revision_id(), staged_value.revision_id());
  EXPECT_NE(committed.allocation_identity(), original.allocation_identity());
  EXPECT_NE(committed.revision_id(), original.revision_id());
  EXPECT_EQ(graph.node(86).hp_region, update);
  EXPECT_EQ(graph.node(86).hp_version, 6);
  EXPECT_EQ(graph.dirty_source_hp_commit_generation.at(86), 91U);
}

TEST(CpuDenseTensorImageOperation,
     DiskReloadMintsFreshRuntimeIdentitiesWithoutChangingCachePath) {
  ScopedTestDirectory directory("photospider-v3-disk-identity");
  const std::filesystem::path node_directory = directory.path() / "81";
  std::filesystem::create_directories(node_directory);
  const std::filesystem::path artifact = node_directory / "image.fake";
  {
    std::ofstream marker(artifact, std::ios::binary);
    ASSERT_TRUE(marker.good());
    marker.put('x');
  }

  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      [](const std::filesystem::path& path) {
        (void)path;
        ImageBuffer decoded =
            make_aligned_cpu_image_buffer(2, 2, 1, DataType::UINT8);
        (void)fill_unsigned8_image(&decoded);
        return decoded;
      });
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache(image_codec, metadata_codec);

  GraphModel graph("cache/value-disk-reload");
  graph.cache_root = directory.path();
  Node node;
  node.id = 81;
  node.name = "disk_identity";
  node.caches.push_back(CacheEntry{"image", "image.fake"});
  graph.add_node(node);

  NodeOutput first_output;
  ASSERT_TRUE(
      cache.try_load_from_disk_cache_into(graph, graph.node(81), first_output));
  const Value first = first_output.image_value;
  ASSERT_TRUE(first.valid());
  EXPECT_EQ(cache.node_cache_dir(graph, 81), node_directory);

  NodeOutput second_output;
  ASSERT_TRUE(cache.try_load_from_disk_cache_into(graph, graph.node(81),
                                                  second_output));
  const Value second = second_output.image_value;
  ASSERT_TRUE(second.valid());
  EXPECT_NE(second.allocation_identity(), first.allocation_identity());
  EXPECT_NE(second.revision_id(), first.revision_id());
  EXPECT_EQ(cache.node_cache_dir(graph, 81), node_directory);

  const auto calls = image_codec->calls();
  ASSERT_EQ(calls.size(), 2U);
  EXPECT_EQ(calls[0].path, artifact);
  EXPECT_EQ(calls[1].path, artifact);
}

TEST(CpuDenseTensorImageOperation,
     DiskSaveSerializesSealedValueInsteadOfMutableCompatibilitySnapshot) {
  ScopedTestDirectory directory("photospider-v3-disk-authority");
  std::optional<std::uint8_t> encoded_first_byte;
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>(
      testing::FakeImageArtifactCodec::DecodeCallback{},
      [&encoded_first_byte](const std::filesystem::path& path,
                            const ImageBuffer& image,
                            ImageArtifactPrecision precision) {
        (void)path;
        (void)precision;
        encoded_first_byte = read_unsigned8_image(image).front();
      });
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache(image_codec, metadata_codec);

  GraphModel graph("cache/value-disk-save-authority");
  graph.cache_root = directory.path();
  Node node;
  node.id = 82;
  node.name = "disk_authority";
  node.caches.push_back(CacheEntry{"image", "image.fake"});
  NodeOutput output;
  output.image_value = make_unsigned8_value(2U, 2U, 1U, 2U);
  output.image_buffer = make_aligned_cpu_image_buffer(2, 2, 1, DataType::UINT8);
  std::memset(output.image_buffer.data.get(), 77,
              output.image_buffer.step *
                  static_cast<std::size_t>(output.image_buffer.height));
  node.cached_output_high_precision = std::move(output);
  node.hp_region = value_image_adapter::full_node_output_region(
      *node.cached_output_high_precision);
  graph.add_node(std::move(node));

  cache.save_cache_if_configured(graph, graph.node(82), "int8");
  ASSERT_TRUE(encoded_first_byte.has_value());
  EXPECT_EQ(*encoded_first_byte, 1U);
  const auto calls = image_codec->calls();
  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(calls.front().path, directory.path() / "82" / "image.fake");
}

/**
 * @brief Proves partial formal HP state cannot produce or reload a disk cache.
 *
 * @return Nothing; GoogleTest reports stale artifact, codec, or reuse failures.
 * @throws Filesystem, Value, Region, Graph, or allocation exceptions unchanged.
 * @note The configured stale files model an older regionless disk cache. Saving
 * partial memory validity must remove them without invoking either codec.
 */
TEST(CpuDenseTensorImageOperation,
     PartialHpValidityRemovesDiskArtifactsAndCannotBeReused) {
  ScopedTestDirectory directory("photospider-v3-partial-disk");
  const std::filesystem::path node_directory = directory.path() / "83";
  std::filesystem::create_directories(node_directory);
  const std::filesystem::path artifact = node_directory / "image.fake";
  const std::filesystem::path metadata = node_directory / "image.yml";
  {
    std::ofstream image_marker(artifact, std::ios::binary);
    std::ofstream metadata_marker(metadata, std::ios::binary);
    ASSERT_TRUE(image_marker.good());
    ASSERT_TRUE(metadata_marker.good());
    image_marker.put('x');
    metadata_marker.put('y');
  }

  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache(image_codec, metadata_codec);
  GraphModel graph("cache/partial-disk");
  graph.cache_root = directory.path();
  Node node;
  node.id = 83;
  node.name = "partial_disk";
  node.caches.push_back(CacheEntry{"image", "image.fake"});
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->image_value =
      make_unsigned8_value(2U, 2U, 1U, 2U);
  node.hp_region =
      RegionSet::from_image_rect({image_region_domain(), 0, 1, 0, 1});

  cache.save_cache_if_configured(graph, node, "int8");

  EXPECT_FALSE(std::filesystem::exists(artifact));
  EXPECT_FALSE(std::filesystem::exists(metadata));
  EXPECT_FALSE(std::filesystem::exists(node_directory));
  EXPECT_TRUE(image_codec->calls().empty());
  EXPECT_TRUE(metadata_codec->calls().empty());
  EXPECT_FALSE(cache.try_load_from_disk_cache(graph, node));
  EXPECT_TRUE(image_codec->calls().empty());
  EXPECT_TRUE(metadata_codec->calls().empty());
}

TEST(CpuDenseTensorImageOperation,
     DenseInvertInferencePreservesExactLogicalDescriptor) {
  const ops::CpuDenseImageOperation operation =
      ops::make_dense_invert_operation();
  ops::DenseImageDescriptor input;
  input.tensor = DenseTensorDescriptor{{4U, 7U, 3U},
                                       ElementSemantics::UnsignedInteger,
                                       StorageEncoding{8U}};
  input.image.x_axis = 1U;
  input.image.y_axis = 0U;
  input.image.channel_axis = 2U;
  ops::CpuDenseImageConfiguration configuration;

  const ops::DenseImageDescriptor inferred =
      operation.infer(configuration, {input});
  EXPECT_EQ(inferred, input);
}

TEST(CpuDenseTensorImageOperation,
     DenseRunnerConsumesSealedValueAndPublishesExactResultRevision) {
  NodeOutput input;
  input.image_value = make_unsigned8_value(3U, 2U, 2U, 8U);
  ASSERT_EQ(input.image_buffer.width, 0);

  Node node;
  const ops::CpuDenseImageOperation operation =
      ops::make_dense_invert_operation();
  const NodeOutput output =
      ops::execute_cpu_dense_image_operation(node, {&input}, operation);

  ASSERT_TRUE(output.image_value.valid());
  EXPECT_NE(output.image_value.allocation_identity(),
            input.image_value.allocation_identity());
  EXPECT_NE(output.image_value.revision_id(), input.image_value.revision_id());
  const ImageView value_view(output.image_value);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*value_view.channel_data(0U, 0U, 0U)),
            254U);
  EXPECT_EQ(std::to_integer<std::uint8_t>(*value_view.channel_data(2U, 1U, 1U)),
            243U);

  ASSERT_EQ(output.image_buffer.width, 3);
  ASSERT_EQ(output.image_buffer.height, 2);
  ASSERT_EQ(output.image_buffer.channels, 2);
  EXPECT_EQ(read_unsigned8_image(output.image_buffer).front(), 254U);
}

TEST(CpuDenseTensorImageOperation,
     ProductRegistryAndExecutorInvertPaddedMultiChannelInput) {
  ops::register_core_operations();
  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*resolved));
  const MonolithicOpFunc& core_operation =
      std::get<MonolithicOpFunc>(*resolved);
  EXPECT_TRUE(ops::find_core_region_monolithic_operation(
                  "image_process", "invert_dense", core_operation)
                  .has_value());
  const MonolithicOpFunc override_operation =
      [](const Node&, const std::vector<const NodeOutput*>&) {
        return NodeOutput{};
      };
  EXPECT_FALSE(ops::find_core_region_monolithic_operation(
                   "image_process", "invert_dense", override_operation)
                   .has_value());

  NodeOutput input;
  input.image_buffer = make_aligned_cpu_image_buffer(5, 3, 3, DataType::UINT8);
  const std::size_t input_row_bytes =
      image_buffer_row_bytes(input.image_buffer);
  ASSERT_GT(input.image_buffer.step, input_row_bytes);
  const std::vector<std::uint8_t> active_input =
      fill_unsigned8_image(&input.image_buffer);

  GraphModel graph("cache/cpu-dense-tensor-image-operation");
  Node node;
  node.id = 79;
  node.name = "cpu_dense_invert";
  node.type = "image_process";
  node.subtype = "invert_dense";
  NodeOutput output =
      compute::NodeExecutor::execute(graph, node, *resolved, {&input});

  ASSERT_TRUE(output.image_value.valid());
  EXPECT_TRUE(output.image_value.revision_id().valid());
  EXPECT_EQ(output.image_buffer.width, input.image_buffer.width);
  EXPECT_EQ(output.image_buffer.height, input.image_buffer.height);
  EXPECT_EQ(output.image_buffer.channels, input.image_buffer.channels);
  EXPECT_EQ(output.image_buffer.type, DataType::UINT8);
  EXPECT_EQ(output.image_buffer.device, Device::CPU);
  EXPECT_GT(output.image_buffer.step,
            image_buffer_row_bytes(output.image_buffer));
  validate_image_buffer(output.image_buffer);

  const std::vector<std::uint8_t> active_output =
      read_unsigned8_image(output.image_buffer);
  ASSERT_EQ(active_output.size(), active_input.size());
  for (std::size_t index = 0U; index < active_input.size(); ++index) {
    EXPECT_EQ(active_output[index],
              static_cast<std::uint8_t>(255U - active_input[index]));
  }
}

TEST(CpuDenseTensorImageOperation,
     ProductExecutorInvertsOnlySelectedPaddedImageRect) {
  ops::register_core_operations();
  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());

  NodeOutput input;
  input.image_value = make_unsigned8_value(5U, 4U, 2U, 16U);
  const ImageView input_view(input.image_value);
  GraphModel graph("");
  Node node;
  node.id = 83;
  node.name = "region_image_invert";
  node.type = "image_process";
  node.subtype = "invert_dense";
  compute::TiledExecutionConfig config;
  config.output_region =
      RegionSet::from_image_rect({image_region_domain(), 1, 4, 1, 3});

  const NodeOutput output =
      compute::NodeExecutor::execute(graph, node, *resolved, {&input}, config);
  const ImageView output_view(output.image_value);

  for (std::size_t y = 0U; y < input_view.height(); ++y) {
    for (std::size_t x = 0U; x < input_view.width(); ++x) {
      for (std::size_t channel = 0U; channel < input_view.channels();
           ++channel) {
        const std::uint8_t source = std::to_integer<std::uint8_t>(
            *input_view.channel_data(x, y, channel));
        const std::uint8_t expected =
            x >= 1U && x < 4U && y >= 1U && y < 3U
                ? static_cast<std::uint8_t>(255U - source)
                : source;
        EXPECT_EQ(std::to_integer<std::uint8_t>(
                      *output_view.channel_data(x, y, channel)),
                  expected);
      }
    }
  }
}

TEST(CpuDenseTensorImageOperation,
     ProductExecutorUsesAllRankFourTensorSliceAxes) {
  ops::register_core_operations();
  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());

  NodeOutput input;
  input.image_value = make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  const ImageView input_view(input.image_value);
  GraphModel graph("");
  Node node;
  node.id = 84;
  node.name = "region_tensor_invert";
  node.type = "image_process";
  node.subtype = "invert_dense";
  compute::TiledExecutionConfig config;
  config.output_region = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});

  const NodeOutput output =
      compute::NodeExecutor::execute(graph, node, *resolved, {&input}, config);
  const ImageView output_view(output.image_value);

  EXPECT_EQ(output_view.descriptor().shape,
            (std::vector<std::size_t>{1U, 3U, 4U, 3U}));
  for (std::size_t y = 0U; y < input_view.height(); ++y) {
    for (std::size_t x = 0U; x < input_view.width(); ++x) {
      for (std::size_t channel = 0U; channel < input_view.channels();
           ++channel) {
        const std::uint8_t source = std::to_integer<std::uint8_t>(
            *input_view.channel_data(x, y, channel));
        const bool selected = y >= 1U && x >= 1U && channel >= 1U;
        const std::uint8_t expected =
            selected ? static_cast<std::uint8_t>(255U - source) : source;
        EXPECT_EQ(std::to_integer<std::uint8_t>(
                      *output_view.channel_data(x, y, channel)),
                  expected);
      }
    }
  }
}

TEST(CpuDenseTensorImageOperation,
     ProductExecutorHandlesEmptyWholeAndRejectsRankMismatch) {
  ops::register_core_operations();
  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  NodeOutput input;
  input.image_value = make_unsigned8_rank4_value(2U, 2U, 2U, 8U);
  const ImageView input_view(input.image_value);
  GraphModel graph("");
  Node node;
  node.id = 85;
  node.name = "region_empty_whole";
  node.type = "image_process";
  node.subtype = "invert_dense";

  compute::TiledExecutionConfig empty_config;
  empty_config.output_region = RegionSet::empty();
  const NodeOutput unchanged = compute::NodeExecutor::execute(
      graph, node, *resolved, {&input}, empty_config);
  const ImageView unchanged_view(unchanged.image_value);
  EXPECT_EQ(
      std::to_integer<std::uint8_t>(*unchanged_view.channel_data(1U, 1U, 1U)),
      std::to_integer<std::uint8_t>(*input_view.channel_data(1U, 1U, 1U)));

  compute::TiledExecutionConfig whole_config;
  whole_config.output_region = RegionSet::whole();
  const NodeOutput inverted = compute::NodeExecutor::execute(
      graph, node, *resolved, {&input}, whole_config);
  EXPECT_EQ(std::to_integer<std::uint8_t>(
                *ImageView(inverted.image_value).channel_data(1U, 1U, 1U)),
            static_cast<std::uint8_t>(
                255U - std::to_integer<std::uint8_t>(
                           *input_view.channel_data(1U, 1U, 1U))));

  compute::TiledExecutionConfig mismatch_config;
  mismatch_config.output_region = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 2U}, {0U, 2U}}});
  try {
    (void)compute::NodeExecutor::execute(graph, node, *resolved, {&input},
                                         mismatch_config);
    FAIL() << "rank mismatch should fail before NodeOutput publication";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
}

/**
 * @brief Proves one clipped TensorSlice flows from dirty planning through the
 * production HP dirty node executor into the registered dense operation.
 *
 * @return Nothing; GoogleTest reports planning, staging, byte-selection,
 * Region-validity, or generation publication failures.
 * @throws Graph, Value, Region, registry, staging, or execution exceptions
 * unchanged.
 * @note GraphModel remains untouched until the request-owned write buffer
 * commits, matching the existing stale/current-generation commit boundary.
 */
TEST(CpuDenseTensorImageOperation,
     TensorDirtyPlanExecutesRegisteredProductAndStagesExactValidity) {
  ops::register_core_operations();
  GraphModel graph("cache/tensor-region-product");
  Node source;
  source.id = 87;
  source.name = "tensor_region_source";
  source.type = "image_generator";
  source.subtype = "constant";
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  source.hp_region = full_rank4_region();
  graph.add_node(std::move(source));
  Node target;
  target.id = 88;
  target.name = "tensor_region_target";
  target.type = "image_process";
  target.subtype = "invert_dense";
  target.image_inputs.push_back({87, "image"});
  graph.add_node(std::move(target));
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const RegionSet requested = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});
  const compute::HighPrecisionDirtyPlan plan =
      planner.plan_high_precision(graph, 88, requested);
  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  const compute::DirtyResolvedOperationMap operations{
      {88, compute::DirtyResolvedOperation{*resolved, Device::CPU}}};
  GraphEventService events;
  compute::DirtyNodeSynchronization synchronization(graph.node_ids());
  compute::HighPrecisionDirtyWriteBuffer staging(false);
  compute::DirtyNodeExecutionContext context{
      graph,          nullptr,    events,
      plan.snapshot,  operations, plan.snapshot.graph_generation,
      synchronization};
  compute::HighPrecisionDirtyNodeExecutor executor(context, staging);
  Node execution_target = graph.node(88);

  executor.execute(execution_target, plan.entries.at(88));

  EXPECT_FALSE(graph.node(88).cached_output_high_precision.has_value());
  ASSERT_NE(staging.find_output(88), nullptr);
  const ImageView input_view(
      graph.node(87).cached_output_high_precision->image_value);
  const ImageView staged_view(staging.find_output(88)->image_value);
  for (std::size_t y = 0U; y < input_view.height(); ++y) {
    for (std::size_t x = 0U; x < input_view.width(); ++x) {
      for (std::size_t channel = 0U; channel < input_view.channels();
           ++channel) {
        const std::uint8_t source_byte = std::to_integer<std::uint8_t>(
            *input_view.channel_data(x, y, channel));
        const bool selected = y >= 1U && x >= 1U && channel >= 1U;
        const std::uint8_t expected =
            selected ? static_cast<std::uint8_t>(255U - source_byte)
                     : source_byte;
        EXPECT_EQ(std::to_integer<std::uint8_t>(
                      *staged_view.channel_data(x, y, channel)),
                  expected);
      }
    }
  }

  staging.commit_to_graph(graph);
  ASSERT_TRUE(graph.node(88).cached_output_high_precision.has_value());
  EXPECT_EQ(graph.node(88).hp_region, requested);
  EXPECT_EQ(graph.node(88).hp_version, 1);
  EXPECT_EQ(graph.dirty_source_hp_commit_generation.at(88),
            plan.snapshot.graph_generation);
}

/**
 * @brief Proves missing and partial intermediate Tensor parents are planned.
 *
 * @return Nothing; GoogleTest reports ordering, dependency, staging, or byte
 * correctness failures.
 * @throws Graph, Value, Region, registry, staging, or execution exceptions
 * unchanged.
 * @note The two iterations differ only in whether the intermediate has no
 * output or a disjoint partial output; neither is a whole-readable boundary.
 */
TEST(CpuDenseTensorImageOperation,
     TensorDirtyPlanRecomputesMissingAndPartialIntermediateParents) {
  ops::register_core_operations();
  const RegionSet requested = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});
  const RegionSet disjoint_partial = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 1U}, {0U, 1U}, {0U, 1U}}});

  for (const bool seed_partial_parent : {false, true}) {
    GraphModel graph(seed_partial_parent ? "cache/tensor-partial-parent"
                                         : "cache/tensor-missing-parent");
    Node source;
    source.id = 90;
    source.name = "tensor_parent_source";
    source.type = "image_generator";
    source.subtype = "constant";
    source.cached_output_high_precision = NodeOutput{};
    source.cached_output_high_precision->image_value =
        make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
    source.hp_region = full_rank4_region();
    graph.add_node(std::move(source));

    Node parent;
    parent.id = 91;
    parent.name = "tensor_parent";
    parent.type = "image_process";
    parent.subtype = "invert_dense";
    parent.image_inputs.push_back({90, "image"});
    if (seed_partial_parent) {
      parent.cached_output_high_precision = NodeOutput{};
      parent.cached_output_high_precision->image_value =
          make_unsigned8_rank4_value(4U, 3U, 3U, 16U, 91U);
      parent.hp_region = disjoint_partial;
    }
    graph.add_node(std::move(parent));

    Node target;
    target.id = 92;
    target.name = "tensor_parent_target";
    target.type = "image_process";
    target.subtype = "invert_dense";
    target.image_inputs.push_back({91, "image"});
    graph.add_node(std::move(target));
    graph.validate_topology();

    GraphTraversalService traversal;
    RoiPropagationService propagation;
    compute::DirtyRegionPlanner planner(traversal, propagation);
    const compute::HighPrecisionDirtyPlan plan =
        planner.plan_high_precision(graph, 92, requested);

    EXPECT_EQ(plan.execution_order, (std::vector<int>{91, 92}));
    ASSERT_EQ(plan.entries.size(), 2U);
    EXPECT_EQ(plan.entries.at(91).region_hp, requested);
    EXPECT_EQ(plan.entries.at(92).region_hp, requested);
    ASSERT_EQ(plan.snapshot.edge_mappings.size(), 2U);
    ASSERT_EQ(plan.snapshot.dirty_source_nodes.size(), 1U);
    EXPECT_EQ(plan.snapshot.dirty_source_nodes.front(), 91);
    ASSERT_EQ(plan.snapshot.source_region_records.at(91).size(), 1U);
    EXPECT_EQ(plan.snapshot.source_region_records.at(91).front().source_region,
              requested);

    const auto resolved = OpRegistry::instance().resolve_for_intent(
        "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
    ASSERT_TRUE(resolved.has_value());
    const compute::DirtyResolvedOperationMap operations{
        {91, compute::DirtyResolvedOperation{*resolved, Device::CPU}},
        {92, compute::DirtyResolvedOperation{*resolved, Device::CPU}}};
    GraphEventService events;
    compute::DirtyNodeSynchronization synchronization(graph.node_ids());
    compute::HighPrecisionDirtyWriteBuffer staging;
    compute::DirtyNodeExecutionContext context{
        graph,          nullptr,    events,
        plan.snapshot,  operations, plan.snapshot.graph_generation,
        synchronization};
    compute::HighPrecisionDirtyNodeExecutor executor(context, staging);
    for (const int planned_id : plan.execution_order) {
      Node execution_node = graph.node(planned_id);
      executor.execute(execution_node, plan.entries.at(planned_id));
    }

    ASSERT_NE(staging.find_output(91), nullptr);
    ASSERT_NE(staging.find_output(92), nullptr);
    const ImageView source_view(
        graph.node(90).cached_output_high_precision->image_value);
    const ImageView target_view(staging.find_output(92)->image_value);
    for (std::size_t y = 1U; y < source_view.height(); ++y) {
      for (std::size_t x = 1U; x < source_view.width(); ++x) {
        for (std::size_t channel = 1U; channel < source_view.channels();
             ++channel) {
          EXPECT_EQ(*target_view.channel_data(x, y, channel),
                    *source_view.channel_data(x, y, channel));
        }
      }
    }
  }
}

/**
 * @brief Proves a local Tensor update preserves old full-validity bytes.
 *
 * @return Nothing; GoogleTest reports selected-byte or retained-byte failures.
 * @throws Graph, Value, Region, registry, staging, or execution exceptions
 * unchanged.
 * @note The current source differs from the source that produced the target's
 * old full cache. Selected coordinates must use current input while every
 * unselected coordinate remains from the old target output.
 */
TEST(CpuDenseTensorImageOperation,
     TensorDirtyUpdateMergesSelectedBytesIntoExistingFullOutput) {
  ops::register_core_operations();
  GraphModel graph("cache/tensor-local-merge");
  Node source;
  source.id = 93;
  source.name = "tensor_merge_source";
  source.type = "image_generator";
  source.subtype = "constant";
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_unsigned8_rank4_value(4U, 3U, 3U, 16U, 51U);
  source.hp_region = full_rank4_region();
  graph.add_node(std::move(source));
  Node target;
  target.id = 94;
  target.name = "tensor_merge_target";
  target.type = "image_process";
  target.subtype = "invert_dense";
  target.image_inputs.push_back({93, "image"});
  graph.add_node(std::move(target));
  graph.validate_topology();

  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  NodeOutput old_source;
  old_source.image_value = make_unsigned8_rank4_value(4U, 3U, 3U, 16U, 1U);
  Node execution_target = graph.node(94);
  NodeOutput old_target = compute::NodeExecutor::execute(
      graph, execution_target, *resolved, {&old_source});
  const Value old_target_value = old_target.image_value;
  const RegionSet complete_target_region =
      value_image_adapter::full_node_output_region(old_target);
  graph.mutate_node_runtime_state(94, [&](GraphModel::NodeRuntimeState& state) {
    state.cached_output_high_precision = std::move(old_target);
    state.hp_region = complete_target_region;
  });

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const RegionSet requested = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});
  const compute::HighPrecisionDirtyPlan plan =
      planner.plan_high_precision(graph, 94, requested);
  ASSERT_EQ(plan.execution_order, (std::vector<int>{94}));

  const compute::DirtyResolvedOperationMap operations{
      {94, compute::DirtyResolvedOperation{*resolved, Device::CPU}}};
  GraphEventService events;
  compute::DirtyNodeSynchronization synchronization(graph.node_ids());
  compute::HighPrecisionDirtyWriteBuffer staging;
  compute::DirtyNodeExecutionContext context{
      graph,          nullptr,    events,
      plan.snapshot,  operations, plan.snapshot.graph_generation,
      synchronization};
  compute::HighPrecisionDirtyNodeExecutor executor(context, staging);
  execution_target = graph.node(94);
  executor.execute(execution_target, plan.entries.at(94));

  ASSERT_NE(staging.find_output(94), nullptr);
  const ImageView current_source(
      graph.node(93).cached_output_high_precision->image_value);
  const ImageView old_source_view(old_source.image_value);
  const ImageView staged_view(staging.find_output(94)->image_value);
  for (std::size_t y = 0U; y < current_source.height(); ++y) {
    for (std::size_t x = 0U; x < current_source.width(); ++x) {
      for (std::size_t channel = 0U; channel < current_source.channels();
           ++channel) {
        const bool selected = y >= 1U && x >= 1U && channel >= 1U;
        const std::uint8_t source_byte = std::to_integer<std::uint8_t>(
            *(selected ? current_source.channel_data(x, y, channel)
                       : old_source_view.channel_data(x, y, channel)));
        const std::uint8_t expected =
            static_cast<std::uint8_t>(255U - source_byte);
        EXPECT_EQ(std::to_integer<std::uint8_t>(
                      *staged_view.channel_data(x, y, channel)),
                  expected);
      }
    }
  }

  staging.commit_to_graph(graph);
  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(94)));
  EXPECT_EQ(graph.node(94).hp_region, complete_target_region);
  EXPECT_NE(
      graph.node(94).cached_output_high_precision->image_value.revision_id(),
      old_target_value.revision_id());
}

/**
 * @brief Proves fresh partial output is not whole-readable until Whole commit.
 *
 * @return Nothing; GoogleTest reports validity, cache-policy, or final-byte
 * failures.
 * @throws Graph, Value, Region, cache, registry, staging, or execution
 * exceptions unchanged.
 * @note The first dirty publication starts with no target output. A subsequent
 * normal whole-output commit replaces it and restores reusable cache authority.
 */
TEST(CpuDenseTensorImageOperation,
     FreshTensorPartialOutputBecomesReusableOnlyAfterWholeCommit) {
  ops::register_core_operations();
  GraphModel graph("cache/tensor-partial-then-whole");
  Node source;
  source.id = 95;
  source.name = "tensor_partial_source";
  source.type = "image_generator";
  source.subtype = "constant";
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_unsigned8_rank4_value(4U, 3U, 3U, 16U);
  source.hp_region = full_rank4_region();
  graph.add_node(std::move(source));
  Node target;
  target.id = 96;
  target.name = "tensor_partial_target";
  target.type = "image_process";
  target.subtype = "invert_dense";
  target.image_inputs.push_back({95, "image"});
  graph.add_node(std::move(target));
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const RegionSet requested = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {1U, 4U}, {1U, 3U}}});
  const compute::HighPrecisionDirtyPlan plan =
      planner.plan_high_precision(graph, 96, requested);
  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  const compute::DirtyResolvedOperationMap operations{
      {96, compute::DirtyResolvedOperation{*resolved, Device::CPU}}};
  GraphEventService events;
  compute::DirtyNodeSynchronization synchronization(graph.node_ids());
  compute::HighPrecisionDirtyWriteBuffer staging(false);
  compute::DirtyNodeExecutionContext context{
      graph,          nullptr,    events,
      plan.snapshot,  operations, plan.snapshot.graph_generation,
      synchronization};
  compute::HighPrecisionDirtyNodeExecutor executor(context, staging);
  Node execution_target = graph.node(96);
  executor.execute(execution_target, plan.entries.at(96));
  staging.commit_to_graph(graph);

  EXPECT_EQ(graph.node(96).hp_region, requested);
  EXPECT_FALSE(
      compute::ComputeCachePolicy::has_reusable_output(graph.node(96)));
  EXPECT_EQ(compute::ComputeCachePolicy::reusable_output(graph.node(96)),
            nullptr);

  const NodeOutput* source_output =
      compute::ComputeCachePolicy::reusable_output(graph.node(95));
  ASSERT_NE(source_output, nullptr);
  execution_target = graph.node(96);
  std::vector<std::optional<NodeOutput>> results(1U);
  results[0] = compute::NodeExecutor::execute(graph, execution_target,
                                              *resolved, {source_output});
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache(image_codec, metadata_codec);
  std::mutex graph_mutex;
  compute::ComputeResultCommitter committer(cache, graph_mutex, "int8");
  committer.commit(graph, {96}, results);

  EXPECT_TRUE(compute::ComputeCachePolicy::has_reusable_output(graph.node(96)));
  EXPECT_EQ(graph.node(96).hp_region,
            value_image_adapter::full_node_output_region(
                *graph.node(96).cached_output_high_precision));
  const ImageView source_view(source_output->image_value);
  const ImageView result_view(
      graph.node(96).cached_output_high_precision->image_value);
  for (std::size_t y = 0U; y < source_view.height(); ++y) {
    for (std::size_t x = 0U; x < source_view.width(); ++x) {
      for (std::size_t channel = 0U; channel < source_view.channels();
           ++channel) {
        const std::uint8_t source_byte = std::to_integer<std::uint8_t>(
            *source_view.channel_data(x, y, channel));
        EXPECT_EQ(std::to_integer<std::uint8_t>(
                      *result_view.channel_data(x, y, channel)),
                  static_cast<std::uint8_t>(255U - source_byte));
      }
    }
  }
}

TEST(CpuDenseTensorImageOperation,
     RunnerRejectsExecuteDescriptorMismatchAsComputeError) {
  NodeOutput input;
  input.image_buffer = make_aligned_cpu_image_buffer(2, 2, 1, DataType::UINT8);
  (void)fill_unsigned8_image(&input.image_buffer);

  ops::CpuDenseImageOperation operation;
  operation.infer = [](const ops::CpuDenseImageConfiguration& configuration,
                       const std::vector<ops::DenseImageDescriptor>& inputs) {
    (void)configuration;
    return inputs.front();
  };
  operation.execute = [](const ops::CpuDenseImageConfiguration& configuration,
                         const std::vector<ImageView>& inputs,
                         const ops::DenseImageDescriptor& inferred) {
    (void)configuration;
    (void)inputs;
    ops::DenseImageDescriptor mismatched = inferred;
    ++mismatched.tensor.shape[mismatched.image.x_axis];
    const std::size_t width = mismatched.tensor.shape[mismatched.image.x_axis];
    const std::size_t height = mismatched.tensor.shape[mismatched.image.y_axis];
    StridedLayout layout{{static_cast<std::ptrdiff_t>(width), 1, 1}};
    std::vector<std::byte> storage(width * height, std::byte{0});
    return Value::from_cpu_dense_tensor(std::move(mismatched.tensor),
                                        mismatched.image, std::move(layout),
                                        std::move(storage));
  };

  Node node;
  try {
    (void)ops::execute_cpu_dense_image_operation(node, {&input}, operation);
    FAIL() << "descriptor mismatch should fail before NodeOutput publication";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
  }
}

}  // namespace
}  // namespace ps
