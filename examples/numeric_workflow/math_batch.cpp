// Manual NUM batch equivalence, fenv/work checks and callback timings.
#include <algorithm>
#include <array>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/numeric_math_operation.hpp"

namespace {
namespace n = ps::plugin_internal::numeric_ops;
template <class T>
T take(ps::Result<T> r) {
  if (!r.ok())
    throw std::runtime_error(r.status().message);
  return r.take_value();
}
void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
ps::Value input(const std::vector<std::uint64_t>& bits, bool narrow,
                unsigned layout) {
  const unsigned width = narrow ? 4 : 8;
  auto buffer = take(ps::BufferAllocator{}.allocate(bits.size() * width + 1));
  for (std::size_t i = 0; i < bits.size(); ++i)
    std::memcpy(
        buffer.data() + 1 + (layout == 1 ? bits.size() - 1 - i : i) * width,
        &bits[i], width);
  return take(ps::Value::from_storage(
      {narrow ? ps::ElementType::Float32 : ps::ElementType::Float64,
       {bits.size()}},
      ps::Region::whole({bits.size()}),
      {layout == 1 ? 1 + (bits.size() - 1) * width : 1,
       {layout == 1   ? -static_cast<std::int64_t>(width)
        : layout == 2 ? 0
                      : static_cast<std::int64_t>(width)}},
      std::move(buffer).freeze()));
}
ps::Value matrix_input(const std::vector<std::uint64_t>& bits, bool narrow,
                       bool transposed) {
  const unsigned width = narrow ? 4 : 8;
  auto buffer = take(ps::BufferAllocator{}.allocate(64 * width + 1));
  for (unsigned i = 0; i < 64; ++i) {
    const unsigned offset = transposed ? (i % 16) * 4 + i / 16 : i;
    std::memcpy(buffer.data() + 1 + offset * width, &bits[i], width);
  }
  return take(ps::Value::from_storage(
      {narrow ? ps::ElementType::Float32 : ps::ElementType::Float64, {4, 16}},
      ps::Region::whole({4, 16}),
      {1,
       {static_cast<std::int64_t>((transposed ? 1 : 16) * width),
        static_cast<std::int64_t>((transposed ? 4 : 1) * width)}},
      std::move(buffer).freeze()));
}
#if defined(__aarch64__)
constexpr auto kProfile = n::SequenceProfile::AppleSilicon;
#else
constexpr auto kProfile = n::SequenceProfile::X86Avx2;
#endif
ps::Result<ps::Value> batch(
    n::CertifiedKind kind, const std::vector<ps::Value>& inputs,
    ps::BufferAllocator allocator = ps::BufferAllocator()) {
  std::vector<ps::Region> regions(inputs.size(), inputs[0].region());
  std::map<std::string, ps::ParameterValue> parameters;
  ps::OperationInvocation call(inputs, regions, parameters, ps::Backend::Cpu,
                               {}, inputs[0].region(), allocator);
  return n::execute_point_math<n::CertifiedMath>(call, kind, kProfile, false);
}
std::vector<std::uint64_t> words(const ps::Value& value) {
  const auto width = ps::Value::element_size(value.descriptor().element_type);
  std::vector<std::uint64_t> out(value.region().element_count().value());
  for (std::size_t i = 0; i < out.size(); ++i)
    std::memcpy(&out[i], value.bytes().data() + i * width, width);
  return out;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const bool timing = argc > 1 && std::string(argv[1]) == "time";
    const unsigned size =
        timing ? (argc > 2 ? std::stoul(argv[2]) : 262144) : 257;
    require(size > 0, "nonempty workload");
    std::mt19937 rng(405);
    std::cout << "kind,dtype,N,scalar_math_us,batch_callback_us\n";
    for (unsigned kind : {0U, 1U, 2U, 3U, 4U, 10U, 11U}) {
      for (bool narrow : {true, false}) {
        const auto dtype =
            narrow ? ps::ElementType::Float32 : ps::ElementType::Float64;
        const bool binary = kind >= 10;
        std::vector<std::uint64_t> a(size), b(size), expected(size);
        for (unsigned i = 0; i < size; ++i) {
          double x = .125 + .75 * static_cast<double>(rng()) / UINT32_MAX;
          a[i] = n::numeric_bits(x, narrow);
          b[i] = n::numeric_bits(.3125, narrow);
        }
        if (!timing) {
          const auto inf =
              narrow ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
          const std::array<std::uint64_t, 8> special{
              0,
              UINT64_C(1) << (narrow ? 31 : 63),
              1,
              inf,
              inf | 42,
              n::numeric_bits(-1, narrow),
              n::numeric_bits(1, narrow),
              n::numeric_bits(2, narrow)};
          for (unsigned i = 0; i < special.size(); ++i) {
            a[i] = special[i];
            if (binary)
              b[i + special.size()] = special[i];
          }
          if (kind == 0) {
            for (unsigned i = 0; i < 2; ++i) {
              const auto edge = n::numeric_bits(i ? 80 : -80, narrow);
              a[16 + 3 * i] = edge - 1;
              a[17 + 3 * i] = edge;
              a[18 + 3 * i] = edge + 1;
            }
            a[22] = n::numeric_bits(-90, narrow);
            a[23] = n::numeric_bits(100, narrow);
            a[24] = n::numeric_bits(1 + 0x1p-30, narrow);
            a[25] = n::numeric_bits(1, narrow);
          }
        }
        n::CertifiedMath scalar(kProfile);
        std::uint64_t work = 2 + size * (binary ? 3 : 2);
        std::vector<double> scalar_times;
        for (unsigned repeat = 0; repeat < (timing ? 8U : 1U); ++repeat) {
          work = 2 + size * (binary ? 3 : 2);
          const auto start = std::chrono::steady_clock::now();
          for (unsigned i = 0; i < size; ++i)
            expected[i] = take(scalar.evaluate(
                static_cast<n::CertifiedKind>(kind), dtype, a[i], b[i],
                [&](std::uint64_t count) {
                  work += count;
                  return ps::Status::success();
                },
                [] { return ps::Status::success(); }));
          const auto elapsed = std::chrono::duration<double, std::micro>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
          if (repeat)
            scalar_times.push_back(elapsed);
        }
        if (!timing && kind == 0 && !narrow)
          require(expected[24] != expected[25],
                  "exp retains binary64 input precision");
        std::sort(scalar_times.begin(), scalar_times.end());
        std::vector<double> times;
        for (unsigned layout = 0; layout < (timing ? 1U : 3U); ++layout) {
          std::vector<ps::Value> inputs{input(a, narrow, layout)};
          if (binary)
            inputs.push_back(input(b, narrow, layout));
          for (int mode :
               {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
            if (timing && mode != FE_TONEAREST)
              continue;
            std::fesetround(mode);
            std::feclearexcept(FE_ALL_EXCEPT);
            std::feraiseexcept(FE_DIVBYZERO);
            const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
            for (unsigned repeat = 0; repeat < (timing ? 8U : 1U); ++repeat) {
              std::feclearexcept(FE_ALL_EXCEPT);
              std::feraiseexcept(FE_DIVBYZERO);
              auto before = std::chrono::steady_clock::now();
              auto output =
                  take(batch(static_cast<n::CertifiedKind>(kind), inputs));
              require(std::fegetround() == mode &&
                          std::fetestexcept(FE_ALL_EXCEPT) == flags,
                      "fenv restoration");
              const auto duration =
                  std::chrono::duration<double, std::micro>(
                      std::chrono::steady_clock::now() - before)
                      .count();
              auto actual = words(output);
              for (unsigned i = 0; i < size; ++i)
                require(actual[i] == expected[layout == 2 ? 0 : i],
                        "batch/scalar identity including special values");
              if (repeat)
                times.push_back(duration);
            }
          }
        }
        std::fesetround(FE_TONEAREST);
        std::feclearexcept(FE_ALL_EXCEPT);
        if (!timing) {
          // Mixed packed/transposed binary ports still need shared coordinates;
          // all-packed multidimensional inputs may skip coordinate increments.
          for (bool transposed : {false, true}) {
            std::vector<ps::Value> inputs{matrix_input(a, narrow, transposed)};
            if (binary)
              inputs.push_back(matrix_input(b, narrow, !transposed));
            const auto actual =
                words(take(batch(static_cast<n::CertifiedKind>(kind), inputs)));
            for (unsigned i = 0; i < 64; ++i)
              require(actual[i] == expected[i], "matrix layout identity");
          }
          for (unsigned chunk : {1U, 3U, 7U, 64U, 65U}) {
            for (unsigned offset = 0; offset < size; offset += chunk) {
              const auto end = std::min(size, offset + chunk);
              std::vector<ps::Value> inputs{
                  input({a.begin() + offset, a.begin() + end}, narrow, 0)};
              if (binary)
                inputs.push_back(
                    input({b.begin() + offset, b.begin() + end}, narrow, 0));
              auto actual = words(
                  take(batch(static_cast<n::CertifiedKind>(kind), inputs)));
              for (unsigned i = offset; i < end; ++i)
                require(actual[i - offset] == expected[i],
                        "partition identity");
            }
          }
          std::vector<ps::Value> inputs{input(a, narrow, 0)};
          if (binary)
            inputs.push_back(input(b, narrow, 0));
          const auto payload = size * (narrow ? 4 : 8) +
                               sizeof(n::CertifiedMath) +
                               sizeof(n::MathBatchWorkspace);
          for (unsigned scenario = 0; scenario < 3; ++scenario) {
            ps::ResourceLimits limits;
            limits.maximum_work = work - (scenario == 1);
            if (scenario == 2)
              limits.capacity[ps::ResourceKind::Payload] = payload - 1;
            ps::ResourceBudget budget(limits);
            {
              ps::ResourceAllocationScope scope(budget);
              const auto result = batch(static_cast<n::CertifiedKind>(kind),
                                        inputs, budget.allocator());
              require(scenario == 0 ? result.ok()
                                    : !result.ok() &&
                                          result.status().code ==
                                              ps::ErrorCode::ResourceExhausted,
                      "work/capacity threshold");
            }
            if (scenario != 2)
              require(budget.statistics().peak[ps::ResourceKind::Payload] ==
                          payload,
                      "batch allocation is charged");
            require(budget.statistics().live[ps::ResourceKind::Payload] == 0,
                    "released batch resources");
          }
        }
        std::sort(times.begin(), times.end());
        std::cout << kind << ',' << (narrow ? 32 : 64) << ',' << size << ','
                  << (scalar_times.empty()
                          ? 0
                          : scalar_times[scalar_times.size() / 2])
                  << ',' << (times.empty() ? 0 : times[times.size() / 2])
                  << '\n';
      }
    }
    std::cout << "PASS batch/scalar, layouts, fenv and work checks\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
