#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "compute/dirty_region_planner.hpp"
#include "compute/resource_demand_estimator.hpp"
#include "compute/task_population_strategy.hpp"
#include "core/ops.hpp"                   // NOLINT(build/include_subdir)
#include "core/region_image_adapter.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_model.hpp"          // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"
#include "graph/roi_propagation_service.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/region.hpp"

namespace ps {
namespace {

/**
 * @brief Creates one bounded image source for propagation contract tests.
 * @param id Graph-local node id.
 * @return Source node with an 8x6 declared extent.
 * @throws std::bad_alloc when parameter storage cannot allocate.
 */
Node make_region_source(int id) {
  Node node;
  node.id = id;
  node.name = "region_source";
  node.type = "image_generator";
  node.subtype = "constant";
  node.parameters["width"] = 8;
  node.parameters["height"] = 6;
  return node;
}

/**
 * @brief Creates one single-input node for Region propagation tests.
 * @param id Graph-local node id.
 * @param parent_id Upstream image source id.
 * @param subtype Operation subtype to exercise.
 * @return Configured image_process node.
 * @throws std::bad_alloc when string or input storage cannot allocate.
 */
Node make_region_child(int id, int parent_id, const std::string& subtype) {
  Node node;
  node.id = id;
  node.name = subtype;
  node.type = "image_process";
  node.subtype = subtype;
  node.image_inputs.push_back({parent_id, "image"});
  return node;
}

/**
 * @brief Creates one sealed non-image rank-four DenseTensor for planner tests.
 * @return Shape [1,3,4,3] with contiguous unsigned-8 bytes and no ImageFacet.
 * @throws std::invalid_argument, std::overflow_error, or std::bad_alloc from
 * Value validation and immutable publication.
 * @note Omitting ImageFacet proves TensorSlice planning does not reinterpret
 * rank-general logical axes as two-dimensional image geometry.
 */
Value make_region_rank_four_tensor() {
  DenseTensorDescriptor descriptor{{1U, 3U, 4U, 3U},
                                   ElementSemantics::UnsignedInteger,
                                   StorageEncoding{8U}};
  StridedLayout layout{{36, 12, 3, 1}};
  std::vector<std::byte> storage(36U, std::byte{7U});
  return Value::from_cpu_dense_tensor(std::move(descriptor), std::nullopt,
                                      std::move(layout), std::move(storage));
}

/**
 * @brief Returns complete validity for make_region_rank_four_tensor().
 * @return Exact rank-four TensorSlice covering shape `[1,3,4,3]`.
 * @throws std::bad_alloc when Region storage cannot allocate.
 */
RegionSet full_region_rank_four_tensor() {
  return RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 3U}, {0U, 4U}, {0U, 3U}}});
}

/**
 * @brief Isolates same-key route-selection tests from the process registry.
 *
 * Each case starts from the canonical core `invert_dense` callback and restores
 * that complete key after device candidates have been exercised.
 *
 * @throws Registry allocation or callback-copy exceptions from setup/teardown.
 * @note GTest invokes TearDown after fatal assertions, so later Region tests
 *       never inherit fake device implementations or a missing core callback.
 */
class RegionRouteSelection : public ::testing::Test {
 protected:
  /**
   * @brief Publishes and retains the exact core dense callback for one case.
   * @return Nothing.
   * @throws Registry allocation or callback-copy exceptions unchanged.
   * @note A fatal assertion leaves TearDown responsible for key restoration.
   */
  void SetUp() override {
    ops::register_core_operations();
    const auto selected = OpRegistry::instance().resolve_for_intent(
        "image_process", "invert_dense", ComputeIntent::GlobalHighPrecision);
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*selected));
    core_operation_ = std::get<MonolithicOpFunc>(*selected);
  }

  /**
   * @brief Removes fake candidates and republishes the canonical core key.
   * @return Nothing.
   * @throws Registry allocation or callback-copy exceptions from restoration.
   * @note Whole-key removal also retires every device implementation identity.
   */
  void TearDown() override {
    (void)OpRegistry::instance().unregister_key("image_process:invert_dense");
    ops::register_core_operations();
  }

  /** @brief Owned exact core callback retained before test registration. */
  MonolithicOpFunc core_operation_;
};

TEST(RegionContract, DistinguishesCanonicalWholeAndEmpty) {
  const RegionSet empty = RegionSet::empty();
  const RegionSet whole = RegionSet::whole();

  EXPECT_TRUE(empty.is_empty());
  EXPECT_TRUE(whole.is_whole());
  EXPECT_FALSE(empty == whole);
  EXPECT_TRUE(empty.atoms().empty());
  EXPECT_TRUE(whole.atoms().empty());
  EXPECT_EQ(region_contains(whole, empty), RegionContainmentStatus::Contains);
  EXPECT_EQ(region_contains(empty, whole),
            RegionContainmentStatus::DoesNotContain);
}

TEST(RegionContract, PreservesRankGeneralTensorSlice) {
  TensorSlice slice;
  slice.axes = {{1U, 2U}, {3U, 7U}, {0U, 1U}, {4U, 9U}};

  const RegionSet region = RegionSet::from_tensor_slice(slice);

  ASSERT_EQ(region.atoms().size(), 1U);
  const TensorSlice& retained = std::get<TensorSlice>(region.atoms().front());
  EXPECT_EQ(retained.domain, dense_tensor_region_domain());
  EXPECT_EQ(retained.axes, slice.axes);
}

