#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfDeepFrameBuffer.h>
#include <OpenEXR/ImfDeepScanLineOutputFile.h>
#include <OpenEXR/ImfDeepTiledOutputFile.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfMultiPartOutputFile.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/ImfTileDescription.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "adapters/openexr/openexr_deep_scanline_adapter.hpp"
#include "execution/compute_io_executor.hpp"
#include "photospider/data/extension.hpp"
#include "photospider/data/region.hpp"
#include "photospider/plugin/data_definition_registry.hpp"

#ifndef PS_OPENEXR_DEEP_PROVIDER_PATH
#error "PS_OPENEXR_DEEP_PROVIDER_PATH must identify the optional provider DSO"
#endif

/**
 * @file test_openexr_deep_scanline_provider.cpp
 * @brief Long-lived real-library tests for the optional V-15 vertical.
 */

namespace {

namespace fs = std::filesystem;
namespace Imf = OPENEXR_IMF_INTERNAL_NAMESPACE;
namespace Imath = IMATH_NAMESPACE;

using ps::compute_content_digest;
using ps::compute_descriptor_digest;
using ps::compute_storage_layout_digest;
using ps::ContentDigestState;
using ps::DataDefinitionRegistry;
using ps::DataProviderCandidate;
using ps::DataProviderLoadResult;
using ps::DataSpec;
using ps::DataSpecRelation;
using ps::ExtensionIdentity;
using ps::PropertyQueryState;
using ps::ProviderOwner;
using ps::ProviderReadLease;
using ps::ProviderRegionState;
using ps::RegionDomainKey;
using ps::RegionInterval;
using ps::RegionSet;
using ps::TensorSlice;
using ps::Value;
using ps::execution::ComputeIoAdmissionStatus;
using ps::execution::ComputeIoCompletionStatus;
using ps::execution::ComputeIoExecutor;
using ps::execution::ComputeIoExecutorLimits;
using ps::openexr_deep::ChannelMapping;
using ps::openexr_deep::decode_mapping_attribute;
using ps::openexr_deep::encode_mapping_attribute;
using ps::openexr_deep::inspect_openexr_deep_value;
using ps::openexr_deep::kDeclaredSampleCountProperty;
using ps::openexr_deep::kLogicalPixelRegionDomain;
using ps::openexr_deep::kLogicalSiteCountProperty;
using ps::openexr_deep::kMappingAttributeMarker;
using ps::openexr_deep::kMappingAttributeName;
using ps::openexr_deep::kProviderIdentity;
using ps::openexr_deep::kVariableSampleFieldSchemaIdentity;
using ps::openexr_deep::make_openexr_deep_value;
using ps::openexr_deep::OpenExrDeepError;
using ps::openexr_deep::OpenExrDeepErrorCode;
using ps::openexr_deep::OpenExrDeepImage;
using ps::openexr_deep::OpenExrDeepIoHooks;
using ps::openexr_deep::OpenExrDeepReadSubmission;
using ps::openexr_deep::SignedBounds;
using ps::openexr_deep::submit_openexr_deep_read;
using ps::openexr_deep::submit_openexr_deep_write;

/** @brief First permanent channel identity used by deterministic fixtures. */
constexpr ExtensionIdentity kFirstChannelIdentity{0x100U, 0x101U};
/** @brief Second permanent channel identity used by deterministic fixtures. */
constexpr ExtensionIdentity kSecondChannelIdentity{0x200U, 0x201U};
/** @brief Semantic role deliberately unrelated to the first diagnostic name. */
constexpr ExtensionIdentity kFirstSemanticRole{0x900U, 0x901U};
/** @brief Semantic role deliberately unrelated to the second diagnostic name.
 */
constexpr ExtensionIdentity kSecondSemanticRole{0xa00U, 0xa01U};

/**
 * @brief Owns one unique runtime test directory and removes only that path.
 * @throws std::filesystem::filesystem_error when creation fails.
 */
class TemporaryDirectory final {
 public:
  /**
   * @brief Creates one process-unique directory below the system temp root.
   * @throws std::filesystem::filesystem_error when creation fails.
   */
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0U};
    const std::uint64_t id = sequence.fetch_add(1U, std::memory_order_relaxed);
    path_ = fs::temp_directory_path() /
            ("photospider-openexr-deep-" + std::to_string(id) + "-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path_);
  }

  /** @brief Copying unique temporary ownership is forbidden. */
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  /** @brief Copy assignment of unique temporary ownership is forbidden. */
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  /**
   * @brief Removes the exact owned directory tree after each test.
   * @throws Nothing; teardown cleanup errors are non-actionable.
   */
  ~TemporaryDirectory() noexcept {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }

  /**
   * @brief Returns the exact owned directory.
   * @return Borrowed immutable path.
   * @throws Nothing.
   */
  const fs::path& path() const noexcept { return path_; }

 private:
  /** @brief Exact directory created by this owner. */
  fs::path path_;
};

/**
 * @brief Test-controlled callback gate that blocks one I/O worker entry.
 * @throws Standard synchronization exceptions from explicit operations.
 */
