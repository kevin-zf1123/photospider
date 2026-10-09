#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/ops.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  PS_CHECK(!fft_spectrum_spec(0, 1).ok());
  PS_CHECK(!fft_spectrum_spec(1, 0).ok());
  PS_CHECK(!fft_spectrum_spec(UINT64_MAX, 2).ok());
  PS_CHECK(!fft_spectrum_spec(1, 1, static_cast<SpectrumPacking>(3)).ok());
  PS_CHECK(!fft_spectrum_spec(1, 1, SpectrumPacking::Full, -1).ok());
  PS_CHECK(!fft_spectrum_spec(1, 1, SpectrumPacking::Full, 0,
                              std::numeric_limits<double>::infinity())
                .ok());
  const auto spec = fft_spectrum_spec(3, 4).take_value();
  for (unsigned variant = 0; variant < 7; ++variant) {
    auto bad = spec;
    if (variant == 0)
      bad.sign = 1;
    if (variant == 1)
      bad.normalization = SpectrumNormalization::InverseBySize;
    if (variant == 2)
      bad.axis_order = {1, 0};
    if (variant == 3)
      bad.transformed_axes = {1};
    if (variant == 4)
      bad.shifts = {1, 0};
    if (variant == 5)
      bad.sample_origin = {1, 0};
    if (variant == 6)
      bad.unit = "different";
    PS_CHECK(!make_fft_operation(FftOperation::ForwardReal, bad).ok());
    PS_CHECK(!fft_spatial_schema(bad).ok());
  }
  auto registry = std::make_shared<OperationRegistry>();
  PS_CHECK(
      registry
          ->register_operation(
              make_fft_operation(FftOperation::ForwardReal, spec).take_value())
          .ok());
  PS_CHECK(
      registry
          ->register_operation(
              make_fft_operation(FftOperation::Multiply, spec).take_value())
          .ok());
  PS_CHECK(registry
               ->register_operation(
                   make_fft_operation(FftOperation::ImportResponse, spec)
                       .take_value())
               .ok());
  PS_CHECK(registry->freeze().ok());
  auto expected = spectrum_schema(spec).take_value();
  auto other =
      spectrum_schema(fft_spectrum_spec(3, 5).take_value()).take_value();
  PS_CHECK(expected.fields[0].rows.value == other.fields[0].rows.value);
  PS_CHECK(!expected.same_schema(other));
  ResultProgramMetadata metadata;
  SchemaTemplate source_schema;
  source_schema.id = "test.fft.source";
  ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = {ElementType::Float64, {3, 4}};
  source_schema.tensors.push_back(tensor);
  metadata.inputs.resize(1);
  metadata.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(source_schema);
  metadata.output.result_schema = std::make_shared<const SchemaTemplate>(other);
  const std::map<std::string, ParameterValue> parameters;
  ResourceBudget budget;
  ResultProgramQuery query(metadata, parameters);
  query.semantic_key = "identity-mismatch";
  // W=4/W=5 have the same half count, but forward cannot publish the other's
  // original shape. Actual public registry validation precedes state
  // allocation.
  auto started =
      registry->start_result("fft.forward_real", query, budget.allocator());
  PS_CHECK(!started.ok() && started.status().code == ErrorCode::TypeMismatch);
  PS_CHECK(budget.statistics().peak[ResourceKind::Host] == 0);
  metadata.output.result_schema =
      std::make_shared<const SchemaTemplate>(expected);
  auto wrong_shape = source_schema;
  wrong_shape.tensors[0].descriptor.shape = {3, 5};
  metadata.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(wrong_shape);
  started =
      registry->start_result("fft.forward_real", query, budget.allocator());
  PS_CHECK(!started.ok() && started.status().code == ErrorCode::TypeMismatch);
  PS_CHECK(budget.statistics().peak[ResourceKind::Host] == 0);
  for (unsigned invalid = 0; invalid < 5; ++invalid) {
    auto bad = source_schema;
    if (invalid == 0)
      bad.tensors[0].descriptor.element_type = ElementType::Float32;
    if (invalid == 1)
      bad.fields = {{"extra", ElementType::UInt8, {}, {}}};
    if (invalid == 2)
      bad.tensors.push_back(tensor);
    if (invalid == 3)
      bad.tensors[0].batch_axes = {2};
    if (invalid == 4)
      bad.tensors[0].facets = {{"test.annotation", 1, {0}}};
    metadata.inputs[0].result_schema =
        std::make_shared<const SchemaTemplate>(bad);
    started =
        registry->start_result("fft.forward_real", query, budget.allocator());
    PS_CHECK(!started.ok() && started.status().code == ErrorCode::TypeMismatch);
    PS_CHECK(budget.statistics().peak[ResourceKind::Host] == 0);
  }
  auto response_schema = source_schema;
  response_schema.tensors[0].descriptor.shape = {3, 3, 2};
  for (const auto& shape : std::vector<std::vector<std::uint64_t>>{{3, 4, 2},
                                                                   {3, 3, 1},
                                                                   {3, 3, 3}}) {
    auto bad = response_schema;
    bad.tensors[0].descriptor.shape = shape;
    metadata.inputs[0].result_schema =
        std::make_shared<const SchemaTemplate>(bad);
    started = registry->start_result("fft.import_response", query,
                                     budget.allocator());
    PS_CHECK(!started.ok() && started.status().code == ErrorCode::TypeMismatch);
    PS_CHECK(budget.statistics().peak[ResourceKind::Host] == 0);
  }
  metadata.inputs.resize(2);
  metadata.inputs[0] = {};
  metadata.inputs[1] = {};
  metadata.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(expected);
  metadata.inputs[1].result_schema =
      std::make_shared<const SchemaTemplate>(other);
  started = registry->start_result("fft.multiply", query, budget.allocator());
  PS_CHECK(!started.ok() && started.status().code == ErrorCode::TypeMismatch);
  PS_CHECK(budget.statistics().peak[ResourceKind::Host] == 0);
  metadata.inputs.resize(1);
  metadata.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(source_schema);
  started =
      registry->start_result("fft.forward_real", query, budget.allocator());
  PS_CHECK(started.ok());
  return 0;
}
