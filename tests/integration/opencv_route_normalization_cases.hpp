#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "compute/compute_service.hpp"  // NOLINT(build/include_subdir)
#include "compute/execution/execution_service.hpp"
#include "core/ps_types.hpp"                  // NOLINT(build/include_subdir)
#include "graph/graph_cache_service.hpp"      // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"              // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/image_view.hpp"
#include "providers/configured_image_artifact_codec.hpp"
#include "providers/opencv/opencv_operation_provider.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"
#include "support/cache_test_dependencies.hpp"
#include "support/scoped_execution_graph_lifecycle.hpp"
#include "support/scoped_test_resources.hpp"

namespace ps {
namespace route_normalization_cases {

/** @brief Execution domain observed by one wrapped tiled implementation. */
enum class ObservedRoute {
  /** @brief Full or dirty high-precision callback revision. */
  HighPrecision,
  /** @brief Dirty real-time callback revision. */
  RealTime,
};

/**
 * @brief Immutable summary of normalized-input pointer observations.
 * @throws Nothing; this value owns only counters and a Boolean verdict.
 * @note Pointer identity is inspected only as an address token. The snapshot
 * never dereferences a Value after its synchronous callback returns.
 */
struct RouteObservationSnapshot final {
  /** @brief Number of pre-allocation inference calls. */
  std::size_t inference_calls = 0U;
  /** @brief Number of provider tile callbacks. */
  std::size_t callback_calls = 0U;
  /** @brief Distinct secondary Values visible to inference. */
  std::size_t inferred_secondary_values = 0U;
  /** @brief Distinct secondary Values visible to provider callbacks. */
  std::size_t callback_secondary_values = 0U;
  /** @brief Whether every callback Value was first used by inference. */
  bool every_callback_value_was_inferred = false;
};

/**
 * @brief Thread-safe observer for exact tiled input-context identity.
 * @throws std::bad_alloc when pointer-set insertion allocates.
 * @note Full parallel siblings may call record_callback concurrently. Dirty
 * HP and RT callbacks may also overlap, so all state is protected by mutex_.
 */
class RouteNormalizationObserver final {
 public:
  /**
   * @brief Records the secondary canonical Value used by output inference.
   * @param route HP or RT implementation revision.
   * @param inputs Exact destination-indexed inputs supplied to inference.
   * @return Nothing.
   * @throws std::bad_alloc when the pointer set grows.
   * @note Missing or non-image secondaries still increment the call counter
   * but do not create a pointer token.
   */
  void record_inference(ObservedRoute route,
                        const std::vector<const NodeOutput*>& inputs) {
    std::lock_guard<std::mutex> lock(mutex_);
    State& state = state_for(route);
    ++state.inference_calls;
    if (inputs.size() > 1U && inputs[1] != nullptr &&
        inputs[1]->has_image_value()) {
      state.inferred_secondary_values.insert(&inputs[1]->image_value());
    }
  }

  /**
   * @brief Records the secondary canonical Value used by one tile callback.
   * @param route HP or RT implementation revision.
   * @param input_tiles Exact provider input views for the current output tile.
   * @return Nothing.
   * @throws std::bad_alloc when the pointer set grows.
   * @note InputTile Values are borrowed only for the callback; this observer
   * retains their addresses without dereferencing them later.
   */
  void record_callback(ObservedRoute route,
                       const std::vector<InputTile>& input_tiles) {
    std::lock_guard<std::mutex> lock(mutex_);
    State& state = state_for(route);
    ++state.callback_calls;
    if (input_tiles.size() > 1U && input_tiles[1].value != nullptr) {
      state.callback_secondary_values.insert(input_tiles[1].value);
    }
  }

  /**
   * @brief Clears all observations between seed and dirty execution.
   * @return Nothing.
   * @throws std::system_error when mutex acquisition fails.
   * @note Callers invoke reset only after the synchronous seed request has
   * settled, so no callback can race this lifecycle boundary.
   */
  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    states_ = {};
  }

  /**
   * @brief Captures one route's counts and pointer-subset verdict.
   * @param route HP or RT implementation revision.
   * @return Immutable address-only observation summary.
   * @throws std::system_error when mutex acquisition fails.
   */
  RouteObservationSnapshot snapshot(ObservedRoute route) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const State& state = state_for(route);
    const bool all_inferred =
        !state.callback_secondary_values.empty() &&
        std::all_of(state.callback_secondary_values.begin(),
                    state.callback_secondary_values.end(),
                    [&](const Value* value) {
                      return state.inferred_secondary_values.count(value) != 0U;
                    });
    return RouteObservationSnapshot{state.inference_calls, state.callback_calls,
                                    state.inferred_secondary_values.size(),
                                    state.callback_secondary_values.size(),
                                    all_inferred};
  }