class BlockingGate final {
 public:
  /**
   * @brief Marks entry and blocks until release.
   * @throws std::system_error from synchronization primitives.
   * @note Invoked on the independent compute-I/O worker.
   */
  void enter_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return released_; });
  }

  /**
   * @brief Waits until the I/O worker has entered the gate.
   * @throws std::runtime_error when entry misses the bounded deadline.
   * @throws std::system_error from synchronization primitives.
   */
  void wait_until_entered() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::seconds(10),
                             [this]() { return entered_; })) {
      throw std::runtime_error("OpenEXR deep test gate was not entered.");
    }
  }

  /**
   * @brief Releases the blocked callback exactly once.
   * @throws std::system_error from synchronization primitives.
   */
  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  /** @brief Serializes gate state. */
  std::mutex mutex_;
  /** @brief Wakes entry/release waiters. */
  std::condition_variable condition_;
  /** @brief Whether the callback reached the gate. */
  bool entered_ = false;
  /** @brief Whether the controlling test released the callback. */
  bool released_ = false;
};

/**
 * @brief Opens the provider DSO and transfers one native reference to a
 * DataProviderCandidate.
 * @param close_count Counter incremented after the candidate's last module
 * lease closes.
 * @return Exact v3 functions plus shared module lifetime.
 * @throws std::runtime_error when load or symbol resolution fails.
 * @note Each invocation owns a distinct platform loader reference even when
 * the loader returns the same native handle value.
 */
DataProviderCandidate open_provider_candidate(
    const std::shared_ptr<std::atomic<std::uint64_t>>& close_count) {
#if defined(_WIN32)
  HMODULE native = LoadLibraryA(PS_OPENEXR_DEEP_PROVIDER_PATH);
  if (native == nullptr) {
    throw std::runtime_error("Failed to load OpenEXR deep provider DSO.");
  }
  const auto resolve = [native](const char* name) -> FARPROC {
    FARPROC symbol = GetProcAddress(native, name);
    if (symbol == nullptr) {
      throw std::runtime_error(std::string("Missing provider symbol: ") + name);
    }
    return symbol;
  };
  DataProviderCandidate candidate;
  candidate.get_abi_version =
      reinterpret_cast<ps_data_provider_get_abi_version_fn_v3>(
          resolve("ps_data_provider_get_abi_version"));
  candidate.get_api = reinterpret_cast<ps_data_provider_get_api_fn_v3>(
      resolve("ps_data_provider_get_api_v3"));
  candidate.module_lease = std::shared_ptr<void>(
      reinterpret_cast<void*>(native), [close_count](void* handle) {
        (void)FreeLibrary(reinterpret_cast<HMODULE>(handle));
        close_count->fetch_add(1U, std::memory_order_relaxed);
      });
#else
  void* native = dlopen(PS_OPENEXR_DEEP_PROVIDER_PATH, RTLD_NOW | RTLD_LOCAL);
  if (native == nullptr) {
    const char* detail = dlerror();
    throw std::runtime_error(
        std::string("Failed to load OpenEXR deep provider DSO: ") +
        (detail != nullptr ? detail : "unknown loader error"));
  }
  const auto resolve = [native](const char* name) -> void* {
    (void)dlerror();
    void* symbol = dlsym(native, name);
    const char* detail = dlerror();
    if (detail != nullptr || symbol == nullptr) {
      throw std::runtime_error(std::string("Missing provider symbol: ") + name);
    }
    return symbol;
  };
  DataProviderCandidate candidate;
  candidate.get_abi_version =
      reinterpret_cast<ps_data_provider_get_abi_version_fn_v3>(
          resolve("ps_data_provider_get_abi_version"));
  candidate.get_api = reinterpret_cast<ps_data_provider_get_api_fn_v3>(
      resolve("ps_data_provider_get_api_v3"));
  candidate.module_lease =
      std::shared_ptr<void>(native, [close_count](void* handle) {
        (void)dlclose(handle);
        close_count->fetch_add(1U, std::memory_order_relaxed);
      });
#endif
  return candidate;
}

/**
 * @brief Loads one fresh DSO generation into the injected registry.
 * @param registry Target process authority.
 * @param close_count Native close counter for this candidate reference.
 * @return Host-owned transaction result.
 * @throws Exceptions from dynamic loading or registry staging.
 */
DataProviderLoadResult load_provider(
    DataDefinitionRegistry& registry,
    const std::shared_ptr<std::atomic<std::uint64_t>>& close_count) {
  return registry.load(open_provider_candidate(close_count));
}

/**
 * @brief Creates the deterministic nonconventional two-channel deep image.
 * @return Six signed-window sites and nine samples per channel.
 * @throws std::bad_alloc when fixture storage cannot allocate.
 * @note Channel identity order is authoritative; names deliberately imply no
 * conventional color/depth role.
 */
OpenExrDeepImage make_image() {
  OpenExrDeepImage image;
  image.data_window = {-2, 3, 1, 5};
  image.display_window = {-4, 1, 4, 7};
  image.channels = {
      {"far_payload", kFirstChannelIdentity, kSecondSemanticRole, 1024U},
      {"near_payload", kSecondChannelIdentity, kFirstSemanticRole, 2048U}};
  image.sample_counts = {2U, 0U, 1U, 3U, 1U, 2U};
  image.channel_samples = {
      {0.5F, 1.5F, 2.5F, 3.5F, 4.5F, 5.5F, 6.5F, 7.5F, 8.5F},
      {10.0F, 11.0F, 12.0F, 13.0F, 14.0F, 15.0F, 16.0F, 17.0F, 18.0F}};
  return image;
}