TEST(RegionContract, ChargesOwnedAtomAndTensorAxisStorage) {
  const RegionSet image =
      RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 6});
  const RegionSet tensor = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 3U}, {0U, 4U}, {0U, 3U}}});

  EXPECT_EQ(compute::region_dynamic_retained_memory_bytes(RegionSet::whole()),
            0U);
  const std::uint64_t image_bytes =
      compute::region_dynamic_retained_memory_bytes(image);
  const std::uint64_t tensor_bytes =
      compute::region_dynamic_retained_memory_bytes(tensor);
  EXPECT_GT(image_bytes, 0U);
  EXPECT_GT(tensor_bytes, image_bytes);
}

TEST(RegionContract, CanonicalizesAnyEmptyIntervalToEmpty) {
  EXPECT_TRUE(RegionSet::from_image_rect({image_region_domain(), -2, -2, 4, 9})
                  .is_empty());

  TensorSlice slice;
  slice.axes = {{0U, 5U}, {8U, 8U}, {0U, 1U}};
  EXPECT_TRUE(RegionSet::from_tensor_slice(std::move(slice)).is_empty());
}

TEST(RegionContract, RejectsInvalidKeysRanksIntervalsAndConflicts) {
  EXPECT_THROW(RegionSet::from_image_rect({{}, 0, 1, 0, 1}),
               std::invalid_argument);
  EXPECT_THROW(RegionSet::from_image_rect({image_region_domain(), 2, 1, 0, 1}),
               std::invalid_argument);
  EXPECT_THROW(RegionSet::from_tensor_slice(
                   TensorSlice{dense_tensor_region_domain(), {}}),
               std::invalid_argument);
  EXPECT_THROW(RegionSet::from_tensor_slice(
                   TensorSlice{dense_tensor_region_domain(), {{3U, 2U}}}),
               std::invalid_argument);

  const RegionDomainKey shared{7U, 11U};
  EXPECT_THROW(
      RegionSet::from_atoms({ImageRect{shared, 0, 2, 0, 2},
                             TensorSlice{shared, {{0U, 2U}, {0U, 2U}}}}),
      std::invalid_argument);
}

TEST(RegionContract, RejectsMoreThanEightNormalizedAtoms) {
  std::vector<RegionAtom> atoms;
  for (std::uint64_t index = 1U; index <= RegionSet::kMaximumAtoms + 1U;
       ++index) {
    atoms.push_back(ImageRect{{index, 1U}, 0, 1, 0, 1});
  }

  EXPECT_THROW(RegionSet::from_atoms(std::move(atoms)), std::length_error);
}

TEST(RegionContract, NormalizesDuplicateDomainsByIntersection) {
  const RegionDomainKey domain{5U, 9U};
  const RegionSet region = RegionSet::from_atoms(
      {ImageRect{domain, -4, 8, 0, 12}, ImageRect{domain, 2, 10, 3, 7}});

  ASSERT_EQ(region.atoms().size(), 1U);
  EXPECT_EQ(std::get<ImageRect>(region.atoms().front()),
            (ImageRect{domain, 2, 8, 3, 7}));
}

TEST(RegionContract, IntersectsAndClipsImageRectExactly) {
  const RegionSet left =
      RegionSet::from_image_rect({image_region_domain(), -4, 10, 2, 12});
  const RegionOperationResult clipped =
      clip_region_to_image_bounds(left, {image_region_domain(), 0, 8, 0, 6});

  ASSERT_EQ(clipped.status(), RegionOperationStatus::Exact);
  ASSERT_TRUE(clipped.region().has_value());
  ASSERT_EQ(clipped.region()->atoms().size(), 1U);
  EXPECT_EQ(std::get<ImageRect>(clipped.region()->atoms().front()),
            (ImageRect{image_region_domain(), 0, 8, 2, 6}));
}

TEST(RegionContract, CanonicalizesDisjointIntersectionsToEmpty) {
  const RegionSet left_image =
      RegionSet::from_image_rect({image_region_domain(), -8, -4, 0, 4});
  const RegionSet right_image =
      RegionSet::from_image_rect({image_region_domain(), 2, 7, 0, 4});
  const RegionSet left_tensor = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 2U}, {4U, 8U}}});
  const RegionSet right_tensor = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{3U, 5U}, {4U, 8U}}});

  const RegionOperationResult image =
      intersect_regions(left_image, right_image);
  const RegionOperationResult tensor =
      intersect_regions(left_tensor, right_tensor);

  ASSERT_EQ(image.status(), RegionOperationStatus::Exact);
  ASSERT_TRUE(image.region().has_value());
  EXPECT_TRUE(image.region()->is_empty());
  ASSERT_EQ(tensor.status(), RegionOperationStatus::Exact);
  ASSERT_TRUE(tensor.region().has_value());
  EXPECT_TRUE(tensor.region()->is_empty());
}