 private:
  /** @brief Mutable per-route address and call-count state. */
  struct State final {
    /** @brief Number of exact inference callback entries. */
    std::size_t inference_calls = 0U;
    /** @brief Number of exact producer callback entries. */
    std::size_t callback_calls = 0U;
    /** @brief Secondary Value addresses passed to output inference. */
    std::set<const Value*> inferred_secondary_values;
    /** @brief Secondary Value addresses passed to tile providers. */
    std::set<const Value*> callback_secondary_values;
  };

  /**
   * @brief Returns mutable state for one route.
   * @param route HP or RT route selector.
   * @return State protected by mutex_.
   * @throws Nothing.
   */
  State& state_for(ObservedRoute route) noexcept {
    return states_[route == ObservedRoute::HighPrecision ? 0U : 1U];
  }

  /**
   * @brief Returns immutable state for one route.
   * @param route HP or RT route selector.
   * @return State protected by mutex_.
   * @throws Nothing.
   */
  const State& state_for(ObservedRoute route) const noexcept {
    return states_[route == ObservedRoute::HighPrecision ? 0U : 1U];
  }

  /** @brief Protects both route states during concurrent callbacks. */
  mutable std::mutex mutex_;
  /** @brief HP state followed by RT state. */
  std::array<State, 2U> states_;
};

/**
 * @brief Best-effort owner for operation keys installed by one route case.
 * @throws std::bad_alloc when tracked key storage grows.
 * @note Destruction removes only unique test keys after all synchronous work
 * settles. The repository-owned OpenCV keys are never tracked or removed.
 */
class ScopedOperationKeys final {
 public:
  /**
   * @brief Tracks one operation key for later removal.
   * @param type Exact operation type.
   * @param subtype Exact operation subtype.
   * @return Nothing.
   * @throws std::bad_alloc when key construction or vector growth allocates.
   */
  void track(const std::string& type, const std::string& subtype) {
    keys_.push_back(make_key(type, subtype));
  }

  /**
   * @brief Removes all tracked test registrations in reverse order.
   * @throws Nothing; teardown errors cannot replace a test verdict.
   */
  ~ScopedOperationKeys() noexcept {
    for (auto iterator = keys_.rbegin(); iterator != keys_.rend(); ++iterator) {
      try {
        OpRegistry::instance().unregister_key(*iterator);
      } catch (...) {
      }
    }
  }

 private:
  /** @brief Canonical unique test keys owned by this guard. */
  std::vector<std::string> keys_;
};

/**
 * @brief Creates one host-readable FP32 ordinary image with a uniform domain.
 * @param width Positive image width.
 * @param height Positive image height.
 * @param channels Positive interleaved channel count.
 * @param samples Exact row-major active samples.
 * @param domain Uniform Sample Domain declaration.
 * @return Fresh immutable Value owning tightly packed storage.
 * @throws std::invalid_argument when shape and payload cardinality disagree.
 * @throws Dense Value validation, arithmetic, or allocation failures unchanged.
 * @note Raw samples are copied verbatim; this fixture performs no conversion.
 */
Value make_uniform_fp32_image(std::size_t width, std::size_t height,
                              std::size_t channels,
                              const std::vector<float>& samples,
                              SampleDomain domain) {
  if (width == 0U || height == 0U || channels == 0U ||
      samples.size() != width * height * channels) {
    throw std::invalid_argument(
        "Route normalization fixture shape does not match its payload.");
  }
  std::vector<std::byte> storage(samples.size() * sizeof(float));
  std::memcpy(storage.data(), samples.data(), storage.size());
  DenseTensorDescriptor descriptor{{height, width, channels},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.sample_domain =
      SampleDomainFacet{1U,
                        SampleEncoding{1U, SampleEncodingKind::Normalized},
                        domain,
                        {}};
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{
          {static_cast<std::ptrdiff_t>(width * channels * sizeof(float)),
           static_cast<std::ptrdiff_t>(channels * sizeof(float)),
           static_cast<std::ptrdiff_t>(sizeof(float))}},
      std::move(storage));
}

