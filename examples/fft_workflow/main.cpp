#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Poll = Result<ResultProgramPoll>;
void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(
        "code=" + std::to_string(static_cast<int>(result.status().code)) +
        " reason=" + std::to_string(static_cast<int>(result.status().reason)) +
        ": " + result.status().message);
  return result.take_value();
}
double sample(std::uint64_t y, std::uint64_t x, std::uint64_t width,
              unsigned variant) {
  if (variant == 4 && y == 0 && x == 0)
    return std::numeric_limits<double>::infinity();
  if (variant == 1)
    return y == 1 && x == 0 ? 1 : 0;
  return static_cast<double>(
             static_cast<std::int64_t>(((y * width + x) * 7) % 13) - 6) +
         (variant == 5 ? 1 : 0);
}
std::shared_ptr<RegionalSource> source(
    std::uint64_t h, std::uint64_t w, std::uint64_t k, bool response,
    unsigned variant, unsigned* reads,
    CancellationSource* cancellation = nullptr) {
  auto result = std::make_shared<RegionalSource>();
  result->descriptor = {ElementType::Float64,
                        response ? std::vector<std::uint64_t>{h, k, 2}
                                 : std::vector<std::uint64_t>{h, w}};
  result->read = [h, w, response, variant, reads, cancellation](
                     const Region& region, std::uint8_t* output,
                     std::uint64_t bytes, const BufferAllocator&,
                     const CancellationToken&) -> Result<Region> {
    ++*reads;
    if (variant == 3 && *reads == 2 && cancellation)
      cancellation->cancel();
    const auto& r = region.dimensions();
    std::uint64_t offset = 0;
    const long double tau = 2 * std::acos(-1.0L);
    for (auto y = r[0].offset; y < r[0].offset + r[0].extent; ++y)
      for (auto x = r[1].offset; x < r[1].offset + r[1].extent; ++x) {
        const auto channels = response ? r[2].extent : 1;
        for (std::uint64_t c = 0; c < channels; ++c) {
          if (offset + 8 > bytes)
            return Result<Region>(
                Status{ErrorCode::InvalidArgument, "source byte range"});
          double value = sample(y, x, w, variant);
          if (response) {
            const auto angle = -tau * (static_cast<long double>(y) / h +
                                       static_cast<long double>(x) / w);
            value = static_cast<double>(r[2].offset + c ? std::sin(angle)
                                                        : std::cos(angle));
            if (variant == 2 && !y && !x && r[2].offset + c == 1)
              value = 1;
          }
          std::memcpy(output + offset, &value, 8);
          offset += 8;
        }
      }
    return offset == bytes ? Result<Region>(region)
                           : Result<Region>(Status{ErrorCode::InvalidArgument,
                                                   "source byte count"});
  };
  return result;
}
struct Sink {
  std::uint64_t h, w, row = 0, batch = 0;
  unsigned variant, stage = 0;
  ResultRef input;
  ResultDescriptor descriptor;
  Sink(std::uint64_t height, std::uint64_t width, unsigned v)
      : h(height), w(width), variant(v) {}
  Poll poll(const ResultProgramPhase& p) {
    if (stage == 0) {
      stage = 1;
      return Poll(ResultProgramNeed{{}, {{0, 0, true, 0}}, {}});
    }
    if (stage == 1) {
      input = p.results.at(0);
      auto d = input.descriptor();
      if (!d.ok())
        return Poll(d.status());
      descriptor = d.take_value();
      stage = 2;
    }
    if (stage == 3) {
      auto fuel = p.consume_work(batch);
      if (!fuel.ok())
        return Poll(fuel);
      const auto& page =
          std::get<std::shared_ptr<const CpuStorage>>(p.io.at(0));
      for (std::uint64_t i = 0; i < batch; ++i) {
        double value;
        std::memcpy(&value, page->bytes().data() + i * 8, 8);
        const auto y = (row + i) / w, x = (row + i) % w;
        // Multiplication by exp(-2pi*i*(u/H+v/W)) is a circular (1,1) shift.
        const double expected =
            sample((y + h - 1) % h, (x + w - 1) % w, w, variant);
        if (std::abs(value - expected) > 1e-8)
          return Poll(Status{ErrorCode::OperationFailed,
                             "independent circular shift pixel oracle"});
      }
      row += batch;
      stage = 2;
    }
    if (row < h * w) {
      batch = std::min(h * w - row, p.query.page_bytes / 8);
      if (!batch)
        return Poll(Status{ErrorCode::ResourceExhausted, "sink page"});
      auto read = input.prepare_read(descriptor, 0, row, batch);
      if (!read.ok())
        return Poll(read.status());
      stage = 3;
      return Poll(ResultProgramNeed{{}, {}, {read.take_value()}});
    }
    auto memory = MutableValue::allocate(p.query.output.descriptor,
                                         Region::whole({1}), p.allocator);
    if (!memory.ok())
      return Poll(memory.status());
    auto buffer = memory.take_value();
    const double count = static_cast<double>(row);
    std::memcpy(buffer.data(), &count, 8);
    auto published = std::move(buffer).publish({});
    if (!published.ok())
      return Poll(published.status());
    const auto held = published.take_value();
    auto fragments = ValueFragments::create_view(
        p.query.output.descriptor, {}, *p.query.value_outputs, &held, 1);
    if (!fragments.ok())
      return Poll(fragments.status());
    auto relation =
        ResultRelation::cartesian(p.resources, 1, {0, 7, 0, h * w + 1},
                                  DependencyGuarantee::Conservative);
    if (!relation.ok())
      return Poll(relation.status());
    return Poll(
        ResultValuePublication{fragments.take_value(), relation.take_value()});
  }
};
void run(std::uint64_t h, std::uint64_t w, SpectrumPacking packing,
         std::uint64_t window, unsigned variant = 0,
         std::uint64_t work = 10000000, std::uint64_t disk = UINT64_MAX,
         std::uint32_t stages = 200000) {
  auto spec = take(fft_spectrum_spec(h, w, packing));
  const auto k = packing == SpectrumPacking::Full ? w : w / 2 + 1, n = h * w;
  auto registry = std::make_shared<OperationRegistry>();
  std::array<unsigned, 4> starts{};
  for (auto op : {FftOperation::ForwardReal, FftOperation::ImportResponse,
                  FftOperation::Multiply, FftOperation::InverseReal}) {
    auto definition = take(make_fft_operation(op, spec));
    auto start = definition.start_result;
    auto index = static_cast<unsigned>(op) - 1;
    definition.start_result = [&starts, index, start](const auto& query,
                                                      const auto& allocator) {
      ++starts[index];
      return start(query, allocator);
    };
    check(registry->register_operation(std::move(definition)).ok(),
          "register FFT");
  }
  OperationDefinition sink;
  sink.key = "example.fft_sink";
  auto& traits = sink.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.workspace_bytes = 4096;
  traits.input_schema[0].kind = OperationPortKind::Result;
  traits.input_schema[0].result_schema_id = "photospider.fft_real_output";
  traits.input_schema[0].result_schema_version = 1;
  auto& out = traits.outputs[0];
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 2;
  out.continuation_bytes = sizeof(Sink);
  out.maximum_dependency_stages = 1000000;
  out.output_element_type = ElementType::Float64;
  out.shape_rule = OperationShapeRule::Fixed;
  out.fixed_output_shape = {1};
  sink.start_result = [h, w, variant](const auto&, const auto& allocator) {
    return ResultContinuation::make<Sink>(allocator, h, w, variant);
  };
  check(registry->register_operation(std::move(sink)).ok(),
        "register FFT sink");
  WorkflowDocument doc;
  doc.inputs = {{1,
                 "pixels",
                 {ElementType::Float64, {h, w}},
                 Region::whole({h, w}),
                 {0, {static_cast<std::int64_t>(w * 8), 8}},
                 {}},
                {2,
                 "response",
                 {ElementType::Float64, {h, k, 2}},
                 Region::whole({h, k, 2}),
                 {0, {static_cast<std::int64_t>(k * 16), 16, 8}},
                 {}}};
  doc.nodes = {
      {1, "fft.forward_real", {WorkflowInputReference{1}}, {}},
      {2, "fft.import_response", {WorkflowInputReference{2}}, {}},
      {3,
       "fft.multiply",
       {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{2, "value"}},
       {}},
      {4, "fft.inverse_real", {WorkflowNodeOutput{3, "value"}}, {}},
      {5, "example.fft_sink", {WorkflowNodeOutput{4, "value"}}, {}},
      {6, "fft.forward_real", {WorkflowInputReference{1}}, {}}};
  doc.outputs = {{"a_sink", 5, "value"},
                 {"fft", 1, "value"},
                 {"alias", 6, "value"},
                 {"filtered", 3, "value"},
                 {"image", 4, "value"}};
  check(registry->freeze().ok(), "freeze FFT registry");
  GraphContext graph(doc);
  auto compiled = take(Compiler(registry).compile(graph));
  unsigned reads = 0;
  ResourceBudget root;
  Result<ExecutionResult> result(Status{ErrorCode::Internal, {}});
  {
    ExecutionContextConfig config;
    config.managed_resources = ResourceLimits{};
    config.managed_resources->capacity[ResourceKind::Host] = 65536;
    config.managed_resources->capacity[ResourceKind::Metadata] = 65536;
    config.managed_resources->capacity[ResourceKind::Disk] = disk;
    ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    ExecutionOptions options;
    options.maximum_result_window_bytes = window;
    options.maximum_dependency_work = work;
    options.dependencies.maximum_stages = stages;
    CancellationSource cancellation;
    ExecutionResult prior;
    if (variant == 5) {
      auto prior_doc = doc;
      prior_doc.outputs = {{"fft", 1, "value"}};
      GraphContext prior_graph(prior_doc);
      auto prior_plan = take(Compiler(registry).compile(prior_graph));
      prior = take(context.execute(
          prior_plan.plan,
          {{{"pixels", {}, source(h, w, k, false, 0, &reads)},
            {"response", {}, source(h, w, k, true, 0, &reads)}}},
          {}, options));
    }
    if (variant == 6)
      graph.replace(doc);
    result = context.execute(
        compiled.plan,
        {{{"pixels",
           {},
           source(h, w, k, false, variant, &reads, &cancellation)},
          {"response",
           {},
           source(h, w, k, true, variant, &reads, &cancellation)}}},
        cancellation.token(), options);
    if (variant == 5 && result.ok()) {
      auto before = prior.results.at("fft"),
           after = result.value().results.at("fft");
      check(before.object_id() != after.object_id(),
            "changed Run snapshots must not share a spectrum");
      auto old =
          take(take(before.prepare_read(take(before.descriptor()), 0, 0, 1))
                   .load(window));
      auto current =
          take(take(after.prepare_read(take(after.descriptor()), 0, 0, 1))
                   .load(window));
      double old_dc, new_dc;
      std::memcpy(&old_dc, old->bytes().data(), 8);
      std::memcpy(&new_dc, current->bytes().data(), 8);
      check(new_dc - old_dc == static_cast<double>(n),
            "retained old and new DC snapshot references");
    }
  }
  if (window < 16 || work < 1000 || disk < 8192 || variant == 2 ||
      variant == 3 || variant == 4 || variant == 6) {
    check(!result.ok(), "expected FFT bounded/association failure");
    if (variant == 6) {
      check(result.status().code == ErrorCode::Stale && reads == 0,
            "stale plan rejected before source reads");
    } else if (variant == 4) {
      check(result.status().reason == FailureReason::InvalidDomain,
            "nonfinite input domain reason");
    } else if (variant == 3) {
      check(result.status().code == ErrorCode::Cancelled,
            "FFT cancellation after temporary writes");
    } else if (variant == 2) {
      check(result.status().code == ErrorCode::TypeMismatch &&
                result.status().detail.scope == FailureScope::Association,
            "Hermitian failure category and scope");
    } else {
      check(result.status().code == ErrorCode::ResourceExhausted,
            "FFT resource failure category");
    }
    check(root.statistics().live[ResourceKind::Disk] == 0 &&
              root.statistics().live[ResourceKind::Payload] == 0,
          "FFT failure cleanup");
    std::cout << "rejected " << h << 'x' << w << " window=" << window << ": "
              << result.status().message << '\n';
    return;
  }
  auto output = take(std::move(result));
  check(starts == std::array<unsigned, 4>{variant == 5 ? 2U : 1U, 1, 1, 1},
        "cache-off FFT producers shared once");
  auto fft = output.results.at("fft"), image = output.results.at("image");
  check(fft.object_id() == output.results.at("alias").object_id(),
        "FFT alias identity");
  auto descriptor = take(fft.descriptor());
  check(descriptor.rows(0) == h * k, "packed row count and original shape");
  const long double tau = 2 * std::acos(-1.0L);
  double maximum = 0;
  if (variant == 1) {
    for (std::uint64_t row = 0; row < h * k;) {
      const auto batch = std::min(h * k - row, window / 16);
      auto page =
          take(take(fft.prepare_read(descriptor, 0, row, batch)).load(window));
      for (std::uint64_t i = 0; i < batch; ++i) {
        const auto angle = -tau * static_cast<long double>((row + i) / k) / h;
        const std::complex<long double> reference =
            h > 1 ? std::complex<long double>(std::cos(angle), std::sin(angle))
                  : std::complex<long double>{};
        std::array<double, 2> actual;
        std::memcpy(actual.data(), page->bytes().data() + i * 16, 16);
        const auto error = static_cast<double>(std::abs(
            std::complex<long double>(actual[0], actual[1]) - reference));
        maximum = std::max(maximum, error);
        check(error < 1e-8, "independent analytic impulse spectrum oracle");
        if (h == 3 && w == 4 && (row + i) / k == 1 && (row + i) % k == 2)
          check(actual[1] < -.8,
                "Nyquist column must retain nonreal conjugate values");
      }
      row += batch;
    }
  }
  for (std::uint64_t q = 0; variant != 1 && q < h * k; ++q) {
    if (n > 1024 && q != 0 && q != 1 && q != k - 1)
      continue;
    const auto u = q / k, v = q % k;
    std::complex<long double> reference{};
    for (std::uint64_t y = 0; y < h; ++y)
      for (std::uint64_t x = 0; x < w; ++x) {
        const auto angle = -tau * (static_cast<long double>(u * y) / h +
                                   static_cast<long double>(v * x) / w);
        reference +=
            static_cast<long double>(sample(y, x, w, variant)) *
            std::complex<long double>(std::cos(angle), std::sin(angle));
      }
    auto page = take(take(fft.prepare_read(descriptor, 0, q, 1)).load(window));
    std::array<double, 2> actual;
    std::memcpy(actual.data(), page->bytes().data(), 16);
    const auto error = static_cast<double>(
        std::abs(std::complex<long double>(actual[0], actual[1]) - reference));
    maximum = std::max(maximum, error);
    check(error < 1e-8, "independent direct DFT spectrum oracle");
  }
  auto image_facts = take(image.descriptor());
  auto residue =
      take(take(image.prepare_read(image_facts, 1, 0, 1)).load(window));
  double imaginary;
  std::memcpy(&imaginary, residue->bytes().data(), 8);
  check(imaginary < 1e-8, "measured inverse imaginary residue");
  auto held = take(take(image.prepare_read(image_facts, 0, 0, 1)).load(window));
  auto weak = fft.weak();
  output = {};
  fft = {};
  image = {};
  check(weak.lock().valid(),
        "inverse window retains FFT association after context");
  residue.reset();
  held.reset();
  check(!weak.lock().valid(), "FFT last window retires ancestors");
  check(root.statistics().live[ResourceKind::Disk] == 0,
        "FFT last owner releases mandatory disk");
  std::cout << "passed " << h << 'x' << w
            << " packing=" << static_cast<unsigned>(packing)
            << " window=" << window << " measured_dft_error=" << maximum
            << " measured_imaginary=" << imaginary
            << " host_peak=" << root.statistics().peak[ResourceKind::Host]
            << " issued_stages=" << root.statistics().issued.stages
            << " source_reads=" << reads << '\n';
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--large") {
      for (auto packing : {SpectrumPacking::Full, SpectrumPacking::R2CHalf})
        run(1024, 1024, packing, 1024, 1, 5000000000ULL, 1ULL << 30, 1000000);
      return 0;
    }
    for (auto packing : {SpectrumPacking::Full, SpectrumPacking::R2CHalf})
      run(32, 32, packing, 1024, 1, 100000000, 1ULL << 30, 1100);
    run(1024, 1, SpectrumPacking::Full, 1024, 1, 100000000, 1ULL << 30, 5000);
    if (argc == 2 && std::string(argv[1]) == "--stage-regression")
      return 0;
    check(argc == 1,
          "usage: photospider_fft_workflow [--large|--stage-regression]");
    for (auto shape : {std::pair<std::uint64_t, std::uint64_t>{1, 1},
                       {1, 5},
                       {1, 6},
                       {3, 4},
                       {4, 5},
                       {3, 7},
                       {5, 1},
                       {2, 2},
                       {3, 12}})
      for (auto packing : {SpectrumPacking::Full, SpectrumPacking::R2CHalf})
        run(shape.first, shape.second, packing, 64);
    run(3, 4, SpectrumPacking::R2CHalf, 64, 1);
    run(3, 4, SpectrumPacking::R2CHalf, 64, 2);
    run(3, 4, SpectrumPacking::R2CHalf, 15);
    run(3, 4, SpectrumPacking::R2CHalf, 64, 0, 10);
    run(3, 4, SpectrumPacking::R2CHalf, 64, 0, 10000000, 4096);
    run(3, 4, SpectrumPacking::R2CHalf, 64, 3);
    run(3, 4, SpectrumPacking::R2CHalf, 64, 4);
    run(3, 4, SpectrumPacking::R2CHalf, 64, 5);
    run(3, 4, SpectrumPacking::R2CHalf, 64, 6);
    run(1, 8192, SpectrumPacking::R2CHalf, 256);
    run(1, 513, SpectrumPacking::R2CHalf, 64, 0, 100000000);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