/**
 * @brief Compares every logical deep-image fact with useful assertion paths.
 * @param expected Expected source image.
 * @param actual Inspected or round-tripped image.
 * @throws Nothing; GoogleTest records mismatches.
 */
void expect_image_equal(const OpenExrDeepImage& expected,
                        const OpenExrDeepImage& actual) {
  EXPECT_TRUE(expected.data_window == actual.data_window);
  EXPECT_TRUE(expected.display_window == actual.display_window);
  EXPECT_EQ(expected.channels, actual.channels);
  EXPECT_EQ(expected.sample_counts, actual.sample_counts);
  EXPECT_EQ(expected.channel_samples, actual.channel_samples);
}

/**
 * @brief Writes a minimal valid deep-scanline file without Photospider mapping.
 * @param path Exact output file.
 * @throws OpenEXR/Iex or allocation exceptions.
 */
void write_unmapped_deep_file(const fs::path& path) {
  Imf::Header header(1, 1);
  header.setType(Imf::DEEPSCANLINE);
  header.compression() = Imf::ZIPS_COMPRESSION;
  header.channels().insert("Z", Imf::Channel(Imf::FLOAT, 1, 1));
  std::vector<std::uint32_t> counts{1U};
  std::vector<float> samples{42.0F};
  std::vector<float*> pointers{samples.data()};
  Imf::DeepFrameBuffer frame_buffer;
  frame_buffer.insertSampleCountSlice(
      Imf::Slice::Make(Imf::UINT, counts.data(), header.dataWindow(),
                       sizeof(std::uint32_t), sizeof(std::uint32_t)));
  const Imf::Slice pointer_grid =
      Imf::Slice::Make(Imf::UINT, pointers.data(), header.dataWindow(),
                       sizeof(float*), sizeof(float*));
  frame_buffer.insert(
      "Z", Imf::DeepSlice(Imf::FLOAT, pointer_grid.base, pointer_grid.xStride,
                          pointer_grid.yStride, sizeof(float)));
  Imf::DeepScanLineOutputFile output(path.string().c_str(), header, 0);
  output.setFrameBuffer(frame_buffer);
  output.writePixels(1);
}

/**
 * @brief Writes a complete mapped deep file whose shared sample total is zero.
 * @param path Exact output file.
 * @throws OpenEXR/Iex or allocation exceptions.
 * @note V-15 omits zero-length channel storage envelopes while preserving the
 * channel mapping in descriptor and Layout payloads.
 */
void write_zero_sample_file(const fs::path& path) {
  Imf::Header header(1, 1);
  header.setType(Imf::DEEPSCANLINE);
  header.compression() = Imf::ZIPS_COMPRESSION;
  header.channels().insert("empty_payload", Imf::Channel(Imf::FLOAT, 1, 1));
  const std::vector<ChannelMapping> mappings{
      {"empty_payload", kFirstChannelIdentity, kFirstSemanticRole, 1024U}};
  header.insert(kMappingAttributeName,
                Imf::StringAttribute(encode_mapping_attribute(mappings)));
  std::vector<std::uint32_t> counts{0U};
  float unused_sample = 0.0F;
  std::vector<float*> pointers{&unused_sample};
  Imf::DeepFrameBuffer frame_buffer;
  frame_buffer.insertSampleCountSlice(
      Imf::Slice::Make(Imf::UINT, counts.data(), header.dataWindow(),
                       sizeof(std::uint32_t), sizeof(std::uint32_t)));
  const Imf::Slice pointer_grid =
      Imf::Slice::Make(Imf::UINT, pointers.data(), header.dataWindow(),
                       sizeof(float*), sizeof(float*));
  frame_buffer.insert(
      "empty_payload",
      Imf::DeepSlice(Imf::FLOAT, pointer_grid.base, pointer_grid.xStride,
                     pointer_grid.yStride, sizeof(float)));
  Imf::DeepScanLineOutputFile output(path.string().c_str(), header, 0);
  output.setFrameBuffer(frame_buffer);
  output.writePixels(1);
}

/**
 * @brief Writes a complete explicitly mapped deep file with mixed FP32/HALF
 * channels.
 * @param path Exact output file.
 * @throws OpenEXR/Iex or allocation exceptions.
 * @note The HALF declaration is valid OpenEXR but outside the first vertical.
 */