/**
 * @brief Registers one immutable image source used by a route graph.
 * @param subtype Unique source subtype.
 * @param value Immutable image copied into every callback result.
 * @param keys Teardown owner for the installed key.
 * @return Nothing.
 * @throws Registry or Value-copy allocation failures unchanged.
 * @note The callback is monolithic and input-free so only the target exercises
 * tiled input normalization.
 */
void register_image_source(const std::string& subtype, Value value,
                           ScopedOperationKeys* keys) {
  constexpr char kSourceType[] = "issue131_route_source";
  keys->track(kSourceType, subtype);
  OpRegistry::instance().register_op_hp_monolithic(
      kSourceType, subtype,
      MonolithicOpFunc([value = std::move(value)](
                           const Node&, const std::vector<const NodeOutput*>&) {
        NodeOutput output;
        output.publish_image_value(value);
        return output;
      }));
}

/**
 * @brief Selects one exact repository OpenCV tiled implementation.
 * @param subtype Repository operation subtype.
 * @param intent HP or RT route intent.
 * @return Exact tiled implementation snapshot.
 * @throws GraphError when no matching CPU tiled revision exists.
 * @throws Registry snapshot allocation or callback-copy failures unchanged.
 */
OpImplementation require_opencv_tiled(const std::string& subtype,
                                      ComputeIntent intent) {
  std::optional<OpImplementation> selected =
      OpRegistry::instance().select_implementation(
          "image_mixing", subtype, {DeviceBackend::CPU}, intent,
          [](const OpImplementation& candidate) {
            return candidate.is_tiled();
          });
  if (!selected.has_value() || !selected->tiled_output_inference.has_value()) {
    throw GraphError(GraphErrc::NoOperation,
                     "Observed OpenCV tiled implementation is unavailable.");
  }
  return std::move(*selected);
}

/**
 * @brief Registers observed HP and RT wrappers around one real OpenCV op.
 * @param source_subtype Repository `add_weighted` or `multiply` subtype.
 * @param observed_subtype Unique wrapper subtype used by the graph.
 * @param observer Shared synchronous input-identity observer.
 * @param keys Teardown owner for the wrapper key.
 * @return Nothing.
 * @throws Registry, callback-copy, or allocation failures unchanged.
 * @note Each wrapper delegates both execution and inference to the same exact
 * repository revision selected before registration. Only address observation
 * is added; pixel and metadata behavior remain production OpenCV behavior.
 */
void register_observed_image_mixing(
    const std::string& source_subtype, const std::string& observed_subtype,
    const std::shared_ptr<RouteNormalizationObserver>& observer,
    ScopedOperationKeys* keys) {
  keys->track("image_mixing", observed_subtype);
  for (const auto route :
       {ObservedRoute::HighPrecision, ObservedRoute::RealTime}) {
    const ComputeIntent intent = route == ObservedRoute::HighPrecision
                                     ? ComputeIntent::GlobalHighPrecision
                                     : ComputeIntent::RealTimeUpdate;
    OpImplementation selected = require_opencv_tiled(source_subtype, intent);
    TileOpFunc operation = std::get<TileOpFunc>(selected.func);
    TiledOutputInferenceFunc inference = *selected.tiled_output_inference;
    TileOpFunc observed_operation =
        [observer, route, operation = std::move(operation)](
            const Node& node, const OutputTile& output,
            const std::vector<InputTile>& inputs) {
          observer->record_callback(route, inputs);
          operation(node, output, inputs);
        };
    TiledOutputInferenceFunc observed_inference =
        [observer, route, inference = std::move(inference)](
            const Node& node, const std::vector<const NodeOutput*>& inputs,
            const PixelSize& output_size) {
          observer->record_inference(route, inputs);
          return inference(node, inputs, output_size);
        };
    OpPlanningCallbacks planning{
        selected.dirty_propagator, selected.forward_propagator,
        selected.dependency_builder, std::move(observed_inference)};
    if (route == ObservedRoute::HighPrecision) {
      OpRegistry::instance().register_op_hp_tiled(
          "image_mixing", observed_subtype, std::move(observed_operation),
          selected.metadata, std::move(planning));
    } else {
      OpRegistry::instance().register_op_rt_tiled(
          "image_mixing", observed_subtype, std::move(observed_operation),
          selected.metadata, std::move(planning));
    }
  }
}

