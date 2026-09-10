#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "foundations_workflow/workflow.hpp"

namespace {
using namespace foundations;  // NOLINT(build/namespaces)
using ps::ErrorCode;
Parameters domain(double a = 0, double b = 1) {
  return {{"domain_min", a},
          {"domain_max", b},
          {"out_of_domain", std::string("reject")}};
}
Parameters samples(std::int64_t n = 5) {
  auto p = domain();
  p["count"] = n;
  return p;
}
Parameters levels(double gamma = 1) {
  return {{"black", 0.},
          {"white", 1.},
          {"gamma", gamma},
          {"out_min", 0.},
          {"out_max", 1.}};
}
ps::Value mask(const std::vector<float>& values,
               std::vector<std::uint64_t> shape) {
  return array(values, shape, facets(ps::coverage_semantics()));
}
void curves() {
  const auto controls = array<float>({0, 0, .5, .25, 1, 1}, {3, 2});
  exact<float>(output(operation("curve.sample_linear", {controls}, samples())),
               {0, .125, .25, .625, 1});
  exact<float>(
      output(operation("curve.sample_monotone", {controls}, samples())),
      {0, .078125, .25, .546875, 1});
  const auto flat = array<double>({0, 2, .3, 2, .6, -1, 1, -1}, {4, 2});
  const auto values =
      output(operation("curve.sample_monotone", {flat}, samples(101)));
  double last = 2;
  for (unsigned i = 0; i <= 100; ++i) {
    double value;
    std::memcpy(&value, values.bytes().data() + 8 * i, 8);
    require(value <= last && value >= -1, "PCHIP shape oracle");
    if (i <= 30)
      require(value == 2, "PCHIP first plateau");
    if (i >= 60)
      require(value == -1, "PCHIP last plateau");
    last = value;
  }
  auto p = samples(3);
  for (const auto* key : {"curve.sample_linear", "curve.sample_monotone"}) {
    exact<double>(
        output(operation(key, {array<double>({0, -1, 1, 1}, {2, 2})}, p)),
        {-1, 0, 1});
    rejected(operation(key, {array<float>({0, 0, 0, 1}, {2, 2})}, p),
             ErrorCode::OperationFailed);
    rejected(operation(key, {array<float>({0, 0}, {1, 2})}, p),
             ErrorCode::TypeMismatch);
    rejected(operation(key, {array<float>({0, 0, 1, INFINITY}, {2, 2})}, p),
             ErrorCode::OperationFailed);
    auto bad = p;
    bad["count"] = std::int64_t{1};
    rejected(operation(key, {controls}, bad), ErrorCode::InvalidArgument);
    bad = p;
    bad["domain_min"] = -1.;
    rejected(operation(key, {controls}, bad), ErrorCode::OperationFailed);
    bad["out_of_domain"] = std::string("clip");
    exact<float>(output(operation(key, {controls}, bad)), {0, 0, 1});
  }
  const auto q = array<float>({0, .25, .5, 1}, {2, 2});
  const auto table = array<float>({0, .25, 1});
  exact<float>(output(operation("field.apply_lut_1d", {q, table}, domain())),
               {0, .125, .25, 1});
  rejected(
      operation("field.apply_lut_1d", {q, array<double>({0, 1})}, domain()),
      ErrorCode::TypeMismatch);
  rejected(
      operation("field.apply_lut_1d", {q, array<float>({0, NAN})}, domain()),
      ErrorCode::OperationFailed);
  rejected(operation("field.apply_lut_1d", {q, table}, domain(1, 0)),
           ErrorCode::InvalidArgument);
  auto clip = domain();
  clip["out_of_domain"] = std::string("clip");
  exact<float>(output(operation("field.apply_lut_1d",
                                {array<float>({-1, 2}, {1, 2}), table}, clip)),
               {0, 1});
  const double max = std::numeric_limits<double>::max();
  exact<double>(
      output(operation("field.apply_lut_1d",
                       {array<double>({0}, {1, 1}), array<double>({-max, max})},
                       domain(-max, max))),
      {0});
  exact<float>(
      output(operation("field.apply_lut_1d",
                       {array<float>({0}, {1, 1}),
                        array<float>({-std::numeric_limits<float>::max(),
                                      std::numeric_limits<float>::max()})},
                       domain(-1, 1))),
      {0});
  exact<float>(
      output(operation("field.apply_lut_1d",
                       {array<float>({0}, {1, 1}), array<float>({1e30F, 0})},
                       domain(-1, 0x1p-52))),
      {222044608266240.F});
  exact<float>(output(operation("field.apply_lut_1d",
                                {array<float>({0}, {1, 1}),
                                 array<float>({0x1p100F, -0x1p48F})},
                                domain(-1, 0x1p-52))),
               {0});
  exact<float>(output(operation("field.apply_lut_1d",
                                {array<float>({2}, {1, 1}),
                                 array<float>({0x1p101F, -3 * 0x1p100F})},
                                domain(0, 5))),
               {0});
  for (const auto* key : {"curve.sample_linear", "curve.sample_monotone"}) {
    auto p = domain(0, 5);
    p["count"] = std::int64_t{6};
    const auto result = output(operation(
        key, {array<float>({0, 0x1p101F, 5, -3 * 0x1p100F}, {2, 2})}, p));
    float middle;
    std::memcpy(&middle, result.bytes().data() + 8, 4);
    require(middle == 0, "integer-domain weighted cancellation");
    exact<float>(
        output(operation(key, {array<float>({0, 0.F, .5F, -0.F, 1, 1}, {3, 2})},
                         samples(3))),
        {0.F, -0.F, 1});
  }
  for (double amplitude : {2., 6.}) {
    const double tiny = amplitude * std::numeric_limits<double>::denorm_min();
    exact<double>(
        output(operation(
            "field.apply_lut_1d",
            {array<double>({.5}, {1, 1}), array<double>({tiny, 0})}, domain())),
        {tiny / 2});
    for (const auto* key : {"curve.sample_linear", "curve.sample_monotone"})
      exact<double>(
          output(operation(key, {array<double>({0, tiny, 1, 0}, {2, 2})},
                           samples(3))),
          {tiny, tiny / 2, 0});
    auto p = levels();
    p["out_min"] = 0.;
    p["out_max"] = tiny;
    exact<double>(
        output(operation("grade.levels", {array<double>({.5}, {1, 1})}, p)),
        {tiny / 2});
  }
}
void masks_and_mix() {
  const auto a = mask({0, .5, 1}, {1, 3});
  exact<float>(output(operation("mask.invert", {a})), {1, .5, 0});
  for (const auto& alg :
       {std::string("fuzzy"), std::string("independent_coverage")}) {
    const bool fuzzy = alg == "fuzzy";
    unsigned op = 0;
    for (const auto* name : {"and", "or", "xor"}) {
      const float mid[] = {fuzzy ? .5F : .25F, fuzzy ? .5F : .75F,
                           fuzzy ? 0.F : .5F};
      exact<float>(output(operation(
                       "mask.combine", {a, a},
                       {{"algebra", alg}, {"operation", std::string(name)}})),
                   {0, mid[op], op == 2 ? 0.F : 1.F});
      ++op;
    }
  }
  rejected(operation("mask.invert", {array<float>({0}, {1, 1})}),
           ErrorCode::TypeMismatch);
  const auto impulse = mask({0, 0, 0, 0, 1, 0, 0, 0, 0}, {3, 3});
  exact<float>(output(operation("mask.dilate", {impulse},
                                {{"radius", std::int64_t{1}},
                                 {"footprint", std::string("disk")}})),
               {0, 1, 0, 1, 1, 1, 0, 1, 0});
  exact<float>(output(operation("mask.dilate", {impulse},
                                {{"radius", std::int64_t{1}},
                                 {"footprint", std::string("square")}})),
               std::vector<float>(9, 1));
  for (const auto* key : {"mask.dilate", "mask.erode"})
    exact<float>(output(operation(key, {a},
                                  {{"radius", std::int64_t{0}},
                                   {"footprint", std::string("disk")}})),
                 {0, .5, 1});
  exact<float>(
      output(operation(
          "mask.erode", {mask(std::vector<float>(9, 1), {3, 3})},
          {{"radius", std::int64_t{1}}, {"footprint", std::string("square")}})),
      {0, 0, 0, 0, 1, 0, 0, 0, 0});
  exact<float>(output(operation("mask.erode", {a},
                                {{"radius", std::int64_t{64}},
                                 {"footprint", std::string("disk")}})),
               {0, 0, 0});
  const auto rgba = facets(ps::rgba_semantics());
  const auto image_a =
      array<float>({-1, 2, 3, .5, 1, 1, 1, 1, 0, 0, 0, 0}, {1, 3, 4}, rgba);
  const auto image_b =
      array<float>({2, 1, 0, 1, 0, 0, 0, 0, -2, 1, 0, 1}, {1, 3, 4}, rgba);
  exact<float>(output(operation("image.mix", {image_a, image_b, a})),
               {-1, 2, 3, .5, .5, .5, .5, .5, -2, 1, 0, 1});
  exact<float>(output(operation("image.mix", {image_a, image_a, a})),
               {-1, 2, 3, .5, 1, 1, 1, 1, 0, 0, 0, 0});
  const auto positive_zero =
      array<float>({0.F, 0.F, 0.F, 0.F}, {1, 1, 4}, rgba);
  const auto negative_zero =
      array<float>({-0.F, -0.F, -0.F, -0.F}, {1, 1, 4}, rgba);
  exact<float>(output(operation("image.mix", {negative_zero, positive_zero,
                                              mask({1}, {1, 1})})),
               {0.F, 0.F, 0.F, 0.F});
  exact<float>(output(operation("image.mix", {positive_zero, negative_zero,
                                              mask({1}, {1, 1})})),
               {-0.F, -0.F, -0.F, -0.F});
}
void fields() {
  const auto input = array<float>({0, .25, .5, 1}, {2, 2});
  exact<float>(output(operation("grade.levels", {input}, levels(2))),
               {0, .5, std::sqrt(.5F), 1});
  auto bad = levels();
  bad["gamma"] = 0.;
  rejected(operation("grade.levels", {input}, bad), ErrorCode::InvalidArgument);
  bad = levels();
  bad["white"] = 0.;
  rejected(operation("grade.levels", {input}, bad), ErrorCode::InvalidArgument);
  const auto smooth = output(
      operation("field.smoothstep", {input}, {{"edge0", 0.}, {"edge1", 1.}}));
  exact<float>(smooth, {0, .15625, .5, 1});
  exact<float>(output(operation("mask.invert", {smooth})), {1, .84375, .5, 0});
  rejected(
      operation("field.smoothstep", {input}, {{"edge0", 1.}, {"edge1", 0.}}),
      ErrorCode::InvalidArgument);
  for (const auto* key : {"numeric.minimum", "numeric.maximum"}) {
    const bool minimum = std::string(key) == "numeric.minimum";
    exact<double>(output(operation(key, {array<double>({-0., 1, -3}),
                                         array<double>({0., 2, -4})})),
                  {minimum ? -0. : 0., minimum ? 1. : 2., minimum ? -4. : -3.});
    rejected(operation(key, {array<double>({1}), array<float>({1})}),
             ErrorCode::TypeMismatch);
  }
  exact<double>(output(operation("numeric.abs", {array<double>({-0., -3, 4})})),
                {0., 3, 4});
  rejected(operation("numeric.abs", {array<float>({NAN})}),
           ErrorCode::OperationFailed);
  Parameters g{{"height", std::int64_t{2}},
               {"width", std::int64_t{3}},
               {"dtype", std::string("float64")},
               {"axis", std::string("x")},
               {"space", std::string("pixel")}};
  exact<double>(output(operation("field.coordinate", {}, g)),
                {.5, 1.5, 2.5, .5, 1.5, 2.5});
  g["axis"] = std::string("y");
  g["space"] = std::string("normalized");
  exact<double>(output(operation("field.coordinate", {}, g)),
                {.25, .25, .25, .75, .75, .75});
  g["height"] = std::int64_t{1};
  g["width"] = std::int64_t{1};
  exact<double>(output(operation("field.coordinate", {}, g)), {.5});
  g.erase("axis");
  g.erase("space");
  g["value"] = -2.;
  exact<double>(output(operation("field.constant", {}, g)), {-2});
  g["dtype"] = std::string("float32");
  g["value"] = std::numeric_limits<double>::max();
  rejected(operation("field.constant", {}, g), ErrorCode::OperationFailed);
  g["height"] = std::int64_t{0};
  rejected(operation("field.constant", {}, g), ErrorCode::InvalidArgument);
  auto cancellation_levels = levels();
  cancellation_levels["white"] = 5.;
  cancellation_levels["out_min"] = -3 * 0x1p100;
  cancellation_levels["out_max"] = 0x1p101;
  exact<float>(output(operation("grade.levels", {array<float>({3}, {1, 1})},
                                cancellation_levels)),
               {0});
}
void filters_and_counts() {
  const auto input = array<float>({1, 2, 3}, {1, 3});
  const auto kernel = array<float>({1, 2}, {1, 2});
  Parameters p{{"anchor_y", std::int64_t{0}},
               {"anchor_x", std::int64_t{0}},
               {"boundary", std::string("zero")}};
  exact<float>(output(operation("field.correlate", {input, kernel}, p)),
               {5, 8, 3});
  exact<float>(output(operation("field.convolve", {input, kernel}, p)),
               {1, 4, 7});
  p["anchor_x"] = std::int64_t{1};
  exact<float>(output(operation("field.correlate", {input, kernel}, p)),
               {2, 5, 8});
  exact<float>(output(operation("field.convolve", {input, kernel}, p)),
               {4, 7, 6});
  p["boundary"] = std::string("clamp");
  exact<float>(output(operation("field.correlate", {input, kernel}, p)),
               {3, 5, 8});
  p["anchor_x"] = std::int64_t{2};
  rejected(operation("field.correlate", {input, kernel}, p),
           ErrorCode::InvalidArgument);
  for (const auto* key : {"field.box_mean", "field.gaussian_blur"}) {
    Parameters blur{{"radius", std::int64_t{1}}};
    if (std::string(key) == "field.gaussian_blur")
      blur["sigma"] = 1.;
    close(output(operation(
              key, {array<float>(std::vector<float>(12, 3), {3, 4})}, blur)),
          std::vector<float>(12, 3));
    const auto covered =
        output(operation(key, {mask(std::vector<float>(6, 1), {2, 3})}, blur));
    close(output(operation("mask.invert", {covered})),
          std::vector<float>(6, 0));
  }
  close(output(operation("field.box_mean", {input},
                         {{"radius", std::int64_t{1}}})),
        {4.F / 3, 2, 8.F / 3});
  exact<float>(output(operation("field.gaussian_blur", {input},
                                {{"radius", std::int64_t{64}}, {"sigma", 0.}})),
               {1, 2, 3});
  const auto impulse = array<float>({0, 0, 0, 0, 1, 0, 0, 0, 0}, {3, 3});
  const double a = std::exp(-.5), z = 1 + 2 * a;
  close(output(operation("field.gaussian_blur", {impulse},
                         {{"radius", std::int64_t{1}}, {"sigma", 1.}})),
        {static_cast<float>(a * a / (z * z)), static_cast<float>(a / (z * z)),
         static_cast<float>(a * a / (z * z)), static_cast<float>(a / (z * z)),
         static_cast<float>(1 / (z * z)), static_cast<float>(a / (z * z)),
         static_cast<float>(a * a / (z * z)), static_cast<float>(a / (z * z)),
         static_cast<float>(a * a / (z * z))});
  Parameters hist{{"bins", std::int64_t{4}},
                  {"range_min", 0.},
                  {"range_max", 1.}};
  const auto values = array<double>({-1, 0, .25, .5, .75, 1, 2}, {1, 7});
  exact<std::int64_t>(output(operation("analysis.histogram", {values}, hist)),
                      {1, 1, 1, 2});
  hist.erase("bins");
  exact<std::int64_t>(
      output(operation("analysis.histogram_out_of_range", {values}, hist)),
      {1, 1});
  rejected(operation("analysis.histogram_out_of_range",
                     {array<float>({INFINITY}, {1, 1})}, hist),
           ErrorCode::OperationFailed);
  std::vector<std::int64_t> boundary_counts(100, 0);
  boundary_counts[29] = 2;
  boundary_counts[30] = 2;
  boundary_counts[58] = 1;
  exact<std::int64_t>(
      output(operation("analysis.histogram",
                       {array<double>({29, 58, std::nextafter(30., 0.), 30.,
                                       std::nextafter(30., 100.)},
                                      {1, 5})},
                       {{"bins", std::int64_t{100}},
                        {"range_min", 0.},
                        {"range_max", 100.}})),
      boundary_counts);
}
void regional() {
  std::vector<float> data(35);
  for (unsigned i = 0; i < data.size(); ++i)
    data[i] = static_cast<float>(i % 11) - 5;
  auto input = array(data, {5, 7});
  for (const auto* key : {"field.box_mean", "field.gaussian_blur",
                          "grade.levels", "field.smoothstep", "numeric.abs"}) {
    Parameters p;
    const std::string name(key);
    if (name == "field.box_mean" || name == "field.gaussian_blur")
      p["radius"] = std::int64_t{2};
    if (name == "field.gaussian_blur")
      p["sigma"] = 1.;
    if (name == "grade.levels")
      p = levels();
    if (name == "field.smoothstep")
      p = {{"edge0", -2.}, {"edge1", 3.}};
    const auto whole = output(operation(key, {input}, p));
    ps::PlanningOptions options;
    options.tile_height = 1;
    options.tile_width = 2;
    options.output_regions["result"] = ps::Region({{1, 3}, {2, 4}});
    const auto roi = output(evaluate(
        {input}, {{1, key, {ps::WorkflowInputReference{1}}, p}}, options));
    std::vector<float> expected;
    for (unsigned y = 1; y < 4; ++y)
      for (unsigned x = 2; x < 6; ++x) {
        float number;
        std::memcpy(&number, whole.bytes().data() + (y * 7 + x) * 4, 4);
        expected.push_back(number);
      }
    exact<float>(roi, expected);
  }
}

void views_and_failures() {
  auto registry = ps::make_default_operation_registry();
  auto invoke = [&](const std::string& key,
                    const std::vector<ps::Value>& inputs, const Parameters& p,
                    ps::Region out = {},
                    ps::BufferAllocator allocator = ps::BufferAllocator{},
                    ps::CancellationToken cancellation = {}) {
    std::vector<ps::Region> demands;
    for (const auto& input : inputs)
      demands.push_back(input.region());
    return registry->invoke(key, {inputs, demands, p, ps::Backend::Cpu,
                                  cancellation, out, allocator});
  };
  std::vector<std::uint8_t> bytes(13);
  const float data[] = {1, -2, 3};
  std::memcpy(bytes.data() + 1, data, 12);
  auto view = take(ps::Value::create({ps::ElementType::Float32, {1, 3}},
                                     ps::Region::whole({1, 3}),
                                     {1, {0, -4}, {0, 2}}, bytes));
  exact<float>(take(invoke("numeric.abs", {view}, {})), {3, 2, 1});
  close(take(invoke("field.box_mean", {view}, {{"radius", std::int64_t{1}}})),
        {4.F / 3, 2.F / 3, 0});
  const float value = .5;
  std::memcpy(bytes.data() + 1, &value, 4);
  auto broadcast = take(ps::Value::create(
      {ps::ElementType::Float32, {2, 3}}, ps::Region::whole({2, 3}),
      {1, {0, 0}}, bytes, facets(ps::coverage_semantics())));
  exact<float>(take(invoke("mask.invert", {broadcast}, {})),
               std::vector<float>(6, .5));
  // A partial view retains full descriptor coordinates and a nonzero origin.
  auto partial = take(ps::Value::create({ps::ElementType::Float32, {9, 9}},
                                        ps::Region({{4, 1}, {5, 3}}),
                                        {1, {0, 4}, {4, 5}}, bytes));
  exact<float>(take(invoke("numeric.abs", {partial}, {}, partial.region())),
               {.5, 2, 3});
  const auto controls = array<double>({0, 0, .5, .25, 1, 1}, {3, 2});
  const auto field = array<float>(std::vector<float>(9, 1), {3, 3});
  for (const auto* key : {"curve.sample_monotone", "field.gaussian_blur"}) {
    const bool curve = std::string(key) == "curve.sample_monotone";
    const std::vector<ps::Value> inputs{curve ? controls : field};
    const Parameters p =
        curve ? samples()
              : Parameters{{"radius", std::int64_t{1}}, {"sigma", 1.}};
    for (unsigned boundary : {1U, 2U}) {
      ps::CancellationSource cancellation;
      std::uint64_t live = 0;
      unsigned allocations = 0;
      ps::BufferAllocator allocator([&](std::uint64_t size) {
        live += size;
        if (++allocations == boundary)
          cancellation.cancel();
        return ps::Result<std::shared_ptr<void>>(
            std::shared_ptr<void>(new int(0), [&, size](void* pointer) {
              delete static_cast<int*>(pointer);
              live -= size;
            }));
      });
      auto result = invoke(key, inputs, p, {}, allocator, cancellation.token());
      require(!result.ok() && result.status().code == ErrorCode::Cancelled &&
                  live == 0 && allocations == boundary,
              "cancelled output/scratch must release allocations");
    }
    std::uint64_t live = 0;
    unsigned allocations = 0;
    ps::BufferAllocator failing([&](std::uint64_t size) {
      if (++allocations == 2)
        return ps::Result<std::shared_ptr<void>>(ps::Status::failure(
            ErrorCode::ResourceExhausted, "fixture scratch refusal"));
      live += size;
      return ps::Result<std::shared_ptr<void>>(
          std::shared_ptr<void>(new int(0), [&, size](void* pointer) {
            delete static_cast<int*>(pointer);
            live -= size;
          }));
    });
    auto failed = invoke(key, inputs, p, {}, failing);
    require(!failed.ok() &&
                failed.status().code == ErrorCode::ResourceExhausted &&
                live == 0,
            "scratch refusal must release output");
  }

  // Oversized borrowed Values must not expand a narrow declared halo demand.
  std::vector<float> large_samples(300, 1);
  large_samples[0] = NAN;
  const auto oversized = array(large_samples, {100, 3});
  const ps::Region demand({{49, 3}, {0, 3}}), requested({{50, 1}, {1, 1}});
  for (const auto* key : {"field.box_mean", "field.gaussian_blur"}) {
    Parameters p{{"radius", std::int64_t{1}}};
    if (std::string(key) == "field.gaussian_blur")
      p["sigma"] = 1.;
    for (const auto& source : {oversized, take(oversized.view(demand))}) {
      std::uint64_t live = 0, peak = 0;
      {
        ps::BufferAllocator bounded([&](std::uint64_t size) {
          if (size > 1108 - live)
            return ps::Result<std::shared_ptr<void>>(ps::Status::failure(
                ErrorCode::ResourceExhausted, "halo fixture budget"));
          live += size;
          peak = std::max(peak, live);
          return ps::Result<std::shared_ptr<void>>(
              std::shared_ptr<void>(new int(0), [&, size](void* pointer) {
                delete static_cast<int*>(pointer);
                live -= size;
              }));
        });
        const std::vector<ps::Value> inputs{source};
        const std::vector<ps::Region> demands{demand};
        close(take(registry->invoke(key, {inputs,
                                          demands,
                                          p,
                                          ps::Backend::Cpu,
                                          {},
                                          requested,
                                          bounded})),
              {1});
      }
      require(live == 0 && peak <= 1108,
              "halo working set follows declared demand");
    }
  }
  // The same public plan accepts new control values without recompilation.
  auto doc = document({controls}, {{1,
                                    "curve.sample_monotone",
                                    {ps::WorkflowInputReference{1}},
                                    samples()}});
  ps::GraphContext graph(doc);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContext execution(registry, {1, false, 8, 4096, 2048});
  exact<double>(take(execution.execute(compiled.plan, bindings({controls})))
                    .values.at("result"),
                {0, .078125, .25, .546875, 1});
  const auto changed = array<double>({0, 1, .5, 1, 1, 1}, {3, 2});
  exact<double>(take(execution.execute(compiled.plan, bindings({changed})))
                    .values.at("result"),
                std::vector<double>(5, 1));
  ps::ExecutionContext limited(registry, {1, false, 8, 8});
  require(limited.execute(compiled.plan, bindings({controls})).status().code ==
              ErrorCode::ResourceExhausted,
          "public execution must enforce scratch budget");
  ps::CancellationSource stopped;
  stopped.cancel();
  require(
      execution.execute(compiled.plan, bindings({controls}), stopped.token())
              .status()
              .code == ErrorCode::Cancelled,
      "public execution cancellation");
  const int rounding = std::fegetround();
  require(std::fesetround(FE_DOWNWARD) == 0, "set rounding fixture");
  auto rounded =
      operation("grade.levels", {array<double>({.25}, {1, 1})}, levels(2));
  const bool restored = std::fegetround() == FE_DOWNWARD;
  std::fesetround(rounding);
  exact<double>(output(std::move(rounded)), {.5});
  require(restored, "numeric environment must be restored");
}
}  // namespace
int main() {
  try {
    curves();
    masks_and_mix();
    fields();
    filters_and_counts();
    regional();
    views_and_failures();
    std::cout << "basic operator public oracles passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