void write_mixed_channel_type_file(const fs::path& path) {
  Imf::Header header(1, 1);
  header.setType(Imf::DEEPSCANLINE);
  header.compression() = Imf::ZIPS_COMPRESSION;
  header.channels().insert("float_payload", Imf::Channel(Imf::FLOAT, 1, 1));
  header.channels().insert("half_payload", Imf::Channel(Imf::HALF, 1, 1));
  const std::vector<ChannelMapping> mappings{
      {"float_payload", kFirstChannelIdentity, kFirstSemanticRole, 1024U},
      {"half_payload", kSecondChannelIdentity, kSecondSemanticRole, 2048U}};
  header.insert(kMappingAttributeName,
                Imf::StringAttribute(encode_mapping_attribute(mappings)));
  std::vector<std::uint32_t> counts{1U};
  std::vector<float> float_samples{1.25F};
  std::vector<Imath::half> half_samples{Imath::half(2.5F)};
  std::vector<float*> float_pointers{float_samples.data()};
  std::vector<Imath::half*> half_pointers{half_samples.data()};
  Imf::DeepFrameBuffer frame_buffer;
  frame_buffer.insertSampleCountSlice(
      Imf::Slice::Make(Imf::UINT, counts.data(), header.dataWindow(),
                       sizeof(std::uint32_t), sizeof(std::uint32_t)));
  const Imf::Slice float_pointer_grid =
      Imf::Slice::Make(Imf::UINT, float_pointers.data(), header.dataWindow(),
                       sizeof(float*), sizeof(float*));
  frame_buffer.insert(
      "float_payload",
      Imf::DeepSlice(Imf::FLOAT, float_pointer_grid.base,
                     float_pointer_grid.xStride, float_pointer_grid.yStride,
                     sizeof(float)));
  const Imf::Slice half_pointer_grid =
      Imf::Slice::Make(Imf::UINT, half_pointers.data(), header.dataWindow(),
                       sizeof(Imath::half*), sizeof(Imath::half*));
  frame_buffer.insert(
      "half_payload",
      Imf::DeepSlice(Imf::HALF, half_pointer_grid.base,
                     half_pointer_grid.xStride, half_pointer_grid.yStride,
                     sizeof(Imath::half)));
  Imf::DeepScanLineOutputFile output(path.string().c_str(), header, 0);
  output.setFrameBuffer(frame_buffer);
  output.writePixels(1);
}

/**
 * @brief Writes a minimal shallow scanline file for typed shape rejection.
 * @param path Exact output file.
 * @throws OpenEXR/Iex or allocation exceptions.
 */
void write_shallow_file(const fs::path& path) {
  Imf::Header header(1, 1);
  header.setType(Imf::SCANLINEIMAGE);
  header.channels().insert("ordinary", Imf::Channel(Imf::FLOAT));
  float sample = 1.0F;
  Imf::FrameBuffer frame_buffer;
  frame_buffer.insert("ordinary",
                      Imf::Slice::Make(Imf::FLOAT, &sample, header.dataWindow(),
                                       sizeof(float), sizeof(float)));
  Imf::OutputFile output(path.string().c_str(), header, 0);
  output.setFrameBuffer(frame_buffer);
  output.writePixels(1);
}

/**
 * @brief Writes a header-valid deep-tiled file for pre-open type rejection.
 * @param path Exact output file.
 * @throws OpenEXR/Iex or allocation exceptions.
 * @note No tile payload is needed because the adapter rejects classification
 * before selecting a deep input-file implementation.
 */
void write_deep_tiled_file(const fs::path& path) {
  Imf::Header header(1, 1);
  header.setType(Imf::DEEPTILE);
  header.compression() = Imf::ZIPS_COMPRESSION;
  header.setTileDescription(Imf::TileDescription(1U, 1U));
  header.channels().insert("tile_payload", Imf::Channel(Imf::FLOAT));
  Imf::DeepTiledOutputFile output(path.string().c_str(), header, 0);
}

/**
 * @brief Writes a two-part header-valid OpenEXR file for multipart rejection.
 * @param path Exact output file.
 * @throws OpenEXR/Iex or allocation exceptions.
 * @note No part payload is needed because classification rejects multipart
 * before selecting a part or attempting fallback.
 */
void write_multipart_file(const fs::path& path) {
  std::array<Imf::Header, 2U> headers{Imf::Header(1, 1), Imf::Header(1, 1)};
  headers[0].setName("deep_part");
  headers[0].setType(Imf::DEEPSCANLINE);
  headers[0].compression() = Imf::ZIPS_COMPRESSION;
  headers[0].channels().insert("deep_payload", Imf::Channel(Imf::FLOAT));
  headers[1].setName("shallow_part");
  headers[1].setType(Imf::SCANLINEIMAGE);
  headers[1].channels().insert("shallow_payload", Imf::Channel(Imf::FLOAT));
  Imf::MultiPartOutputFile output(path.string().c_str(), headers.data(),
                                  headers.size(), false, 0);
}

/**
 * @brief Creates a transaction lifetime token accepted by ComputeIoExecutor.
 * @return Non-null erased shared owner.
 * @throws std::bad_alloc when token allocation fails.
 */
std::shared_ptr<const void> make_transaction_token() {
  return std::static_pointer_cast<const void>(std::make_shared<int>(15));
}

/**
 * @brief Loads one provider and asserts a successful transaction.
 * @param registry Target registry.
 * @return Native DSO close counter retained by the caller.
 * @throws Exceptions from provider loading.
 */
