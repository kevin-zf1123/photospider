#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <opencv2/core.hpp>
#include <optional>
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
 * @brief Publishes one padded signed-window three-channel blend source.
 *
 * @param with_per_channel_overrides Whether stable-ID sample overrides make
 *        the source interpretation non-uniform.
 * @param default_sample_domain Uniform default sample declaration.
 * @param stable_id_offset Offset applied to every channel and group identity.
 * @param color_transfer Explicit transfer function bound to the channel group.
 * @return Fresh host-readable FP32 Value with channel schema, sample
 *         interpretation, color authority, display metadata, and row padding.
 * @throws std::invalid_argument, std::overflow_error, or std::length_error
 *         when fixture metadata violates the dense-image contract.
 * @throws std::bad_alloc when metadata or payload allocation fails.
 * @note Active samples occupy 36 bytes per row while the y stride is 48 bytes;
 *       padding is initialized to `0xA5` and is never an active sample.
 */
Value make_weighted_blend_primary(
    bool with_per_channel_overrides,
    SampleDomain default_sample_domain =
        SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0},
    std::uint64_t stable_id_offset = 0U,
    ColorTransferFunction color_transfer = ColorTransferFunction::Rec709) {
  constexpr std::array<float, 18U> kSamples{0.2F, 0.4F, 0.6F, 0.3F, 0.5F, 0.7F,
                                            0.4F, 0.6F, 0.8F, 0.5F, 0.7F, 0.9F,
                                            0.6F, 0.8F, 1.0F, 0.7F, 0.9F, 0.1F};
  constexpr std::size_t kActiveRowBytes = 9U * sizeof(float);
  constexpr std::size_t kRowStride = 12U * sizeof(float);
  std::vector<std::byte> storage(kRowStride + kActiveRowBytes, std::byte{0xA5});
  std::memcpy(storage.data(), kSamples.data(), kActiveRowBytes);
  std::memcpy(storage.data() + kRowStride, kSamples.data() + 9U,
              kActiveRowBytes);

  DenseTensorDescriptor descriptor{{2U, 3U, 3U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.data_window = ImageBounds{-7, 11, -4, 13};
  facet.display_window = ImageBounds{-8, 10, -3, 14};
  const ChannelId left{11U + stable_id_offset};
  const ChannelId middle{12U + stable_id_offset};
  const ChannelId right{13U + stable_id_offset};
  const ChannelGroupId triple{20U + stable_id_offset};
  facet.channel_schema =
      ChannelSchema{{{left, "left"}, {middle, "middle"}, {right, "right"}},
                    {{triple, "triple", {left, middle, right}}}};
  SampleDomainFacet sample_domain{
      1U,
      SampleEncoding{1U, SampleEncodingKind::Normalized},
      default_sample_domain,
      {}};
  if (with_per_channel_overrides) {
    sample_domain.per_channel = {
        {left, SampleDomain{SampleDomainKind::Legal, -1.0, 1.0}},
        {middle, SampleDomain{SampleDomainKind::CodeValue, 0.0, 255.0}}};
  }
  facet.sample_domain = std::move(sample_domain);
  facet.color = ColorFacet{1U, triple, color_transfer, ColorPrimaries::Rec2020};
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{{static_cast<std::ptrdiff_t>(kRowStride),
                     static_cast<std::ptrdiff_t>(3U * sizeof(float)),
                     static_cast<std::ptrdiff_t>(sizeof(float))}},
      std::move(storage));
}

/**
 * @brief Publishes one uniform single-channel secondary blend source.
 *
 * @return Fresh tightly packed FP32 Value with normalized `[0,1]` sample
 *         interpretation and no channel or color authority.
 * @throws std::invalid_argument or std::overflow_error when the fixture
 *         descriptor, metadata, layout, or storage is invalid.
 * @throws std::bad_alloc when metadata or payload allocation fails.
 * @note The operation expands this source to the primary channel count before
 *       mapping it into a distinct fourth destination channel.
 */
Value make_weighted_blend_secondary() {
  constexpr std::array<float, 6U> kSamples{0.8F, 0.7F, 0.6F, 0.5F, 0.4F, 0.3F};
  std::vector<std::byte> storage(sizeof(kSamples));
  std::memcpy(storage.data(), kSamples.data(), storage.size());
  DenseTensorDescriptor descriptor{{2U, 3U, 1U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.sample_domain =
      SampleDomainFacet{1U,
                        SampleEncoding{1U, SampleEncodingKind::Normalized},
                        SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0},
                        {}};
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{{static_cast<std::ptrdiff_t>(3U * sizeof(float)),
                     static_cast<std::ptrdiff_t>(sizeof(float)),
                     static_cast<std::ptrdiff_t>(sizeof(float))}},
      std::move(storage));
}

/**
 * @brief Executes a weighted blend that maps three primary channels to four.
 *
 * @param primary Three-channel primary Value moved into callback-local input
 *        authority.
 * @param secondary Single-channel secondary Value moved into callback-local
 *        input authority.
 * @return Fresh four-channel provider output after both local input owners and
 *         every borrowed OpenCV header have been destroyed.
 * @throws GraphError when the callback is unavailable or rejects the request.
 * @throws std::invalid_argument, std::overflow_error, std::length_error, or
 *         std::bad_alloc from metadata projection and Value publication.
 * @note The mapping sends the three primary channels to matching destinations
 *       zero through two and secondary channel zero to destination three.
 *       Invalid-map fallback behavior is not exercised.
 */
NodeOutput execute_channel_expanding_weighted_blend(Value primary,
                                                    Value secondary) {
  auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_mixing", "add_weighted", ComputeIntent::GlobalHighPrecision);
  if (!resolved ||
      !std::holds_alternative<MonolithicOpFunc>(resolved.value())) {
    throw GraphError(GraphErrc::NoOperation,
                     "image_mixing:add_weighted is not monolithic");
  }

  plugin::ParameterValue::Object input0_mapping;
  input0_mapping.emplace(
      "0", plugin::ParameterValue::Array{plugin::ParameterValue{0}});
  input0_mapping.emplace(
      "1", plugin::ParameterValue::Array{plugin::ParameterValue{1}});
  input0_mapping.emplace(
      "2", plugin::ParameterValue::Array{plugin::ParameterValue{2}});
  plugin::ParameterValue::Object input1_mapping;
  input1_mapping.emplace(
      "0", plugin::ParameterValue::Array{plugin::ParameterValue{3}});
  plugin::ParameterValue::Object channel_mapping;
  channel_mapping.emplace("input0", std::move(input0_mapping));
  channel_mapping.emplace("input1", std::move(input1_mapping));

  Node node;
  node.id = 205;
  node.type = "image_mixing";
  node.subtype = "add_weighted";
  node.runtime_parameters["alpha"] = 0.5;
  node.runtime_parameters["beta"] = 0.25;
  node.runtime_parameters["gamma"] = 0.0;
  node.runtime_parameters["merge_strategy"] = "resize";
  node.runtime_parameters["channel_mapping"] = std::move(channel_mapping);

  NodeOutput primary_input;
  primary_input.publish_image_value(std::move(primary));
  NodeOutput secondary_input;
  secondary_input.publish_image_value(std::move(secondary));
  return std::get<MonolithicOpFunc>(resolved.value())(
      node, {&primary_input, &secondary_input});
}

/**
 * @brief Executes an ordinary same-channel weighted blend without mapping.
 *
 * @param primary Three-channel primary Value moved into callback-local input.
 * @param secondary Three-channel secondary Value moved into callback-local
 *        input.
 * @return Fresh three-channel provider output after local inputs retire.
 * @throws GraphError when the configured production callback is unavailable or
 *         rejects the request.
 * @throws std::invalid_argument, std::overflow_error, std::length_error, or
 *         std::bad_alloc from metadata projection and Value publication.
 * @note Equal weights and zero gamma keep pixel assertions simple. The helper
 *       supplies no channel mapping, sample conversion, or metadata override.
 */
NodeOutput execute_same_channel_weighted_blend(Value primary, Value secondary) {
  auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_mixing", "add_weighted", ComputeIntent::GlobalHighPrecision);
  if (!resolved ||
      !std::holds_alternative<MonolithicOpFunc>(resolved.value())) {
    throw GraphError(GraphErrc::NoOperation,
                     "image_mixing:add_weighted is not monolithic");
  }

  Node node;
  node.id = 207;
  node.type = "image_mixing";
  node.subtype = "add_weighted";
  node.runtime_parameters["alpha"] = 0.5;
  node.runtime_parameters["beta"] = 0.5;
  node.runtime_parameters["gamma"] = 0.0;
  node.runtime_parameters["merge_strategy"] = "resize";

  NodeOutput primary_input;
  primary_input.publish_image_value(std::move(primary));
  NodeOutput secondary_input;
  secondary_input.publish_image_value(std::move(secondary));
  return std::get<MonolithicOpFunc>(resolved.value())(
      node, {&primary_input, &secondary_input});
}

/**
 * @brief Executes one small frozen coordinate-pattern generator request.
 *
 * @return Fresh `3x2x3` FP32 provider output for seed zero.
 * @throws GraphError when the callback is unavailable or rejects the request.
 * @throws std::invalid_argument, std::overflow_error, std::length_error, or
 *         std::bad_alloc from metadata and Value publication.
 * @note The returned Value outlives every callback-local OpenCV allocation.
 */
NodeOutput execute_coordinate_pattern() {
  auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_generator", "coordinate_pattern",
      ComputeIntent::GlobalHighPrecision);
  if (!resolved ||
      !std::holds_alternative<MonolithicOpFunc>(resolved.value())) {
    throw GraphError(GraphErrc::NoOperation,
                     "image_generator:coordinate_pattern is not monolithic");
  }
  Node node;
  node.id = 206;
  node.type = "image_generator";
  node.subtype = "coordinate_pattern";
  node.runtime_parameters["width"] = 3;
  node.runtime_parameters["height"] = 2;
  node.runtime_parameters["channels"] = 3;
  node.runtime_parameters["seed"] = 0;
  return std::get<MonolithicOpFunc>(resolved.value())(node, {});
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
 * @brief Proves a channel-expanding blend publishes only compatible facts.
 *
 * @return Nothing; GoogleTest reports payload, authority, or metadata failures.
 * @throws Provider resolution, OpenCV execution, metadata validation, and
 *         allocation exceptions unchanged to the test runner.
 * @note The first pass rejects per-channel/color/schema reuse while retaining
 *       signed spatial facts. The second proves a uniform sample endpoint
 *       shared by both sources remains legal after expansion. Returned Values
 *       are inspected after callback-local inputs and matrices are destroyed.
 */
TEST(OpenCvOperationProviderMetadataContract,
     WeightedBlendProjectsExpandedChannelSemantics) {
  ASSERT_NO_THROW(register_provider());

  Value rich_primary = make_weighted_blend_primary(true);
  const ValueRevisionId rich_revision = rich_primary.revision_id();
  const NodeOutput rich_output = execute_channel_expanding_weighted_blend(
      std::move(rich_primary), make_weighted_blend_secondary());
  ASSERT_TRUE(rich_output.has_image_value());
  const Value& rich_value = rich_output.image_value();
  EXPECT_NE(rich_value.revision_id(), rich_revision);
  EXPECT_EQ(rich_value.dense_tensor_descriptor().shape,
            (std::vector<std::size_t>{2U, 3U, 4U}));
  ASSERT_TRUE(rich_value.image_facet().has_value());
  const ImageFacet& rich_facet = *rich_value.image_facet();
  EXPECT_EQ(rich_facet.x_axis, 1U);
  EXPECT_EQ(rich_facet.y_axis, 0U);
  EXPECT_EQ(rich_facet.channel_axis, std::optional<std::size_t>(2U));
  EXPECT_EQ(rich_facet.data_window, (ImageBounds{-7, 11, -4, 13}));
  EXPECT_EQ(rich_facet.display_window,
            std::optional<ImageBounds>(ImageBounds{-8, 10, -3, 14}));
  EXPECT_FALSE(rich_facet.channel_schema.has_value());
  EXPECT_FALSE(rich_facet.sample_domain.has_value());
  EXPECT_FALSE(rich_facet.color.has_value());

  const ImageView rich_view(rich_value);
  ASSERT_EQ(rich_view.channels(), 4U);
  std::array<float, 4U> first_pixel{};
  for (std::size_t channel = 0U; channel < first_pixel.size(); ++channel) {
    std::memcpy(&first_pixel[channel], rich_view.channel_data(0U, 0U, channel),
                sizeof(float));
  }
  EXPECT_FLOAT_EQ(first_pixel[0], 0.1F);
  EXPECT_FLOAT_EQ(first_pixel[1], 0.2F);
  EXPECT_FLOAT_EQ(first_pixel[2], 0.3F);
  EXPECT_FLOAT_EQ(first_pixel[3], 0.2F);

  const NodeOutput uniform_output = execute_channel_expanding_weighted_blend(
      make_weighted_blend_primary(false), make_weighted_blend_secondary());
  ASSERT_TRUE(uniform_output.has_image_value());
  ASSERT_TRUE(uniform_output.image_value().image_facet().has_value());
  const ImageFacet& uniform_facet = *uniform_output.image_value().image_facet();
  EXPECT_FALSE(uniform_facet.channel_schema.has_value());
  ASSERT_TRUE(uniform_facet.sample_domain.has_value());
  EXPECT_EQ(uniform_facet.sample_domain->encoding,
            (SampleEncoding{1U, SampleEncodingKind::Normalized}));
  EXPECT_EQ(uniform_facet.sample_domain->default_domain,
            (SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}));
  EXPECT_TRUE(uniform_facet.sample_domain->per_channel.empty());
  EXPECT_FALSE(uniform_facet.color.has_value());
}