TEST(RegionContract, ReturnsTooComplexBeforeNineAtomIntersectionConstruction) {
  std::vector<RegionAtom> left_atoms;
  std::vector<RegionAtom> right_atoms;
  for (std::uint64_t index = 1U; index <= 5U; ++index) {
    left_atoms.push_back(ImageRect{{10U, index}, 0, 2, 0, 2});
  }
  for (std::uint64_t index = 1U; index <= 4U; ++index) {
    right_atoms.push_back(ImageRect{{20U, index}, 0, 2, 0, 2});
  }

  const RegionOperationResult result =
      intersect_regions(RegionSet::from_atoms(std::move(left_atoms)),
                        RegionSet::from_atoms(std::move(right_atoms)));

  EXPECT_EQ(result.status(), RegionOperationStatus::TooComplex);
  EXPECT_FALSE(result.region().has_value());
}

TEST(RegionContract, ClipsTensorShapeAndRejectsRankMismatch) {
  const RegionSet slice = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{1U, 7U}, {0U, 9U}, {0U, 1U}, {2U, 8U}}});
  const RegionOperationResult clipped = clip_region_to_tensor_shape(
      slice, dense_tensor_region_domain(), {4U, 5U, 1U, 6U});

  ASSERT_EQ(clipped.status(), RegionOperationStatus::Exact);
  ASSERT_EQ(clipped.region()->atoms().size(), 1U);
  EXPECT_EQ(
      std::get<TensorSlice>(clipped.region()->atoms().front()).axes,
      (std::vector<RegionInterval>{{1U, 4U}, {0U, 5U}, {0U, 1U}, {2U, 6U}}));

  const RegionOperationResult mismatch = clip_region_to_tensor_shape(
      slice, dense_tensor_region_domain(), {4U, 5U, 1U});
  EXPECT_EQ(mismatch.status(), RegionOperationStatus::Unsupported);
  EXPECT_FALSE(mismatch.region().has_value());
}

TEST(RegionContract, LabelsNonrectangularUnionOnlyWhenRequested) {
  const RegionSet left =
      RegionSet::from_image_rect({image_region_domain(), 0, 2, 0, 2});
  const RegionSet right =
      RegionSet::from_image_rect({image_region_domain(), 4, 6, 3, 5});

  const RegionOperationResult exact_only = union_regions(left, right);
  EXPECT_EQ(exact_only.status(), RegionOperationStatus::TooComplex);
  EXPECT_FALSE(exact_only.region().has_value());

  RegionComplexityBudget budget;
  budget.allow_conservative_superset = true;
  const RegionOperationResult widened = union_regions(left, right, budget);
  ASSERT_EQ(widened.status(), RegionOperationStatus::ConservativeSuperset);
  ASSERT_TRUE(widened.region().has_value());
  EXPECT_FALSE(widened.reason().empty());
  EXPECT_EQ(std::get<ImageRect>(widened.region()->atoms().front()),
            (ImageRect{image_region_domain(), 0, 6, 0, 5}));
}

TEST(RegionContract, ComputesRepresentableUnionAndDifference) {
  const RegionSet upper =
      RegionSet::from_image_rect({image_region_domain(), 0, 8, 0, 4});
  const RegionSet lower =
      RegionSet::from_image_rect({image_region_domain(), 0, 8, 4, 9});
  const RegionOperationResult joined = union_regions(upper, lower);
  ASSERT_EQ(joined.status(), RegionOperationStatus::Exact);
  EXPECT_EQ(std::get<ImageRect>(joined.region()->atoms().front()),
            (ImageRect{image_region_domain(), 0, 8, 0, 9}));

  const RegionOperationResult remainder =
      difference_regions(*joined.region(), upper);
  ASSERT_EQ(remainder.status(), RegionOperationStatus::Exact);
  EXPECT_EQ(std::get<ImageRect>(remainder.region()->atoms().front()),
            (ImageRect{image_region_domain(), 0, 8, 4, 9}));
}

/**
 * @brief Proves exact difference preserves equal atoms in a shared clause.
 * @return Nothing; GoogleTest reports clause or remainder mismatches.
 * @throws Region validation or allocation exceptions from test construction.
 * @note Only the image atom varies; the tensor atom is an identical
 * conjunctive constraint that must survive the representable edge removal.
 */
TEST(RegionContract, DifferencePreservesSharedClauseAtoms) {
  const RegionDomainKey image_domain{30U, 1U};
  const RegionDomainKey shared_domain{30U, 2U};
  const TensorSlice shared{shared_domain, {{2U, 5U}, {7U, 9U}}};
  const RegionSet left =
      RegionSet::from_atoms({ImageRect{image_domain, 0, 10, 0, 3}, shared});
  const RegionSet right =
      RegionSet::from_atoms({ImageRect{image_domain, 0, 4, 0, 3}, shared});

  const RegionOperationResult remainder = difference_regions(left, right);

  ASSERT_EQ(remainder.status(), RegionOperationStatus::Exact);
  ASSERT_TRUE(remainder.region().has_value());
  EXPECT_EQ(
      *remainder.region(),
      RegionSet::from_atoms({ImageRect{image_domain, 4, 10, 0, 3}, shared}));
}