/** @brief Production compute route selected by one fixture invocation. */
enum class RouteExecution {
  /** @brief Full high-precision task-graph parallel dispatch. */
  FullParallel,
  /** @brief Dirty high-precision update. */
  DirtyHighPrecision,
  /** @brief Dirty real-time update, including its HP sibling. */
  DirtyRealTime,
};

/**
 * @brief Complete result of one synchronous production-route invocation.
 * @throws std::bad_alloc when NodeOutput ownership is copied.
 */
struct RouteCaseResult final {
  /** @brief Returned formal HP or staged RT output copied after settlement. */
  NodeOutput output;
  /** @brief HP observation after seed state was cleared when applicable. */
  RouteObservationSnapshot hp_observation;
  /** @brief RT observation after seed state was cleared when applicable. */
  RouteObservationSnapshot rt_observation;
};

/**
 * @brief Runs one real OpenCV image_mixing graph through a production route.
 * @param label Unique diagnostic and registry suffix.
 * @param source_subtype Repository `add_weighted` or `multiply` operation.
 * @param execution Full parallel, dirty HP, or dirty RT route.
 * @param primary Immutable primary image fixture.
 * @param secondary Immutable secondary image fixture requiring normalization.
 * @param parameters Effective image_mixing parameters.
 * @return Copied output plus HP/RT input-identity observations.
 * @throws Graph, registry, runtime, compute, provider, or allocation failures
 * unchanged.
 * @note Dirty cases seed a correct full output through the synchronous direct
 * NodeExecutor route, reset observations, then execute a full-frame dirty ROI.
 * Prior output bytes only seed the exact frozen plan and never enter inputs.
 */
RouteCaseResult run_route_case(const std::string& label,
                               const std::string& source_subtype,
                               RouteExecution execution, Value primary,
                               Value secondary,
                               plugin::ParameterMap parameters) {
  const ImageView primary_view(primary);
  const PixelSize extent{static_cast<int>(primary_view.width()),
                         static_cast<int>(primary_view.height())};
  const std::string primary_subtype = label + "_primary";
  const std::string secondary_subtype = label + "_secondary";
  const std::string target_subtype = label + "_target";
  ScopedOperationKeys keys;
  auto observer = std::make_shared<RouteNormalizationObserver>();
  register_image_source(primary_subtype, std::move(primary), &keys);
  register_image_source(secondary_subtype, std::move(secondary), &keys);
  register_observed_image_mixing(source_subtype, target_subtype, observer,
                                 &keys);

  test_support::ScopedTempDir root("photospider-issue131-" + label);
  GraphRuntime::Info info;
  info.name = "issue131-" + label;
  info.root = root.root();
  info.cache_root = root.root() / "cache";
  GraphRuntime runtime(info);
  runtime.replace_execution_route(ComputeIntent::GlobalHighPrecision, "cpu");
  runtime.replace_execution_route(ComputeIntent::RealTimeUpdate, "cpu");
  runtime.start();
  GraphModel& graph = runtime.model();

  Node primary_node;
  primary_node.id = 1;
  primary_node.name = "primary";
  primary_node.type = "issue131_route_source";
  primary_node.subtype = primary_subtype;
  primary_node.parameters["width"] = extent.width;
  primary_node.parameters["height"] = extent.height;
  Node secondary_node;
  secondary_node.id = 2;
  secondary_node.name = "secondary";
  secondary_node.type = "issue131_route_source";
  secondary_node.subtype = secondary_subtype;
  Node target;
  target.id = 3;
  target.name = "observed image mixing";
  target.type = "image_mixing";
  target.subtype = target_subtype;
  target.parameters = std::move(parameters);
  target.parameters["width"] = extent.width;
  target.parameters["height"] = extent.height;
  target.image_inputs.push_back(
      ImageInput{1, std::string(NodeOutput::kImageOutputName)});
  target.image_inputs.push_back(
      ImageInput{2, std::string(NodeOutput::kImageOutputName)});
  graph.add_node(std::move(primary_node));
  graph.add_node(std::move(secondary_node));
  graph.add_node(std::move(target));
  graph.validate_topology();

  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec(),
                          testing::make_yaml_cache_metadata_codec()};
  GraphEventService events;
  compute::ExecutionService execution_service(4U);
  ComputeService service(traversal, cache, events, execution_service);
  testing::ScopedExecutionGraphLifecycle lifecycle(execution_service, graph);
  ComputeService::Request request;
  request.node_id = 3;
  request.cache.precision = "float32";
  request.cache.disable_disk_cache = true;

  NodeOutput* output = nullptr;
  if (execution == RouteExecution::FullParallel) {
    request.cache.force_recache = true;
    output = &service.compute_parallel(graph, runtime, request);
  } else {
    request.cache.force_recache = true;
    static_cast<void>(service.compute(graph, request));
    observer->reset();
    request.cache.force_recache = false;
    request.intent = execution == RouteExecution::DirtyHighPrecision
                         ? ComputeIntent::GlobalHighPrecision
                         : ComputeIntent::RealTimeUpdate;
    request.dirty_roi = PixelRect{0, 0, extent.width, extent.height};
    output = &service.compute(graph, request);
  }

  RouteCaseResult result{*output,
                         observer->snapshot(ObservedRoute::HighPrecision),
                         observer->snapshot(ObservedRoute::RealTime)};
  runtime.stop();
  return result;
}

