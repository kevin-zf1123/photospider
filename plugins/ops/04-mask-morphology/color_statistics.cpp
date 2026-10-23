#include <algorithm>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

#include "04-mask-morphology/color_math.hpp"
#include "04-mask-morphology/exact_sum.hpp"
#include "common/registry.hpp"
#include "photospider/ops/mask.hpp"

namespace ps::plugin_internal::mask {
namespace {
struct Observation final {
  std::uint64_t id, index;
};
using Binary = numeric_ops::BinaryParts;

// All products have at most three finite binary operands. Including 2^40
// observations, 1024/6656 bits cover the entire Float32/64 exponent domain.
template <unsigned Words>
struct Moment final {
  static constexpr int scale = Words == 16 ? -447 : -3222;
  WeightedSum<Words> value;
  void add(Binary a, Binary b, Binary c, unsigned degree) {
    std::array<std::uint64_t, 3> coefficient{1, 0, 0};
    const std::array<Binary, 3> operands{a, b, c};
    int exponent = 0;
    bool negative = false;
    for (unsigned i = 0; i < degree; ++i) {
      exponent += operands[i].exponent;
      negative ^= operands[i].negative;
      unsigned __int128 carry = 0;
      for (auto& limb : coefficient) {
        const auto v =
            static_cast<unsigned __int128>(limb) * operands[i].significand +
            carry;
        limb = static_cast<std::uint64_t>(v);
        carry = v >> 64;
      }
    }
    typename WeightedSum<Words>::Integer term;
    const auto shift = static_cast<unsigned>(exponent - scale);
    const auto first = shift / 64, tail = shift % 64;
    for (unsigned j = 0; j < 3; ++j) {
      if (first + j < Words)
        term.words[first + j] |= coefficient[j] << tail;
      if (tail && first + j + 1 < Words)
        term.words[first + j + 1] |= coefficient[j] >> (64 - tail);
    }
    value.signed_add(term, negative);
  }
  Rational rational(const Work& work) const {
    work.poll(Words);
    Rational r;
    unsigned trailing = 0;
    for (auto word : value.numerator.words) {
      if (word) {
        trailing += static_cast<unsigned>(__builtin_ctzll(word));
        break;
      }
      trailing += 64;
    }
    const auto remove = std::min(trailing, static_cast<unsigned>(-scale));
    const auto whole = remove / 64, tail = remove % 64;
    for (unsigned j = 0; j + whole < Words; ++j) {
      r.n.words[j] = value.numerator.words[j + whole] >> tail;
      if (tail && j + whole + 1 < Words)
        r.n.words[j] |= value.numerator.words[j + whole + 1] << (64 - tail);
    }
    r.d.words.fill(0);
    const auto power = static_cast<unsigned>(-scale) - remove;
    r.d.words[power / 64] = UINT64_C(1) << (power % 64);
    r.negative = value.negative;
    return r;
  }
};
template <unsigned Words>
struct FitScratch final {
  Math math;
  Rational weight;
  std::array<Rational, 4> mean, pivot, epsilon;
  std::array<Rational, 16> covariance, lower;
  std::array<Moment<Words>, 21> moments;
  explicit FitScratch(const Work& work) : math(work) {}
};
struct Writer final {
  const Kernel& kernel;
  const ps_kernel_invocation_v8& invocation;
  std::array<unsigned, 6> begin{}, end{}, current{};
  explicit Writer(const Kernel& k, const ps_kernel_invocation_v8& inv)
      : kernel(k), invocation(inv) {
    begin.fill(static_cast<unsigned>(k.writes.size()));
    for (unsigned i = 0; i < k.writes.size(); ++i) {
      const auto id =
          k.writes[i].first ? 4 + k.writes[i].second : k.writes[i].second;
      begin[id] = std::min(begin[id], i);
      end[id] = i + 1;
    }
    current = begin;
  }
  void put(unsigned field, Coordinate at, std::uint64_t bits) {
    const auto contains = [&](unsigned slot) {
      const auto& l = *invocation.outputs[slot].layout;
      for (unsigned a = 0; a < l.rank; ++a)
        if (at[a] < l.origin[a] || at[a] - l.origin[a] >= l.shape[a])
          return false;
      return true;
    };
    // Each member is written in increasing model-row order; coordinate axes
    // are atomic, so a retired row tile is never revisited.
    while (current[field] < end[field] && !contains(current[field]))
      ++current[field];
    require(current[field] < end[field], "mask fitting output binding");
    store(invocation.outputs[current[field]], at, bits);
  }
};
struct Fit final : Program {
  bool planar;
  ElementType type;
  std::vector<std::uint64_t> selected;
  std::uint64_t count, width = 0;
  Fit(bool image_form, const std::vector<OperationMetadata>& in,
      const Parameters& p)
      : planar(image_form) {
    const auto& s = image(in[0], planar ? 3 : 2);
    const auto dims = s.sample_shape();
    type = s.descriptor.element_type;
    if (planar) {
      selected = channels(text(p, "channels"), dims[0]);
      count = dims[1] * dims[2];
      width = dims[2];
      shape(in[1], {dims[1], dims[2]}, false);
      shape(in[2], {dims[1], dims[2]});
      spatial_axes(tensor(in[1]), 2);
      spatial_axes(tensor(in[2]), 2);
    } else {
      require(dims[1] <= 4, "mask fitting requires one complete color group");
      selected.resize(dims[1]);
      std::iota(selected.begin(), selected.end(), 0);
      count = dims[0];
      shape(in[1], {count}, false);
      shape(in[2], {count});
    }
    validate_description(in[0], p, selected, true);
    require(tensor(in[1]).descriptor.element_type == ElementType::Int64 &&
                tensor(in[2]).descriptor.element_type == type,
            "mask fitting IDs/weights dtype");
    shape(in[3], {selected.size()});
    require(tensor(in[3]).descriptor.element_type == ElementType::Float64,
            "mask regularization requires Float64");
    complete = true;
    scratch_bytes =
        bytes(static_cast<unsigned __int128>(count) * sizeof(Observation) +
              (type == ElementType::Float32 ? sizeof(FitScratch<16>)
                                            : sizeof(FitScratch<104>)));
    add_output(
        this, take(ps::mask::color_group_model_schema(count, selected.size())));
    add_output(this, take(ps::mask::skipped_color_groups_schema(count)));
  }
  std::vector<ReadRegion> support(unsigned, const Region&) const override {
    std::vector<ReadRegion> result;
    if (planar) {
      const auto dims = tensor(inputs[0]).sample_shape();
      for (auto c : selected)
        result.push_back({0, 0, Region({{c, 1}, {0, dims[1]}, {0, dims[2]}})});
    } else {
      result.push_back({0, 0, Region::whole(tensor(inputs[0]).sample_shape())});
    }
    for (unsigned p = 1; p < 4; ++p)
      result.push_back({p, 0, Region::whole(tensor(inputs[p]).sample_shape())});
    return result;
  }
  Coordinate at(std::uint64_t index) const {
    return planar ? Coordinate{index / width, index % width}
                  : Coordinate{index};
  }
  Coordinate color_at(std::uint64_t index, unsigned channel) const {
    return planar ? Coordinate{selected[channel], index / width, index % width}
                  : Coordinate{index, channel};
  }
  template <unsigned Words>
  void calculate(const Kernel& k, const ps_kernel_invocation_v8& inv) const {
    Work work{inv, work_limit};
    auto* scratch = new (inv.scratch[0].allocation) FitScratch<Words>(work);
    struct Cleanup {
      FitScratch<Words>* p;
      ~Cleanup() { p->~FitScratch<Words>(); }
    } cleanup{scratch};
    auto& m = scratch->math;
    auto* entries = reinterpret_cast<Observation*>(scratch + 1);
    const Reader r{k, inv};
    Writer writer{k, inv};
    const auto c = static_cast<unsigned>(selected.size());
    const bool narrow = type == ElementType::Float32;
    unsigned pending = 0;
    const auto visit = [&] {
      if (++pending == 256) {
        work.poll(pending);
        pending = 0;
      }
    };
    // Complete output ownership is initialized, including all padding.
    for (unsigned i = 0; i < inv.output_count; ++i)
      samples(inv.outputs[i], [&](const Coordinate& pos, std::uint64_t) {
        visit();
        store(inv.outputs[i], pos, 0);
      });
    for (unsigned j = 0; j < c; ++j)
      scratch->epsilon[j] = r.exact(m.exact, 3, 0, {j});
    std::uint64_t active = 0;
    for (std::uint64_t j = 0; j < count; ++j) {
      visit();
      const auto id = r.raw(1, 0, at(j));
      if (id)
        entries[active++] = {id, j};
    }
    std::sort(entries, entries + active,
              [&](const Observation& a, const Observation& b) {
                visit();
                return a.id < b.id || (a.id == b.id && a.index < b.index);
              });
    std::uint64_t models = 0, skipped = 0, first = 0;
    while (first < active) {
      work.poll(pending + 1);
      pending = 0;
      m.exact.reset();
      auto last = first + 1;
      while (last < active && entries[last].id == entries[first].id) {
        visit();
        ++last;
      }
      for (auto& v : scratch->moments)
        v = {};
      for (auto sample = first; sample < last; ++sample) {
        visit();
        const auto index = entries[sample].index;
        const auto weight_bits = r.raw(2, 0, at(index));
        std::array<std::uint64_t, 4> color_bits{};
        for (unsigned j = 0; j < c; ++j)
          color_bits[j] = r.raw(0, 0, color_at(index, j));
        const auto weight = Binary::decode(weight_bits, narrow);
        std::array<Binary, 4> color{};
        for (unsigned j = 0; j < c; ++j)
          color[j] = Binary::decode(color_bits[j], narrow);
        work.poll(Words);
        scratch->moments[0].add(weight, {}, {}, 1);
        for (unsigned j = 0; j < c; ++j) {
          work.poll(Words);
          scratch->moments[1 + j].add(weight, color[j], {}, 2);
          for (unsigned h = 0; h <= j; ++h) {
            work.poll(Words);
            scratch->moments[5 + j * 4 + h].add(weight, color[j], color[h], 3);
          }
        }
      }
      scratch->weight = scratch->moments[0].rational(work);
      for (unsigned j = 0; j < c; ++j) {
        scratch->mean[j] = scratch->moments[1 + j].rational(work);
        for (unsigned h = 0; h <= j; ++h)
          scratch->covariance[j * 4 + h] =
              scratch->moments[5 + j * 4 + h].rational(work);
      }
      if (Exact::zero(scratch->weight)) {
        writer.put(5, {skipped++}, entries[first].id);
        first = last;
        continue;
      }
      writer.put(1, {models}, entries[first].id);
      for (unsigned j = 0; j < c; ++j) {
        scratch->mean[j] = m.div(scratch->mean[j], scratch->weight);
        writer.put(2, {models, j}, m.rounded(scratch->mean[j], false));
      }
      for (unsigned j = 0; j < c; ++j)
        for (unsigned h = 0; h <= j; ++h) {
          auto value =
              m.sub(m.div(scratch->covariance[j * 4 + h], scratch->weight),
                    m.mul(scratch->mean[j], scratch->mean[h]));
          if (j == h)
            value = m.add(value, m.sq(scratch->epsilon[j]));
          scratch->covariance[j * 4 + h] = value;
        }
      // Exact LDL^T isolates all irrationality to one final sqrt per entry.
      for (unsigned j = 0; j < c; ++j) {
        auto pivot = scratch->covariance[j * 4 + j];
        for (unsigned h = 0; h < j; ++h)
          pivot = m.sub(
              pivot, m.mul(m.sq(scratch->lower[j * 4 + h]), scratch->pivot[h]));
        scratch->pivot[j] = pivot;
        scratch->lower[j * 4 + j] = m.exact.integer(1);
        for (unsigned row = j + 1; row < c; ++row) {
          auto v = scratch->covariance[row * 4 + j];
          for (unsigned h = 0; h < j; ++h)
            v = m.sub(v, m.mul(m.mul(scratch->lower[row * 4 + h],
                                     scratch->lower[j * 4 + h]),
                               scratch->pivot[h]));
          scratch->lower[row * 4 + j] = m.div(v, pivot);
        }
        for (unsigned row = j; row < c; ++row) {
          const auto& factor = scratch->lower[row * 4 + j];
          auto v = m.rounded_sqrt(m.mul(m.sq(factor), pivot), false);
          if (factor.negative && !Exact::zero(factor))
            v |= UINT64_C(1) << 63;
          writer.put(3, {models, row, j}, v);
        }
      }
      check(m.exact.status());
      ++models;
      first = last;
    }
    work.poll(pending);
    writer.put(0, {0}, models);
    writer.put(4, {0}, skipped);
  }
  void run(const Kernel& k, const ps_kernel_invocation_v8& inv) const override {
    if (type == ElementType::Float32)
      calculate<16>(k, inv);
    else
      calculate<104>(k, inv);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::mask

namespace ps::plugin_internal {
Status register_mask_statistics(OperationRegistry* registry) {
  using namespace mask;  // NOLINT(build/namespaces)
  for (bool planar : {false, true}) {
    std::vector<Parameter> p{{"input_description", Type::String}};
    if (planar)
      p.push_back({"channels", Type::String});
    auto op = generation::operation(
        planar ? "mask.fit_color_groups_image_strict"
               : "mask.fit_color_groups_table_strict",
        4, std::move(p),
        {{"model", take(ps::mask::color_group_model_schema(1, 1))},
         {"skipped", take(ps::mask::skipped_color_groups_schema(1))}},
        [planar](const auto& in, const auto& params) {
          return std::make_shared<Fit>(planar, in, params);
        });
    const auto status = registry->register_operation(std::move(op));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
