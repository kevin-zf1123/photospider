#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <opencv2/core.hpp>
#include <string>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#include "core/pending_value.hpp"  // NOLINT(build/include_subdir)
#include "core/ps_types.hpp"       // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"   // NOLINT(build/include_subdir)
#include "photospider/core/graph_error.hpp"
#include "photospider/data/image_view.hpp"
#include "providers/opencv/opencv_operation_provider.hpp"
#include "providers/opencv/opencv_operation_provider_test_access.hpp"

namespace ps::providers::opencv {
namespace {

/**
 * @brief Publishes one signed-window four-channel image with rich semantics.
 *
 * @return Fresh host-readable FP32 Value with B/G/R/A identities, three channel
 *         groups, per-channel sample overrides, and color interpretation.
 * @throws std::invalid_argument, std::overflow_error, or std::length_error
 *         when fixture metadata violates the dense-image contract.
 * @throws std::bad_alloc when metadata or payload allocation fails.
 * @note The fixture makes channel selection observable without inferring any
 *       role from diagnostic names. It owns all storage through the returned
 *       immutable Value.
 */
Value make_semantic_channel_image() {
  constexpr std::array<float, 8U> kSamples{1.0F, 2.0F, 3.0F, 4.0F,
                                           5.0F, 6.0F, 7.0F, 8.0F};
  std::vector<std::byte> storage(sizeof(kSamples));
  std::memcpy(storage.data(), kSamples.data(), storage.size());

  DenseTensorDescriptor descriptor{{1U, 2U, 4U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.data_window = ImageBounds{-9, 14, -7, 15};
  facet.display_window = ImageBounds{-20, 10, 20, 30};

  ChannelSchema schema;
  schema.channels = {{ChannelId{10U}, "B"},
                     {ChannelId{20U}, "G"},
                     {ChannelId{30U}, "R"},
                     {ChannelId{40U}, "A"}};
  schema.groups = {{ChannelGroupId{100U},
                    "color",
                    {ChannelId{10U}, ChannelId{20U}, ChannelId{30U}}},
                   {ChannelGroupId{150U}, "red-only", {ChannelId{30U}}},
                   {ChannelGroupId{200U}, "alpha", {ChannelId{40U}}}};
  facet.channel_schema = std::move(schema);
  facet.sample_domain = SampleDomainFacet{
      1U,
      SampleEncoding{1U, SampleEncodingKind::Normalized},
      SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0},
      {{ChannelId{10U}, SampleDomain{SampleDomainKind::Legal, -2.0, 2.0}},
       {ChannelId{30U}, SampleDomain{SampleDomainKind::Legal, -1.0, 1.0}},
       {ChannelId{40U}, SampleDomain{SampleDomainKind::Legal, 0.0, 2.0}}}};
  facet.color =
      ColorFacet{1U, ChannelGroupId{150U}, ColorTransferFunction::Srgb,
                 ColorPrimaries::DisplayP3D65};

  return Value::from_cpu_dense_tensor(std::move(descriptor), std::move(facet),
                                      StridedLayout{{32, 16, 4}},
                                      std::move(storage));
}

/**
 * @brief Executes the registered full-image channel extractor.
 *
 * @param source Retained host-readable input Value.
 * @param selector Stable operation parameter spelling such as `r` or `a`.
 * @return Fresh single-channel provider output.
 * @throws GraphError when the callback is unavailable or rejects the channel.
 * @throws std::invalid_argument, std::overflow_error, std::length_error, or
 *         std::bad_alloc from metadata projection and Value publication.
 * @note The returned Value owns fresh output storage; no OpenCV matrix or
 *       source payload alias escapes the synchronous callback.
 */
NodeOutput execute_channel_extract(const Value& source,
                                   const std::string& selector) {
  auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "extract_channel", ComputeIntent::GlobalHighPrecision);
  if (!resolved ||
      !std::holds_alternative<MonolithicOpFunc>(resolved.value())) {
    throw GraphError(GraphErrc::NoOperation,
                     "image_process:extract_channel is not monolithic");
  }

  Node node;
  node.id = 201;
  node.type = "image_process";
  node.subtype = "extract_channel";
  node.runtime_parameters["channel"] = selector;
  NodeOutput input;
  input.publish_image_value(source);
  return std::get<MonolithicOpFunc>(resolved.value())(node, {&input});
}

/**
 * @brief Publishes one Ready imported image with no host-readable storage.
 *
 * @return Canonical NodeOutput containing a signed-window CUDA Imported Value.
 * @throws std::invalid_argument, std::out_of_range, std::overflow_error,
 *         std::length_error, or std::bad_alloc from device Value publication.
 * @throws std::logic_error when the test producer cannot publish Ready.
 * @note The retained fake native owner supplies no host pointer. Consumers may
 *       inspect metadata but must not acquire a payload lease or ImageView.
 */
NodeOutput make_imported_cached_image() {
  DenseTensorDescriptor descriptor{{7U, 11U, 4U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.data_window = ImageBounds{-37, 19, -26, 26};
  auto native_owner = std::make_shared<int>(202);
  PendingDeviceValuePublication publication =
      PendingDeviceValuePublisher::publish_dense_tensor(
          std::move(descriptor), std::move(facet), StridedLayout{{44, 4, 1}},
          native_owner, native_owner.get(), nullptr, 308U,
          DeviceId(DeviceBackend::CUDA), MemoryDomain::Imported);
  if (!publication.producer.complete_ready()) {
    throw std::logic_error("imported fixture failed to publish Ready");
  }
  NodeOutput output;
  output.publish_image_value(std::move(publication.value));
  return output;
}

/**
 * @brief Proves initialization retry and both provider exception fences.
 *
 * @throws Nothing when all GTest assertions pass; tested exceptions are caught
 *         and inspected inside the case.
 * @note CTest discovery launches this case in its own filtered process, so its
 *       first `register_provider()` call precedes successful OpenCV provider
 *       initialization. The private hooks are compiled out of production
 *       builds and inject OpenCV status objects rather than attempting real
 *       resource exhaustion. The later metadata cases initialize themselves.
 */
TEST(OpenCvOperationProviderExceptionContract,
     InitializationRetryAndFencesTranslate) {
  constexpr char kResizeType[] = "image_process";
  constexpr char kResizeSubtype[] = "resize";

  EXPECT_FALSE(OpRegistry::instance()
                   .resolve_for_intent(kResizeType, kResizeSubtype,
                                       ComputeIntent::GlobalHighPrecision)
                   .has_value());

  set_opencv_process_initialization_failure_for_testing(cv::Error::StsError);
  bool initialization_translated = false;
  try {
    register_provider();
    ADD_FAILURE() << "injected OpenCV initialization unexpectedly succeeded";
  } catch (const GraphError& error) {
    initialization_translated = true;
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
    EXPECT_NE(std::string(error.what()).find("initialization"),
              std::string::npos);
  } catch (const cv::Exception& error) {
    ADD_FAILURE() << "OpenCV initialization exception escaped: "
                  << error.what();
  } catch (const std::exception& error) {
    ADD_FAILURE() << "unexpected initialization exception: " << error.what();
  } catch (...) {
    ADD_FAILURE() << "unknown initialization exception";
  }
  EXPECT_TRUE(initialization_translated);
  EXPECT_FALSE(OpRegistry::instance()
                   .resolve_for_intent(kResizeType, kResizeSubtype,
                                       ComputeIntent::GlobalHighPrecision)
                   .has_value());

  EXPECT_NO_THROW(register_provider());
  EXPECT_TRUE(OpRegistry::instance()
                  .resolve_for_intent(kResizeType, kResizeSubtype,
                                      ComputeIntent::GlobalHighPrecision)
                  .has_value());
  EXPECT_EQ(cv::getNumThreads(), 1);

  int exact_bad_alloc_count = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    try {
      invoke_monolithic_opencv_exception_fence_for_testing(cv::Error::StsNoMem);
      ADD_FAILURE() << "injected OpenCV exhaustion unexpectedly returned";
    } catch (const std::bad_alloc& error) {
      ++exact_bad_alloc_count;
      EXPECT_EQ(typeid(error), typeid(std::bad_alloc));
    } catch (const cv::Exception& error) {
      ADD_FAILURE() << "OpenCV exhaustion exception escaped: " << error.what();
    } catch (const std::exception& error) {
      ADD_FAILURE() << "unexpected exhaustion exception: " << error.what();
    } catch (...) {
      ADD_FAILURE() << "unknown exhaustion exception";
    }
  }
  EXPECT_EQ(exact_bad_alloc_count, 2);

  bool tiled_error_translated = false;
  try {
    invoke_tiled_opencv_exception_fence_for_testing(cv::Error::StsBadArg);
    ADD_FAILURE() << "injected tiled OpenCV failure unexpectedly returned";
  } catch (const GraphError& error) {
    tiled_error_translated = true;
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
    EXPECT_NE(std::string(error.what()).find("testing:tiled_exception_fence"),
              std::string::npos);
  } catch (const cv::Exception& error) {
    ADD_FAILURE() << "tiled OpenCV exception escaped: " << error.what();
  } catch (const std::exception& error) {
    ADD_FAILURE() << "unexpected tiled exception: " << error.what();
  } catch (...) {
    ADD_FAILURE() << "unknown tiled exception";
  }
  EXPECT_TRUE(tiled_error_translated);
}

/**
 * @brief Proves extraction projects channel-dependent ImageFacet semantics.
 *
 * @return Nothing; GoogleTest reports payload or metadata projection failures.
 * @throws Provider resolution, OpenCV execution, metadata validation, and
 *         allocation exceptions unchanged to the test runner.
 * @note A selected color component retains its exact stable singleton group
 *       and color interpretation. Selecting alpha removes the unrelated color
 *       binding while preserving global sample facts and alpha-specific data.
 */
TEST(OpenCvOperationProviderMetadataContract,
     ExtractChannelProjectsSelectedSemanticFacts) {
  ASSERT_NO_THROW(register_provider());
  const Value source = make_semantic_channel_image();

  const NodeOutput red_output = execute_channel_extract(source, "r");
  ASSERT_TRUE(red_output.has_image_value());
  const Value& red_value = red_output.image_value();
  EXPECT_EQ(red_value.dense_tensor_descriptor().shape,
            (std::vector<std::size_t>{1U, 2U, 1U}));
  ASSERT_TRUE(red_value.image_facet().has_value());
  const ImageFacet& red_facet = *red_value.image_facet();
  EXPECT_EQ(red_facet.x_axis, 1U);
  EXPECT_EQ(red_facet.y_axis, 0U);
  EXPECT_EQ(red_facet.channel_axis, std::optional<std::size_t>(2U));
  EXPECT_EQ(red_facet.data_window, (ImageBounds{-9, 14, -7, 15}));
  EXPECT_EQ(red_facet.display_window,
            std::optional<ImageBounds>(ImageBounds{-20, 10, 20, 30}));
  ASSERT_TRUE(red_facet.channel_schema.has_value());
  ASSERT_EQ(red_facet.channel_schema->channels.size(), 1U);
  EXPECT_EQ(red_facet.channel_schema->channels[0].id, ChannelId{30U});
  EXPECT_EQ(red_facet.channel_schema->channels[0].diagnostic_name, "R");
  ASSERT_EQ(red_facet.channel_schema->groups.size(), 1U);
  EXPECT_EQ(red_facet.channel_schema->groups[0].id, ChannelGroupId{150U});
  EXPECT_EQ(red_facet.channel_schema->groups[0].diagnostic_name, "red-only");
  EXPECT_EQ(red_facet.channel_schema->groups[0].members,
            (std::vector<ChannelId>{ChannelId{30U}}));
  ASSERT_TRUE(red_facet.sample_domain.has_value());
  EXPECT_EQ(red_facet.sample_domain->encoding,
            (SampleEncoding{1U, SampleEncodingKind::Normalized}));
  EXPECT_EQ(red_facet.sample_domain->default_domain,
            (SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}));
  EXPECT_EQ(
      red_facet.sample_domain->per_channel,
      (std::vector<ChannelSampleDomain>{
          {ChannelId{30U}, SampleDomain{SampleDomainKind::Legal, -1.0, 1.0}}}));
  EXPECT_EQ(red_facet.color,
            std::optional<ColorFacet>(ColorFacet{
                1U, ChannelGroupId{150U}, ColorTransferFunction::Srgb,
                ColorPrimaries::DisplayP3D65}));
  const ImageView red_view(red_value);
  ASSERT_EQ(red_view.channels(), 1U);
  float red_first = 0.0F;
  float red_second = 0.0F;
  std::memcpy(&red_first, red_view.channel_data(0U, 0U, 0U), sizeof(red_first));
  std::memcpy(&red_second, red_view.channel_data(1U, 0U, 0U),
              sizeof(red_second));
  EXPECT_FLOAT_EQ(red_first, 3.0F);
  EXPECT_FLOAT_EQ(red_second, 7.0F);

  const NodeOutput alpha_output = execute_channel_extract(source, "a");
  ASSERT_TRUE(alpha_output.has_image_value());
  const Value& alpha_value = alpha_output.image_value();
  ASSERT_TRUE(alpha_value.image_facet().has_value());
  const ImageFacet& alpha_facet = *alpha_value.image_facet();
  ASSERT_TRUE(alpha_facet.channel_schema.has_value());
  ASSERT_EQ(alpha_facet.channel_schema->channels.size(), 1U);
  EXPECT_EQ(alpha_facet.channel_schema->channels[0].id, ChannelId{40U});
  EXPECT_EQ(alpha_facet.channel_schema->channels[0].diagnostic_name, "A");
  ASSERT_EQ(alpha_facet.channel_schema->groups.size(), 1U);
  EXPECT_EQ(alpha_facet.channel_schema->groups[0].id, ChannelGroupId{200U});
  EXPECT_EQ(alpha_facet.channel_schema->groups[0].members,
            (std::vector<ChannelId>{ChannelId{40U}}));
  ASSERT_TRUE(alpha_facet.sample_domain.has_value());
  EXPECT_EQ(
      alpha_facet.sample_domain->per_channel,
      (std::vector<ChannelSampleDomain>{
          {ChannelId{40U}, SampleDomain{SampleDomainKind::Legal, 0.0, 2.0}}}));
  EXPECT_FALSE(alpha_facet.color.has_value());
}

/**
 * @brief Proves resize and ratio-crop dirty ROI use cached metadata only.
 *
 * @return Nothing; GoogleTest reports extent or projected ROI failures.
 * @throws Graph/model allocation and registry exceptions unchanged to the
 *         test runner.
 * @note The cached Value is Ready but intentionally non-host-visible. Signed
 *       logical bounds provide an 11x7 span, while callback results remain
 *       zero-based storage-relative PixelRects and never access payload bytes.
 */
TEST(OpenCvOperationProviderMetadataContract,
     ImportedCachedBoundsDriveResizeAndRatioCropDirtyRoi) {
  ASSERT_NO_THROW(register_provider());
  GraphModel graph("opencv-imported-cached-bounds");
  Node source;
  source.id = 202;
  source.type = "image_source";
  source.subtype = "opaque_imported";
  source.cached_output_high_precision = make_imported_cached_image();
  graph.add_node(source);

  const Value& cached =
      graph.node(202).cached_output_high_precision->image_value();
  EXPECT_EQ(cached.image_bounds(), (ImageBounds{-37, 19, -26, 26}));
  EXPECT_EQ(cached.storage_binding().memory_domain, MemoryDomain::Imported);
  EXPECT_FALSE(cached.storage_binding().host_visible);
  EXPECT_THROW((void)ImageView(cached), BufferAccessError);

  Node resize;
  resize.id = 203;
  resize.type = "image_process";
  resize.subtype = "resize";
  resize.image_inputs.push_back(ImageInput{202, "image"});
  resize.runtime_parameters["width"] = 22;
  resize.runtime_parameters["height"] = 14;
  resize.runtime_parameters["interpolation"] = "nearest";
  const DirtyRoiPropFunc resize_dirty =
      OpRegistry::instance().get_dirty_propagator("image_process", "resize");
  EXPECT_EQ(
      resize_dirty(resize, PixelRect{4, 2, 6, 4}, graph, PixelSize{22, 14}, {},
                   resize.runtime_parameters, nullptr),
      (PixelRect{2, 1, 3, 2}));

  Node crop;
  crop.id = 204;
  crop.type = "image_process";
  crop.subtype = "crop";
  crop.image_inputs.push_back(ImageInput{202, "image"});
  crop.runtime_parameters["mode"] = "ratio";
  crop.runtime_parameters["x"] = 0.25;
  crop.runtime_parameters["y"] = 0.0;
  crop.runtime_parameters["width"] = 0.5;
  crop.runtime_parameters["height"] = 1.0;
  const DirtyRoiPropFunc crop_dirty =
      OpRegistry::instance().get_dirty_propagator("image_process", "crop");
  EXPECT_EQ(crop_dirty(crop, PixelRect{1, 2, 3, 4}, graph, PixelSize{5, 7}, {},
                       crop.runtime_parameters, nullptr),
            (PixelRect{3, 2, 3, 4}));
}

}  // namespace
}  // namespace ps::providers::opencv