/**
 * @brief Proves same-channel blend metadata is the two-source intersection.
 *
 * @return Nothing; GoogleTest reports pixel, identity, or facet drift.
 * @throws Provider resolution, OpenCV execution, metadata validation, and
 *         allocation exceptions unchanged to the test runner.
 * @note Incompatible sample facts cannot inherit the primary declaration;
 *       identical uniform facts may survive. Stable channel/color authority
 *       survives only when both sources agree exactly. Signed windows and raw
 *       pixels remain primary-shaped, and every output receives a fresh Value
 *       revision after callback-local inputs retire.
 */
TEST(OpenCvOperationProviderMetadataContract,
     WeightedBlendProjectsSameChannelSemanticIntersection) {
  ASSERT_NO_THROW(register_provider());

  Value sample_primary = make_weighted_blend_primary(false);
  const ValueRevisionId sample_primary_revision = sample_primary.revision_id();
  const ImageFacet sample_primary_facet = *sample_primary.image_facet();
  const NodeOutput incompatible_sample = execute_same_channel_weighted_blend(
      std::move(sample_primary),
      make_weighted_blend_primary(
          false, SampleDomain{SampleDomainKind::Legal, -1.0, 1.0}));
  ASSERT_TRUE(incompatible_sample.has_image_value());
  const Value& incompatible_sample_value = incompatible_sample.image_value();
  EXPECT_NE(incompatible_sample_value.revision_id(), sample_primary_revision);
  EXPECT_EQ(incompatible_sample_value.dense_tensor_descriptor().shape,
            (std::vector<std::size_t>{2U, 3U, 3U}));
  ASSERT_TRUE(incompatible_sample_value.image_facet().has_value());
  const ImageFacet& incompatible_sample_facet =
      *incompatible_sample_value.image_facet();
  EXPECT_EQ(incompatible_sample_facet.data_window,
            sample_primary_facet.data_window);
  EXPECT_EQ(incompatible_sample_facet.display_window,
            sample_primary_facet.display_window);
  EXPECT_EQ(incompatible_sample_facet.channel_schema,
            sample_primary_facet.channel_schema);
  EXPECT_EQ(incompatible_sample_facet.color, sample_primary_facet.color);
  EXPECT_FALSE(incompatible_sample_facet.sample_domain.has_value());

  Value uniform_primary = make_weighted_blend_primary(false);
  const ValueRevisionId uniform_primary_revision =
      uniform_primary.revision_id();
  const ImageFacet uniform_primary_facet = *uniform_primary.image_facet();
  const NodeOutput uniform = execute_same_channel_weighted_blend(
      std::move(uniform_primary), make_weighted_blend_primary(false));
  ASSERT_TRUE(uniform.has_image_value());
  EXPECT_NE(uniform.image_value().revision_id(), uniform_primary_revision);
  ASSERT_TRUE(uniform.image_value().image_facet().has_value());
  EXPECT_EQ(*uniform.image_value().image_facet(), uniform_primary_facet);
  const ImageView uniform_view(uniform.image_value());
  float first_sample = 0.0F;
  std::memcpy(&first_sample, uniform_view.channel_data(0U, 0U, 0U),
              sizeof(first_sample));
  EXPECT_FLOAT_EQ(first_sample, 0.2F);

  Value schema_primary = make_weighted_blend_primary(false);
  const ImageFacet schema_primary_facet = *schema_primary.image_facet();
  const NodeOutput incompatible_schema = execute_same_channel_weighted_blend(
      std::move(schema_primary),
      make_weighted_blend_primary(
          false, SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}, 100U));
  ASSERT_TRUE(incompatible_schema.has_image_value());
  ASSERT_TRUE(incompatible_schema.image_value().image_facet().has_value());
  const ImageFacet& incompatible_schema_facet =
      *incompatible_schema.image_value().image_facet();
  EXPECT_EQ(incompatible_schema_facet.data_window,
            schema_primary_facet.data_window);
  EXPECT_EQ(incompatible_schema_facet.display_window,
            schema_primary_facet.display_window);
  EXPECT_FALSE(incompatible_schema_facet.channel_schema.has_value());
  ASSERT_TRUE(incompatible_schema_facet.sample_domain.has_value());
  EXPECT_EQ(incompatible_schema_facet.sample_domain,
            schema_primary_facet.sample_domain);
  EXPECT_FALSE(incompatible_schema_facet.color.has_value());

  Value color_primary = make_weighted_blend_primary(false);
  const ImageFacet color_primary_facet = *color_primary.image_facet();
  const NodeOutput incompatible_color = execute_same_channel_weighted_blend(
      std::move(color_primary),
      make_weighted_blend_primary(
          false, SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}, 0U,
          ColorTransferFunction::Srgb));
  ASSERT_TRUE(incompatible_color.has_image_value());
  ASSERT_TRUE(incompatible_color.image_value().image_facet().has_value());
  const ImageFacet& incompatible_color_facet =
      *incompatible_color.image_value().image_facet();
  EXPECT_EQ(incompatible_color_facet.data_window,
            color_primary_facet.data_window);
  EXPECT_EQ(incompatible_color_facet.display_window,
            color_primary_facet.display_window);
  EXPECT_EQ(incompatible_color_facet.channel_schema,
            color_primary_facet.channel_schema);
  EXPECT_EQ(incompatible_color_facet.sample_domain,
            color_primary_facet.sample_domain);
  EXPECT_FALSE(incompatible_color_facet.color.has_value());
}