/**
 * @brief Proves shared-clause difference also supports one TensorSlice edge.
 * @return Nothing; GoogleTest reports clause or tensor remainder mismatches.
 * @throws Region validation or allocation exceptions from test construction.
 * @note The equal ImageRect remains conjunctive while one tensor axis loses
 * its low edge.
 */
TEST(RegionContract, DifferencePreservesSharedTensorClauseAtoms) {
  const RegionDomainKey tensor_domain{30U, 3U};
  const RegionDomainKey shared_domain{30U, 4U};
  const ImageRect shared{shared_domain, -2, 3, 4, 8};
  const RegionSet left = RegionSet::from_atoms(
      {TensorSlice{tensor_domain, {{0U, 10U}, {2U, 5U}}}, shared});
  const RegionSet right = RegionSet::from_atoms(
      {TensorSlice{tensor_domain, {{0U, 4U}, {2U, 5U}}}, shared});

  const RegionOperationResult remainder = difference_regions(left, right);

  ASSERT_EQ(remainder.status(), RegionOperationStatus::Exact);
  ASSERT_TRUE(remainder.region().has_value());
  EXPECT_EQ(*remainder.region(),
            RegionSet::from_atoms(
                {TensorSlice{tensor_domain, {{4U, 10U}, {2U, 5U}}}, shared}));
}

/**
 * @brief Proves changing two clause atoms remains outside one-clause exactness.
 * @return Nothing; GoogleTest reports accidental constraint loss.
 * @throws Region validation or allocation exceptions from test construction.
 * @note The mathematical remainder needs two clauses, so neither atom may be
 * silently selected as the sole varying constraint.
 */
TEST(RegionContract, DifferenceRejectsTwoVaryingClauseAtoms) {
  const RegionDomainKey first_domain{30U, 5U};
  const RegionDomainKey second_domain{30U, 6U};
  const RegionSet left =
      RegionSet::from_atoms({ImageRect{first_domain, 0, 10, 0, 3},
                             ImageRect{second_domain, 0, 10, 0, 3}});
  const RegionSet right =
      RegionSet::from_atoms({ImageRect{first_domain, 0, 4, 0, 3},
                             ImageRect{second_domain, 0, 4, 0, 3}});

  const RegionOperationResult remainder = difference_regions(left, right);

  EXPECT_EQ(remainder.status(), RegionOperationStatus::TooComplex);
  EXPECT_FALSE(remainder.region().has_value());
}

TEST(RegionContract, MergesOneVaryingAtomWhilePreservingSharedClauseAtoms) {
  const RegionDomainKey image_domain{31U, 1U};
  const RegionDomainKey shared_domain{31U, 2U};
  const TensorSlice shared{shared_domain, {{2U, 5U}, {7U, 9U}}};
  const RegionSet left =
      RegionSet::from_atoms({ImageRect{image_domain, 0, 2, 0, 3}, shared});
  const RegionSet right =
      RegionSet::from_atoms({ImageRect{image_domain, 2, 4, 0, 3}, shared});

  const RegionOperationResult joined = union_regions(left, right);

  ASSERT_EQ(joined.status(), RegionOperationStatus::Exact);
  ASSERT_TRUE(joined.region().has_value());
  EXPECT_EQ(
      *joined.region(),
      RegionSet::from_atoms({ImageRect{image_domain, 0, 4, 0, 3}, shared}));
}

TEST(RegionContract, MergesOneVaryingTensorAtomInSharedClause) {
  const RegionDomainKey tensor_domain{32U, 1U};
  const RegionDomainKey shared_domain{32U, 2U};
  const ImageRect shared{shared_domain, -2, 3, 4, 8};
  const RegionSet left = RegionSet::from_atoms(
      {TensorSlice{tensor_domain, {{0U, 1U}, {0U, 2U}, {3U, 5U}}}, shared});
  const RegionSet right = RegionSet::from_atoms(
      {TensorSlice{tensor_domain, {{0U, 1U}, {2U, 4U}, {3U, 5U}}}, shared});

  const RegionOperationResult joined = union_regions(left, right);

  ASSERT_EQ(joined.status(), RegionOperationStatus::Exact);
  ASSERT_TRUE(joined.region().has_value());
  EXPECT_EQ(*joined.region(),
            RegionSet::from_atoms(
                {TensorSlice{tensor_domain, {{0U, 1U}, {0U, 4U}, {3U, 5U}}},
                 shared}));
}

TEST(RegionContract, RejectsUnionWhenTwoClauseAtomsDiffer) {
  const RegionDomainKey first_domain{33U, 1U};
  const RegionDomainKey second_domain{33U, 2U};
  const RegionSet left =
      RegionSet::from_atoms({ImageRect{first_domain, 0, 2, 0, 1},
                             ImageRect{second_domain, 0, 2, 0, 1}});
  const RegionSet right =
      RegionSet::from_atoms({ImageRect{first_domain, 2, 4, 0, 1},
                             ImageRect{second_domain, 2, 4, 0, 1}});

  const RegionOperationResult joined = union_regions(left, right);

  EXPECT_EQ(joined.status(), RegionOperationStatus::TooComplex);
  EXPECT_FALSE(joined.region().has_value());
}