/**
 * @brief Asserts exact FP32 payload and optional uniform Sample Domain state.
 * @param output Route result containing one ordinary FP32 image.
 * @param width Expected width.
 * @param height Expected height.
 * @param channels Expected channel count.
 * @param expected Exact interleaved active samples.
 * @param expect_sample_domain Whether uniform Sample Domain must survive.
 * @return Nothing; GoogleTest records any metadata or raw-pixel drift.
 * @throws ImageView validation and payload access failures unchanged.
 */
void expect_route_output(const NodeOutput& output, std::size_t width,
                         std::size_t height, std::size_t channels,
                         const std::vector<float>& expected,
                         bool expect_sample_domain) {
  ASSERT_TRUE(output.has_image_value());
  const Value& value = output.image_value();
  const ImageView view(value);
  ASSERT_EQ(view.width(), width);
  ASSERT_EQ(view.height(), height);
  ASSERT_EQ(view.channels(), channels);
  ASSERT_EQ(expected.size(), width * height * channels);
  ASSERT_TRUE(value.image_facet().has_value());
  EXPECT_EQ(value.image_facet()->sample_domain.has_value(),
            expect_sample_domain);
  std::size_t sample_index = 0U;
  for (std::size_t y = 0U; y < height; ++y) {
    for (std::size_t x = 0U; x < width; ++x) {
      for (std::size_t channel = 0U; channel < channels; ++channel) {
        float actual = 0.0F;
        std::memcpy(&actual, view.channel_data(x, y, channel), sizeof(actual));
        EXPECT_FLOAT_EQ(actual, expected[sample_index]);
        ++sample_index;
      }
    }
  }
}

/**
 * @brief Proves full parallel siblings share one normalized inference owner.
 * @return Nothing; GoogleTest records route, context, metadata, or raw drift.
 * @throws Registry, graph, provider, runtime, and allocation failures
 * unchanged.
 * @note A 513x257 target expands to six 256x256 planned siblings. Crop padding
 * a `[1,1]` secondary emits raw zero, so inference must omit Sample Domain and
 * every sibling callback must borrow the same node-level normalized Value.
 */
TEST(OpenCvRouteNormalization,
     FullParallelNormalizesOnceBeforeInferenceAndAllocation) {
  ASSERT_NO_THROW(providers::opencv::register_provider());
  constexpr std::size_t kWidth = 513U;
  constexpr std::size_t kHeight = 257U;
  const SampleDomain domain{SampleDomainKind::Legal, 1.0, 1.0};
  std::vector<float> expected(kWidth * kHeight, 0.0F);
  expected.front() = 1.0F;
  const RouteCaseResult result = run_route_case(
      "full-parallel-zero", "multiply", RouteExecution::FullParallel,
      make_uniform_fp32_image(kWidth, kHeight, 1U,
                              std::vector<float>(kWidth * kHeight, 1.0F),
                              domain),
      make_uniform_fp32_image(1U, 1U, 1U, {1.0F}, domain),
      {{"scale", 1.0}, {"merge_strategy", "crop"}});

  expect_route_output(result.output, kWidth, kHeight, 1U, expected, false);
  EXPECT_EQ(result.hp_observation.inference_calls, 1U);
  EXPECT_GT(result.hp_observation.callback_calls, 1U);
  EXPECT_EQ(result.hp_observation.inferred_secondary_values, 1U);
  EXPECT_EQ(result.hp_observation.callback_secondary_values, 1U);
  EXPECT_TRUE(result.hp_observation.every_callback_value_was_inferred);
}