std::shared_ptr<std::atomic<std::uint64_t>> load_initial_provider(
    DataDefinitionRegistry& registry) {
  auto closes = std::make_shared<std::atomic<std::uint64_t>>(0U);
  const DataProviderLoadResult loaded = load_provider(registry, closes);
  EXPECT_TRUE(loaded.ok()) << loaded.diagnostic;
  EXPECT_EQ(loaded.generation, 1U);
  EXPECT_TRUE(loaded.provider_identity == kProviderIdentity);
  return closes;
}

/**
 * @brief Proves the real codec round-trip preserves generic semantic facts.
 * @throws Exceptions from provider loading, codec I/O, or generic contracts.
 */
TEST(OpenExrDeepScanlineProvider, RoundTripPreservesGenericSemantics) {
  TemporaryDirectory temporary;
  auto registry = std::make_shared<DataDefinitionRegistry>();
  const auto closes = load_initial_provider(*registry);
  const OpenExrDeepImage expected = make_image();
  const Value original = make_openexr_deep_value(*registry, expected);
  ASSERT_EQ(original.provider_generation(), 1U);

  const auto logical_sites =
      original.query_property({kLogicalSiteCountProperty});
  ASSERT_EQ(logical_sites.state, PropertyQueryState::Available);
  ASSERT_TRUE(logical_sites.unsigned_value.has_value());
  EXPECT_EQ(*logical_sites.unsigned_value, 6U);
  const auto samples = original.query_property({kDeclaredSampleCountProperty});
  ASSERT_TRUE(samples.unsigned_value.has_value());
  EXPECT_EQ(*samples.unsigned_value, 9U);
  const auto spec = original.evaluate_data_spec(
      {kVariableSampleFieldSchemaIdentity, 1U, 1U, 6U, 6U});
  EXPECT_EQ(spec.relation, DataSpecRelation::Subset);
  TensorSlice slice;
  slice.domain = {kLogicalPixelRegionDomain.high,
                  kLogicalPixelRegionDomain.low};
  slice.axes = {{0U, 2U}, {0U, 1U}};
  const auto region =
      original.evaluate_region(RegionSet::from_tensor_slice(std::move(slice)));
  EXPECT_EQ(region.state, ProviderRegionState::Exact);
  EXPECT_EQ(region.selected_logical_sites, 2U);

  const fs::path file = temporary.path() / "roundtrip.exr";
  ComputeIoExecutor executor({4U, 4U * 1024U * 1024U});
  const auto write = submit_openexr_deep_write(
      executor, original, file.string(), 4096U, make_transaction_token());
  ASSERT_TRUE(write.accepted());
  const auto write_result = write.completion().wait();
  ASSERT_EQ(write_result.status(), ComputeIoCompletionStatus::Succeeded);
  ASSERT_TRUE(fs::exists(file));

  const auto read = submit_openexr_deep_read(executor, registry, file.string(),
                                             4096U, make_transaction_token());
  ASSERT_TRUE(read.io_submission().accepted());
  const Value decoded = read.wait();
  EXPECT_EQ(decoded.provider_generation(), original.provider_generation());
  expect_image_equal(expected, inspect_openexr_deep_value(decoded));
  EXPECT_EQ(compute_descriptor_digest(original.provider_defined_descriptor()),
            compute_descriptor_digest(decoded.provider_defined_descriptor()));
  EXPECT_EQ(compute_storage_layout_digest(original.provider_defined_layout()),
            compute_storage_layout_digest(decoded.provider_defined_layout()));
  const auto original_content = compute_content_digest(original);
  const auto decoded_content = compute_content_digest(decoded);
  ASSERT_EQ(original_content.state, ContentDigestState::Available);
  ASSERT_EQ(decoded_content.state, ContentDigestState::Available);
  EXPECT_EQ(original_content.digest, decoded_content.digest);
  executor.shutdown();
  EXPECT_EQ(closes->load(std::memory_order_relaxed), 0U);
}

/**
 * @brief Proves missing metadata and unsupported file/channel forms reject by
 * type.
 * @throws Exceptions from fixture generation or bounded I/O.
 */
