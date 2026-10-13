#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "01-numeric/expression.hpp"
#include "01-numeric/expression_metadata.hpp"
#include "common/floating_environment.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::take;
using plugin_internal::expression_result::infer_expression_facets;
int metadata() {
  plugin_internal::TensorMetadata coefficients;
  coefficients.descriptor = {ElementType::Float64, {2}};
  const ValueDescriptor output{ElementType::Float32, {5}};
  std::map<std::string, ParameterValue> parameters{
      {"expression", std::string("c[0] + c[1]*x")},
      {"start", -2.0},
      {"step", 0.5}};
  fenv_t previous;
  PS_CHECK(fegetenv(&previous) == 0);
  for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    PS_CHECK(fesetround(mode) == 0);
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_DIVBYZERO);
    auto facets = take(
        infer_expression_facets(false, {coefficients}, parameters, output));
    PS_CHECK(facets.size() == 1);
    auto semantic = take(decode_semantic(facets.front()));
    PS_CHECK(semantic.kind == SemanticKind::SampledSignal);
    PS_CHECK(semantic.sample_origin == -2 && semantic.sample_step == 0.5);
    PS_CHECK(semantic.channels.size() == 1 &&
             semantic.channels[0].unit == "dimensionless" &&
             semantic.sample_axis_unit == "dimensionless");
    PS_CHECK(fegetround() == mode &&
             fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
    auto invalid = parameters;
    invalid["step"] = 0.0;
    PS_CHECK(infer_expression_facets(false, {coefficients}, invalid, output)
                 .status()
                 .code == ErrorCode::InvalidArgument);
    invalid["step"] = std::numeric_limits<double>::max();
    PS_CHECK(infer_expression_facets(false, {coefficients}, invalid, output)
                 .status()
                 .code == ErrorCode::InvalidArgument);
    invalid = parameters;
    invalid["expression"] = std::string("c[2]");
    PS_CHECK(infer_expression_facets(false, {coefficients}, invalid, output)
                 .status()
                 .code == ErrorCode::InvalidArgument);
    PS_CHECK(fegetround() == mode &&
             fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
  }
  PS_CHECK(fesetenv(&previous) == 0);
  auto wrong = coefficients;
  wrong.descriptor.element_type = ElementType::Float32;
  PS_CHECK(infer_expression_facets(false, {wrong}, parameters, output)
               .status()
               .code == ErrorCode::TypeMismatch);
  auto table_facets =
      take(infer_expression_facets(false, {coefficients}, parameters, output));
  plugin_internal::TensorMetadata query, table;
  query.descriptor = {ElementType::Float32, {3}};
  table.descriptor = output;
  query.facets = table.facets = table_facets;
  for (const char* policy : {"reject", "clip"}) {
    auto result = infer_expression_facets(
        true, {query, table}, {{"out_of_domain", std::string(policy)}},
        query.descriptor);
    PS_REQUIRE_OK(result);
    PS_CHECK(result.value().empty());
  }
  PS_CHECK(infer_expression_facets(true, {query, table},
                                   {{"out_of_domain", std::string("wrap")}},
                                   query.descriptor)
               .status()
               .code == ErrorCode::InvalidArgument);
  auto semantic = take(decode_semantic(table.facets.front()));
  semantic.sample_axis_unit = "seconds";
  table.facets = {take(encode_semantic(semantic))};
  PS_CHECK(infer_expression_facets(true, {query, table},
                                   {{"out_of_domain", std::string("clip")}},
                                   query.descriptor)
               .status()
               .code == ErrorCode::TypeMismatch);
  semantic.sample_axis_unit = "dimensionless";
  semantic.sample_origin = 0x1p60;
  semantic.sample_step = 1;
  table.facets = {take(encode_semantic(semantic))};
  PS_CHECK(infer_expression_facets(true, {query, table},
                                   {{"out_of_domain", std::string("clip")}},
                                   query.descriptor)
               .status()
               .code == ErrorCode::TypeMismatch);
  return 0;
}
int evaluation() {
  plugin_internal::FloatingEnvironment environment;
  PS_CHECK(environment.active());
  struct Case {
    const char* text;
    double expected;
  };
  // Exact binary64 arithmetic for these integer/power-of-two cases provides
  // an oracle independent of the parser's AST and precedence implementation.
  const Case cases[] = {{"c[0]+c[1]*x", 14},
                        {"-2^2", -4},
                        {"2^3^2", 512},
                        {"(c[1]-c[0])/2", .5},
                        {"min(abs(-7),max(3,5))", 5}};
  std::uint8_t coefficients[256 * sizeof(double)]{},
      scratch[sizeof(coefficients)]{};
  const double values[] = {2, 3};
  std::memcpy(coefficients, values, sizeof(values));
  for (const auto& test : cases) {
    auto expression = take(expression_internal::parse(test.text, 2));
    auto result =
        expression_internal::evaluate(expression, 4, coefficients, scratch, {});
    PS_REQUIRE_OK(result);
    PS_CHECK(result.value() == test.expected);
  }
  for (const char* text : {"1/0", "sqrt(-1)", "exp(1000)"}) {
    auto expression = take(expression_internal::parse(text, 2));
    PS_CHECK(
        expression_internal::evaluate(expression, 4, coefficients, scratch, {})
            .status()
            .code == ErrorCode::OperationFailed);
  }
  CancellationSource cancellation;
  cancellation.cancel();
  auto expression = take(expression_internal::parse("x", 2));
  PS_CHECK(expression_internal::evaluate(expression, 4, coefficients, scratch,
                                         cancellation.token())
               .status()
               .code == ErrorCode::Cancelled);
  return 0;
}
}  // namespace
int main() {
  return metadata() || evaluation();
}
