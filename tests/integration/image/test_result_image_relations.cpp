#include <algorithm>
#include <iostream>
#include <utility>
#include <vector>

#include "support/result_image_fixture.hpp"

namespace {
using namespace ps::test_image;  // NOLINT(build/namespaces)
void reshape_relations() {
  ResourceBudget root;
  const std::vector<std::vector<uint64_t>> shapes{
      {24},   {2, 12},   {3, 8},    {4, 6},
      {6, 4}, {2, 3, 4}, {4, 2, 3}, {1, 2, 1, 12}};
  uint64_t random = 17;
  const auto next = [&] {
    random = random * 6364136223846793005ULL + 1;
    return random;
  };
  for (const auto& output_shape : shapes)
    for (const auto& input_shape : shapes) {
      auto relation = take(ResultRelation::reshape(
          root, output_shape, Region::whole(output_shape), input_shape,
          Region::whole(input_shape),
          {0, 5, 0, 0, ResultSupportTarget::Tensor, 0}));
      for (unsigned trial = 0; trial < 12; ++trial) {
        std::vector<RegionDimension> dims;
        for (auto n : output_shape) {
          const auto first = next() % n;
          dims.push_back({first, 1 + next() % (n - first)});
        }
        auto request =
            take(Footprint::from_regions(output_shape, {Region(dims)}));
        std::vector<Region> expected_boxes;
        require(
            request
                .visit(
                    [&](const auto& at) {
                      uint64_t ordinal = 0;
                      for (size_t i = 0; i < at.size(); ++i)
                        ordinal = ordinal * output_shape[i] + at[i];
                      std::vector<RegionDimension> source(input_shape.size());
                      for (size_t i = input_shape.size(); i-- > 0;) {
                        source[i] = {ordinal % input_shape[i], 1};
                        ordinal /= input_shape[i];
                      }
                      expected_boxes.emplace_back(std::move(source));
                      return Status::success();
                    },
                    1000)
                .ok(),
            "reshape scalar projection oracle");
        auto expected =
            take(Footprint::from_regions(input_shape, expected_boxes));
        Footprint actual;
        require(relation.project(request,
                                 [&](auto support, const auto* samples) {
                                   require(
                                       samples &&
                                           support.target ==
                                               ResultSupportTarget::Tensor &&
                                           support.roles == 5,
                                       "reshape typed support");
                                   actual = *samples;
                                   return Status::success();
                                 })
                        .ok() &&
                    actual == expected,
                "reshape compact projection equals independent ordinal oracle");
        auto full = take(Footprint::all(output_shape));
        auto inverse = take(relation.preimage(
            full, {0, 4, 0, 0, ResultSupportTarget::Tensor, 0}, expected));
        require(inverse == request && relation.certify(request).ok(),
                "reshape compact inverse and structural witness");
      }
    }
  const std::vector<uint64_t> source_shape{4, 5}, output_shape{2, 3};
  const Region source({{1, 2}, {1, 3}}), witness({{1, 1}, {0, 3}});
  auto cropped = take(
      ResultRelation::reshape(root, output_shape, witness, source_shape, source,
                              {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto request =
      take(Footprint::from_regions(output_shape, {Region({{1, 1}, {0, 1}})}));
  auto expected =
      take(Footprint::from_regions(source_shape, {Region({{2, 1}, {1, 1}})}));
  Footprint actual;
  require(cropped.project(request,
                          [&](auto, const auto* samples) {
                            actual = *samples;
                            return Status::success();
                          })
                  .ok() &&
              actual == expected,
          "reshape ROI keeps full output ordinal basis and source offsets");
  require(take(cropped.preimage(take(Footprint::all(output_shape)),
                                {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                                expected)) == request,
          "reshape cropped inverse retains source offsets and output witness");
  auto outside =
      take(Footprint::from_regions(output_shape, {Region({{0, 1}, {0, 1}})}));
  require(cropped.certify(outside).code == ErrorCode::NotFound,
          "reshape witness cannot certify unrequested output samples");
  const std::vector<uint64_t> huge_source{UINT64_MAX, 6},
      huge_output{UINT64_MAX, 2, 3};
  auto huge = take(
      ResultRelation::reshape(root, huge_output, Region::whole(huge_output),
                              huge_source, Region::whole(huge_source),
                              {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto columns = take(Footprint::from_regions(
      huge_output, {Region({{0, UINT64_MAX}, {0, 2}, {1, 1}})}));
  auto source_columns = take(Footprint::from_regions(
      huge_source,
      {Region({{0, UINT64_MAX}, {1, 1}}), Region({{0, UINT64_MAX}, {4, 1}})}));
  FootprintLimits limits;
  limits.maximum_work = 5000;
  require(huge.project(
                  columns,
                  [&](auto, const auto* samples) {
                    actual = *samples;
                    return Status::success();
                  },
                  limits)
                  .ok() &&
              actual == source_columns,
          "huge shared reshape axis projects without row enumeration");
  require(
      take(huge.preimage(columns, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                         source_columns, limits)) == columns &&
          huge.certify(columns, limits).ok(),
      "huge reshape inverse and certification do not flatten the domain");
  constexpr uint64_t large = 1000000;
  for (bool reverse : {false, true}) {
    const std::vector<uint64_t> input = reverse
                                            ? std::vector<uint64_t>{2, large}
                                            : std::vector<uint64_t>{large, 2};
    const std::vector<uint64_t> output = reverse
                                             ? std::vector<uint64_t>{large, 2}
                                             : std::vector<uint64_t>{2, large};
    const Region queried =
        reverse ? Region({{0, large}, {0, 1}}) : Region({{0, 1}, {0, 1}});
    const Region changed_region =
        reverse ? Region({{0, 1}, {0, 1}}) : Region({{0, large}, {0, 1}});
    auto limited = take(ResultRelation::reshape(
        root, output, queried, input, Region::whole(input),
        {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
    auto q = take(Footprint::from_regions(output, {queried}));
    auto changes = take(Footprint::from_regions(input, {changed_region}));
    auto bound = limits;
    bound.maximum_boxes = 16;
    require(
        take(limited.preimage(q, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                              changes, bound)) ==
            take(Footprint::from_regions(output, {Region({{0, 1}, {0, 1}})})),
        "reshape dirty chooses the bounded direction for large stripe and "
        "singleton intersections");
    require(take(limited.preimage(take(Footprint::none(output)),
                                  {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                                  changes, bound))
                .empty(),
            "empty reshape demand does not expand changed stripes");
    auto empty_witness = take(ResultRelation::reshape(
        root, output,
        Region(std::vector<RegionDimension>(output.size(), {0, 0})), input,
        Region::whole(input), {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
    require(take(empty_witness.preimage(
                     q, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, changes,
                     bound))
                .empty(),
            "empty reshape witness has no dirty sample work");
  }
  auto partial = take(ResultRelation::reshape(
      root, {large, 2}, Region::whole({large, 2}), {2, large},
      Region::whole({2, large}), {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto half_column = take(
      Footprint::from_regions({large, 2}, {Region({{0, large / 2}, {0, 1}})}));
  auto first_row =
      take(Footprint::from_regions({2, large}, {Region({{0, 1}, {0, large}})}));
  auto bounded_projection = limits;
  bounded_projection.maximum_boxes = 16;
  require(take(partial.preimage(half_column,
                                {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                                first_row, bounded_projection)) == half_column,
          "reshape dirty uses compact row inverse despite a smaller scattered "
          "output cardinality");
  auto whole_column =
      take(Footprint::from_regions({large, 2}, {Region({{0, large}, {0, 1}})}));
  require(
      take(partial.preimage(
          whole_column, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
          take(Footprint::all({2, large})), bounded_projection)) ==
          whole_column,
      "whole changed reshape domain does not expand a scattered output query");
  ResourceBudget direct_work, callback_work;
  auto direct_relation = take(ResultRelation::reshape(
      direct_work, {2, 3}, Region::whole({2, 3}), {3, 2}, Region::whole({3, 2}),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto callback_relation = take(ResultRelation::reshape(
      callback_work, {2, 3}, Region::whole({2, 3}), {3, 2},
      Region::whole({3, 2}), {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto charged_query =
      take(Footprint::from_regions({2, 3}, {Region({{0, 2}, {1, 1}})}));
  uint64_t reported = 0;
  auto callback_limits = limits;
  callback_limits.consume_work = [&](uint64_t n) {
    reported += n;
    return callback_work.consume({n});
  };
  require(
      direct_relation
              .project(
                  charged_query,
                  [](auto, const auto*) { return Status::success(); }, limits)
              .ok() &&
          callback_relation
              .project(
                  charged_query,
                  [](auto, const auto*) { return Status::success(); },
                  callback_limits)
              .ok() &&
          reported > 0 &&
          direct_work.statistics().issued.work ==
              callback_work.statistics().issued.work,
      "reshape host callback precharges each work unit exactly once");
  const std::vector<uint64_t> wide(8, UINT64_MAX);
  auto extreme = take(ResultRelation::reshape(
      root, wide, Region::whole(wide), wide, Region::whole(wide),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  std::vector<RegionDimension> final_point(8, {UINT64_MAX - 1, 1});
  auto point = take(Footprint::from_regions(wide, {Region(final_point)}));
  require(extreme.project(
                     point,
                     [&](auto, const auto* samples) {
                       actual = *samples;
                       return Status::success();
                     },
                     limits)
                  .ok() &&
              actual == point,
          "rank8 reshape retains complete 512-bit coordinate identity");
  const std::vector<uint64_t> reassociated_input{UINT64_MAX, UINT64_MAX,
                                                 UINT64_MAX, 2},
      reassociated_output{2, UINT64_MAX, UINT64_MAX, UINT64_MAX};
  auto reassociated = take(ResultRelation::reshape(
      root, reassociated_output, Region::whole(reassociated_output),
      reassociated_input, Region::whole(reassociated_input),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto middle_output = take(Footprint::from_regions(
      reassociated_output, {Region({{1, 1}, {0, 1}, {0, 1}, {0, 1}})}));
  auto middle_input = take(
      Footprint::from_regions(reassociated_input, {Region({{UINT64_MAX / 2, 1},
                                                           {UINT64_MAX / 2, 1},
                                                           {UINT64_MAX / 2, 1},
                                                           {1, 1}})}));
  require(reassociated
                  .project(
                      middle_output,
                      [&](auto, const auto* samples) {
                        actual = *samples;
                        return Status::success();
                      },
                      limits)
                  .ok() &&
              actual == middle_input,
          "reshape reassociation uses ordinals exceeding 128 bits");
  require(take(reassociated.preimage(
              middle_output, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
              middle_input, limits)) == middle_output,
          "wide reassociation inverse retains complete coordinates");
  auto tight = limits;
  tight.maximum_boxes = 1;
  bool visited = false;
  require(huge.project(
                  columns,
                  [&](auto, const auto*) {
                    visited = true;
                    return Status::success();
                  },
                  tight)
                      .code == ErrorCode::ResourceExhausted &&
              !visited,
          "reshape holes exceeding rectangle budget never emit a bounding-box "
          "support");
  ResourceLimits unavailable;
  unavailable.capacity[ResourceKind::Entries] = 0;
  ResourceBudget denied(unavailable);
  auto exhausted = ResultRelation::reshape(
      denied, {3, 2}, Region::whole({3, 2}), {2, 3}, Region::whole({2, 3}),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0});
  require(!exhausted.ok() &&
              exhausted.status().code == ErrorCode::ResourceExhausted &&
              denied.statistics().live[ResourceKind::Metadata] == 0,
          "reshape constructor resource failure releases temporary metadata");
  require(!ResultRelation::reshape(root, {3}, Region::whole({3}), {2},
                                   Region::whole({2}),
                                   {0, 1, 0, 0, ResultSupportTarget::Tensor, 0})
               .ok(),
          "reshape cardinality mismatch is invalid");
  ResourceBudget measurement;
  auto measured = take(ResultRelation::reshape(
      measurement, {2, 3}, Region::whole({2, 3}), {3, 2}, Region::whole({3, 2}),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  ResourceLimits tight_metadata;
  tight_metadata.capacity[ResourceKind::Metadata] =
      measurement.statistics().live[ResourceKind::Metadata] + 1024;
  ResourceBudget constrained(tight_metadata);
  auto bounded = take(ResultRelation::reshape(
      constrained, {2, 3}, Region::whole({2, 3}), {3, 2}, Region::whole({3, 2}),
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto touched =
      take(Footprint::from_regions({3, 2}, {Region({{0, 3}, {1, 1}})}));
  const auto capacity_before = constrained.statistics().live.values;
  auto limited_inverse =
      bounded.preimage(take(Footprint::all({2, 3})),
                       {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, touched);
  require(!limited_inverse.ok() &&
              limited_inverse.status().code == ErrorCode::ResourceExhausted &&
              constrained.statistics().live.values == capacity_before,
          "reshape inverse metadata admission is typed and releases all "
          "temporary boxes");
  CancellationSource cancelled;
  cancelled.cancel();
  limits.cancellation = cancelled.token();
  require(huge.project(
                  columns, [](auto, const auto*) { return Status::success(); },
                  limits)
                  .code == ErrorCode::Cancelled,
          "reshape projection observes cancellation");
}
void prefix_relations() {
  ResourceBudget root;
  const ResultSupport address{3, 5, 0, 0, ResultSupportTarget::Tensor, 2};
  auto relation = take(
      ResultRelation::prefix(root, 6, 3, 5, ResultSupportTarget::Tensor, 2));
  auto set = [](unsigned mask) {
    std::vector<Region> points;
    for (uint64_t i = 0; i < 6; ++i)
      if (mask & (1U << i))
        points.emplace_back(std::vector<RegionDimension>{{i, 1}});
    return take(Footprint::from_regions({6}, points));
  };
  for (uint64_t i = 0; i < 6; ++i) {
    unsigned calls = 0;
    require(
        relation.visit(
                    i, 1,
                    [&](auto span) {
                      ++calls;
                      require(
                          span.input == 3 && span.roles == 5 &&
                              span.slot == 2 &&
                              span.target == ResultSupportTarget::Tensor &&
                              !span.first && span.count == i + 1,
                          "prefix scalar support preserves address and roles");
                      return Status::success();
                    })
                .ok() &&
            calls == 1,
        "one prefix node visits one inclusive span");
  }
  for (unsigned wanted = 0; wanted < 64; ++wanted) {
    auto outputs = set(wanted);
    require(relation.certify(outputs).ok(), "prefix sparse certificate");
    uint64_t actual = 0, expected = 0;
    for (uint64_t i = 0; i < 6; ++i)
      if (wanted & (1U << i))
        expected = i + 1;
    require(relation.project(outputs,
                             [&](auto span, const Footprint* points) {
                               require(!points && !span.first,
                                       "prefix flat projection");
                               actual = span.count;
                               return Status::success();
                             })
                    .ok() &&
                actual == expected,
            "prefix projection matches enumerated union including Empty");
    for (unsigned edited = 0; edited < 64; ++edited) {
      unsigned expected_dirty = 0;
      for (unsigned i = 0; i < 6; ++i)
        for (unsigned j = 0; j <= i; ++j)
          if ((wanted & (1U << i)) && (edited & (1U << j)))
            expected_dirty |= 1U << i;
      auto dirty = take(relation.preimage(outputs, address, set(edited)));
      require(dirty == set(expected_dirty),
              "prefix inverse matches independent per-point dependency");
    }
  }
  auto all = take(Footprint::all({6}));
  auto unrelated = address;
  unrelated.roles = 2;
  require(take(relation.preimage(all, unrelated, all)).empty(),
          "prefix dirty address requires role overlap");
  unrelated = address;
  unrelated.slot = 1;
  require(take(relation.preimage(all, unrelated, all)).empty(),
          "prefix dirty address requires the same member");
  require(
      relation.certify(take(Footprint::all({2, 3}))).code ==
              ErrorCode::InvalidArgument &&
          relation.project(take(Footprint::all({2, 3})),
                           [](auto, const auto*) { return Status::success(); })
                  .code == ErrorCode::InvalidArgument,
      "prefix flattened count does not replace rank-one domain");
  auto wrong = ResultRelation::unite(
      root, {relation, take(ResultRelation::mapped(
                           root, {2, 3}, Region::whole({2, 3}), {2, 3},
                           {{0, 0, 1, 1}, {1, 0, 1, 1}}, address))});
  require(!wrong.ok() && wrong.status().code == ErrorCode::InvalidArgument,
          "prefix union rejects a different coordinate domain");
  {
    auto combined = take(ResultRelation::unite(
        root, {relation, take(ResultRelation::prefix(
                             root, 6, 4, 2, ResultSupportTarget::Tensor, 1))}));
    require(take(combined.preimage(all, address, set(8))) == set(56),
            "prefix union preserves each source address and exact suffix");
  }
  require(!ResultRelation::prefix(root, 0).ok() &&
              !ResultRelation::prefix(root, 6, 0, 8).ok() &&
              !ResultRelation::prefix(root, 6, 0, 1,
                                      ResultSupportTarget::Descriptor)
                   .ok(),
          "prefix rejects empty domain and reserved metadata observations");
  relation = {};
  const auto baseline = root.statistics().live;
  relation = take(ResultRelation::prefix(root, UINT64_MAX, 3, 5,
                                         ResultSupportTarget::Tensor, 2));
  auto requested = take(
      Footprint::from_regions({UINT64_MAX}, {Region({{UINT64_MAX - 4, 3}})}));
  auto changed = take(
      Footprint::from_regions({UINT64_MAX}, {Region({{UINT64_MAX - 3, 1}})}));
  const auto before_work = root.statistics().issued.work;
  uint64_t projected = 0;
  require(relation.project(take(Footprint::all({UINT64_MAX})),
                           [&](auto span, const auto*) {
                             projected = span.count;
                             return Status::success();
                           })
                  .ok() &&
              projected == UINT64_MAX,
          "complete uint64 prefix projection retains the full interval");
  require(relation.certify(requested).ok() &&
              take(relation.preimage(requested, address, changed)) ==
                  take(Footprint::from_regions(
                      {UINT64_MAX}, {Region({{UINT64_MAX - 3, 2}})})),
          "uint64 prefix inverse retains coordinates and requested holes");
  uint64_t count = 0;
  require(relation.visit(UINT64_MAX - 1, 1,
                         [&](auto span) {
                           count = span.count;
                           return Status::success();
                         })
                  .ok() &&
              count == UINT64_MAX,
          "last uint64 prefix span does not overflow");
  require(root.statistics().issued.work - before_work < 1000,
          "huge prefix work depends on rectangles, not samples");
  const auto huge_charge = root.statistics().live;
  relation = {};
  require(root.statistics().live.values == baseline.values,
          "prefix owner retires all admitted metadata");
  relation = take(
      ResultRelation::prefix(root, 6, 3, 5, ResultSupportTarget::Tensor, 2));
  require(root.statistics().live.values == huge_charge.values,
          "prefix metadata size is independent of domain cardinality");
  FootprintLimits limited;
  limited.maximum_work = 0;
  require(relation.project(
                      all, [](auto, const auto*) { return Status::success(); },
                      limited)
                  .code == ErrorCode::ResourceExhausted,
          "prefix projection obeys finite work limit");
  CancellationSource cancelled;
  cancelled.cancel();
  limited = {};
  limited.cancellation = cancelled.token();
  auto rejected = relation.preimage(all, address, all, limited);
  require(!rejected.ok() && rejected.status().code == ErrorCode::Cancelled,
          "prefix inverse observes cancellation");
  ResourceLimits capacity;
  capacity.capacity[ResourceKind::Entries] = 1;
  ResourceBudget exhausted(capacity);
  auto failed = ResultRelation::prefix(exhausted, 6);
  require(!failed.ok() && failed.status().code == ErrorCode::ResourceExhausted,
          "prefix partial allocation returns typed capacity failure");
  for (auto live : exhausted.statistics().live.values)
    require(!live, "failed prefix allocation retires all leases");
}
void mapped_relation_capacity_failure() {
  ResourceLimits limits;
  limits.capacity[ResourceKind::Entries] = 3;
  ResourceBudget exhausted(limits);
  auto failed = ResultRelation::mapped(
      exhausted, {2}, Region::whole({2}), {2}, {{0, 0, 1, 1}},
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0});
  require(!failed.ok() && failed.status().code == ErrorCode::ResourceExhausted,
          "mapped metadata admission returns typed failure");
  for (auto count : exhausted.statistics().live.values) {
    require(!count, "failed mapped construction retires every partial lease");
  }
  limits.capacity[ResourceKind::Entries] = 128;
  ResourceBudget root(limits);
  auto relation = take(
      ResultRelation::mapped(root, {2}, Region::whole({2}), {2}, {{0, 0, 1, 1}},
                             {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto selected = take(Footprint::all({2}));
  const auto before = root.statistics().live;
  ResourceCapacity occupied;
  occupied[ResourceKind::Entries] =
      limits.capacity[ResourceKind::Entries] - before[ResourceKind::Entries];
  auto blocker = take(root.reserve(occupied));
  auto projected = relation.project(
      selected, [](auto, const Footprint*) { return Status::success(); });
  require(projected.code == ErrorCode::ResourceExhausted,
          "mapped projection metadata failure does not throw");
  auto inverse = relation.preimage(
      selected, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, selected);
  require(
      !inverse.ok() && inverse.status().code == ErrorCode::ResourceExhausted,
      "mapped inverse metadata failure does not throw");
  blocker = {};
  require(root.statistics().live.values == before.values,
          "failed compact relation queries retain no extra metadata");
}
void anchored_tensor_relations() {
  ResourceBudget root;
  for (std::int64_t step : {-2, -1, 0, 1, 2}) {
    for (std::uint64_t anchor = 0; anchor < 9; ++anchor) {
      for (std::uint64_t width = 1; width < 4; ++width) {
        ResultMappedAxis axis{0, anchor, step, width, 1};
        const auto first = axis.source_coordinate(1),
                   last = axis.source_coordinate(4);
        if (!first.ok() || !last.ok() || first.value() >= 9 ||
            last.value() >= 9) {
          continue;
        }
        auto relation = take(ResultRelation::mapped(
            root, {8}, Region({{1, 4}}), {9}, {axis},
            {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
        const auto selected =
            take(Footprint::from_regions({8}, {Region({{1, 4}})}));
        for (std::uint64_t dirty = 0; dirty < 9; ++dirty) {
          std::vector<Region> expected;
          for (std::uint64_t output = 1; output < 5; ++output) {
            const auto start = static_cast<std::int64_t>(anchor) +
                               step * (static_cast<std::int64_t>(output) - 1);
            if (static_cast<std::int64_t>(dirty) >= start &&
                static_cast<std::int64_t>(dirty) <
                    start + static_cast<std::int64_t>(width)) {
              expected.emplace_back(std::vector<RegionDimension>{{output, 1}});
            }
          }
          auto changed =
              take(Footprint::from_regions({9}, {Region({{dirty, 1}})}));
          auto got = take(relation.preimage(
              selected, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, changed));
          require(got == take(Footprint::from_regions({8}, expected)),
                  "anchored signed inverse matches independent point support");
        }
        Footprint got;
        auto projected =
            relation.project(selected, [&](auto, const Footprint* input) {
              got = *input;
              return Status::success();
            });
        const auto magnitude = step < 0 ? -step : step;
        if (static_cast<std::uint64_t>(magnitude) > width) {
          require(projected.code == ErrorCode::NotFound,
                  "strided holes never become an exact bounding box");
        } else {
          std::vector<Region> expected;
          for (std::uint64_t output = 1; output < 5; ++output) {
            const auto start = static_cast<std::int64_t>(anchor) +
                               step * (static_cast<std::int64_t>(output) - 1);
            expected.emplace_back(std::vector<RegionDimension>{
                {static_cast<std::uint64_t>(start),
                 std::min<std::uint64_t>(width, 9 - start)}});
          }
          require(projected.ok() &&
                      got == take(Footprint::from_regions({9}, expected)),
                  "signed projection preserves exact clipped interval support");
        }
      }
    }
  }
  const std::uint64_t origin = std::uint64_t{1} << 63;
  ResultMappedAxis anchored{0, 0, 1, 1, origin};
  auto relation = take(ResultRelation::mapped(
      root, {UINT64_MAX}, Region({{origin, 2}}), {2}, {anchored},
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto selected =
      take(Footprint::from_regions({UINT64_MAX}, {Region({{origin, 2}})}));
  auto dirty = take(relation.preimage(
      selected, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
      take(Footprint::from_regions({2}, {Region({{1, 1}})}))));
  require(dirty == take(Footprint::from_regions({UINT64_MAX},
                                                {Region({{origin + 1, 1}})})),
          "u64 anchored translation does not narrow logical coordinates");
  ResultMappedAxis extreme{0, UINT64_MAX, INT64_MIN, 1, UINT64_MAX};
  require(extreme.source_coordinate(UINT64_MAX).value() == UINT64_MAX &&
              !extreme.source_coordinate(0).ok(),
          "INT64_MIN signed step checks product before overflow");
  const std::vector<std::uint64_t> huge{UINT64_MAX, 2};
  relation = take(ResultRelation::mapped(
      root, huge, Region::whole(huge), huge, {{0, 0, 1, 1}, {1, 0, 1, 1}},
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  require(relation.certify(take(Footprint::all(huge))).ok(),
          "compact tensor witness certifies unflattenable domain without "
          "enumeration");
  relation = take(
      ResultRelation::mapped(root, {1}, Region::whole({1}), huge,
                             {{-1, UINT64_MAX - 1, 1, 1}, {-1, 0, 1, 1}},
                             {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  Footprint input;
  require(
      relation.project(take(Footprint::all({1})),
                       [&](auto, const Footprint* samples) {
                         input = *samples;
                         return Status::success();
                       })
              .ok() &&
          input.contains({UINT64_MAX - 1, 0}),
      "unflattenable exact support still has an exact coordinate projection");
  auto visited = relation.visit(0, 64, [](auto) { return Status::success(); });
  require(visited.code == ErrorCode::ResourceExhausted,
          "flat support access returns typed overflow without wrapping exact "
          "evidence");
  auto table = take(ResultRelation::sample_rows(root, UINT64_MAX, 1, [](auto) {
    return Result<ResultRelationRow>(
        ResultRelationRow{UINT64_MAX - 3,
                          {0, 1, 0, 1, ResultSupportTarget::Tensor, 0}});
  }));
  auto samples = take(
      Footprint::from_regions(huge, {Region({{UINT64_MAX - 1, 1}, {0, 1}})}));
  require(table.certify(samples).code == ErrorCode::ResourceExhausted,
          "sample table certificate cannot authorize a wrapped coordinate");
}
void projection_preflight() {
  ResourceBudget root;
  auto selected = take(Footprint::from_regions({4}, {Region({{0, 1}})}));
  auto supported = take(
      ResultRelation::mapped(root, {4}, Region({{0, 1}}), {8}, {{0, 0, 1, 1}},
                             {0, 5, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto sparse = take(
      ResultRelation::mapped(root, {4}, Region({{3, 1}}), {8}, {{0, 0, 2, 1}},
                             {0, 5, 0, 0, ResultSupportTarget::Tensor, 0}));
  for (bool reverse : {false, true}) {
    auto relation = take(ResultRelation::unite(
        root, reverse ? std::vector<ResultRelation>{supported, sparse}
                      : std::vector<ResultRelation>{sparse, supported}));
    unsigned visits = 0;
    auto status = relation.project(selected, [&](auto, auto) {
      ++visits;
      return Status::success();
    });
    require(status.code == ErrorCode::NotFound && visits == 0,
            "projection checks unsupported disjoint leaves before any visitor");
  }
  auto partial = take(
      ResultRelation::mapped(root, {4, 5}, Region({{1, 2}, {1, 3}}), {4, 5},
                             {{0, 0, 1, 1}, {1, 0, 1, 1}},
                             {0, 5, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto stripes = take(Footprint::from_regions(
      {4, 5}, {Region({{0, 4}, {0, 2}}), Region({{0, 4}, {3, 2}})}));
  Footprint intersection;
  require(partial
              .project(stripes,
                       [&](auto, const auto* samples) {
                         require(samples != nullptr,
                                 "mapped rectangle projection");
                         intersection = *samples;
                         return Status::success();
                       })
              .ok(),
          "partial witness projection");
  for (std::uint64_t y = 0; y < 4; ++y)
    for (std::uint64_t x = 0; x < 5; ++x)
      require(intersection.contains({y, x}) ==
                  (y >= 1 && y < 3 && (x == 1 || x == 3)),
              "rectangular clipping preserves separated stripes and holes");
  require(intersection.element_count().value() == 4,
          "clipping intersection cardinality");
  CancellationSource stop;
  FootprintLimits limits;
  limits.cancellation = stop.token();
  limits.consume_work = [&](std::uint64_t) {
    stop.cancel();
    return Status::success();
  };
  unsigned visits = 0;
  auto cancelled = partial.project(
      stripes,
      [&](auto, auto) {
        ++visits;
        return Status::success();
      },
      limits);
  require(
      cancelled.code == ErrorCode::Cancelled && visits == 0,
      "projection observes cancellation from work admission before visitor");
}
void mapped_relations() {
  ResourceBudget root;
  const std::vector<std::uint64_t> output{2, 3, 257, 513};
  const std::vector<std::uint64_t> input{2, 3, 513, 1025, 4};
  const std::vector<ResultMappedAxis> axes{{0, 0, 1, 1},
                                           {1, 0, 1, 1},
                                           {2, 0, 2, 2},
                                           {3, 0, 2, 2},
                                           {-1, 0, 1, 4}};
  auto relation = take(
      ResultRelation::mapped(root, output, Region::whole(output), input, axes,
                             {0, 5, 0, 0, ResultSupportTarget::Tensor, 0}));
  auto all = take(Footprint::all(output));
  Footprint projected;
  require(relation.project(all,
                           [&](auto support, const auto* samples) {
                             require(support.target ==
                                             ResultSupportTarget::Tensor &&
                                         support.roles == 5 && samples,
                                     "compact relation address and roles");
                             projected = *samples;
                             return Status::success();
                           })
                  .ok() &&
              projected == take(Footprint::all(input)),
          "large rectangle projection has no per-sample table");
  const Region last({{1, 1}, {2, 1}, {512, 1}, {1024, 1}, {3, 1}});
  auto changed = take(Footprint::from_regions(input, {last}));
  auto dirty = take(relation.preimage(
      all, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, changed));
  require(dirty == take(Footprint::from_regions(
                       output, {Region({{1, 1}, {2, 1}, {256, 1}, {512, 1}})})),
          "large inverse clipped downsample cell");
  std::uint64_t count = 0;
  const auto last_index = take(all.element_count()) - 1;
  require(relation.visit(last_index, 16,
                         [&](auto span) {
                           count += span.count;
                           return Status::success();
                         })
                  .ok() &&
              count == 4,
          "mapped visit exact clipped support");
}
void gather_cached_mask() {
  ResourceBudget root;
  constexpr std::uint64_t count = 512;
  const std::vector<std::uint64_t> output{2 * count, 4}, input{1, 4};
  std::vector<Region> even, odd;
  for (std::uint64_t i = 0; i < count; ++i) {
    even.emplace_back(std::vector<RegionDimension>{{2 * i, 1}, {0, 4}});
    odd.emplace_back(std::vector<RegionDimension>{{2 * i + 1, 1}, {0, 4}});
  }
  auto mask = take(Footprint::from_regions(output, even));
  // Different owners with identical canonical geometry must hit the full-mask
  // path, while equal box counts with different coordinates must not.
  auto same = take(Footprint::from_regions(output, even));
  auto different = take(Footprint::from_regions(output, odd));
  std::vector<ResultMappedAxis> axes(2);
  axes[1].extent = 4;
  auto relation = take(
      ResultRelation::gather(root, output, mask, 1, input, axes, 0, 1,
                             [](std::uint64_t, std::uint32_t) {
                               ResultGatherSample sample;
                               sample.present = true;
                               return Result<ResultGatherSample>(sample);
                             },
                             {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  FootprintLimits limits;
  limits.maximum_work = 8 * count + 128;
  std::uint64_t issued = 0;
  limits.consume_work = [&](auto n) {
    issued += n;
    return Status::success();
  };
  unsigned calls = 0;
  Footprint projected;
  require(relation
              .project(
                  same,
                  [&](auto, const auto* samples) {
                    ++calls;
                    projected = *samples;
                    return Status::success();
                  },
                  limits)
              .ok(),
          "full sparse Gather projection avoids geometry rebuild");
  require(calls == 1 && projected == take(Footprint::all(input)) &&
              issued <= limits.maximum_work,
          "cached mask retains exact support and linear query work");
  for (const auto maximum : {0U, 32U, 128U, 1024U, 4096U, 65536U}) {
    limits.maximum_work = maximum;
    issued = 0;
    calls = 0;
    auto status = relation.project(
        different,
        [&](auto, const auto*) {
          ++calls;
          return Status::success();
        },
        limits);
    require(issued <= maximum && calls == 0 &&
                (status.ok() || status.code == ErrorCode::ResourceExhausted),
            "unequal-mask fallback shares comparison work and preserves holes");
  }
  CancellationSource cancel;
  limits = {};
  limits.cancellation = cancel.token();
  limits.consume_work = [&](auto n) {
    if (n >= count)
      cancel.cancel();
    return Status::success();
  };
  calls = 0;
  require(relation.project(
                      same,
                      [&](auto, const auto*) {
                        ++calls;
                        return Status::success();
                      },
                      limits)
                      .code == ErrorCode::Cancelled &&
              calls == 0,
          "full-mask comparison observes cancellation after its charge");
  limits = {};
  limits.maximum_boxes = count - 1;
  require(relation.project(
                      same,
                      [&](auto, const auto*) {
                        ++calls;
                        return Status::success();
                      },
                      limits)
                      .code == ErrorCode::ResourceExhausted &&
              calls == 0,
          "full-mask alias still obeys the box limit");
}
void gather_relations() {
  ResourceBudget root;
  const std::vector<std::uint64_t> output{3, 4}, input{8, 8, 4};
  std::vector<ResultMappedAxis> axes(3);
  axes[0].output_axis = 0;
  axes[0].step = 2;
  axes[1].output_axis = 0;
  axes[2].extent = 4;
  const auto reader = [](std::uint64_t, std::uint32_t) {
    ResultGatherSample sample;
    sample.present = true;
    return Result<ResultGatherSample>(sample);
  };
  auto relation = take(ResultRelation::gather(
      root, output, Region::whole(output), 1, input, axes, 0, 1, reader,
      {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, 2));
  const auto request =
      take(Footprint::from_regions(output, {Region({{1, 1}, {2, 1}})}));
  const auto expected =
      take(Footprint::from_regions(input, {Region({{2, 1}, {1, 1}, {0, 4}})}));
  Footprint data, validation;
  require(relation
              .project(request,
                       [&](auto support, const auto* samples) {
                         require(samples != nullptr, "gather tensor footprint");
                         if (support.roles & 1U)
                           data = *samples;
                         if (support.roles & 4U)
                           validation = *samples;
                         return Status::success();
                       })
              .ok(),
          "gather enumerates mappings with gaps and repeated output axes");
  require(data == expected,
          "gather partial channel retains exact full input tuple");
  require(validation == take(Footprint::from_regions(
                            input, {Region({{2, 1}, {0, 8}, {0, 4}})})),
          "gather Validation expands without broadening Data");
  const auto edit =
      take(Footprint::from_regions(input, {Region({{2, 1}, {1, 1}, {3, 1}})}));
  require(take(relation.preimage(
              request, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, edit)) ==
              request,
          "gather inverse detects any channel and respects requested output "
          "channels");
  const auto full = take(Footprint::all(output));
  FootprintLimits limits;
  limits.maximum_boxes = 1;
  unsigned calls = 0;
  require(relation.project(
                      full,
                      [&](auto, const auto*) {
                        ++calls;
                        return Status::success();
                      },
                      limits)
                      .code == ErrorCode::ResourceExhausted &&
              calls == 0,
          "cached gather support obeys box limit");
  CancellationSource cancel;
  limits = {};
  limits.cancellation = cancel.token();
  calls = 0;
  require(relation.project(
                      request,
                      [&](auto, const auto*) {
                        ++calls;
                        cancel.cancel();
                        return Status::success();
                      },
                      limits)
                      .code == ErrorCode::Cancelled &&
              calls == 1,
          "Data visitor cancellation prevents Validation callback");
  auto two = take(Footprint::from_regions(
      output, {Region({{0, 1}, {0, 4}}), Region({{2, 1}, {0, 4}})}));
  auto sparse = take(
      ResultRelation::gather(root, output, two, 1, input, axes, 0, 1, reader,
                             {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}, 2));
  require(sparse.certify(two).ok() &&
              sparse.certify(full).code == ErrorCode::NotFound,
          "one gather witness retains output holes");
  for (const auto maximum : {0U, 5U, 20U, 100U}) {
    limits = {};
    limits.maximum_work = maximum;
    std::uint64_t issued = 0;
    limits.consume_work = [&](auto n) {
      issued += n;
      return Status::success();
    };
    auto status = sparse.project(
        two, [](auto, const auto*) { return Status::success(); }, limits);
    require(issued <= maximum,
            "gather query cannot reset local work between boxes");
    require(status.ok() || status.code == ErrorCode::ResourceExhausted,
            "gather bounded query status");
  }
  const std::vector<std::uint64_t> huge{UINT64_MAX, 4};
  auto huge_q = take(
      Footprint::from_regions(huge, {Region({{UINT64_MAX - 4, 1}, {0, 4}}),
                                     Region({{UINT64_MAX - 2, 1}, {0, 4}})}));
  std::vector<ResultMappedAxis> sampled(2);
  sampled[1].extent = 4;
  auto enormous = take(
      ResultRelation::gather(root, huge, huge_q, 1, huge, sampled, 1, 1,
                             [](auto row, auto) {
                               ResultGatherSample sample;
                               sample.present = true;
                               sample.coordinates[0] = UINT64_MAX - 4 + 2 * row;
                               return Result<ResultGatherSample>(sample);
                             },
                             {0, 5, 0, 0, ResultSupportTarget::Tensor, 0}));
  Footprint enormous_support;
  require(enormous.project(huge_q,
                           [&](auto, const auto* samples) {
                             enormous_support = *samples;
                             return Status::success();
                           })
                  .ok() &&
              enormous_support == huge_q,
          "gather uses local rows in unflattenable sparse domain");
  const std::vector<std::uint64_t> wide_input{UINT64_MAX, UINT64_MAX};
  const auto wide_query = take(Footprint::all({2, 1}));
  auto wide_gather = take(
      ResultRelation::gather(root, {2, 1}, wide_query, 1, wide_input,
                             std::vector<ResultMappedAxis>(2), 3, 1,
                             [](auto row, auto) {
                               ResultGatherSample sample;
                               sample.present = true;
                               sample.coordinates[0] = row ? 0 : UINT64_MAX - 1;
                               sample.coordinates[1] = row ? UINT64_MAX - 1 : 0;
                               return Result<ResultGatherSample>(sample);
                             },
                             {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
  const auto wide_expected = take(Footprint::from_regions(
      wide_input, {Region({{UINT64_MAX - 1, 1}, {0, 1}}),
                   Region({{0, 1}, {UINT64_MAX - 1, 1}})}));
  Footprint wide_support;
  require(wide_gather
                  .project(wide_query,
                           [&](auto, const auto* samples) {
                             wide_support = *samples;
                             return Status::success();
                           })
                  .ok() &&
              wide_support == wide_expected,
          "gather retains full coordinates when support keys need more than 64 "
          "bits");
  const std::vector<std::vector<std::uint64_t>> key_shapes{
      {1, UINT64_MAX},
      {UINT64_C(1) << 32, UINT64_C(1) << 32},
      {2, UINT64_MAX},
      {1, 1}};
  for (const auto& shape : key_shapes) {
    const auto point = Region({{shape[0] - 1, 1}, {shape[1] - 1, 1}});
    auto keys = take(
        ResultRelation::gather(root, {2, 1}, wide_query, 1, shape,
                               std::vector<ResultMappedAxis>(2), 3, 2,
                               [&](auto row, auto) {
                                 ResultGatherSample sample;
                                 sample.present = true;
                                 sample.coordinates[0] = row ? 0 : shape[0] - 1;
                                 sample.coordinates[1] = row ? 0 : shape[1] - 1;
                                 return Result<ResultGatherSample>(sample);
                               },
                               {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
    const auto expected =
        take(Footprint::from_regions(shape, {point, Region({{0, 1}, {0, 1}})}));
    Footprint projected;
    require(keys.project(wide_query,
                         [&](auto, const auto* samples) {
                           projected = *samples;
                           return Status::success();
                         })
                    .ok() &&
                projected == expected,
            "zero, 64 and 65-bit gather support keys are exact");
    const auto first_output =
        take(Footprint::from_regions({2, 1}, {Region({{0, 1}, {0, 1}})}));
    const auto change = take(Footprint::from_regions(shape, {point}));
    require(keys.project(first_output,
                         [&](auto, const auto* samples) {
                           projected = *samples;
                           return Status::success();
                         })
                    .ok() &&
                projected == change,
            "packed key partial projection preserves UINT64_MAX keys");
    require(
        take(keys.preimage(first_output,
                           {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
                           change)) == first_output,
        "packed normalization leaves exact inverse dependency queries intact");
  }
  CancellationSource after_reader;
  limits = {};
  limits.cancellation = after_reader.token();
  bool done = false;
  unsigned later = 0;
  limits.consume_work = [&](auto) {
    if (done && ++later == 4)
      after_reader.cancel();
    return Status::success();
  };
  auto cancelled = ResultRelation::gather(
      root, {16, 4}, Region::whole({16, 4}), 1, {32, 4}, sampled, 1, 1,
      [&](auto row, auto) {
        ResultGatherSample sample;
        sample.present = true;
        sample.coordinates[0] = 31 - row;
        done = row == 15;
        return Result<ResultGatherSample>(sample);
      },
      {0, 5, 0, 0, ResultSupportTarget::Tensor, 0}, 0, limits);
  require(
      done && later == 4 && cancelled.status().code == ErrorCode::Cancelled,
      "gather cancellation reaches radix normalization after reader completes");
}
}  // namespace
int main() {
  try {
    gather_relations();
    gather_cached_mask();
    reshape_relations();
    prefix_relations();
    mapped_relation_capacity_failure();
    anchored_tensor_relations();
    mapped_relations();
    projection_preflight();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
