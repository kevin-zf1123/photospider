#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "01-numeric/accelerated_math.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "data/exact_numeric.hpp"
#include "photospider/ops/format/transfer.hpp"

namespace ps::plugin_internal::transfer_ops {
using data_internal::format_numeric::Natural;
using data_internal::format_numeric::Rational;
using numeric_ops::numeric_bits;
using numeric_ops::numeric_double;
using numeric_ops::SequenceProfile;

enum class Code {
  Input,
  Rational,
  Binary,
  Beta,
  Ln2,
  Add,
  Subtract,
  Multiply,
  Divide,
  Power,
  Exp,
  Log,
  Sqrt,
  MaxZero
};
struct Node final {
  Code code = Code::Input;
  unsigned a = 0, b = 0;
  std::int64_t numerator = 0;
  std::uint64_t denominator = 1, bits = 0;
  bool varying = true;
};
// A bounded real-expression DAG, NOT NUM's stepwise-RN64 expression language.
// Nodes preserve exact constants/intermediates until the output rounding.
struct Program final {
  static constexpr unsigned kNodes = 32;
  std::array<Node, kNodes> nodes{};
  unsigned size = 1, result = 0;
  bool rational = true, final_sqrt = false;
  unsigned push(Node n) {
    if (size == nodes.size())
      throw std::logic_error("FMT-09 program capacity");
    result = size;
    nodes[size++] = n;
    return result;
  }
  unsigned q(std::int64_t n, std::uint64_t d = 1) {
    for (unsigned i = 1; i < size; ++i)
      if (nodes[i].code == Code::Rational && nodes[i].numerator == n &&
          nodes[i].denominator == d)
        return i;
    return push({Code::Rational, 0, 0, n, d, 0, false});
  }
  unsigned binary(double x) {
    return push({Code::Binary, 0, 0, 0, 1, numeric_bits(x), false});
  }
  unsigned beta() {
    rational = false;
    return push({Code::Beta, 0, 0, 0, 1, 0, false});
  }
  unsigned ln2() {
    rational = false;
    return push({Code::Ln2, 0, 0, 0, 1, 0, false});
  }
  unsigned op(Code code, unsigned a, unsigned b = 0) {
    if (code == Code::Power || code == Code::Exp || code == Code::Log)
      rational = false;
    final_sqrt = code == Code::Sqrt;
    return push(
        {code, a, b, 0, 1, 0,
         nodes[a].varying || ((code == Code::Add || code == Code::Subtract ||
                               code == Code::Multiply || code == Code::Divide ||
                               code == Code::Power) &&
                              nodes[b].varying)});
  }
  unsigned add(unsigned a, unsigned b) { return op(Code::Add, a, b); }
  unsigned sub(unsigned a, unsigned b) { return op(Code::Subtract, a, b); }
  unsigned mul(unsigned a, unsigned b) { return op(Code::Multiply, a, b); }
  unsigned div(unsigned a, unsigned b) { return op(Code::Divide, a, b); }
  unsigned pow(unsigned a, unsigned b) { return op(Code::Power, a, b); }
  unsigned exp(unsigned a) { return op(Code::Exp, a); }
  unsigned log(unsigned a) { return op(Code::Log, a); }
  unsigned sqrt(unsigned a) { return op(Code::Sqrt, a); }
  unsigned max0(unsigned a) { return op(Code::MaxZero, a); }
  void finish(unsigned out) { result = out; }
};
inline Rational rational(std::int64_t n, std::uint64_t d = 1) {
  auto r = Rational::integer(n);
  r.d = Natural(d);
  return r;
}
// First binary64 operand in the upper branch. A binary32 promoted to binary64
// is exact, so these cuts implement the SAME exact-rational branch rule for
// either dtype. No comparison is made against an RN32 decimal breakpoint.
inline double upper_cut(std::int64_t n, std::uint64_t d, bool lower_closed) {
  auto exact = rational(n, d);
  auto bits = exact.floating_bits(false);
  double rounded = numeric_double(bits);
  const int cmp = Rational::binary(bits, false).compare(exact);
  if (cmp < 0 || (cmp == 0 && lower_closed))
    rounded = std::nextafter(rounded, std::numeric_limits<double>::infinity());
  return rounded;
}
struct CurveProgram final {
  TransferDefinition definition;
  bool encode = false, signed_curve = false, identity = false;
  std::array<Program, 3> branches{};
  std::array<double, 2> cuts{};
  unsigned branch_count = 1;
  unsigned branch(double x) const {
    const double u = signed_curve ? std::abs(x) : x;
    unsigned i = 0;
    while (i + 1 < branch_count && u >= cuts[i])
      ++i;
    return i;
  }
};
inline CurveProgram make_program(const TransferDefinition& d, bool encode) {
  CurveProgram c;
  c.definition = d;
  c.encode = encode;
  const auto k = d.curve;
  c.signed_curve = k == TransferCurve::PowerGamma || k == TransferCurve::Srgb ||
                   k == TransferCurve::Bt709 || k == TransferCurve::Bt2020;
  c.identity = k == TransferCurve::Linear ||
               (k == TransferCurve::PowerGamma && *d.gamma == 1);
  if (c.identity)
    return c;
  if (k == TransferCurve::PowerGamma) {
    auto& p = c.branches[0];
    if (*d.gamma == 2) {
      p.finish(encode ? p.sqrt(0) : p.mul(0, 0));
    } else {
      const auto exponent = p.binary(*d.gamma);
      p.finish(p.pow(0, encode ? p.div(p.q(1), exponent) : exponent));
    }
  } else if (k == TransferCurve::Srgb) {
    c.branch_count = 2;
    c.cuts[0] =
        encode ? upper_cut(7827, 2500000, true) : upper_cut(809, 20000, true);
    auto& l = c.branches[0];
    l.finish(l.mul(0, encode ? l.q(323, 25) : l.q(25, 323)));
    auto& p = c.branches[1];
    if (encode)
      p.finish(p.sub(p.mul(p.q(211, 200), p.pow(0, p.q(5, 12))), p.q(11, 200)));
    else
      p.finish(p.pow(p.div(p.add(0, p.q(11, 200)), p.q(211, 200)), p.q(12, 5)));
  } else if (k == TransferCurve::Bt709 || k == TransferCurve::Bt2020) {
    const auto v =
        k == TransferCurve::Bt709
            ? Bt2020Coefficients::Rounded10Bit
            : d.coefficient_variant.value_or(Bt2020Coefficients::Smooth);
    c.branch_count = 2;
    // Certified from the 4096-bit isolating interval in transfer_constants.hpp.
    if (v == Bt2020Coefficients::Smooth)
      c.cuts[0] = encode ? 0x1.27cbd51448a2ep-6 : 0x1.4cc54fb6d1b74p-4;
    else if (v == Bt2020Coefficients::Rounded10Bit)
      c.cuts[0] =
          encode ? upper_cut(18, 1000, false) : upper_cut(81, 1000, false);
    else
      c.cuts[0] =
          encode ? upper_cut(181, 10000, false) : upper_cut(1629, 20000, false);
    auto& l = c.branches[0];
    l.finish(l.mul(0, encode ? l.q(9, 2) : l.q(2, 9)));
    auto& p = c.branches[1];
    const auto a = v == Bt2020Coefficients::Smooth
                       ? p.add(p.q(1), p.mul(p.q(11, 2), p.beta()))
                   : v == Bt2020Coefficients::Rounded10Bit ? p.q(1099, 1000)
                                                           : p.q(10993, 10000);
    const auto b = p.sub(a, p.q(1));
    p.finish(encode ? p.sub(p.mul(a, p.pow(0, p.q(9, 20))), b)
                    : p.pow(p.div(p.add(0, b), a), p.q(20, 9)));
  } else if (k == TransferCurve::Bt1886) {
    auto& p = c.branches[0];
    const auto exponent = p.q(5, 12);
    const auto black = p.pow(p.binary(*d.black_luminance), exponent);
    const auto span =
        p.sub(p.pow(p.binary(*d.white_luminance), exponent), black);
    p.finish(encode ? p.div(p.sub(p.pow(0, exponent), black), span)
                    : p.pow(p.max0(p.add(p.mul(span, 0), black)), p.q(12, 5)));
  } else if (k == TransferCurve::Pq) {
    auto& p = c.branches[0];
    if (encode) {
      const auto y = p.pow(p.div(0, p.q(10000)), p.q(2610, 16384));
      p.finish(p.pow(p.div(p.add(p.q(3424, 4096), p.mul(p.q(2413, 128), y)),
                           p.add(p.q(1), p.mul(p.q(2392, 128), y))),
                     p.q(2523, 32)));
    } else {
      const auto z = p.pow(0, p.q(32, 2523));
      const auto r = p.div(p.max0(p.sub(z, p.q(3424, 4096))),
                           p.sub(p.q(2413, 128), p.mul(p.q(2392, 128), z)));
      p.finish(p.mul(p.q(10000), p.pow(r, p.q(16384, 2610))));
    }
  } else if (k == TransferCurve::HlgOetf) {
    c.branch_count = 2;
    c.cuts[0] = encode ? upper_cut(1, 12, true) : upper_cut(1, 2, true);
    auto& l = c.branches[0];
    l.finish(encode ? l.sqrt(l.mul(l.q(3), 0)) : l.div(l.mul(0, 0), l.q(3)));
    auto& p = c.branches[1];
    const auto a = p.q(17883277, 100000000), four_a = p.mul(p.q(4), a);
    const auto b = p.sub(p.q(1), four_a);
    const auto offset = p.sub(p.q(1, 2), p.mul(a, p.log(four_a)));
    p.finish(encode
                 ? p.add(p.mul(a, p.log(p.sub(p.mul(p.q(12), 0), b))), offset)
                 : p.div(p.add(p.exp(p.div(p.sub(0, offset), a)), b), p.q(12)));
  } else {
    const bool cc = k == TransferCurve::Acescc;
    if (encode) {
      c.branch_count = cc ? 3 : 2;
      if (cc) {
        c.cuts[0] = std::nextafter(0.0, 1.0);
        c.cuts[1] = 0x1p-15;
        c.branches[0].finish(c.branches[0].q(-157, 438));
        auto& p = c.branches[1];
        const auto log =
            p.div(p.log(p.add(p.q(1, 65536), p.mul(0, p.q(1, 2)))), p.ln2());
        p.finish(p.div(p.add(log, p.q(243, 25)), p.q(438, 25)));
      } else {
        c.cuts[0] = upper_cut(1, 128, true);
        auto& l = c.branches[0];
        l.finish(l.add(l.mul(l.q(105402377416545, UINT64_C(10000000000000)), 0),
                       l.q(729055341958355, UINT64_C(10000000000000000))));
      }
      auto& p = c.branches[cc ? 2 : 1];
      p.finish(
          p.div(p.add(p.div(p.log(0), p.ln2()), p.q(243, 25)), p.q(438, 25)));
    } else {
      c.branch_count = 3;
      c.cuts[0] =
          cc ? upper_cut(-22, 73, true)
             : upper_cut(155251141552511, UINT64_C(1000000000000000), true);
      // ceil_binary64((log2(65504)+9.72)/17.52), independently checked by
      // oracle.
      c.cuts[1] = 0x1.77ce9b36e1732p+0;
      auto& l = c.branches[0];
      if (cc) {
        const auto e =
            l.mul(l.sub(l.mul(l.q(438, 25), 0), l.q(243, 25)), l.ln2());
        l.finish(l.mul(l.q(2), l.sub(l.exp(e), l.q(1, 65536))));
      } else {
        l.finish(
            l.div(l.sub(0, l.q(729055341958355, UINT64_C(10000000000000000))),
                  l.q(105402377416545, UINT64_C(10000000000000))));
      }
      auto& p = c.branches[1];
      p.finish(
          p.exp(p.mul(p.sub(p.mul(p.q(438, 25), 0), p.q(243, 25)), p.ln2())));
      c.branches[2].finish(c.branches[2].q(65504));
    }
  }
  return c;
}
}  // namespace ps::plugin_internal::transfer_ops
