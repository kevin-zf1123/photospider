#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "compute/request/compute_cache_policy.hpp"  // NOLINT(build/include_subdir)
#include "core/value_region.hpp"  // NOLINT(build/include_subdir)
#include "execution/device/compute_io_executor.hpp"  // NOLINT(build/include_subdir)
#include "execution/transfer/value_transfer_task.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_cache_service.hpp"  // NOLINT(build/include_subdir)
#include "photospider/core/graph_error.hpp"
#include "photospider/data/image_view.hpp"
#include "photospider/data/packed_dense_tensor_view.hpp"
#include "support/fake_cache_metadata_codec.hpp"
#include "support/fake_image_artifact_codec.hpp"

namespace ps {
namespace {

/**
 * @brief Deterministic single-thread callback queue for fence continuations.
 *
 * @throws std::bad_alloc when construction cannot reserve its fixed queue.
 * @note The fixture models asynchronous admission without a native device or
 * worker thread. Tests explicitly execute each queued callback.
 */
class ManualFenceExecutor final : public ReadyFenceExecutor {
 public:
  /** @brief Reserves the maximum callbacks used by this test binary. */
  ManualFenceExecutor() { tasks_.reserve(8U); }

  /**
   * @brief Appends one nonempty callback without executing it inline.
   * @param task Callback transferred from ReadyFence.
   * @throws Nothing; contract violations terminate the test process.
   */
  void submit(Task task) noexcept override {
    if (!task) {
      std::terminate();
    }
    try {
      tasks_.push_back(std::move(task));
    } catch (...) {
      std::terminate();
    }
  }

  /**
   * @brief Returns the number of callbacks not yet executed.
   * @return Current queue remainder.
   * @throws Nothing; the function only observes preallocated queue state.
   */
  std::size_t pending_count() const noexcept {
    return tasks_.size() - next_task_;
  }

  /**
   * @brief Executes the oldest queued callback.
   * @throws std::logic_error when no callback remains.
   * @throws Any callback exception unchanged.
   */
  void run_next() {
    if (pending_count() == 0U) {
      throw std::logic_error("ManualFenceExecutor queue is empty.");
    }
    Task task = std::move(tasks_[next_task_]);
    ++next_task_;
    task();
  }

 private:
  /** @brief Preallocated callback FIFO. */
  std::vector<Task> tasks_;
  /** @brief Index of the next unexecuted callback. */
  std::size_t next_task_ = 0U;
};

/**
 * @brief Fake retained allocation for one external packed transfer.
 * @throws std::bad_alloc when byte storage cannot allocate.
 */
struct FakePackedAllocation {
  /**
   * @brief Allocates one zero-initialized fake native envelope.
   * @param size Positive byte length.
   * @throws std::bad_alloc when vector allocation fails.
   */
  explicit FakePackedAllocation(std::size_t size) : bytes(size, std::byte{0}) {}

