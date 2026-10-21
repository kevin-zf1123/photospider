// Independent stock OpenColorIO 2.5.2 reference generator.
// Numerics use one-point planar F32 with absent alpha, for both public dtypes.
// F64 input narrowing is integer nearest-even; output widening is exact.
#include <OpenColorIO/OpenColorIO.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace O = OCIO_NAMESPACE;
template <class To, class From>
To bits(From x) {
  static_assert(sizeof(To) == sizeof(From));
  To out;
  std::memcpy(&out, &x, sizeof(out));
  return out;
}
std::uint64_t shift(std::uint64_t m, int n) {
  if (n <= 0) {
    return m << (-n);
  }
  if (n >= 64) {
    return 0;
  }
  auto q = m >> n, r = m & ((UINT64_C(1) << n) - 1), h = UINT64_C(1) << (n - 1);
  return q + (r > h || (r == h && (q & 1)));
}
std::uint32_t rn32(double x) {
  auto b = bits<std::uint64_t>(x);
  std::uint32_t sign = static_cast<std::uint32_t>(b >> 63) << 31;
  unsigned exp = (b >> 52) & 2047;
  auto m = b & ((UINT64_C(1) << 52) - 1);
  if (exp == 2047) {
    throw std::runtime_error("nonfinite reference input");
  }
  if (!exp && !m) {
    return sign;
  }
  int p = exp ? static_cast<int>(exp) - 1023 - 52 : -1074;
  if (exp) {
    m |= UINT64_C(1) << 52;
  }
  int hi = 0;
  for (auto n = m; n >>= 1;) {
    ++hi;
  }
  int e = hi + p;
  if (e < -126) {
    return sign | static_cast<std::uint32_t>(shift(m, -(p + 149)));
  }
  auto sig = shift(m, hi - 23);
  if (sig == (UINT64_C(1) << 24)) {
    sig >>= 1;
    ++e;
  }
  if (e > 127) {
    throw std::runtime_error("reference F32 overflow");
  }
  return sign | (static_cast<std::uint32_t>(e + 127) << 23) |
         (static_cast<std::uint32_t>(sig) & 0x7fffff);
}
O::MatrixTransformRcPtr matrix(std::array<double, 9> a,
                               std::array<double, 3> b) {
  auto m = O::MatrixTransform::Create();
  double v[16] = {a[0], a[1], a[2], 0, a[3], a[4], a[5], 0,
                  a[6], a[7], a[8], 0, 0,    0,    0,    1};
  double d[4] = {b[0], b[1], b[2], 0};
  m->setMatrix(v);
  m->setOffset(d);
  return m;
}
std::string hex(double v, bool wide) {
  std::ostringstream s;
  s << std::hex << std::setfill('0');
  if (wide) {
    s << std::setw(16) << bits<std::uint64_t>(v);
  } else {
    s << std::setw(8) << bits<std::uint32_t>(static_cast<float>(v));
  }
  return s.str();
}
int main(int argc, char** argv) try {
  if (argc != 2) {
    throw std::runtime_error("usage: ocio_reference DIRECTORY");
  }
  if (std::string(O::GetVersion()) != "2.5.2") {
    throw std::runtime_error("wrong OCIO version");
  }
  namespace fs = std::filesystem;
  auto dir = fs::absolute(argv[1]);
  fs::create_directories(dir);
  const auto lutpath = dir / "oracle.spi1d";
  {
    std::ofstream f(lutpath);
    f << "Version 1\nFrom 0 1\nLength 2\nComponents 3\n{\n0 0.1 0.2\n1 0.9 "
         "0.8\n}\n";
  }
  auto cfg = O::Config::Create();
  cfg->setMajorVersion(2);
  cfg->setMinorVersion(1);
  auto src = O::ColorSpace::Create(O::REFERENCE_SPACE_SCENE);
  src->setName("src");
  src->addAlias("source_alias");
  src->setTransform(
      matrix({.90, .05, 0, .02, 1.10, .03, 0, .04, .80}, {.01, -.02, .03}),
      O::COLORSPACE_DIR_TO_REFERENCE);
  cfg->addColorSpace(src);
  auto dst = O::ColorSpace::Create(O::REFERENCE_SPACE_SCENE);
  dst->setName("dst");
  cfg->addColorSpace(dst);
  cfg->setRole("default", "src");
  cfg->setRole("scene_linear", "dst");
  auto look = O::Look::Create();
  look->setName("oracle_look");
  look->setProcessSpace("dst");
  look->setTransform(
      matrix({1.05, 0, 0, 0, .95, 0, 0, 0, 1.10}, {.02, .01, -.01}));
  cfg->addLook(look);
  auto named = O::NamedTransform::Create();
  named->setName("oracle_named");
  named->addAlias("named_alias");
  named->setTransform(
      matrix({.75, .10, 0, 0, 1.20, .05, .04, 0, .90}, {.03, -.01, .02}),
      O::TRANSFORM_DIR_FORWARD);
  cfg->addNamedTransform(named);
  cfg->addDisplayView("oracle_display", "oracle_view", "dst", "");
  cfg->setActiveDisplays("oracle_display");
  cfg->setActiveViews("oracle_view");
  cfg->validate();
  // Reparse exactly the bytes consumed by production's immutable snapshot.
  std::ostringstream serialized;
  cfg->serialize(serialized);
  {
    std::ofstream f(dir / "oracle.ocio");
    f << serialized.str();
  }
  std::istringstream serialized_input(serialized.str());
  auto config = O::Config::CreateFromStream(serialized_input);
  auto space = O::ColorSpaceTransform::Create();
  space->setSrc("src");
  space->setDst("dst");
  auto display = O::DisplayViewTransform::Create();
  display->setSrc("src");
  display->setDisplay("oracle_display");
  display->setView("oracle_view");
  auto looks = O::LookTransform::Create();
  looks->setSrc("src");
  looks->setDst("dst");
  looks->setLooks("+oracle_look");
  auto file = O::FileTransform::Create();
  file->setSrc(lutpath.string().c_str());
  file->setInterpolation(O::INTERP_LINEAR);
  auto group = O::GroupTransform::Create();
  group->appendTransform(
      matrix({.85, .08, 0, 0, 1.05, .02, .03, 0, .95}, {.01, .02, .03}));
  auto exposure = O::ExposureContrastTransform::Create();
  exposure->setStyle(O::EXPOSURE_CONTRAST_LINEAR);
  exposure->setExposure(.25);
  exposure->setContrast(1.10);
  exposure->setGamma(.90);
  exposure->setPivot(.18);
  exposure->makeExposureDynamic();
  exposure->makeContrastDynamic();
  exposure->makeGammaDynamic();
  exposure->makeExposureNonDynamic();
  exposure->makeContrastNonDynamic();
  exposure->makeGammaNonDynamic();
  group->appendTransform(exposure);
  {
    std::ofstream f(dir / "oracle-tree.yaml");
    f << "!<GroupTransform>\n  children:\n"
         "    - !<MatrixTransform> {matrix: [0.85, 0.08, 0, 0, 0, 1.05, 0.02, "
         "0, 0.03, 0, 0.95, 0, 0, 0, 0, 1], offset: [0.01, 0.02, 0.03, 0]}\n"
         "    - !<ExposureContrastTransform> {style: linear, exposure: 0.25, "
         "contrast: 1.1, gamma: 0.9, pivot: 0.18}\n";
  }
  std::vector<std::pair<std::string, O::ConstTransformRcPtr>> entries = {
      {"space", space},
      {"display_view", display},
      {"looks", looks},
      {"named", config->getNamedTransform("oracle_named")
                    ->getTransform(O::TRANSFORM_DIR_FORWARD)},
      {"file", file},
      {"group", group}};
  const std::array<std::array<double, 3>, 10> values{
      {{{0, 0, 0}},
       {{1, 1, 1}},
       {{1, 0, 0}},
       {{0, 1, 0}},
       {{0, 0, 1}},
       {{.18, .37, .63}},
       {{-.25, .5, 1.5}},
       {{1 + std::ldexp(1., -24), 1 + 3 * std::ldexp(1., -24),
         std::ldexp(1., -150)}},
       {{-0., 0, -0.}},
       {{std::ldexp(1., -149), std::ldexp(1., -126), -.125}}}};
  std::ofstream out(dir / "reference.tsv");
  out << "# stock OCIO " << O::GetVersion()
      << "; one-point planar F32, absent alpha\n";
  out << "entry\tmode\tdirection\tdtype\trow\tinput_bits\toutput_bits\n";
  unsigned count = 0;
  for (const auto& entry : entries) {
    for (unsigned inverse = 0; inverse < 2; ++inverse) {
      const auto processor = config->getProcessor(
          entry.second,
          inverse ? O::TRANSFORM_DIR_INVERSE : O::TRANSFORM_DIR_FORWARD);
      for (unsigned optimized = 0; optimized < 2; ++optimized) {
        const auto cpu = processor->getOptimizedCPUProcessor(
            O::BIT_DEPTH_F32, O::BIT_DEPTH_F32,
            optimized ? O::OPTIMIZATION_DEFAULT : O::OPTIMIZATION_NONE);
        for (bool wide : {false, true}) {
          for (unsigned i = 0; i < values.size(); ++i) {
            float rgb[3];
            double public_in[3];
            for (unsigned c = 0; c < 3; ++c) {
              rgb[c] = bits<float>(rn32(values[i][c]));
              public_in[c] = wide ? values[i][c] : static_cast<double>(rgb[c]);
            }
            out << entry.first << '\t'
                << (optimized ? "optimized" : "reference") << '\t'
                << (inverse ? "inverse" : "forward") << '\t'
                << (wide ? "f64" : "f32") << '\t' << i << '\t';
            for (unsigned c = 0; c < 3; ++c) {
              if (c) {
                out << ',';
              }
              out << hex(public_in[c], wide);
            }
            out << '\t';
            O::PlanarImageDesc image(&rgb[0], &rgb[1], &rgb[2], nullptr, 1, 1,
                                     O::BIT_DEPTH_F32, sizeof(float),
                                     sizeof(float));
            cpu->apply(image);
            for (unsigned c = 0; c < 3; ++c) {
              if (c) {
                out << ',';
              }
              if (!std::isfinite(rgb[c])) {
                throw std::runtime_error("nonfinite stock result");
              }
              out << hex(static_cast<double>(rgb[c]), wide);
            }
            out << '\n';
            ++count;
          }
        }
      }
    }
  }
  if (!out) {
    throw std::runtime_error("reference write failure");
  }
  std::cout << "stock OCIO rows=" << count << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
