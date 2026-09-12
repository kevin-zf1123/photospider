#include <cfenv>  // NOLINT(build/c++11)
#include <cstdint>
#include <limits>
#include <utility>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  SpectrumSpec spectrum;
  spectrum.original_shape = {1, 4};
  spectrum.transformed_axes = {0, 1};
  spectrum.axis_order = {0, 1};
  spectrum.shifts = {0, 0};
  spectrum.sample_origin = {0, 0};
  spectrum.sample_step = {1, 1};
  auto schema = spectrum_schema(spectrum).take_value();
  PS_CHECK(schema.fields[0].rows.value == 3 && schema.domain[1].value == 4);
  auto odd = spectrum;
  odd.original_shape[1] = 5;
  auto odd_schema = spectrum_schema(odd).take_value();
  PS_CHECK(odd_schema.fields[0].rows.value == 3 &&
           !schema.same_schema(odd_schema));
  for (std::size_t length = 0; length < schema.metadata[0].payload.size();
       ++length) {
    auto truncated = schema;
    truncated.metadata[0].payload.resize(length);
    PS_CHECK(!truncated.validate(true).ok());
  }
  auto trailing = schema;
  trailing.metadata[0].payload.push_back(0);
  PS_CHECK(!trailing.validate(true).ok());
  auto wrong = schema;
  wrong.fields[0].record_shape = {1};
  PS_CHECK(!wrong.validate(true).ok());
  auto duplicate_axis = spectrum;
  duplicate_axis.transformed_axes = {1, 1};
  PS_CHECK(!spectrum_schema(duplicate_axis).ok());
  auto infinite_tolerance = spectrum;
  infinite_tolerance.atol = std::numeric_limits<double>::infinity();
  PS_CHECK(!spectrum_schema(infinite_tolerance).ok());
  auto overflow = spectrum;
  overflow.original_shape = {UINT64_MAX, 4};
  PS_CHECK(!spectrum_schema(overflow).ok());
  PointSetSpec empty_points;
  empty_points.maximum_count = 0;
  empty_points.basis_count = 0;
  PS_CHECK(point_set_schema(empty_points).ok());
  auto invalid_points = empty_points;
  invalid_points.dimensions = 4;
  PS_CHECK(!point_set_schema(invalid_points).ok());

  ResourceLimits limits;
  limits.maximum_work = 1000000;
  ResourceBudget root(limits);
  auto builder =
      ResultBuilder::start(root, components_schema({1, 1, 0}).take_value(),
                           "budget-test")
          .take_value();
  auto descriptor = ResultRelation::cartesian(root, 1, {0, 15, 0, 0},
                                              DependencyGuarantee::Conservative)
                        .take_value();
  auto empty = ResultRelation::cartesian(root, 0, {0, 15, 0, 0},
                                         DependencyGuarantee::Conservative)
                   .take_value();
  PS_CHECK(builder.bind_descriptor_relation(descriptor).ok());
  std::int64_t zero = 0;
  PS_CHECK(
      builder
          .append(0, 1,
                  ByteView(reinterpret_cast<const std::uint8_t*>(&zero), 8))
          .ok());
  PS_CHECK(builder.publish(0, 1, descriptor, {true, true, true, true}).ok());
  PS_CHECK(builder.publish(1, 0, empty, {true, true, true, true}).ok());
  auto object = builder.seal().take_value();
  PS_CHECK(validate_representation(object, root, 24).ok());
  const auto issued = root.statistics().issued;
  PS_CHECK(root.consume({limits.maximum_work - issued.work}).ok());
  unsigned hook_calls = 0;
  const auto exhausted =
      validate_representation(object, root, 24, {}, [&](std::uint64_t) {
        ++hook_calls;
        return Status::success();
      });
  PS_CHECK(exhausted.code == ErrorCode::ResourceExhausted && hook_calls == 0);
  PS_CHECK(root.statistics().issued.io_requests == issued.io_requests);

  ResourceLimits brush_limits;
  brush_limits.maximum_work = 8;
  ResourceBudget brush_root(brush_limits);
  BrushSpec brush;
  brush.spacing = 1e-6;
  const BrushEvent events[] = {{0, 0}, {1, 1}};
  CausalBrushState initial;
  auto failed =
      advance_causal_brush(brush, initial, events, 2, true, brush_root);
  PS_CHECK(failed.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(initial.next_event == 0 && !initial.started && !initial.ended);
  PS_CHECK(brush_root.statistics().live[ResourceKind::Host] == 0);
  PS_CHECK(brush_root.statistics().issued.work <= 8);
  ResourceLimits payload_limits;
  payload_limits.capacity[ResourceKind::Payload] = 24;
  ResourceBudget payload_root(payload_limits);
  const BrushEvent payload_events[] = {{0, 0}, {1, 1}};
  auto payload_result = advance_causal_brush(BrushSpec{}, {}, payload_events, 2,
                                             true, payload_root);
  PS_CHECK(payload_result.ok() &&
           payload_root.statistics().live[ResourceKind::Payload] == 16);
  bool refused_copy = false;
  try {
    ResourceAllocationScope scope(payload_root);
    auto copy = payload_result.value().dabs;
    static_cast<void>(copy);
  } catch (const std::bad_alloc&) {
    refused_copy = true;
  }
  PS_CHECK(refused_copy &&
           payload_root.statistics().live[ResourceKind::Payload] == 16);
  payload_result = Result<BrushAdvance>(Status{ErrorCode::Cancelled, {}});
  PS_CHECK(payload_root.statistics().live[ResourceKind::Payload] == 0);
  const auto rounding = std::fegetround();
  PS_CHECK(std::fesetround(FE_UPWARD) == 0);
  BrushSpec tiny;
  tiny.spacing = std::numeric_limits<double>::denorm_min();
  const BrushEvent tiny_events[] = {{0, 0}, {1, tiny.spacing}};
  ResourceBudget tiny_root;
  auto tiny_result =
      advance_causal_brush(tiny, {}, tiny_events, 2, true, tiny_root);
  PS_CHECK(std::fegetround() == FE_UPWARD);
  PS_CHECK(std::fesetround(rounding) == 0);
  PS_CHECK(tiny_result.ok() && tiny_result.value().dabs.size() == 2 &&
           tiny_result.value().dabs[1] == tiny.spacing);
  return 0;
}