  /** @brief Bytes treated as the fake native allocation. */
  std::vector<std::byte> bytes;
};

/**
 * @brief Returns the canonical V-13 test descriptor.
 * @return Shape [2,8], FP4 E2M1, block [1,4], and four row-major scales.
 * @throws std::bad_alloc when vector storage cannot allocate.
 */
DenseTensorDescriptor packed_descriptor() {
  return DenseTensorDescriptor{
      {2U, 8U},
      ElementSemantics::FloatingPoint,
      StorageEncoding{4U, StorageEncodingKind::Fp4E2M1},
      QuantizationSchema{{1U, 4U}, {1.0F, 2.0F, 3.0F, 4.0F}}};
}

/**
 * @brief Returns the canonical nonzero-offset V-13 test layout.
 * @param order Explicit intra-byte nibble order.
 * @return Version-1 block [1,4], block strides [32,16], bit offset four.
 * @throws std::bad_alloc when vector storage cannot allocate.
 */
BlockedLayout packed_layout(PackedBitOrder order) {
  return BlockedLayout{1U, {1U, 4U}, {32U, 16U}, 4U, order};
}

/**
 * @brief Writes one independent-oracle code into the fixed test envelope.
 * @param bytes Mutable nine-byte canonical envelope.
 * @param row Logical row in [0,2).
 * @param column Logical column in [0,8).
 * @param order Explicit packed bit order.
 * @param code Four-bit code.
 * @throws std::out_of_range when vector indexing detects a fixture error.
 * @note The formula is stated directly rather than calling production layout
 * helpers: bit = 4 + row*32 + (column/4)*16 + (column%4)*4.
 */
void write_oracle_code(std::vector<std::byte>* bytes, std::size_t row,
                       std::size_t column, PackedBitOrder order,
                       std::uint8_t code) {
  const std::size_t bit =
      4U + row * 32U + (column / 4U) * 16U + (column % 4U) * 4U;
  const std::size_t bit_in_byte = bit % 8U;
  const std::size_t shift = order == PackedBitOrder::LeastSignificantFirst
                                ? bit_in_byte
                                : 4U - bit_in_byte;
  const std::size_t byte_index = bit / 8U;
  const std::uint8_t prior =
      std::to_integer<std::uint8_t>(bytes->at(byte_index));
  const std::uint8_t mask = static_cast<std::uint8_t>(0x0FU << shift);
  bytes->at(byte_index) = static_cast<std::byte>(
      (prior & static_cast<std::uint8_t>(~mask)) |
      static_cast<std::uint8_t>((code & 0x0FU) << shift));
}

/**
 * @brief Publishes the fixed packed tensor through the production builder.
 * @param order Explicit supported packed bit order.
 * @return Ready CPU Value containing codes zero through fifteen in row-major
 * logical order and sentinel value 0xA0 in otherwise unused bits.
 * @throws Validation, allocation, or publication exceptions unchanged.
 */
Value make_packed_value(PackedBitOrder order) {
  std::vector<std::byte> storage(9U, std::byte{0xA0});
  for (std::size_t row = 0U; row < 2U; ++row) {
    for (std::size_t column = 0U; column < 8U; ++column) {
      write_oracle_code(&storage, row, column, order,
                        static_cast<std::uint8_t>(row * 8U + column));
    }
  }
  return Value::from_cpu_blocked_dense_tensor(
      packed_descriptor(), packed_layout(order), std::move(storage));
}

/**
 * @brief Copies every immutable storage byte for exact transfer comparison.
 * @param value Ready host-readable Value.
 * @return Detached byte vector matching value.storage_size().
 * @throws ReadyFenceAccessError or BufferAccessError when bytes are unreadable.
 * @throws std::bad_alloc when output storage cannot allocate.
 */
std::vector<std::byte> copy_storage(const Value& value) {
  const ReadLease read = value.buffer_handle().acquire_read();
  return std::vector<std::byte>(read.data(), read.data() + read.size());
}

/**
 * @brief Returns one unique test-only cache root path without creating it.
 * @param suffix Stable test-specific suffix.
 * @return Path below the platform temporary directory.
 * @throws std::bad_alloc or filesystem exceptions from path construction.
 */
std::filesystem::path cache_root(const std::string& suffix) {
  return std::filesystem::temp_directory_path() /
         ("photospider-packed-fp4-" + suffix);
}

TEST(PackedFp4DenseTensor, ReadsBothBitOrdersAndCopiesAlignedSlice) {
  for (const PackedBitOrder order : {PackedBitOrder::LeastSignificantFirst,
                                     PackedBitOrder::MostSignificantFirst}) {
    const Value source = make_packed_value(order);
    EXPECT_EQ(source.storage_layout_kind(), StorageLayoutKind::Blocked);
    EXPECT_EQ(source.blocked_layout(), packed_layout(order));
    EXPECT_THROW((void)source.strided_layout(), std::logic_error);
    EXPECT_THROW(
        (void)dense_tensor_element_bytes(source.dense_tensor_descriptor()),
        std::invalid_argument);
    EXPECT_THROW((void)DenseTensorView(source), std::invalid_argument);
    EXPECT_THROW((void)ImageView(source), std::invalid_argument);

    const PackedDenseTensorView view(source);
    EXPECT_EQ(view.encoded_element({0U, 0U}), 0U);
    EXPECT_EQ(view.encoded_element({0U, 7U}), 7U);
    EXPECT_EQ(view.encoded_element({1U, 6U}), 14U);
    EXPECT_FLOAT_EQ(view.dequantized_element({0U, 6U}), 8.0F);
    EXPECT_FLOAT_EQ(view.dequantized_element({1U, 6U}), -16.0F);

    const TensorSlice aligned{dense_tensor_region_domain(),
                              {{1U, 2U}, {4U, 8U}}};
    const Value sliced = copy_packed_dense_tensor_slice(source, aligned);
    const PackedDenseTensorView sliced_view(sliced);
    EXPECT_EQ(sliced.dense_tensor_descriptor().shape,
              (std::vector<std::size_t>{1U, 4U}));
    ASSERT_TRUE(sliced.dense_tensor_descriptor().quantization.has_value());
    EXPECT_EQ(sliced.dense_tensor_descriptor().quantization->scales,
              (std::vector<float>{4.0F}));
    EXPECT_EQ(sliced.blocked_layout().bit_offset, 4U);
    EXPECT_EQ(sliced.blocked_layout().bit_order, order);
    EXPECT_EQ(sliced.storage_size(), 3U);
    EXPECT_NE(sliced.allocation_identity(), source.allocation_identity());
    EXPECT_NE(sliced.revision_id(), source.revision_id());
    for (std::size_t column = 0U; column < 4U; ++column) {
      EXPECT_EQ(sliced_view.encoded_element({0U, column}), 12U + column);
    }

    const TensorSlice misaligned{dense_tensor_region_domain(),
                                 {{0U, 1U}, {2U, 8U}}};
    EXPECT_THROW((void)copy_packed_dense_tensor_slice(source, misaligned),
                 std::invalid_argument);
  }
}

TEST(PackedFp4DenseTensor, RejectsMalformedQuantizationLayoutAndEnvelope) {
  DenseTensorDescriptor bad_scales = packed_descriptor();
  bad_scales.quantization->scales.pop_back();
  EXPECT_THROW(
      (void)ValueBuilder::allocate_cpu_blocked_dense_tensor(
          bad_scales, packed_layout(PackedBitOrder::LeastSignificantFirst), 9U),
      std::invalid_argument);

  DenseTensorDescriptor bad_quantization_rank = packed_descriptor();
  bad_quantization_rank.quantization->block_shape.pop_back();
  EXPECT_THROW((void)ValueBuilder::allocate_cpu_blocked_dense_tensor(
                   bad_quantization_rank,
                   packed_layout(PackedBitOrder::LeastSignificantFirst), 9U),
               std::invalid_argument);

  DenseTensorDescriptor zero_block_extent = packed_descriptor();
  zero_block_extent.quantization->block_shape[1] = 0U;
  EXPECT_THROW((void)ValueBuilder::allocate_cpu_blocked_dense_tensor(
                   zero_block_extent,
                   packed_layout(PackedBitOrder::LeastSignificantFirst), 9U),
               std::invalid_argument);

  DenseTensorDescriptor nondivisible_extent = packed_descriptor();
  nondivisible_extent.shape[1] = 7U;
  EXPECT_THROW((void)ValueBuilder::allocate_cpu_blocked_dense_tensor(
                   nondivisible_extent,
                   packed_layout(PackedBitOrder::LeastSignificantFirst), 9U),
               std::invalid_argument);

  DenseTensorDescriptor nonfinite_scale = packed_descriptor();
  nonfinite_scale.quantization->scales[0] =
      std::numeric_limits<float>::infinity();
  EXPECT_THROW((void)ValueBuilder::allocate_cpu_blocked_dense_tensor(
                   nonfinite_scale,
                   packed_layout(PackedBitOrder::LeastSignificantFirst), 9U),
               std::invalid_argument);

  DenseTensorDescriptor nonpositive_scale = packed_descriptor();
  nonpositive_scale.quantization->scales[0] = 0.0F;
  EXPECT_THROW((void)ValueBuilder::allocate_cpu_blocked_dense_tensor(
                   nonpositive_scale,
                   packed_layout(PackedBitOrder::LeastSignificantFirst), 9U),
               std::invalid_argument);

  BlockedLayout bad_version =
      packed_layout(PackedBitOrder::LeastSignificantFirst);
  bad_version.version = 2U;
  EXPECT_THROW((void)ValueBuilder::allocate_cpu_blocked_dense_tensor(
                   packed_descriptor(), bad_version, 9U),
               std::invalid_argument);

  BlockedLayout misaligned =
      packed_layout(PackedBitOrder::LeastSignificantFirst);
  misaligned.bit_offset = 2U;
  EXPECT_THROW((void)ValueBuilder::allocate_cpu_blocked_dense_tensor(
                   packed_descriptor(), misaligned, 9U),
               std::invalid_argument);

  BlockedLayout overlap = packed_layout(PackedBitOrder::LeastSignificantFirst);
  overlap.block_bit_strides = {16U, 16U};
  EXPECT_THROW((void)ValueBuilder::allocate_cpu_blocked_dense_tensor(
                   packed_descriptor(), overlap, 7U),
               std::invalid_argument);

  EXPECT_THROW((void)ValueBuilder::allocate_cpu_blocked_dense_tensor(
                   packed_descriptor(),
                   packed_layout(PackedBitOrder::LeastSignificantFirst), 8U),
               std::invalid_argument);

  DenseTensorDescriptor quantized_native{{1U, 4U},
                                         ElementSemantics::FloatingPoint,
                                         StorageEncoding{32U},
                                         QuantizationSchema{{1U, 4U}, {1.0F}}};
  EXPECT_THROW((void)ValueBuilder::allocate_cpu_dense_tensor(
                   quantized_native, std::nullopt, StridedLayout{{16, 4}}, 16U),
               std::invalid_argument);

  DenseTensorDescriptor native = {{16U},
                                  ElementSemantics::UnsignedInteger,
                                  StorageEncoding{8U}};
  const Value backing =
      Value::from_cpu_dense_tensor(native, std::nullopt, StridedLayout{{1}},
                                   std::vector<std::byte>(16U, std::byte{0}));
  const Value oversized_alias = Value::from_cpu_blocked_dense_tensor(
      packed_descriptor(), packed_layout(PackedBitOrder::LeastSignificantFirst),
      backing.buffer_handle());
  EXPECT_EQ(oversized_alias.storage_size(), 16U);
  EXPECT_THROW((void)ValueTransferTask::prepare_cpu_copy(oversized_alias),
               std::invalid_argument);

  bool provider_called = false;
  auto owner = std::make_shared<FakePackedAllocation>(16U);
  EXPECT_THROW(
      (void)ValueTransferTask::prepare_external_transfer(
          oversized_alias,
          AccessTarget{DeviceId(DeviceBackend::Metal), MemoryDomain::HostPinned,
                       true, true},
          owner, owner.get(), owner->bytes.data(),
          [&provider_called](const Value&,
                             const std::shared_ptr<DeviceTransferCompletion>&) {
            provider_called = true;
          }),
      std::invalid_argument);
  EXPECT_FALSE(provider_called);
}

TEST(PackedFp4DenseTensor, TransfersCpuAndFakeExternalRepresentationExactly) {
  const Value source = make_packed_value(PackedBitOrder::MostSignificantFirst);
  const std::vector<std::byte> expected_bytes = copy_storage(source);

  ValueTransferTask cpu_transfer = ValueTransferTask::prepare_cpu_copy(source);
  const Value cpu_destination = cpu_transfer.destination();
  auto cpu_executor = std::make_shared<ManualFenceExecutor>();
  EXPECT_EQ(cpu_destination.ready_fence().poll().state(),
            ReadyFenceState::Pending);
  EXPECT_EQ(cpu_destination.dense_tensor_descriptor(),
            source.dense_tensor_descriptor());
  EXPECT_EQ(cpu_destination.blocked_layout(), source.blocked_layout());
  EXPECT_EQ(cpu_destination.revision_id(), source.revision_id());
  EXPECT_NE(cpu_destination.allocation_identity(),
            source.allocation_identity());
  cpu_transfer.enqueue(cpu_executor);
  ASSERT_EQ(cpu_executor->pending_count(), 1U);
  cpu_executor->run_next();
  EXPECT_EQ(cpu_destination.ready_fence().poll().state(),
            ReadyFenceState::Ready);
  EXPECT_EQ(copy_storage(cpu_destination), expected_bytes);

  auto external_allocation =
      std::make_shared<FakePackedAllocation>(source.storage_size());
  ValueTransferTask external_transfer =
      ValueTransferTask::prepare_external_transfer(
          source,
          AccessTarget{DeviceId(DeviceBackend::Metal), MemoryDomain::HostPinned,
                       true, true},
          external_allocation, external_allocation.get(),
          external_allocation->bytes.data(),
          [external_allocation](
              const Value& ready_source,
              const std::shared_ptr<DeviceTransferCompletion>& completion) {
            const ReadLease read = ready_source.buffer_handle().acquire_read();
            std::memcpy(external_allocation->bytes.data(), read.data(),
                        read.size());
            if (!completion->complete_ready()) {
              throw std::logic_error(
                  "Fake packed transfer lost completion authority.");
            }
          });
  const Value external_destination = external_transfer.destination();
  auto external_executor = std::make_shared<ManualFenceExecutor>();
  EXPECT_EQ(external_destination.dense_tensor_descriptor(),
            source.dense_tensor_descriptor());
  EXPECT_EQ(external_destination.blocked_layout(), source.blocked_layout());
  EXPECT_EQ(external_destination.revision_id(), source.revision_id());
  EXPECT_EQ(external_destination.storage_binding().device,
            DeviceId(DeviceBackend::Metal));
  external_transfer.enqueue(external_executor);
  ASSERT_EQ(external_executor->pending_count(), 1U);
  external_executor->run_next();
  EXPECT_EQ(external_destination.ready_fence().poll().state(),
            ReadyFenceState::Ready);
  EXPECT_EQ(external_allocation->bytes, expected_bytes);
  EXPECT_EQ(
      PackedDenseTensorView(external_destination).encoded_element({1U, 6U}),
      14U);
}

TEST(PackedFp4DenseTensor, MemoryCacheRetainsAndDiskCacheRejectsBeforeEffects) {
  const Value source = make_packed_value(PackedBitOrder::LeastSignificantFirst);
  Node node;
  node.id = 17;
  node.caches.push_back({"image", "packed.png"});
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->publish_image_value(source);
  node.cached_output_high_precision->data["tag"] = std::string("packed");
  node.hp_region =
      value_region::full_node_output_region(*node.cached_output_high_precision);
  ASSERT_TRUE(compute::ComputeCachePolicy::has_reusable_output(node));
  const NodeOutput* cached = compute::ComputeCachePolicy::reusable_output(node);
  ASSERT_NE(cached, nullptr);
  EXPECT_EQ(cached->image_value().revision_id(), source.revision_id());
  EXPECT_EQ(cached->image_value().blocked_layout(), source.blocked_layout());

  const std::filesystem::path root = cache_root("fail-closed");
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  GraphModel graph(root);
  auto image_codec = std::make_shared<testing::FakeImageArtifactCodec>();
  auto metadata_codec = std::make_shared<testing::FakeCacheMetadataCodec>();
  GraphCacheService cache(image_codec, metadata_codec);
  const std::filesystem::path node_cache_directory =
      root / std::to_string(node.id);

  node.caches.front().cache_type = "unsupported";
  EXPECT_NO_THROW(cache.save_cache_if_configured(graph, node, "int16"));
  EXPECT_FALSE(std::filesystem::exists(node_cache_directory));
  node.caches.front().cache_type = "image";

  try {
    cache.save_cache_if_configured(graph, node, "int16");
    FAIL() << "packed disk-cache save unexpectedly succeeded";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
  }
  EXPECT_FALSE(std::filesystem::exists(node_cache_directory));
  EXPECT_TRUE(image_codec->calls().empty());
  EXPECT_TRUE(metadata_codec->calls().empty());

  execution::ComputeIoExecutor executor({1U, 1U << 20U});
  const std::shared_ptr<const void> lifetime = std::make_shared<const int>(1);
  try {
    cache.save_cache_if_configured_via_executor(executor, lifetime, graph, node,
                                                "int16");
    FAIL() << "packed executor cache save unexpectedly succeeded";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
  }
  const execution::ComputeIoExecutorSnapshot snapshot = executor.snapshot();
  EXPECT_EQ(snapshot.active_tasks, 0U);
  EXPECT_EQ(snapshot.active_planned_bytes, 0U);
  EXPECT_EQ(snapshot.constructing_tasks, 0U);
  EXPECT_EQ(snapshot.queued_tasks, 0U);
  EXPECT_EQ(snapshot.running_tasks, 0U);
  EXPECT_FALSE(std::filesystem::exists(node_cache_directory));
  EXPECT_TRUE(image_codec->calls().empty());
  EXPECT_TRUE(metadata_codec->calls().empty());
  executor.shutdown();
  std::filesystem::remove_all(root, ignored);
}

}  // namespace
}  // namespace ps