/**
 * @brief Proves coordinate-pattern publication declares its exact endpoint.
 *
 * @return Nothing; GoogleTest reports descriptor or sample-domain failures.
 * @throws Provider resolution, OpenCV execution, metadata validation, and
 *         allocation exceptions unchanged to the test runner.
 * @note Payload formula bits are locked by the optional-provider integration
 *       test; this case locks the independent interpretation authority used by
 *       codecs and CLI conversion.
 */
TEST(OpenCvOperationProviderMetadataContract,
     CoordinatePatternDeclaresNormalizedSampleDomain) {
  ASSERT_NO_THROW(register_provider());
  const NodeOutput output = execute_coordinate_pattern();
  ASSERT_TRUE(output.has_image_value());
  const Value& value = output.image_value();
  EXPECT_EQ(value.dense_tensor_descriptor().element_semantics,
            ElementSemantics::FloatingPoint);
  EXPECT_EQ(value.dense_tensor_descriptor().storage_encoding.bit_width, 32U);
  ASSERT_TRUE(value.image_facet().has_value());
  ASSERT_TRUE(value.image_facet()->sample_domain.has_value());
  const SampleDomainFacet& samples = *value.image_facet()->sample_domain;
  EXPECT_EQ(samples.encoding,
            (SampleEncoding{1U, SampleEncodingKind::Normalized}));
  EXPECT_EQ(samples.default_domain,
            (SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}));
  EXPECT_TRUE(samples.per_channel.empty());
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