TEST(RegionContract, PreservesTypedBudgetAndKindFailures) {
  const RegionSet image =
      RegionSet::from_image_rect({image_region_domain(), 0, 4, 0, 4});
  const RegionSet tensor = RegionSet::from_tensor_slice(
      {image_region_domain(), {{0U, 4U}, {0U, 4U}}});

  RegionComplexityBudget no_budget;
  no_budget.maximum_atoms = 0U;
  RegionComplexityBudget one_atom;
  one_atom.maximum_atoms = 1U;
  const RegionSet two_atoms = RegionSet::from_atoms(
      {ImageRect{{9U, 1U}, 0, 4, 0, 4}, ImageRect{{9U, 2U}, 0, 4, 0, 4}});
  EXPECT_EQ(intersect_regions(image, image, no_budget).status(),
            RegionOperationStatus::TooComplex);
  EXPECT_EQ(union_regions(RegionSet::empty(), two_atoms, one_atom).status(),
            RegionOperationStatus::TooComplex);
  EXPECT_EQ(
      difference_regions(two_atoms, RegionSet::empty(), one_atom).status(),
      RegionOperationStatus::TooComplex);
  EXPECT_EQ(intersect_regions(image, tensor).status(),
            RegionOperationStatus::Unsupported);
  EXPECT_EQ(region_contains(image, tensor),
            RegionContainmentStatus::Unsupported);
}

TEST(RegionContract, DoesNotOverflowSignedImageEndpointsDuringAlgebra) {
  const RegionSet extremes = RegionSet::from_image_rect(
      {image_region_domain(), std::numeric_limits<std::int64_t>::min(),
       std::numeric_limits<std::int64_t>::max(),
       std::numeric_limits<std::int64_t>::min(),
       std::numeric_limits<std::int64_t>::max()});
  const RegionSet center =
      RegionSet::from_image_rect({image_region_domain(), -1, 1, -1, 1});

  const RegionOperationResult overlap = intersect_regions(extremes, center);
  ASSERT_EQ(overlap.status(), RegionOperationStatus::Exact);
  EXPECT_TRUE(*overlap.region() == center);
}

TEST(RegionImageAdapter, RoundTripsOnlyExactImageRect) {
  const PixelRect source{-4, 7, 9, 11};
  const RegionSet region = region_image_adapter::from_pixel_rect(source);

  EXPECT_EQ(region_image_adapter::to_pixel_rect(region), source);
  EXPECT_EQ(region_image_adapter::exact_result_to_pixel_rect(
                RegionOperationResult::exact(region)),
            source);
}

TEST(RegionImageAdapter, RejectsTensorWholeUncertaintyAndOverflow) {
  EXPECT_THROW(region_image_adapter::to_pixel_rect(RegionSet::whole()),
               std::invalid_argument);
  EXPECT_THROW(region_image_adapter::to_pixel_rect(RegionSet::from_tensor_slice(
                   {dense_tensor_region_domain(), {{0U, 1U}, {0U, 1U}}})),
               std::invalid_argument);
  EXPECT_THROW(region_image_adapter::exact_result_to_pixel_rect(
                   RegionOperationResult::failure(
                       RegionOperationStatus::Unknown, "missing transform")),
               std::invalid_argument);
  EXPECT_THROW(
      region_image_adapter::to_pixel_rect(RegionSet::from_image_rect(
          {image_region_domain(),
           static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1,
           static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 2, 0,
           1})),
      std::overflow_error);
  EXPECT_THROW(
      region_image_adapter::to_pixel_rect(RegionSet::from_image_rect(
          {image_region_domain(), std::numeric_limits<std::int64_t>::min(),
           std::numeric_limits<std::int64_t>::max(), 0, 1})),
      std::overflow_error);
  EXPECT_THROW(region_image_adapter::to_pixel_rect(
                   RegionSet::from_image_rect({{7U, 9U}, 0, 1, 0, 1})),
               std::invalid_argument);
  EXPECT_THROW(region_image_adapter::from_pixel_rect({0, 0, -1, 1}),
               std::invalid_argument);
}

TEST(RegionPropagation, PreservesTensorSliceThroughExplicitCoreIdentity) {
  ops::register_core_operations();
  GraphModel graph("");
  graph.add_node(make_region_source(1));
  graph.add_node(make_region_child(2, 1, "invert_dense"));
  graph.validate_topology();
  const RegionSet slice = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {2U, 5U}, {1U, 4U}, {0U, 1U}}});
  RoiPropagationService propagation;

  const RegionOperationResult forward =
      propagation.project_region_forward(graph, 1, slice, 2);
  const RegionOperationResult backward =
      propagation.project_region_backward(graph, 2, slice, 1);
  std::unordered_map<int, PixelSize> size_cache;
  const RegionOperationResult immediate = propagation.compute_upstream_region(
      graph.node(2), slice, graph, size_cache);

  ASSERT_EQ(forward.status(), RegionOperationStatus::Exact);
  ASSERT_EQ(backward.status(), RegionOperationStatus::Exact);
  ASSERT_EQ(immediate.status(), RegionOperationStatus::Exact);
  EXPECT_TRUE(*forward.region() == slice);
  EXPECT_TRUE(*backward.region() == slice);
  EXPECT_TRUE(*immediate.region() == slice);
}

