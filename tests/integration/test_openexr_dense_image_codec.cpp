#include <Imath/half.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfPartType.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "adapters/openexr/openexr_dense_image_codec.hpp"
#include "photospider/data/image_view.hpp"

/**
 * @file test_openexr_dense_image_codec.cpp
 * @brief Ordinary OpenEXR DenseImage Value round-trip contract tests.
 */

namespace ps::openexr_dense {
namespace {

namespace Imf = OPENEXR_IMF_INTERNAL_NAMESPACE;
namespace Imath = IMATH_NAMESPACE;

/**
 * @brief Recoverable isolated test directory with one cross-process identity.
 * @throws std::filesystem::filesystem_error when creation fails.
 */
class TempDirectory final {
 public:
  /**
   * @brief Creates one exclusively owned directory below the temporary root.
   * @throws std::filesystem::filesystem_error when creation fails.
   * @note The process id separates CTest's independently launched GoogleTest
   * processes; the atomic ordinal separates multiple owners in one process.
   */
  TempDirectory() {
    static std::atomic<std::uint64_t> next{1U};
    const std::filesystem::path temporary_root =
        std::filesystem::temp_directory_path();
    for (;;) {
      const std::filesystem::path candidate =
          temporary_root /
          ("photospider-openexr-dense-" +
           std::to_string(static_cast<std::uint64_t>(::getpid())) + "-" +
           std::to_string(next.fetch_add(1U, std::memory_order_relaxed)));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        return;
      }
      if (error) {
        throw std::filesystem::filesystem_error(
            "cannot create OpenEXR test directory", candidate, error);
      }
    }
  }

