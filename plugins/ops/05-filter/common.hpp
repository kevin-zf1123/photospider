#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "01-numeric/accelerated_math.hpp"
#include "01-numeric/numeric_nan.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "03-generation/math.hpp"
#include "04-mask-morphology/common.hpp"

namespace ps::plugin_internal::filter {
using generation::add_output;
using generation::bits;
using generation::check;
using generation::Coordinate;
using generation::floating;
using generation::integer;
using generation::Kernel;
using generation::number;
using generation::Parameters;
using generation::Program;
using generation::ReadRegion;
using generation::real;
using generation::require;
using generation::samples;
using generation::size_product;
using generation::store;
using generation::take;
using generation::tensor;
using generation::tensor_schema;
using generation::text;
using generation::Work;
using mask::bytes;
using mask::parameter;
using Reader = mask::Reader;
using Exact = generation::Exact;
using Rational = Exact::Rational;
using Fast = numeric_ops::FastInterval;
using Profile = numeric_ops::SequenceProfile;
using Parameter = OperationParameterSpec;
using Type = OperationParameterType;

// Both coordinate visits and integer limb work share this bounded consumer.
struct Fuel {
  const Work& work;
  mutable std::uint64_t pending = 0;
  Status operator()(std::uint64_t count) const {
    while (count >= 512 - pending) {
      count -= 512 - pending;
      pending = 0;
      const auto result = work(512);
      if (!result.ok()) {
        return result;
      }
    }
    pending += count;
    return Status::success();
  }
  void poll(std::uint64_t count = 1) const { check((*this)(count)); }
  void flush() const {
    check(work(pending));
    pending = 0;
  }
};

enum class Boundary { Constant, Clamp, Wrap, Half, Whole, Truncate };
enum class Kind {
  Convolve,
  Correlate,
  Normalized,
  Color,
  Separable,
  Correlation,
  BoxSum,
  BoxMean,
  Gaussian
};
struct Geometry {
  unsigned ya = 0, xa = 1;
  std::uint64_t h = 0, w = 0, kh = 1, kw = 1, ay = 0, ax = 0;
  std::int64_t oy = 0, ox = 0;
  Boundary boundary = Boundary::Constant;
  bool correlation = false;
  double cval = 0;
  std::vector<std::uint64_t> dims;
  Geometry(const ResultTensorSpec&, const Parameters&, std::uint64_t,
           std::uint64_t, Kind);
  std::int64_t map(__int128 value, std::uint64_t extent) const;
  std::vector<Region> support(const Region&) const;
  Coordinate position(const Coordinate&, std::uint64_t, std::uint64_t,
                      bool* outside) const;
};

struct Filter final : Program {
  Kind kind;
  Profile profile;
  ElementType type;
  Geometry geometry;
  std::string normalization, empty;
  double bias = 0, sx = 0, sy = 0;
  int component_axis = -1;
  bool broadcast = false;
  int color_axis = -1;
  std::vector<std::uint64_t> color_channels;
  std::optional<std::uint64_t> alpha;
  Filter(Kind, Profile, const std::vector<OperationMetadata>&,
         const Parameters&);
  std::vector<ReadRegion> support(unsigned, const Region&) const override;
  std::uint64_t task_scratch(unsigned, const Region&) const override;
  void run(const Kernel&, const ps_kernel_invocation_v8&) const override;
  bool separable() const;
  bool box() const;
  bool selected(const Coordinate&) const;
  bool alpha_sample(const Coordinate&) const;
};

std::uint64_t gaussian_coefficient(generation::Math&, double, std::uint64_t);
OperationDefinition make_filter(const std::string&, Kind, Profile);
OperationDefinition make_coefficients(const std::string&, Profile);
}  // namespace ps::plugin_internal::filter