TEST_F(RegionRouteSelection,
       RejectsTensorSliceWhenGpuRouteReplacesCoreIdentity) {
  OpMetadata gpu_metadata;
  gpu_metadata.cost_score = 1;
  OpRegistry::instance().register_impl(
      "image_process", "invert_dense", Device::GPU_METAL,
      MonolithicOpFunc([](const Node&, const std::vector<const NodeOutput*>&) {
        return NodeOutput{};
      }),
      gpu_metadata);
  const std::vector<Device> routed_devices{Device::GPU_METAL, Device::CPU};
  const auto selected = OpRegistry::instance().select_implementation(
      "image_process", "invert_dense", routed_devices,
      ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(selected.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(selected->func));
  EXPECT_EQ(selected->metadata.device_preference, Device::GPU_METAL);
  EXPECT_FALSE(ops::find_core_region_monolithic_operation(
                   "image_process", "invert_dense",
                   std::get<MonolithicOpFunc>(selected->func))
                   .has_value());

  GraphModel graph("");
  Node source = make_region_source(1);
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_region_rank_four_tensor();
  source.hp_region = full_region_rank_four_tensor();
  graph.add_node(std::move(source));
  graph.add_node(make_region_child(2, 1, "invert_dense"));
  graph.validate_topology();
  const RegionSet requested = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 2U}, {1U, 4U}, {0U, 2U}}});

  RoiPropagationService routed_propagation(routed_devices,
                                           ComputeIntent::GlobalHighPrecision);
  EXPECT_FALSE(
      routed_propagation.supports_tensor_region_execution(graph.node(2)));
  const RegionOperationResult forward =
      routed_propagation.project_region_forward(graph, 1, requested, 2);
  const RegionOperationResult backward =
      routed_propagation.project_region_backward(graph, 2, requested, 1);
  std::unordered_map<int, PixelSize> size_cache;
  const RegionOperationResult immediate =
      routed_propagation.compute_upstream_region(graph.node(2), requested,
                                                 graph, size_cache);
  EXPECT_EQ(forward.status(), RegionOperationStatus::Unsupported);
  EXPECT_EQ(backward.status(), RegionOperationStatus::Unsupported);
  EXPECT_EQ(immediate.status(), RegionOperationStatus::Unsupported);

  GraphTraversalService traversal;
  compute::DirtyRegionPlanner routed_planner(traversal, routed_propagation);
  EXPECT_THROW(routed_planner.plan_high_precision(graph, 2, requested),
               GraphError);

  RoiPropagationService cpu_propagation;
  EXPECT_TRUE(cpu_propagation.supports_tensor_region_execution(graph.node(2)));
  const RegionOperationResult cpu_forward =
      cpu_propagation.project_region_forward(graph, 1, requested, 2);
  ASSERT_EQ(cpu_forward.status(), RegionOperationStatus::Exact);
  EXPECT_EQ(cpu_forward.region(), requested);
  compute::DirtyRegionPlanner cpu_planner(traversal, cpu_propagation);
  const compute::HighPrecisionDirtyPlan cpu_plan =
      cpu_planner.plan_high_precision(graph, 2, requested);
  EXPECT_EQ(cpu_plan.execution_order, (std::vector<int>{2}));
}

TEST_F(RegionRouteSelection, UsesRequestIntentWhenSelectingTensorIdentity) {
  OpMetadata cpu_metadata;
  cpu_metadata.cost_score = 1;
  OpRegistry::instance().register_impl("image_process", "invert_dense",
                                       Device::CPU, core_operation_,
                                       cpu_metadata);
  OpMetadata accelerator_metadata;
  accelerator_metadata.cost_score = 100;
  OpRegistry::instance().register_impl(
      "image_process", "invert_dense", Device::ASIC_NPU,
      MonolithicOpFunc([](const Node&, const std::vector<const NodeOutput*>&) {
        return NodeOutput{};
      }),
      accelerator_metadata);
  const std::vector<Device> routed_devices{Device::CPU, Device::ASIC_NPU};
  const auto hp_selected = OpRegistry::instance().select_implementation(
      "image_process", "invert_dense", routed_devices,
      ComputeIntent::GlobalHighPrecision);
  const auto rt_selected = OpRegistry::instance().select_implementation(
      "image_process", "invert_dense", routed_devices,
      ComputeIntent::RealTimeUpdate);
  ASSERT_TRUE(hp_selected.has_value());
  ASSERT_TRUE(rt_selected.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(hp_selected->func));
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(rt_selected->func));
  EXPECT_EQ(hp_selected->metadata.device_preference, Device::ASIC_NPU);
  EXPECT_EQ(rt_selected->metadata.device_preference, Device::CPU);
  EXPECT_FALSE(ops::find_core_region_monolithic_operation(
                   "image_process", "invert_dense",
                   std::get<MonolithicOpFunc>(hp_selected->func))
                   .has_value());
  EXPECT_TRUE(ops::find_core_region_monolithic_operation(
                  "image_process", "invert_dense",
                  std::get<MonolithicOpFunc>(rt_selected->func))
                  .has_value());

  GraphModel graph("");
  graph.add_node(make_region_source(1));
  graph.add_node(make_region_child(2, 1, "invert_dense"));
  graph.validate_topology();
  const RegionSet requested = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 2U}, {1U, 4U}, {0U, 2U}}});
  RoiPropagationService hp_propagation(routed_devices,
                                       ComputeIntent::GlobalHighPrecision);
  RoiPropagationService rt_propagation(routed_devices,
                                       ComputeIntent::RealTimeUpdate);

  EXPECT_FALSE(hp_propagation.supports_tensor_region_execution(graph.node(2)));
  EXPECT_TRUE(rt_propagation.supports_tensor_region_execution(graph.node(2)));
  const RegionOperationResult hp_forward =
      hp_propagation.project_region_forward(graph, 1, requested, 2);
  const RegionOperationResult rt_forward =
      rt_propagation.project_region_forward(graph, 1, requested, 2);
  EXPECT_EQ(hp_forward.status(), RegionOperationStatus::Unsupported);
  ASSERT_EQ(rt_forward.status(), RegionOperationStatus::Exact);
  EXPECT_EQ(rt_forward.region(), requested);
}

