#include "photospider/plugin/fft_operation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"

namespace ps {
namespace {
using Op = FftOperation;
using Poll = Result<ResultProgramPoll>;
using Complex = std::array<double, 2>;
Status invalid(const char* message) {
  return {ErrorCode::TypeMismatch,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Group}};
}
Status overflow() {
  return {ErrorCode::OperationFailed,
          "external FFT nonfinite arithmetic",
          FailureReason::ArithmeticOverflow,
          {FailureOrigin::Domain, FailureScope::Group}};
}
bool finite(Complex value) {
  return std::isfinite(value[0]) && std::isfinite(value[1]);
}
Result<Complex> multiply(Complex a, Complex b) {
  const double ac = a[0] * b[0], bd = a[1] * b[1], ad = a[0] * b[1],
               bc = a[1] * b[0];
  Complex result{ac - bd, ad + bc};
  if (!std::isfinite(ac) || !std::isfinite(bd) || !std::isfinite(ad) ||
      !std::isfinite(bc) || !finite(result))
    return Result<Complex>(overflow());
  return Result<Complex>(result);
}
std::uint64_t modular_product(std::uint64_t a, std::uint64_t b,
                              std::uint64_t n) {
  if (!a || !b)
    return 0;
  if (a <= UINT64_MAX / b)
    return a * b % n;
  std::uint64_t result = 0;
  while (b) {
    if (b & 1)
      result = result >= n - a ? result - (n - a) : result + a;
    b >>= 1;
    a = a >= n - a ? a - (n - a) : a + a;
  }
  return result;
}
Complex twiddle(std::uint64_t index, std::uint64_t length, bool inverse) {
  constexpr double tau = 0x1.921fb54442d18p+2;
  const double angle = (inverse ? tau : -tau) * (static_cast<double>(index) /
                                                 static_cast<double>(length));
  return {std::cos(angle), std::sin(angle)};
}
std::uint64_t reverse_bits(std::uint64_t value, unsigned bits) {
  std::uint64_t result = 0;
  while (bits--) {
    result = (result << 1) | (value & 1);
    value >>= 1;
  }
  return result;
}
Status profile(const SpectrumSpec& spec) {
  if (spec.original_shape.size() != 2)
    return invalid("FFT requires original HW shape");
  auto canonical =
      fft_spectrum_spec(spec.original_shape[0], spec.original_shape[1],
                        spec.packing, spec.atol, spec.rtol);
  if (!canonical.ok())
    return canonical.status();
  auto expected = spectrum_schema(canonical.value()),
       actual = spectrum_schema(spec);
  if (!expected.ok() || !actual.ok() ||
      !expected.value().same_schema(actual.value()))
    return invalid("unsupported external FFT identity");
  return Status::success();
}
Status inputs(Op op, const SpectrumSpec& spec,
              const std::vector<OperationMetadata>& metadata) {
  if (metadata.size() != (op == Op::Multiply ? 2U : 1U))
    return invalid("FFT input count mismatch");
  const auto height = spec.original_shape[0], width = spec.original_shape[1];
  const auto columns =
      spec.packing == SpectrumPacking::Full ? width : width / 2 + 1;
  const auto schema = spectrum_schema(spec);
  for (const auto& input : metadata) {
    if (op == Op::ForwardReal || op == Op::ImportResponse) {
      auto shape = std::vector<std::uint64_t>{
          height, op == Op::ForwardReal ? width : columns};
      if (op == Op::ImportResponse)
        shape.push_back(2);
      if (input.result_schema ||
          input.descriptor.element_type != ElementType::Float64 ||
          input.descriptor.shape != shape || !input.facets.empty())
        return invalid(
            "FFT source shape/type must match complete transform identity");
    } else if (!input.result_schema || !schema.ok() ||
               !input.result_schema->same_schema(schema.value())) {
      return invalid("FFT spectrum shape/basis/numeric identity mismatch");
    }
  }
  return Status::success();
}
struct State {
  enum Stage {
    Start,
    Inputs,
    Created,
    Extended,
    Source,
    SourceReady,
    Dif,
    DifReady,
    DifWritten,
    Leaf,
    LeafReady,
    LeafWritten,
    Transpose,
    TransposeReady,
    TransposeWritten,
    Output,
    OutputReady,
    OutputWritten,
    Simple,
    SimpleReady,
    SimpleWritten,
    Residual,
    Finish
  } stage = Start;
  Op op;
  std::uint64_t h, w, k, n, stored, position = 0, batch = 0;
  std::uint64_t length = 0, span = 0, group = 0, j = 0, odd = 0, power = 0,
                term = 0;
  std::uint64_t fill = 0, slab_rows = 0;
  unsigned axis = 0, bits = 0;
  bool small = false, conjugate = false;
  Complex sum{};
  double residual = 0;
  TemporaryStorage a, b;
  MutableBuffer slab;
  std::array<ResultRef, 2> input;
  std::array<ResultDescriptor, 2> descriptors;
  ResultBuilder builder;
  State(Op operation, std::uint64_t height, std::uint64_t width,
        SpectrumPacking packing)
      : op(operation),
        h(height),
        w(width),
        k(packing == SpectrumPacking::Full ? width : width / 2 + 1),
        n(height * width),
        stored(height * k) {}
  bool inverse() const { return op == Op::InverseReal; }
  std::uint64_t window(const ResultProgramPhase& p) const {
    return std::min<std::uint64_t>(1024, p.query.page_bytes) / 16;
  }
  Complex read_pair(const ResultProgramPhase& p, unsigned reply,
                    std::uint64_t index = 0) {
    Complex value;
    std::memcpy(value.data(),
                std::get<std::shared_ptr<const CpuStorage>>(p.io.at(reply))
                        ->bytes()
                        .data() +
                    index * 16,
                16);
    return value;
  }
  Result<ResultRelation> relation(const ResultProgramPhase& p,
                                  std::uint64_t outputs) {
    std::array<ResultRelation, 4> parts;
    unsigned used = 0;
    for (unsigned i = 0; i < p.query.inputs.size(); ++i) {
      auto made =
          ResultRelation::cartesian(p.resources, outputs,
                                    {i, input[i].valid() ? 7U : 15U, 0,
                                     op == Op::ForwardReal ? n : stored * 2},
                                    DependencyGuarantee::Conservative);
      if (!made.ok())
        return made;
      parts[used++] = made.take_value();
      if (input[i].valid()) {
        made = ResultRelation::cartesian(p.resources, outputs, {i, 8, 0, 1},
                                         DependencyGuarantee::Conservative);
        if (!made.ok())
          return made;
        parts[used++] = made.take_value();
      }
    }
    auto capacity = p.resources.reserve(ResourceCapacity::host(
        used * sizeof(ResultRelation), used * sizeof(ResultRelation)));
    if (!capacity.ok())
      return Result<ResultRelation>(capacity.status());
    return ResultRelation::unite(
        p.resources,
        std::vector<ResultRelation>(parts.begin(), parts.begin() + used));
  }
  Status begin(const ResultProgramPhase& p) {
    auto capacity = p.resources.reserve(ResourceCapacity::host(16, 16));
    if (!capacity.ok())
      return capacity.status();
    std::vector<std::uint64_t> association;
    association.reserve(2);
    for (const auto& value : input)
      if (value.valid())
        association.push_back(value.object_id());
    auto made = ResultBuilder::start(p.resources, *p.query.output.result_schema,
                                     p.query.semantic_key, {n, n * 16},
                                     std::move(association));
    if (!made.ok())
      return made.status();
    builder = made.take_value();
    auto support = relation(p, 1);
    return support.ok() ? builder.bind_descriptor_relation(support.take_value())
                        : support.status();
  }
  void begin_axis() {
    length = axis ? h : w;
    span = length;
    group = 0;
    j = 0;
    position = 0;
    odd = length;
    bits = 0;
    while (odd % 2 == 0) {
      odd /= 2;
      ++bits;
    }
    power = length / odd;
    term = 0;
    fill = 0;
    sum = {};
    stage = Dif;
  }
  Poll finish(const ResultProgramPhase& p) {
    for (unsigned field = 0; field < builder.reference().schema().fields.size();
         ++field) {
      const auto rows = field ? 1 : inverse() ? n : stored;
      auto support = relation(p, rows);
      if (!support.ok())
        return Poll(support.status());
      auto status = builder.publish(field, rows, support.take_value(),
                                    {true, true, true, true});
      if (!status.ok())
        return Poll(status);
    }
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
  Poll source_need(const ResultProgramPhase& p, bool response) {
    const auto columns = response ? k : w;
    batch = std::min({n - position, columns - position % columns, window(p)});
    if (response)
      batch = std::min(batch, stored - position);
    auto dimensions = std::vector<RegionDimension>{{position / columns, 1},
                                                   {position % columns, batch}};
    if (response)
      dimensions.push_back({0, 2});
    auto requested = Footprint::from_regions(p.query.inputs[0].descriptor.shape,
                                             {Region(std::move(dimensions))});
    if (!requested.ok())
      return Poll(requested.status());
    stage = response ? SimpleReady : SourceReady;
    return Poll(ResultProgramNeed{{{0, requested.take_value()}}, {}, {}});
  }
  Result<MutableBuffer> values(const ResultProgramPhase& p, bool response) {
    auto memory = p.allocator.allocate(batch * 16);
    if (!memory.ok())
      return memory;
    auto result = memory.take_value();
    auto fuel = p.consume_work(batch * 2);
    if (!fuel.ok())
      return Result<MutableBuffer>(fuel);
    const auto columns = response ? k : w;
    for (std::uint64_t i = 0; i < batch; ++i) {
      Complex value{};
      for (unsigned c = 0; c < (response ? 2U : 1U); ++c) {
        auto coordinate = std::vector<std::uint64_t>{position / columns,
                                                     position % columns + i};
        if (response)
          coordinate.push_back(c);
        auto status = p.read(0, coordinate, &value[c], 8);
        if (!status.ok())
          return Result<MutableBuffer>(status);
      }
      if (!finite(value))
        return Result<MutableBuffer>(
            Status{ErrorCode::OperationFailed,
                   "nonfinite FFT source sample",
                   FailureReason::InvalidDomain,
                   {FailureOrigin::Domain, FailureScope::Group}});
      std::memcpy(result.data() + i * 16, value.data(), 16);
    }
    return Result<MutableBuffer>(std::move(result));
  }
  Status allocate_slab(const ResultProgramPhase& p, std::uint64_t remaining,
                       unsigned bytes = 16) {
    slab_rows = std::min(remaining, window(p));
    fill = 0;
    auto made = p.allocator.allocate(slab_rows * bytes);
    if (!made.ok())
      return made.status();
    slab = made.take_value();
    return Status::success();
  }
  Poll poll(const ResultProgramPhase& p) {
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Poll(overflow());
    if (!window(p))
      return Poll(Status{ErrorCode::ResourceExhausted,
                         "FFT complex record exceeds page window"});
    for (;;) {
      switch (stage) {
        case Start: {
          stage = Inputs;
          if (op == Op::Multiply || inverse()) {
            ResultProgramNeed need;
            for (unsigned i = 0; i < p.query.inputs.size(); ++i)
              need.results.push_back({i, 0, true, 0});
            return Poll(std::move(need));
          }
          break;
        }
        case Inputs: {
          for (const auto& item : p.results) {
            input[item.first] = item.second;
            auto descriptor = item.second.descriptor();
            if (!descriptor.ok())
              return Poll(descriptor.status());
            descriptors[item.first] = descriptor.take_value();
          }
          auto status = begin(p);
          if (!status.ok())
            return Poll(status);
          if (op == Op::ImportResponse || op == Op::Multiply) {
            stage = Simple;
            break;
          }
          status = p.consume_work(n * 2);
          if (!status.ok())
            return Poll(status);
          stage = Created;
          return Poll(ResultProgramNeed{
              {},
              {},
              {ResultCreateTemporary{}, ResultCreateTemporary{}}});
        }
        case Created:
          a = std::get<TemporaryStorage>(p.io.at(0));
          b = std::get<TemporaryStorage>(p.io.at(1));
          stage = Extended;
          return Poll(ResultProgramNeed{{},
                                        {},
                                        {ResultExtendTemporary{a, n * 16},
                                         ResultExtendTemporary{b, n * 16}}});
        case Extended:
          stage = Source;
          break;
        case Source: {
          if (position == n) {
            begin_axis();
            break;
          }
          if (!inverse())
            return source_need(p, false);
          const auto u = position / w, v = position % w;
          conjugate = v >= k;
          batch = std::min(
              {n - position, w - v, window(p), conjugate ? w - v : k - v});
          const auto row =
              conjugate ? ((h - u) % h) * k + (w - v - batch + 1) : u * k + v;
          auto read = input[0].prepare_read(descriptors[0], 0, row, batch);
          if (!read.ok())
            return Poll(read.status());
          stage = SourceReady;
          return Poll(ResultProgramNeed{{}, {}, {read.take_value()}});
        }
        case SourceReady: {
          Result<MutableBuffer> memory(Status{ErrorCode::Internal, {}});
          if (!inverse()) {
            memory = values(p, false);
          } else {
            memory = p.allocator.allocate(batch * 16);
            if (memory.ok()) {
              auto buffer = memory.take_value();
              auto fuel = p.consume_work(batch);
              if (!fuel.ok())
                return Poll(fuel);
              for (std::uint64_t i = 0; i < batch; ++i) {
                auto value = read_pair(p, 0, conjugate ? batch - i - 1 : i);
                if (conjugate)
                  value[1] = -value[1];
                if (!finite(value))
                  return Poll(overflow());
                std::memcpy(buffer.data() + i * 16, value.data(), 16);
              }
              memory = Result<MutableBuffer>(std::move(buffer));
            }
          }
          if (!memory.ok())
            return Poll(memory.status());
          auto buffer = memory.take_value();
          const auto offset = position * 16;
          position += batch;
          stage = Source;
          return Poll(ResultProgramNeed{
              {},
              {},
              {ResultWriteTemporary{a, offset, std::move(buffer).freeze()}}});
        }
        case Dif: {
          if (span == odd) {
            position = 0;
            term = 0;
            fill = 0;
            sum = {};
            stage = Leaf;
            break;
          }
          small = span <= window(p);
          ResultProgramNeed need;
          if (small) {
            batch = std::min(n - group, (window(p) / span) * span);
            need.io.push_back(ResultReadTemporary{a, group * 16, batch * 16});
          } else {
            batch = std::min(span / 2 - j, window(p));
            need.io.push_back(
                ResultReadTemporary{a, (group + j) * 16, batch * 16});
            need.io.push_back(ResultReadTemporary{
                a, (group + j + span / 2) * 16, batch * 16});
          }
          stage = DifReady;
          return Poll(std::move(need));
        }
        case DifReady: {
          auto first = p.allocator.allocate(batch * 16);
          if (!first.ok())
            return Poll(first.status());
          auto left = first.take_value();
          MutableBuffer right;
          if (!small) {
            auto second = p.allocator.allocate(batch * 16);
            if (!second.ok())
              return Poll(second.status());
            right = second.take_value();
          }
          auto fuel = p.consume_work(batch * 16);
          if (!fuel.ok())
            return Poll(fuel);
          for (std::uint64_t index = 0; index < (small ? batch / 2 : batch);
               ++index) {
            const auto local = small ? index % (span / 2) : j + index;
            const auto x = small ? (index / (span / 2)) * span + local : index;
            const auto y = x + span / 2;
            const auto u = read_pair(p, 0, x),
                       v = read_pair(p, small ? 0 : 1, small ? y : index);
            const Complex add{u[0] + v[0], u[1] + v[1]},
                difference{u[0] - v[0], u[1] - v[1]};
            if (!finite(add) || !finite(difference))
              return Poll(overflow());
            auto product =
                multiply(difference, twiddle(local, span, inverse()));
            if (!product.ok())
              return Poll(product.status());
            std::memcpy(left.data() + x * 16, add.data(), 16);
            std::memcpy(
                small ? left.data() + y * 16 : right.data() + index * 16,
                product.value().data(), 16);
          }
          ResultProgramNeed need;
          need.io.push_back(ResultWriteTemporary{
              a, (group + (small ? 0 : j)) * 16, std::move(left).freeze()});
          if (!small)
            need.io.push_back(ResultWriteTemporary{
                a, (group + j + span / 2) * 16, std::move(right).freeze()});
          stage = DifWritten;
          return Poll(std::move(need));
        }
        case DifWritten:
          if (small) {
            group += batch;
          } else {
            j += batch;
            if (j == span / 2) {
              j = 0;
              group += span;
            }
          }
          if (group == n) {
            group = 0;
            span /= 2;
          }
          stage = Dif;
          break;
        case Leaf: {
          if (position == n) {
            if (axis == 0 && h > 1) {
              position = 0;
              stage = Transpose;
              break;
            }
            position = 0;
            stage = Output;
            break;
          }
          if (!slab.size()) {
            auto status = allocate_slab(p, n - position);
            if (!status.ok())
              return Poll(status);
          }
          const auto frequency = position % length;
          const auto leaf = reverse_bits(frequency % power, bits);
          const auto offset = (position / length) * length + leaf * odd + term;
          batch = std::min(odd - term, window(p));
          stage = LeafReady;
          return Poll(ResultProgramNeed{
              {},
              {},
              {ResultReadTemporary{a, offset * 16, batch * 16}}});
        }
        case LeafReady: {
          auto fuel = p.consume_work(batch * 80);
          if (!fuel.ok())
            return Poll(fuel);
          const auto frequency = (position % length) / power;
          for (std::uint64_t t = 0; t < batch; ++t) {
            auto value = read_pair(p, 0, t);
            if (odd != 1) {
              auto product = multiply(
                  value, twiddle(modular_product(frequency, term + t, odd), odd,
                                 inverse()));
              if (!product.ok())
                return Poll(product.status());
              value = product.take_value();
            }
            sum = {sum[0] + value[0], sum[1] + value[1]};
            if (!finite(sum))
              return Poll(overflow());
          }
          term += batch;
          if (term < odd) {
            stage = Leaf;
            break;
          }
          std::memcpy(slab.data() + fill * 16, sum.data(), 16);
          ++fill;
          ++position;
          term = 0;
          sum = {};
          if (fill < slab_rows) {
            stage = Leaf;
            break;
          }
          const auto offset = (position - fill) * 16;
          fill = 0;
          stage = LeafWritten;
          return Poll(ResultProgramNeed{
              {},
              {},
              {ResultWriteTemporary{b, offset, std::move(slab).freeze()}}});
        }
        case LeafWritten:
          stage = Leaf;
          break;
        case Transpose: {
          if (position == n) {
            axis = 1;
            begin_axis();
            break;
          }
          batch = std::min({n - position, window(p), std::uint64_t{8}});
          ResultProgramNeed need;
          for (std::uint64_t i = 0; i < batch; ++i) {
            const auto q = position + i;
            need.io.push_back(
                ResultReadTemporary{b, ((q % h) * w + q / h) * 16, 16});
          }
          stage = TransposeReady;
          return Poll(std::move(need));
        }
        case TransposeReady: {
          auto memory = p.allocator.allocate(batch * 16);
          if (!memory.ok())
            return Poll(memory.status());
          auto buffer = memory.take_value();
          auto fuel = p.consume_work(batch);
          if (!fuel.ok())
            return Poll(fuel);
          for (std::uint64_t i = 0; i < batch; ++i) {
            const auto value = read_pair(p, static_cast<unsigned>(i));
            std::memcpy(buffer.data() + i * 16, value.data(), 16);
          }
          const auto offset = position * 16;
          position += batch;
          stage = TransposeWritten;
          return Poll(ResultProgramNeed{
              {},
              {},
              {ResultWriteTemporary{a, offset, std::move(buffer).freeze()}}});
        }
        case TransposeWritten:
          stage = Transpose;
          break;
        case Output: {
          const auto count = inverse() ? n : stored;
          if (position == count) {
            a = {};
            b = {};
            stage = inverse() ? Residual : Finish;
            break;
          }
          if (!slab.size()) {
            auto status =
                allocate_slab(p, count - position, inverse() ? 8 : 16);
            if (!status.ok())
              return Poll(status);
          }
          batch = std::min({slab_rows - fill, std::uint64_t{8}, window(p)});
          ResultProgramNeed need;
          const auto columns = inverse() ? w : k;
          for (std::uint64_t i = 0; i < batch; ++i) {
            const auto q = position + i, u = q / columns, v = q % columns;
            need.io.push_back(
                ResultReadTemporary{b, (h == 1 ? v : v * h + u) * 16, 16});
          }
          stage = OutputReady;
          return Poll(std::move(need));
        }
        case OutputReady: {
          auto fuel = p.consume_work(batch * 2);
          if (!fuel.ok())
            return Poll(fuel);
          for (std::uint64_t i = 0; i < batch; ++i) {
            auto value = read_pair(p, static_cast<unsigned>(i));
            if (inverse()) {
              value = {value[0] / static_cast<double>(n),
                       value[1] / static_cast<double>(n)};
              residual = std::max(residual, std::abs(value[1]));
            }
            if (!finite(value))
              return Poll(overflow());
            std::memcpy(slab.data() + (fill + i) * (inverse() ? 8 : 16),
                        value.data(), inverse() ? 8 : 16);
          }
          fill += batch;
          position += batch;
          if (fill < slab_rows) {
            stage = Output;
            break;
          }
          auto write =
              builder.prepare_append(0, fill, std::move(slab).freeze());
          if (!write.ok())
            return Poll(write.status());
          fill = 0;
          stage = OutputWritten;
          return Poll(ResultProgramNeed{{}, {}, {write.take_value()}});
        }
        case OutputWritten:
          stage = Output;
          break;
        case Simple: {
          if (position == stored) {
            stage = Finish;
            break;
          }
          if (op == Op::ImportResponse)
            return source_need(p, true);
          batch = std::min(stored - position, window(p));
          ResultProgramNeed need;
          for (unsigned i = 0; i < 2; ++i) {
            auto read =
                input[i].prepare_read(descriptors[i], 0, position, batch);
            if (!read.ok())
              return Poll(read.status());
            need.io.push_back(read.take_value());
          }
          stage = SimpleReady;
          return Poll(std::move(need));
        }
        case SimpleReady: {
          auto memory = op == Op::ImportResponse
                            ? values(p, true)
                            : p.allocator.allocate(batch * 16);
          if (!memory.ok())
            return Poll(memory.status());
          auto buffer = memory.take_value();
          if (op == Op::Multiply) {
            auto fuel = p.consume_work(batch * 8);
            if (!fuel.ok())
              return Poll(fuel);
            for (std::uint64_t i = 0; i < batch; ++i) {
              auto product = multiply(read_pair(p, 0, i), read_pair(p, 1, i));
              if (!product.ok())
                return Poll(product.status());
              std::memcpy(buffer.data() + i * 16, product.value().data(), 16);
            }
          }
          auto write =
              builder.prepare_append(0, batch, std::move(buffer).freeze());
          if (!write.ok())
            return Poll(write.status());
          position += batch;
          stage = SimpleWritten;
          return Poll(ResultProgramNeed{{}, {}, {write.take_value()}});
        }
        case SimpleWritten:
          stage = Simple;
          break;
        case Residual: {
          auto memory = p.allocator.allocate(8);
          if (!memory.ok())
            return Poll(memory.status());
          auto buffer = memory.take_value();
          std::memcpy(buffer.data(), &residual, 8);
          auto write = builder.prepare_append(1, 1, std::move(buffer).freeze());
          if (!write.ok())
            return Poll(write.status());
          stage = Finish;
          return Poll(ResultProgramNeed{{}, {}, {write.take_value()}});
        }
        case Finish:
          return finish(p);
      }
    }
  }
};
}  // namespace
Result<SpectrumSpec> fft_spectrum_spec(std::uint64_t height,
                                       std::uint64_t width,
                                       SpectrumPacking packing, double atol,
                                       double rtol) {
  constexpr auto maximum = (static_cast<std::uint64_t>(INT64_MAX) - 4095) / 16;
  if (!height || !width || height > maximum / width ||
      (packing != SpectrumPacking::Full &&
       packing != SpectrumPacking::R2CHalf) ||
      !std::isfinite(atol) || !std::isfinite(rtol) || atol < 0 || rtol < 0)
    return Result<SpectrumSpec>(invalid("invalid FFT shape/packing/tolerance"));
  SpectrumSpec spec;
  spec.original_shape = {height, width};
  spec.transformed_axes = {0, 1};
  spec.axis_order = {0, 1};
  spec.shifts = {0, 0};
  spec.sample_origin = {0, 0};
  spec.sample_step = {1, 1};
  spec.packing = packing;
  spec.packed_axis = 1;
  spec.sign = -1;
  spec.normalization = SpectrumNormalization::Unscaled;
  spec.real_policy = SpectrumRealPolicy::RealProjectionMeasured;
  spec.atol = atol;
  spec.rtol = rtol;
  spec.unit = "sample";
  return Result<SpectrumSpec>(std::move(spec));
}
Result<SchemaTemplate> fft_spatial_schema(const SpectrumSpec& spectrum) {
  auto checked = profile(spectrum);
  if (!checked.ok())
    return Result<SchemaTemplate>(checked);
  auto result = spectrum_schema(spectrum);
  if (!result.ok())
    return result;
  auto schema = result.take_value();
  schema.id = "photospider.fft_real_output";
  schema.metadata[0].key = "fft_inverse_identity_v1";
  schema.fields = {{"pixels",
                    ElementType::Float64,
                    {ResultExtentKind::Fixed,
                     spectrum.original_shape[0] * spectrum.original_shape[1]},
                    {}},
                   {"imaginary_residual", ElementType::Float64, {}, {}}};
  checked = schema.validate(true);
  return checked.ok() ? Result<SchemaTemplate>(std::move(schema))
                      : Result<SchemaTemplate>(checked);
}
Result<OperationDefinition> make_fft_operation(Op operation,
                                               const SpectrumSpec& spectrum) {
  const auto number = static_cast<unsigned>(operation);
  auto checked = profile(spectrum);
  if (!checked.ok())
    return Result<OperationDefinition>(checked);
  if (!number || number > 4)
    return Result<OperationDefinition>(invalid("unknown FFT operation"));
  OperationDefinition definition;
  const char* names[] = {"", "fft.forward_real", "fft.import_response",
                         "fft.multiply", "fft.inverse_real"};
  definition.key = names[number];
  auto& traits = definition.traits;
  traits.input_count = operation == Op::Multiply ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  traits.workspace_bytes = 4096;
  auto& out = traits.outputs[0];
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 2;
  out.continuation_bytes = sizeof(State);
  out.maximum_dependency_stages = 1000000;
  auto schema = operation == Op::InverseReal ? fft_spatial_schema(spectrum)
                                             : spectrum_schema(spectrum);
  if (!schema.ok())
    return Result<OperationDefinition>(schema.status());
  out.result_schema = schema.take_value();
  out.output_schema.kind = OperationPortKind::Result;
  out.output_schema.result_schema_id = std::string(out.result_schema->id);
  out.output_schema.result_schema_version = 1;
  if (operation == Op::Multiply || operation == Op::InverseReal) {
    for (auto& port : traits.input_schema) {
      port.kind = OperationPortKind::Result;
      port.result_schema_id = "photospider.spectrum";
      port.result_schema_version = 1;
    }
  }
  definition.validate_dependency = [operation, spectrum](const auto& metadata,
                                                         const auto&) {
    return inputs(operation, spectrum, metadata);
  };
  definition.start_result = [operation, spectrum](
                                const ResultProgramQuery& query,
                                const BufferAllocator& allocator) {
    auto status = inputs(operation, spectrum, query.inputs);
    if (!status.ok())
      return Result<ResultContinuation>(status);
    return ResultContinuation::make<State>(
        allocator, operation, spectrum.original_shape[0],
        spectrum.original_shape[1], spectrum.packing);
  };
  return Result<OperationDefinition>(std::move(definition));
}
}  // namespace ps
