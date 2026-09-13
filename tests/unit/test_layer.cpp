#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
int raster_admission(std::uint64_t height, std::uint64_t width) {
  using namespace ps;  // NOLINT(build/namespaces)
  const LayerSpec spec{height, width};
  auto definition = make_layer_operation(LayerOperation::Assemble, spec);
  PS_CHECK(definition.ok());
  ResultProgramMetadata metadata;
  metadata.output.result_schema = std::make_shared<const SchemaTemplate>(
      *definition.value().traits.outputs[0].result_schema);
  metadata.inputs.resize(2);
  metadata.inputs[0].descriptor = {ElementType::Float32, {height, width, 4}};
  metadata.inputs[0].facets = {encode_semantic(rgba_semantics()).take_value()};
  metadata.inputs[1].descriptor = {ElementType::Float32, {height, width, 3}};
  OperationRegistry registry;
  PS_CHECK(registry.register_operation(definition.take_value()).ok());
  PS_CHECK(registry.freeze().ok());
  std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(metadata, parameters);
  query.semantic_key = "large-layer-admission";
  query.page_bytes = 64;
  ResourceLimits limits;
  limits.capacity[ResourceKind::Host] = 65536;
  limits.capacity[ResourceKind::Metadata] = 65536;
  ResourceBudget root(limits);
  auto allocator = root.allocator();
  auto started = registry.start_result("layer.assemble", query, allocator);
  PS_CHECK(started.ok());
  auto continuation = started.take_value();
  ResultValueInputs values;
  ResultObjectInputs objects;
  ResourceVector<ResultIoReply> io;
  auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
  ResultProgramPhase phase{
      query,
      values,
      objects,
      io,
      allocator,
      root,
      [&](std::uint64_t work) { return root.consume({work}); },
      failure};
  auto polled = continuation.poll(phase);
  PS_CHECK(polled.ok());
  auto* need = std::get_if<ResultProgramNeed>(&polled.value());
  PS_CHECK(need && need->values.size() == 2 && need->io.empty());
  // Admission only: no pixel callback/I/O has run. Full execution remains
  // subject to explicitly configured stage, work, disk and capacity budgets.
  PS_CHECK(root.statistics().live[ResourceKind::Disk] == 0 &&
           root.statistics().peak[ResourceKind::Host] <= 65536);
  return 0;
}
}  // namespace

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(raster_admission(1, 1048576) == 0);
  PS_CHECK(raster_admission(1, 1048577) == 0);
  PS_CHECK(raster_admission(1080, 1920) == 0);
  PS_CHECK(raster_admission(1, 67108864) == 0);
  const float maximum = std::numeric_limits<float>::max();
  const float tiny = std::numeric_limits<float>::denorm_min();
  LayerPixel underflow{{{1, 0, 0}, tiny}, {3, -4, 5}};
  auto opacity = layer_opacity(underflow, .5F);
  PS_CHECK(!opacity.ok() &&
           opacity.status().code == ErrorCode::OperationFailed &&
           opacity.status().reason == FailureReason::AssociationUnderflow);
  PS_CHECK(underflow.coverage.a == tiny && underflow.emission[0] == 3);
  LayerPixel emission_only{{{0, 0, 0}, 0}, {4, -2, 3}};
  auto zero = layer_opacity(emission_only, 0).take_value();
  PS_CHECK(zero.emission == emission_only.emission);
  LayerPixel opaque{{{1, 2, 3}, 1}, {}};
  PS_CHECK(layer_over(emission_only, opaque).value().emission[0] == 4);
  PS_CHECK(layer_over(opaque, emission_only).value().emission[0] == 0);
  PS_CHECK(layer_emit(opaque, {5, -4, 3}, 2, false).value().emission[0] == 10);
  PS_CHECK(layer_emit(opaque, {5, -4, 3}, 2, true).value().emission[0] == 0);
  LayerPixel limit{{{maximum, 0, 0}, .5F}, {-maximum, 0, 0}};
  PS_CHECK(layer_over(limit, limit).status().reason ==
           FailureReason::ArithmeticOverflow);
  auto response = layer_response(limit).take_value();
  auto response_pair = response_over(response, response).take_value();
  PS_CHECK(response_pair.q[0] == 0 && response_pair.t == .25F);
  LayerPixel opposite{{{maximum, 0, 0}, 1}, {maximum, 0, 0}};
  PS_CHECK(layer_over(opposite, {}).ok());
  PS_CHECK(layer_response(opposite).status().reason ==
           FailureReason::ArithmeticOverflow);
  LayerPixel fma_front{{{-1, 0, 0}, 0x1p-24F}, {}},
      fma_back{{{0x1.000002p0F, 0, 0}, 1}, {}};
  PS_CHECK(layer_over(fma_front, fma_back).value().coverage.p[0] == 0);
  PS_CHECK(layer_response({{{0, 0, 0}, 0x1p-25F}, {}}).value().t == 1);
  LayerPixel a{{{0x1p24F, 0, 0}, .5F}, {}}, b{{{-0x1p25F, 0, 0}, .5F}, {}},
      c{{{2, 0, 0}, .5F}, {}};
  PS_CHECK(layer_over(layer_over(a, b).value(), c).value().coverage.p[0] ==
           .5F);
  PS_CHECK(layer_over(a, layer_over(b, c).value()).value().coverage.p[0] == 0);
  auto raw = raw_rgba_plus({{2, 0, -1}, .75F}, {{3, 1, 0}, .75F}).take_value();
  PS_CHECK(raw.mass == 1.5F && !raw_rgba_coverage(raw, false).ok());
  auto capped = raw_rgba_coverage(raw, true).take_value();
  PS_CHECK(capped.a == 1 && capped.p == raw.p);
  auto first = weighted_layer_leaf({{{2, 0, -1}, .75F}, {}}, 1).take_value();
  auto second = weighted_layer_leaf({{{3, 1, 0}, .75F}, {}}, 1).take_value();
  auto mean = weighted_layer_finalize(weighted_layer_add(first, second).value())
                  .take_value();
  PS_CHECK(mean.valid && mean.value.coverage.a == .75F &&
           mean.value.coverage.p[0] == 2.5F);
  PS_CHECK(!weighted_layer_finalize({}).value().valid);
  PS_CHECK(
      weighted_layer_finalize(weighted_layer_leaf(emission_only, 1).value())
          .value()
          .valid);
  PS_CHECK(!weighted_layer_leaf(opaque, -1).ok());
  PS_CHECK(!weighted_layer_leaf({{{1, 0, 0}, 0}, {}}, 0).ok());
  const double weight = std::numeric_limits<double>::denorm_min();
  auto leaf = weighted_layer_leaf({{{1, 0, 0}, .5F}, {}}, weight);
  PS_CHECK(leaf.status().reason == FailureReason::AssociationUnderflow);
  PS_CHECK(validate_layer_contribution({{1, 0, 0, .5, 0, 0, 0, weight}}).ok());
  WeightedLayerSum final_underflow{{1, 0, 0, 0x1p-150, 0, 0, 0, 1}};
  PS_CHECK(weighted_layer_finalize(final_underflow).status().reason ==
           FailureReason::AssociationUnderflow);
  WeightedLayerSum bad_zero{{0, 0, 0, 0, 1, 0, 0, 0}};
  PS_CHECK(!validate_weighted_layer_sum(bad_zero).ok());
  const auto rounding = std::fegetround();
  PS_CHECK(std::fesetround(FE_UPWARD) == 0);
  PS_CHECK(!layer_opacity(underflow, .5F).ok());
  PS_CHECK(std::fegetround() == FE_UPWARD);
  PS_CHECK(std::fesetround(rounding) == 0);
  LayerPixel signed_zero{{{-0.0F, 0, -0.0F}, -0.0F}, {-0.0F, 0, -0.0F}};
  auto unchanged = layer_opacity(signed_zero, 1).take_value();
  PS_CHECK(std::signbit(unchanged.coverage.p[0]) &&
           std::signbit(unchanged.emission[0]));
  for (unsigned k = 1; k <= 6; ++k) {
    auto schema =
        layer_schema(static_cast<LayerRepresentation>(k)).take_value();
    PS_CHECK(schema.validate(true).ok());
    auto wrong = schema;
    wrong.metadata[0].payload[16] = 2;
    PS_CHECK(!wrong.validate(true).ok());
    wrong = schema;
    wrong.fields[0].record_shape[0]++;
    PS_CHECK(!wrong.validate(true).ok());
    wrong = schema;
    wrong.publication = PublishPolicy::StablePrefix;
    PS_CHECK(!wrong.validate(true).ok());
  }
  // Independent integer numerator reference: all selected dyadic arithmetic is
  // exactly representable, so each rational identity checks bit-exact output.
  for (int pa = -2; pa <= 2; ++pa)
    for (int pb = -2; pb <= 2; ++pb)
      for (int aa = 1; aa <= 4; ++aa)
        for (int ab = 0; ab <= 4; ++ab) {
          LayerPixel x{{{pa / 4.0F, 0, 0}, aa / 4.0F}, {pa / 2.0F, 0, 0}};
          LayerPixel y{{{ab ? pb / 4.0F : 0, 0, 0}, ab / 4.0F},
                       {pb / 2.0F, 0, 0}};
          const auto actual = layer_over(x, y).take_value();
          PS_CHECK(actual.coverage.p[0] ==
                   (pa * 4 + (4 - aa) * (ab ? pb : 0)) / 16.0F);
          PS_CHECK(actual.coverage.a == (aa * 4 + (4 - aa) * ab) / 16.0F);
          PS_CHECK(actual.emission[0] == (pa * 4 + (4 - aa) * pb) / 8.0F);
        }
  return 0;
}