TEST(RegionPropagation, RejectsTensorSliceBeforeRectangularCallback) {
  ops::register_core_operations();
  GraphModel graph("");
  graph.add_node(make_region_source(1));
  graph.add_node(make_region_child(2, 1, "gaussian_blur"));
  graph.validate_topology();
  const RegionSet slice = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 4U}, {0U, 4U}}});
  RoiPropagationService propagation;
  std::unordered_map<int, PixelSize> size_cache;

  const RegionOperationResult projected =
      propagation.project_region_forward(graph, 1, slice, 2);
  const RegionOperationResult immediate = propagation.compute_upstream_region(
      graph.node(2), slice, graph, size_cache);

  EXPECT_EQ(projected.status(), RegionOperationStatus::Unsupported);
  EXPECT_EQ(immediate.status(), RegionOperationStatus::Unsupported);
  EXPECT_FALSE(projected.region().has_value());
  EXPECT_FALSE(immediate.region().has_value());
}

TEST(RegionPropagation, AdaptsExactImageRectThroughCurrentV2Callback) {
  ops::register_core_operations();
  GraphModel graph("");
  graph.add_node(make_region_source(1));
  graph.add_node(make_region_child(2, 1, "invert_dense"));
  graph.validate_topology();
  const RegionSet image =
      RegionSet::from_image_rect({image_region_domain(), 1, 6, 2, 5});
  RoiPropagationService propagation;

  const RegionOperationResult projected =
      propagation.project_region_backward(graph, 2, image, 1);

  ASSERT_EQ(projected.status(), RegionOperationStatus::Exact);
  ASSERT_TRUE(projected.region().has_value());
  EXPECT_TRUE(*projected.region() == image);
}

TEST(RegionPlanning, ClipsRankGeneralTensorAndRejectsRtProjection) {
  ops::register_core_operations();
  GraphModel graph("");
  Node source = make_region_source(1);
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_region_rank_four_tensor();
  source.hp_region = full_region_rank_four_tensor();
  graph.add_node(std::move(source));
  graph.add_node(make_region_child(2, 1, "invert_dense"));
  graph.validate_topology();
  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const RegionSet requested = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 9U}, {2U, 7U}, {1U, 3U}}});
  const RegionSet expected = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {2U, 4U}, {1U, 3U}}});

  const compute::HighPrecisionDirtyPlan plan =
      planner.plan_high_precision(graph, 2, requested);

  ASSERT_EQ(plan.execution_order, (std::vector<int>{2}));
  ASSERT_EQ(plan.entries.size(), 1U);
  EXPECT_EQ(plan.entries.at(2).region_hp, expected);
  EXPECT_EQ(plan.entries.at(2).roi_hp, PixelRect{});
  ASSERT_EQ(plan.snapshot.dirty_monolithic_nodes.size(), 1U);
  EXPECT_EQ(plan.snapshot.dirty_monolithic_nodes.front().region, expected);
  EXPECT_EQ(plan.snapshot.dirty_monolithic_nodes.front().pixel_roi,
            PixelRect{});
  ASSERT_EQ(plan.snapshot.actual_dirty_regions.at(2).size(), 1U);
  EXPECT_EQ(plan.snapshot.actual_dirty_regions.at(2).front(), expected);
  ASSERT_EQ(plan.snapshot.source_region_records.at(2).size(), 1U);
  EXPECT_EQ(plan.snapshot.source_region_records.at(2).front().source_region,
            expected);
  ASSERT_EQ(plan.snapshot.edge_mappings.size(), 1U);
  EXPECT_EQ(plan.snapshot.edge_mappings.front().from_node_id, 1);
  EXPECT_EQ(plan.snapshot.edge_mappings.front().to_node_id, 2);
  EXPECT_EQ(plan.snapshot.edge_mappings.front().from_roi, PixelRect{});
  EXPECT_EQ(plan.snapshot.edge_mappings.front().to_roi, PixelRect{});
  EXPECT_EQ(plan.snapshot.edge_mappings.front().from_region, expected);
  EXPECT_EQ(plan.snapshot.edge_mappings.front().to_region, expected);

  compute::ComputePlan task_plan;
  task_plan.intent = ComputeIntent::GlobalHighPrecision;
  task_plan.target_node_id = 2;
  task_plan.planned_nodes = plan.execution_order;
  compute::PlannedNodeWork work;
  work.node_id = 2;
  work.domain = compute::DirtyDomain::HighPrecision;
  work.whole_output = true;
  task_plan.planned_work.push_back(work);
  compute::TaskPopulationStrategy{}.populate(
      task_plan, &plan.snapshot, compute::DirtyDomain::HighPrecision, &graph);

  ASSERT_EQ(task_plan.task_graph.tasks.size(), 1U);
  EXPECT_EQ(task_plan.task_graph.tasks.front().kind,
            compute::PlannedTaskKind::Monolithic);
  EXPECT_EQ(task_plan.task_graph.tasks.front().output_roi, PixelRect{});
  EXPECT_TRUE(task_plan.task_graph.tasks.front().dirty_selected);
  EXPECT_THROW(planner.plan_real_time(graph, 2, requested), GraphError);
}