/**
 * @brief Proves dirty HP uses normalized inputs for plan and blend callbacks.
 * @return Nothing; GoogleTest records metadata, pointer, or pixel drift.
 * @throws Registry, graph, provider, runtime, and allocation failures
 * unchanged.
 * @note 1x1x3 to 2x2x4 crop/opaque expansion synthesizes both zero and one.
 * `[0,0.5]` must fail closed while `[0,1]` remains authoritative; the raw
 * blend output is identical in both cases.
 */
TEST(OpenCvRouteNormalization,
     DirtyHighPrecisionNormalizesBeforeFreezeAndBlendCallbacks) {
  ASSERT_NO_THROW(providers::opencv::register_provider());
  const std::vector<float> expected{0.5F, 0.5F, 0.5F, 1.0F, 0.0F, 0.0F,
                                    0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                    0.0F, 0.0F, 0.0F, 1.0F};
  const plugin::ParameterMap parameters{{"alpha", 0.0},
                                        {"beta", 1.0},
                                        {"gamma", 0.0},
                                        {"merge_strategy", "crop"}};
  const auto run = [&](const std::string& label, SampleDomain domain,
                       bool expect_domain) {
    const RouteCaseResult result = run_route_case(
        label, "add_weighted", RouteExecution::DirtyHighPrecision,
        make_uniform_fp32_image(2U, 2U, 4U, std::vector<float>(16U, 0.5F),
                                domain),
        make_uniform_fp32_image(1U, 1U, 3U, std::vector<float>(3U, 0.5F),
                                domain),
        parameters);
    expect_route_output(result.output, 2U, 2U, 4U, expected, expect_domain);
    EXPECT_GT(result.hp_observation.inference_calls, 0U);
    EXPECT_GT(result.hp_observation.callback_calls, 0U);
    EXPECT_TRUE(result.hp_observation.every_callback_value_was_inferred);
  };
  run("dirty-hp-opaque-unsafe", SampleDomain{SampleDomainKind::Legal, 0.0, 0.5},
      false);
  run("dirty-hp-opaque-safe",
      SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}, true);
}

/**
 * @brief Proves dirty RT uses normalized inputs for plan and multiply tiles.
 * @return Nothing; GoogleTest records metadata, pointer, or pixel drift.
 * @throws Registry, graph, provider, runtime, and allocation failures
 * unchanged.
 * @note The same zero/opaque normalization pair is exercised through the RT
 * revision. Exact raw multiplication remains independent of whether `[0,0.5]`
 * loses authority or `[0,1]` survives.
 */
TEST(OpenCvRouteNormalization,
     DirtyRealTimeNormalizesBeforeFreezeAndMultiplyCallbacks) {
  ASSERT_NO_THROW(providers::opencv::register_provider());
  const std::vector<float> expected{0.25F, 0.25F, 0.25F, 0.5F, 0.0F, 0.0F,
                                    0.0F,  0.5F,  0.0F,  0.0F, 0.0F, 0.5F,
                                    0.0F,  0.0F,  0.0F,  0.5F};
  const plugin::ParameterMap parameters{{"scale", 1.0},
                                        {"merge_strategy", "crop"}};
  const auto run = [&](const std::string& label, SampleDomain domain,
                       bool expect_domain) {
    const RouteCaseResult result =
        run_route_case(label, "multiply", RouteExecution::DirtyRealTime,
                       make_uniform_fp32_image(
                           8U, 8U, 4U, std::vector<float>(256U, 0.5F), domain),
                       make_uniform_fp32_image(
                           4U, 4U, 3U, std::vector<float>(48U, 0.5F), domain),
                       parameters);
    expect_route_output(result.output, 2U, 2U, 4U, expected, expect_domain);
    EXPECT_GT(result.rt_observation.inference_calls, 0U);
    EXPECT_GT(result.rt_observation.callback_calls, 0U);
    EXPECT_TRUE(result.rt_observation.every_callback_value_was_inferred);
  };
  run("dirty-rt-opaque-unsafe", SampleDomain{SampleDomainKind::Legal, 0.0, 0.5},
      false);
  run("dirty-rt-opaque-safe",
      SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0}, true);
}

}  // namespace route_normalization_cases
}  // namespace ps