TEST(OpenExrDeepScanlineProvider, RejectsMissingMappingAndUnsupportedInputs) {
  TemporaryDirectory temporary;
  auto registry = std::make_shared<DataDefinitionRegistry>();
  (void)load_initial_provider(*registry);
  const fs::path unmapped = temporary.path() / "unmapped.exr";
  const fs::path shallow = temporary.path() / "shallow.exr";
  const fs::path deep_tiled = temporary.path() / "deep-tiled.exr";
  const fs::path multipart = temporary.path() / "multipart.exr";
  const fs::path mixed_channels = temporary.path() / "mixed-channels.exr";
  write_unmapped_deep_file(unmapped);
  write_shallow_file(shallow);
  write_deep_tiled_file(deep_tiled);
  write_multipart_file(multipart);
  write_mixed_channel_type_file(mixed_channels);
  ComputeIoExecutor executor({2U, 1024U * 1024U});

  const auto missing = submit_openexr_deep_read(
      executor, registry, unmapped.string(), 1024U, make_transaction_token());
  ASSERT_TRUE(missing.io_submission().accepted());
  try {
    (void)missing.wait();
    FAIL() << "Missing mapping metadata unexpectedly decoded";
  } catch (const OpenExrDeepError& error) {
    EXPECT_EQ(error.code(), OpenExrDeepErrorCode::MissingMappingMetadata);
  }

  const auto unsupported = submit_openexr_deep_read(
      executor, registry, shallow.string(), 1024U, make_transaction_token());
  ASSERT_TRUE(unsupported.io_submission().accepted());
  try {
    (void)unsupported.wait();
    FAIL() << "Shallow input unexpectedly decoded";
  } catch (const OpenExrDeepError& error) {
    EXPECT_EQ(error.code(), OpenExrDeepErrorCode::UnsupportedFileShape);
  }

  for (const fs::path& unsupported_path : {deep_tiled, multipart}) {
    const auto typed_rejection =
        submit_openexr_deep_read(executor, registry, unsupported_path.string(),
                                 1024U, make_transaction_token());
    ASSERT_TRUE(typed_rejection.io_submission().accepted());
    try {
      (void)typed_rejection.wait();
      FAIL() << "Unsupported OpenEXR shape unexpectedly decoded";
    } catch (const OpenExrDeepError& error) {
      EXPECT_EQ(error.code(), OpenExrDeepErrorCode::UnsupportedFileShape)
          << unsupported_path << ": " << error.what();
    }
  }

  const auto unsupported_channel =
      submit_openexr_deep_read(executor, registry, mixed_channels.string(),
                               1024U, make_transaction_token());
  ASSERT_TRUE(unsupported_channel.io_submission().accepted());
  try {
    (void)unsupported_channel.wait();
    FAIL() << "Mixed FP32/HALF channels unexpectedly decoded";
  } catch (const OpenExrDeepError& error) {
    EXPECT_EQ(error.code(), OpenExrDeepErrorCode::UnsupportedChannel)
        << error.what();
  }

  executor.shutdown();
}

/**
 * @brief Proves a valid all-zero deep image round-trips without fake payload
 * buffers while retaining generic query, Region, DataSpec, and digest facts.
 * @throws Exceptions from provider loading, codec I/O, or generic contracts.
 */
TEST(OpenExrDeepScanlineProvider, ZeroSampleRoundTripOmitsEmptyPayloadBuffers) {
  TemporaryDirectory temporary;
  auto registry = std::make_shared<DataDefinitionRegistry>();
  (void)load_initial_provider(*registry);
  const fs::path source_file = temporary.path() / "zero-samples-source.exr";
  const fs::path roundtrip_file =
      temporary.path() / "zero-samples-roundtrip.exr";
  write_zero_sample_file(source_file);
  ComputeIoExecutor executor({2U, 1024U * 1024U});

  const auto first_read =
      submit_openexr_deep_read(executor, registry, source_file.string(), 1024U,
                               make_transaction_token());
  ASSERT_TRUE(first_read.io_submission().accepted());
  const Value decoded = first_read.wait();
  ASSERT_EQ(decoded.buffer_count(), 2U);
  ASSERT_EQ(decoded.provider_defined_layout().buffers.size(), 2U);
  const OpenExrDeepImage zero_image = inspect_openexr_deep_value(decoded);
  ASSERT_EQ(zero_image.sample_counts, std::vector<std::uint32_t>({0U}));
  ASSERT_EQ(zero_image.channel_samples.size(), 1U);
  EXPECT_TRUE(zero_image.channel_samples[0].empty());
  const auto sample_count =
      decoded.query_property({kDeclaredSampleCountProperty});
  ASSERT_EQ(sample_count.state, PropertyQueryState::Available);
  ASSERT_TRUE(sample_count.unsigned_value.has_value());
  EXPECT_EQ(*sample_count.unsigned_value, 0U);
  const auto whole = decoded.evaluate_region(RegionSet::whole());
  EXPECT_EQ(whole.state, ProviderRegionState::Exact);
  EXPECT_EQ(whole.selected_logical_sites, 1U);
  const auto spec = decoded.evaluate_data_spec(
      {kVariableSampleFieldSchemaIdentity, 1U, 1U, 1U, 1U});
  EXPECT_EQ(spec.relation, DataSpecRelation::Subset);
  const auto first_content = compute_content_digest(decoded);
  ASSERT_EQ(first_content.state, ContentDigestState::Available);

  const auto write =
      submit_openexr_deep_write(executor, decoded, roundtrip_file.string(),
                                1024U, make_transaction_token());
  ASSERT_TRUE(write.accepted());
  ASSERT_EQ(write.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);
  const auto second_read =
      submit_openexr_deep_read(executor, registry, roundtrip_file.string(),
                               1024U, make_transaction_token());
  ASSERT_TRUE(second_read.io_submission().accepted());
  const Value roundtripped = second_read.wait();
  expect_image_equal(zero_image, inspect_openexr_deep_value(roundtripped));
  EXPECT_EQ(
      compute_descriptor_digest(decoded.provider_defined_descriptor()),
      compute_descriptor_digest(roundtripped.provider_defined_descriptor()));
  EXPECT_EQ(
      compute_storage_layout_digest(decoded.provider_defined_layout()),
      compute_storage_layout_digest(roundtripped.provider_defined_layout()));
  const auto second_content = compute_content_digest(roundtripped);
  ASSERT_EQ(second_content.state, ContentDigestState::Available);
  EXPECT_EQ(first_content.digest, second_content.digest);
  executor.shutdown();
}