TEST(RegionPlanning, RejectsRankMismatchAndMissingTensorContract) {
  ops::register_core_operations();
  GraphModel graph("");
  Node source = make_region_source(1);
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_region_rank_four_tensor();
  source.hp_region = full_region_rank_four_tensor();
  graph.add_node(std::move(source));
  graph.add_node(make_region_child(2, 1, "invert_dense"));
  graph.add_node(make_region_child(3, 1, "gaussian_blur"));
  graph.validate_topology();
  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const RegionSet rank_three = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 3U}, {0U, 4U}}});
  const RegionSet rank_four = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 3U}, {0U, 4U}, {0U, 3U}}});
  const RegionSet custom_domain = RegionSet::from_tensor_slice(
      {{8U, 2U}, {{0U, 1U}, {0U, 3U}, {0U, 4U}, {0U, 3U}}});

  EXPECT_THROW(planner.plan_high_precision(graph, 2, rank_three), GraphError);
  EXPECT_THROW(planner.plan_high_precision(graph, 3, rank_four), GraphError);
  EXPECT_THROW(planner.plan_high_precision(graph, 2, custom_domain),
               GraphError);
}

TEST(RegionLifecycle, RetainsTensorSourceFactsWithoutPixelProjection) {
  ops::register_core_operations();
  GraphModel graph("");
  Node source = make_region_source(1);
  source.cached_output_high_precision = NodeOutput{};
  source.cached_output_high_precision->image_value =
      make_region_rank_four_tensor();
  source.hp_region = full_region_rank_four_tensor();
  graph.add_node(std::move(source));
  graph.add_node(make_region_child(2, 1, "invert_dense"));
  graph.validate_topology();
  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  const RegionSet first = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {0U, 2U}, {1U, 4U}, {0U, 2U}}});
  const RegionSet second = RegionSet::from_tensor_slice(
      {dense_tensor_region_domain(), {{0U, 1U}, {1U, 3U}, {0U, 2U}, {1U, 3U}}});

  EXPECT_THROW(planner.begin_dirty_source(
                   graph, 2, compute::DirtyDomain::RealTime, first),
               GraphError);
  EXPECT_EQ(graph.dirty_generation_counter, 0U);
  const compute::DirtyRegionSnapshot begun = planner.begin_dirty_source(
      graph, 2, compute::DirtyDomain::HighPrecision, first);
  ASSERT_EQ(begun.source_region_records.at(2).size(), 1U);
  EXPECT_TRUE(begun.source_roi_records.empty());
  ASSERT_EQ(begun.actual_dirty_regions.at(2).size(), 1U);
  EXPECT_EQ(begun.actual_dirty_regions.at(2).front(), first);
  EXPECT_TRUE(begun.actual_dirty_rois.empty());
  ASSERT_EQ(begun.dirty_monolithic_nodes.size(), 1U);
  EXPECT_EQ(begun.dirty_updating_count, 1U);
  EXPECT_NE(
      compute::DirtyRegionPlanner::describe_snapshot(begun).find("actual=1"),
      std::string::npos);

  const compute::DirtyRegionSnapshot updated = planner.update_dirty_source(
      graph, 2, compute::DirtyDomain::HighPrecision, second);
  EXPECT_EQ(updated.graph_generation, begun.graph_generation);
  ASSERT_EQ(updated.source_region_records.at(2).size(), 2U);
  ASSERT_EQ(updated.actual_dirty_regions.at(2).size(), 2U);
  EXPECT_EQ(updated.actual_dirty_regions.at(2).at(0), first);
  EXPECT_EQ(updated.actual_dirty_regions.at(2).at(1), second);
  EXPECT_TRUE(updated.actual_dirty_rois.empty());
  ASSERT_EQ(updated.dirty_monolithic_nodes.size(), 2U);

  const compute::DirtyRegionSnapshot ended =
      planner.end_dirty_source(graph, 2, compute::DirtyDomain::HighPrecision);
  EXPECT_EQ(ended.dirty_updating_count, 0U);
  EXPECT_EQ(ended.dirty_source_state.at(2).lifecycle,
            compute::DirtySourceLifecycleState::Settled);
  EXPECT_EQ(ended.actual_dirty_regions.at(2).size(), 2U);
}

}  // namespace
}  // namespace ps
