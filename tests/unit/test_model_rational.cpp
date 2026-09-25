#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "02-format-color/model_rational.hpp"

namespace {
namespace m = ps::plugin_internal::model_ops;
using Math = m::RationalMath;

Math::Integer magnitude(const std::string& text) {
  Math::Integer result;
  const auto first = !text.empty() && text.front() == '-' ? 1U : 0U;
  if (text.size() <= first || text.size() - first > Math::kWords * 16)
    throw std::runtime_error("invalid oracle integer length");
  for (std::size_t offset = first; offset < text.size(); ++offset) {
    const char digit = text[text.size() - 1 - (offset - first)];
    const unsigned value = digit >= '0' && digit <= '9'   ? digit - '0'
                           : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10
                                                          : 16;
    if (value == 16)
      throw std::runtime_error("invalid oracle hex digit");
    const auto nibble = offset - first;
    result.words[nibble / 16] |= static_cast<std::uint64_t>(value)
                                 << (4 * (nibble % 16));
  }
  return result;
}
Math::Rational rational(const std::string& numerator,
                        const std::string& denominator) {
  Math::Rational result;
  result.n = magnitude(numerator);
  result.d = magnitude(denominator);
  result.negative = numerator.front() == '-';
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected rational oracle file");
    std::ifstream input(argv[1]);
    if (!input)
      throw std::runtime_error("cannot open rational oracle file");
    Math math(ps::plugin_internal::numeric_ops::SequenceProfile::Strict);
    std::uint64_t charged = 0;
    const Math::Work work = [&](std::uint64_t amount) {
      charged += amount;
      return ps::Status::success();
    };
    math.bind(work);
    std::string op, an, ad, bn, bd, en, ed;
    std::size_t count = 0;
    while (input >> op) {
      if (!(input >> an >> ad >> bn >> bd >> en >> ed))
        throw std::runtime_error("truncated rational oracle");
      const auto a = rational(an, ad), b = rational(bn, bd);
      const auto expected = rational(en, ed);
      const auto actual = op == "+"   ? math.add(a, b)
                          : op == "-" ? math.subtract(a, b)
                          : op == "*" ? math.multiply(a, b)
                                      : math.divide(a, b);
      if (actual.n.words != expected.n.words ||
          actual.d.words != expected.d.words ||
          actual.negative != expected.negative)
        throw std::runtime_error("rational oracle mismatch at case " +
                                 std::to_string(count + 1));
      ++count;
    }
    if (!count || !input.eof())
      throw std::runtime_error("empty or unreadable rational oracle");
    // A failure from proof accounting must propagate unchanged.
    const Math::Work cancelled = [](std::uint64_t) {
      return ps::Status{ps::ErrorCode::Cancelled, "rational test cancellation",
                        ps::FailureReason::Cancelled};
    };
    math.bind(cancelled);
    bool stopped = false;
    try {
      static_cast<void>(math.fraction(12345, 54321));
    } catch (const ps::Status& status) {
      stopped = status.code == ps::ErrorCode::Cancelled &&
                status.reason == ps::FailureReason::Cancelled;
    }
    if (!stopped)
      throw std::runtime_error("GCD lost cancellation");
    std::cout << count
              << " exact rational oracle cases passed; proof work=" << charged
              << '\n';
    return 0;
  } catch (const ps::Status& status) {
    std::cerr << status.message << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