/**
 * @brief Proves mapping parsing rejects oversized and noncanonical fields
 * before unbounded decoded-name or channel staging.
 * @throws Nothing; malformed inputs are expected to throw invalid_argument.
 */
TEST(OpenExrDeepScanlineProvider, RejectsUnboundedMappingAttributeFraming) {
  std::string oversized_name(kMappingAttributeMarker);
  oversized_name.append("\n");
  oversized_name.append(512U, 'a');
  oversized_name.append("|00000000000001000000000000000101|");
  oversized_name.append("00000000000009000000000000000901|1024\n");
  EXPECT_THROW((void)decode_mapping_attribute(oversized_name),
               std::invalid_argument);

  const std::string noncanonical_role =
      std::string(kMappingAttributeMarker) +
      "\n61|00000000000001000000000000000101|"
      "00000000000009000000000000000901|+1024\n";
  EXPECT_THROW((void)decode_mapping_attribute(noncanonical_role),
               std::invalid_argument);
}

/**
 * @brief Proves truncated payload failures cross the codec boundary by value.
 * @throws Exceptions from provider loading, codec I/O, or file truncation.
 */
TEST(OpenExrDeepScanlineProvider, TranslatesTruncatedFileFailure) {
  TemporaryDirectory temporary;
  auto registry = std::make_shared<DataDefinitionRegistry>();
  (void)load_initial_provider(*registry);
  const Value value = make_openexr_deep_value(*registry, make_image());
  const fs::path file = temporary.path() / "truncated.exr";
  ComputeIoExecutor executor({2U, 1024U * 1024U});
  const auto write = submit_openexr_deep_write(executor, value, file.string(),
                                               4096U, make_transaction_token());
  ASSERT_TRUE(write.accepted());
  ASSERT_EQ(write.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);
  const std::uintmax_t complete_size = fs::file_size(file);
  ASSERT_GT(complete_size, 1U);
  fs::resize_file(file, complete_size - 1U);

  const auto read = submit_openexr_deep_read(executor, registry, file.string(),
                                             4096U, make_transaction_token());
  ASSERT_TRUE(read.io_submission().accepted());
  try {
    (void)read.wait();
    FAIL() << "Truncated OpenEXR file unexpectedly decoded";
  } catch (const OpenExrDeepError& error) {
    EXPECT_EQ(error.code(), OpenExrDeepErrorCode::CorruptOrIncompleteFile)
        << error.what();
  }
  executor.shutdown();
}

/**
 * @brief Proves dual-budget rejection is lazy and creates no output side
 * effect.
 * @throws Exceptions from provider loading, task synchronization, or codec I/O.
 */
TEST(OpenExrDeepScanlineProvider, AdmissionRejectsBeforePathSideEffects) {
  TemporaryDirectory temporary;
  auto registry = std::make_shared<DataDefinitionRegistry>();
  (void)load_initial_provider(*registry);
  const Value value = make_openexr_deep_value(*registry, make_image());
  const fs::path first = temporary.path() / "first.exr";
  const fs::path rejected = temporary.path() / "rejected.exr";
  auto gate = std::make_shared<BlockingGate>();
  OpenExrDeepIoHooks hooks;
  hooks.before_codec = [gate]() { gate->enter_and_wait(); };
  ComputeIoExecutor executor({1U, 8192U});
  const auto accepted = submit_openexr_deep_write(
      executor, value, first.string(), 4096U, make_transaction_token(), hooks);
  ASSERT_TRUE(accepted.accepted());
  gate->wait_until_entered();

  const auto denied = submit_openexr_deep_write(
      executor, value, rejected.string(), 4096U, make_transaction_token());
  EXPECT_EQ(denied.admission_status(), ComputeIoAdmissionStatus::TaskLimit);
  EXPECT_FALSE(denied.completion().active());
  EXPECT_FALSE(fs::exists(rejected));
  const auto snapshot = executor.snapshot();
  EXPECT_EQ(snapshot.active_tasks, 1U);
  EXPECT_EQ(snapshot.active_planned_bytes, 4096U);

  gate->release();
  EXPECT_EQ(accepted.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);
  EXPECT_TRUE(fs::exists(first));
  executor.shutdown();
}

/**
 * @brief Proves replacement, running cancellation, Value/read/owner leases,
 * and final DSO close obey the existing generation ordering.
 * @throws Exceptions from provider loading, task synchronization, or codec I/O.
 */