  /**
   * @brief Removes only the uniquely owned test directory.
   * @throws Nothing; cleanup errors are intentionally ignored.
   */
  ~TempDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  /**
   * @brief Returns the retained unique directory path.
   * @return Borrowed path valid for this owner lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Exact directory exclusively owned by this fixture. */
  std::filesystem::path path_;
};

/**
 * @brief Returns the OpenEXR pixel type corresponding to one test scalar.
 * @tparam Scalar Supported uint32, float, or half scalar.
 * @return Exact file channel type.
 * @throws Nothing.
 */
template <typename Scalar>
constexpr Imf::PixelType pixel_type() noexcept {
  if constexpr (std::is_same_v<Scalar, std::uint32_t>) {
    return Imf::UINT;
  }
  if constexpr (std::is_same_v<Scalar, float>) {
    return Imf::FLOAT;
  }
  return Imf::HALF;
}

/**
 * @brief Writes one complete channel-major shallow scanline fixture.
 * @tparam Scalar Supported OpenEXR native scalar.
 * @param path New output path.
 * @param data_window Inclusive payload window.
 * @param display_window Independent inclusive presentation window.
 * @param names Unique file channel names.
 * @param planes Channel-major payload with equal full-resolution planes.
 * @throws OpenEXR/Iex, allocation, or invalid fixture exceptions.
 */
template <typename Scalar>
void write_fixture(const std::filesystem::path& path,
                   const Imath::Box2i& data_window,
                   const Imath::Box2i& display_window,
                   const std::vector<std::string>& names,
                   std::vector<Scalar>* planes) {
  const std::size_t width =
      static_cast<std::size_t>(data_window.max.x - data_window.min.x + 1);
  const std::size_t height =
      static_cast<std::size_t>(data_window.max.y - data_window.min.y + 1);
  const std::size_t sites = width * height;
  if (planes == nullptr || names.empty() ||
      planes->size() != names.size() * sites) {
    throw std::invalid_argument("OpenEXR test fixture shape is invalid.");
  }
  Imf::Header header(display_window, data_window, 1.0F, Imath::V2f(0, 0), 1.0F,
                     Imf::INCREASING_Y, Imf::ZIP_COMPRESSION);
  header.setType(Imf::SCANLINEIMAGE);
  for (const std::string& name : names) {
    header.channels().insert(name, Imf::Channel(pixel_type<Scalar>(), 1, 1));
  }
  Imf::FrameBuffer frame_buffer;
  for (std::size_t channel = 0U; channel < names.size(); ++channel) {
    frame_buffer.insert(
        names[channel],
        Imf::Slice::Make(pixel_type<Scalar>(), planes->data() + channel * sites,
                         data_window, sizeof(Scalar), width * sizeof(Scalar)));
  }
  Imf::OutputFile output(path.string().c_str(), header, 0);
  output.setFrameBuffer(frame_buffer);
  output.writePixels(static_cast<int>(height));
}

/**
 * @brief Creates one explicit decoded-sample endpoint.
 * @param kind Sample-domain kind.
 * @param minimum Finite inclusive lower bound.
 * @param maximum Finite inclusive upper bound.
 * @return Version-one Value-encoded endpoint.
 * @throws Nothing.
 */
SampleEndpoint endpoint(SampleDomainKind kind, double minimum,
                        double maximum) noexcept {
  return {SampleEncoding{1U, SampleEncodingKind::Value},
          SampleDomain{kind, minimum, maximum}};
}

/**
 * @brief Reads one interleaved Value in row-major pixel/channel order.
 * @tparam Scalar Exact native Value scalar.
 * @param value Ready host-readable ordinary image.
 * @return Row-major interleaved scalar copy.
 * @throws ImageView validation, coordinate, or allocation exceptions.
 */
template <typename Scalar>
std::vector<Scalar> read_value(const Value& value) {
  const ImageView view(value);
  std::vector<Scalar> result(view.width() * view.height() * view.channels());
  std::size_t index = 0U;
  for (std::size_t y = 0U; y < view.height(); ++y) {
    for (std::size_t x = 0U; x < view.width(); ++x) {
      for (std::size_t channel = 0U; channel < view.channels(); ++channel) {
        std::memcpy(&result[index], view.channel_data(x, y, channel),
                    sizeof(Scalar));
        ++index;
      }
    }
  }
  return result;
}

TEST(OpenExrDenseImageCodec, FloatRoundTripPreservesIndependentWindows) {
  TempDirectory directory;
  const std::filesystem::path input = directory.path() / "input.exr";
  const std::filesystem::path output = directory.path() / "output.exr";
  const Imath::Box2i data_window{{-3, 4}, {-1, 5}};
  const Imath::Box2i display_window{{-8, -2}, {7, 9}};
  const std::vector<std::string> names{"B", "R"};
  std::vector<float> planes{1.0F,  2.0F,  3.0F,  4.0F,  5.0F,  6.0F,
                            -1.0F, -2.0F, -3.0F, -4.0F, -5.0F, -6.0F};
  write_fixture(input, data_window, display_window, names, &planes);

  OpenExrDenseImageCodec codec;
  const SampleEndpoint samples = endpoint(SampleDomainKind::Legal, -16.0, 16.0);
  const ImageArtifactDecodeRequest request{
      {ImageArtifactDecodeRule{ElementSemantics::FloatingPoint,
                               StorageEncoding{32U}, samples, std::nullopt}}};
  const Value decoded = codec.decode(input, request);
  ASSERT_TRUE(decoded.image_facet().has_value());
  EXPECT_EQ(decoded.image_facet()->data_window, (ImageBounds{-3, 4, 0, 6}));
  ASSERT_TRUE(decoded.image_facet()->display_window.has_value());
  EXPECT_EQ(*decoded.image_facet()->display_window,
            (ImageBounds{-8, -2, 8, 10}));
  ASSERT_TRUE(decoded.image_facet()->channel_schema.has_value());
  ASSERT_EQ(decoded.image_facet()->channel_schema->channels.size(), 2U);
  EXPECT_EQ(decoded.image_facet()->channel_schema->channels[0].diagnostic_name,
            "B");
  EXPECT_EQ(decoded.image_facet()->channel_schema->channels[1].diagnostic_name,
            "R");
  EXPECT_EQ(read_value<float>(decoded),
            (std::vector<float>{1.0F, -1.0F, 2.0F, -2.0F, 3.0F, -3.0F, 4.0F,
                                -4.0F, 5.0F, -5.0F, 6.0F, -6.0F}));

  codec.encode(output, decoded, {});
  const Value round_trip = codec.decode(output, request);
  EXPECT_EQ(round_trip.image_facet()->data_window,
            decoded.image_facet()->data_window);
  EXPECT_EQ(round_trip.image_facet()->display_window,
            decoded.image_facet()->display_window);
  EXPECT_EQ(read_value<float>(round_trip), read_value<float>(decoded));
}

TEST(OpenExrDenseImageCodec, HalfDecodePromotesExactlyToFp32) {
  TempDirectory directory;
  const std::filesystem::path input = directory.path() / "half.exr";
  const Imath::Box2i data_window{{5, -3}, {6, -2}};
  const Imath::Box2i display_window{{0, -8}, {10, 4}};
  const std::vector<std::string> names{"Y"};
  std::vector<Imath::half> planes{Imath::half(0.5F), Imath::half(-2.0F),
                                  Imath::half(3.25F), Imath::half(0.0F)};
  write_fixture(input, data_window, display_window, names, &planes);

  OpenExrDenseImageCodec codec;
  const SampleEndpoint samples = endpoint(SampleDomainKind::Legal, -4.0, 4.0);
  const Value decoded = codec.decode(
      input,
      {{ImageArtifactDecodeRule{ElementSemantics::FloatingPoint,
                                StorageEncoding{32U}, samples, std::nullopt}}});
  EXPECT_EQ(decoded.dense_tensor_descriptor().element_semantics,
            ElementSemantics::FloatingPoint);
  EXPECT_EQ(decoded.dense_tensor_descriptor().storage_encoding,
            (StorageEncoding{32U}));
  EXPECT_EQ(read_value<float>(decoded),
            (std::vector<float>{0.5F, -2.0F, 3.25F, 0.0F}));
  EXPECT_EQ(decoded.image_facet()->data_window, (ImageBounds{5, -3, 7, -1}));
  EXPECT_EQ(*decoded.image_facet()->display_window,
            (ImageBounds{0, -8, 11, 5}));
}

TEST(OpenExrDenseImageCodec, UintRoundTripPreservesFullCodeValues) {
  TempDirectory directory;
  const std::filesystem::path input = directory.path() / "uint.exr";
  const std::filesystem::path output = directory.path() / "uint-out.exr";
  const Imath::Box2i data_window{{-1, -1}, {0, 0}};
  const Imath::Box2i display_window{{-2, -2}, {2, 2}};
  const std::vector<std::string> names{"code"};
  std::vector<std::uint32_t> planes{0U, 65535U, 4000000000U, 4294967295U};
  write_fixture(input, data_window, display_window, names, &planes);

  OpenExrDenseImageCodec codec;
  const SampleEndpoint samples =
      endpoint(SampleDomainKind::CodeValue, 0.0, 4294967295.0);
  const ImageArtifactDecodeRequest request{
      {ImageArtifactDecodeRule{ElementSemantics::UnsignedInteger,
                               StorageEncoding{32U}, samples, std::nullopt}}};
  const Value decoded = codec.decode(input, request);
  EXPECT_EQ(read_value<std::uint32_t>(decoded), planes);
  codec.encode(output, decoded, {});
  EXPECT_EQ(read_value<std::uint32_t>(codec.decode(output, request)), planes);
}

TEST(OpenExrDenseImageCodec, RejectsMixedChannelStorageWithoutConversionGuess) {
  TempDirectory directory;
  const std::filesystem::path input = directory.path() / "mixed.exr";
  const Imath::Box2i window{{0, 0}, {0, 0}};
  Imf::Header header(window, window);
  header.setType(Imf::SCANLINEIMAGE);
  header.channels().insert("F", Imf::Channel(Imf::FLOAT));
  header.channels().insert("U", Imf::Channel(Imf::UINT));
  float floating = 1.0F;
  std::uint32_t integer = 1U;
  Imf::FrameBuffer frame_buffer;
  frame_buffer.insert("F",
                      Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(&floating),
                                 sizeof(float), sizeof(float)));
  frame_buffer.insert("U",
                      Imf::Slice(Imf::UINT, reinterpret_cast<char*>(&integer),
                                 sizeof(integer), sizeof(integer)));
  {
    Imf::OutputFile output(input.string().c_str(), header);
    output.setFrameBuffer(frame_buffer);
    output.writePixels(1);
  }

  OpenExrDenseImageCodec codec;
  const SampleEndpoint samples = endpoint(SampleDomainKind::Legal, 0.0, 1.0);
  const ImageArtifactDecodeRequest request{
      {ImageArtifactDecodeRule{ElementSemantics::UnsignedInteger,
                               StorageEncoding{32U}, samples, std::nullopt},
       ImageArtifactDecodeRule{ElementSemantics::FloatingPoint,
                               StorageEncoding{32U}, samples, std::nullopt}}};
  EXPECT_THROW((void)codec.decode(input, request), std::invalid_argument);
}

}  // namespace
}  // namespace ps::openexr_dense