TEST(OpenExrDeepScanlineProvider,
     ReplacementAndCancellationRetainOldGenerationUntilLastLease) {
  TemporaryDirectory temporary;
  auto registry = std::make_shared<DataDefinitionRegistry>();
  auto first_closes = std::make_shared<std::atomic<std::uint64_t>>(0U);
  const DataProviderLoadResult first_load =
      load_provider(*registry, first_closes);
  ASSERT_TRUE(first_load.ok()) << first_load.diagnostic;
  std::optional<Value> old_value =
      make_openexr_deep_value(*registry, make_image());
  std::optional<ProviderReadLease> old_read =
      old_value->acquire_provider_read(0U);
  std::optional<ProviderOwner> old_owner = old_value->create_provider_owner();

  auto gate = std::make_shared<BlockingGate>();
  OpenExrDeepIoHooks hooks;
  hooks.before_codec = [gate]() { gate->enter_and_wait(); };
  ComputeIoExecutor executor({2U, 8192U});
  const fs::path file = temporary.path() / "cancelled-late.exr";
  const auto running =
      submit_openexr_deep_write(executor, *old_value, file.string(), 4096U,
                                make_transaction_token(), hooks);
  ASSERT_TRUE(running.accepted());
  gate->wait_until_entered();

  auto second_closes = std::make_shared<std::atomic<std::uint64_t>>(0U);
  const DataProviderLoadResult replacement =
      load_provider(*registry, second_closes);
  ASSERT_TRUE(replacement.ok()) << replacement.diagnostic;
  ASSERT_GT(replacement.generation, first_load.generation);
  std::optional<Value> new_value =
      make_openexr_deep_value(*registry, make_image());
  EXPECT_EQ(new_value->provider_generation(), replacement.generation);
  EXPECT_EQ(old_value->provider_generation(), first_load.generation);
  EXPECT_EQ(old_read->provider_generation(), first_load.generation);
  EXPECT_EQ(old_owner->provider_generation(), first_load.generation);
  EXPECT_EQ(first_closes->load(std::memory_order_relaxed), 0U);

  EXPECT_TRUE(running.completion().cancel());
  gate->release();
  EXPECT_EQ(running.completion().wait().status(),
            ComputeIoCompletionStatus::Cancelled);
  EXPECT_EQ(first_closes->load(std::memory_order_relaxed), 0U);
  ASSERT_TRUE(registry->unload(kProviderIdentity));
  new_value.reset();
  EXPECT_EQ(second_closes->load(std::memory_order_relaxed), 1U);
  old_value.reset();
  EXPECT_EQ(first_closes->load(std::memory_order_relaxed), 0U);
  old_read.reset();
  EXPECT_EQ(first_closes->load(std::memory_order_relaxed), 0U);
  old_owner.reset();
  EXPECT_EQ(first_closes->load(std::memory_order_relaxed), 1U);
  executor.shutdown();
}

/**
 * @brief Proves a cancelled late read hides its result but retains its exact
 * decoded generation until the submission handle releases.
 * @throws Exceptions from provider loading, task synchronization, or codec I/O.
 */
TEST(OpenExrDeepScanlineProvider,
     CancelledReadRetainsDecodedGenerationUntilHandleRelease) {
  TemporaryDirectory temporary;
  auto registry = std::make_shared<DataDefinitionRegistry>();
  auto first_closes = std::make_shared<std::atomic<std::uint64_t>>(0U);
  const DataProviderLoadResult first_load =
      load_provider(*registry, first_closes);
  ASSERT_TRUE(first_load.ok()) << first_load.diagnostic;
  std::optional<Value> source =
      make_openexr_deep_value(*registry, make_image());
  const fs::path file = temporary.path() / "cancelled-read.exr";
  ComputeIoExecutor executor({2U, 8192U});
  const auto write = submit_openexr_deep_write(executor, *source, file.string(),
                                               4096U, make_transaction_token());
  ASSERT_TRUE(write.accepted());
  ASSERT_EQ(write.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);

  auto gate = std::make_shared<BlockingGate>();
  OpenExrDeepIoHooks hooks;
  hooks.before_read_publication = [gate]() { gate->enter_and_wait(); };
  std::optional<OpenExrDeepReadSubmission> read =
      submit_openexr_deep_read(executor, registry, file.string(), 4096U,
                               make_transaction_token(), hooks);
  ASSERT_TRUE(read->io_submission().accepted());
  gate->wait_until_entered();

  auto second_closes = std::make_shared<std::atomic<std::uint64_t>>(0U);
  const DataProviderLoadResult replacement =
      load_provider(*registry, second_closes);
  ASSERT_TRUE(replacement.ok()) << replacement.diagnostic;
  ASSERT_GT(replacement.generation, first_load.generation);
  EXPECT_TRUE(read->io_submission().completion().cancel());
  ASSERT_TRUE(registry->unload(kProviderIdentity));
  EXPECT_EQ(second_closes->load(std::memory_order_relaxed), 1U);
  source.reset();
  EXPECT_EQ(first_closes->load(std::memory_order_relaxed), 0U);

  gate->release();
  EXPECT_EQ(read->io_submission().completion().wait().status(),
            ComputeIoCompletionStatus::Cancelled);
  try {
    (void)read->wait();
    FAIL() << "Cancelled late read unexpectedly published its Value";
  } catch (const OpenExrDeepError& error) {
    EXPECT_EQ(error.code(), OpenExrDeepErrorCode::Cancelled);
  }
  EXPECT_EQ(first_closes->load(std::memory_order_relaxed), 0U);
  read.reset();
  EXPECT_EQ(first_closes->load(std::memory_order_relaxed), 1U);
  executor.shutdown();
}

}  // namespace
