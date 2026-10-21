#include <OpenColorIO/OpenColorIO.h>
#include <pystring.h>
#include <yaml-cpp/emitter.h>
#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/parser.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "engine_memory.h"  // NOLINT(build/include_subdir)
#include "src/exp.h"

namespace OCIO = OCIO_NAMESPACE;
namespace {
void require(bool ok, const char* message) {
  if (!ok) {
    throw std::invalid_argument(message);
  }
}
struct Scope final {
  ps_engine_scope_v1 local{};
  ps_engine_scope_v1* previous;
  Scope(const ps_color_host_v1& host, char* error, size_t size)
      : previous(ps_engine_scope) {
    local.host = host;
    local.armed = 1;
    local.error = error;
    local.error_size = size;
    if (size) {
      error[0] = 0;
    }
    ps_engine_scope = &local;
  }
  ~Scope() { ps_engine_scope = previous; }
};
struct Build final {
  bool allow_bridge = false;
  std::string trace;
  std::set<const void*> strict_files;
  bool strict_file = false;
  void record(const std::string& text) {
    require(trace.size() + text.size() < 1024 * 1024,
            "OCIO construction trace limit");
    trace += text;
    trace += "; ";
  }
};
thread_local Build* active_build = nullptr;
struct Observe final {
  Build* previous;
  explicit Observe(Build* b) : previous(active_build) { active_build = b; }
  ~Observe() { active_build = previous; }
};
std::string normalized(const std::string& name) {
  require(!name.empty() && name.find('\0') == name.npos &&
              name.find('$') == name.npos,
          "invalid logical file name");
  std::string out;
  out = pystring::os::path::normpath(name[0] == '/' ? name : "/" + name);
  return out;
}
class Proxy final : public OCIO::ConfigIOProxy {
 public:
  std::string config, identity;
  std::map<std::string, ps_color_bytes_v1> files;
  std::map<std::string, std::string> context, spaces;
  std::vector<uint8_t> getLutData(const char* path) const override {
    auto at = files.find(normalized(path));
    if (at == files.end()) {
      throw OCIO::ExceptionMissingFile(
          "File absent from immutable color resource");
    }
    ps_engine_cpp_tick(at->second.size);
    if (active_build) {
      active_build->record(std::string("file=") + at->first);
    }
    return {at->second.data, at->second.data + at->second.size};
  }
  std::string getConfigData() const override { return config; }
  std::string getFastLutFileHash(const char* path) const override {
    auto at = files.find(normalized(path));
    return at == files.end() ? std::string() : identity + ":" + at->first;
  }
};
constexpr const char* raw_config = R"config(ocio_profile_version: 2.5
environment: {}
search_path: ''
roles: {default: raw}
file_rules:
  - !<Rule> {name: Default, colorspace: raw}
displays:
  explicit:
    - !<View> {name: raw, colorspace: raw}
colorspaces:
  - !<ColorSpace> {name: raw, family: '', equalitygroup: '', bitdepth: 32f, isdata: false, allocation: uniform}
)config";
std::string bytes_string(ps_color_bytes_v1 b) {
  return b.size ? std::string(reinterpret_cast<const char*>(b.data), b.size)
                : std::string();
}
std::shared_ptr<Proxy> proxy(const ps_ocio_request_v1& q) {
  auto p = std::make_shared<Proxy>();
  p->identity = q.identity;
  if (!q.snapshot.size) {
    p->config = raw_config;
    p->spaces.emplace("raw", "scene");
  } else {
    size_t offset = 0;
    unsigned entries = 0;
    std::set<std::pair<std::string, std::string>> keys;
    auto field = [&]() {
      require(q.snapshot.size - offset >= 8, "truncated OCIO snapshot");
      uint64_t n = 0;
      for (unsigned k = 0; k < 8; ++k) {
        n |= static_cast<uint64_t>(q.snapshot.data[offset++]) << (8 * k);
      }
      require(n <= q.snapshot.size - offset, "truncated OCIO field");
      ps_color_bytes_v1 value{q.snapshot.data + offset, static_cast<size_t>(n)};
      offset += n;
      return value;
    };
    std::map<std::string, std::string> identity;
    while (offset < q.snapshot.size) {
      require(++entries <= 1030, "OCIO manifest entry limit");
      ps_engine_cpp_tick(1);
      auto type = bytes_string(field()), name = bytes_string(field());
      auto value = field();
      require(keys.emplace(type, name).second, "duplicate OCIO manifest field");
      if (type == "config") {
        require(name.empty(), "invalid config field");
        p->config = bytes_string(value);
      } else if (type == "identity") {
        identity.emplace(name, bytes_string(value));
      } else if (type == "files") {
        require(p->files.emplace(normalized(name), value).second,
                "duplicate normalized file");
      } else if (type == "context") {
        auto v = bytes_string(value);
        require(
            !name.empty() && v.find('$') == v.npos && v.find('\0') == v.npos,
            "unresolved context override");
        p->context.emplace(name, std::move(v));
      } else if (type == "spaces") {
        auto v = bytes_string(value);
        require(v == "scene" || v == "display",
                "invalid reference-space declaration");
        p->spaces.emplace(name, std::move(v));
      } else {
        throw std::invalid_argument("unknown OCIO manifest field");
      }
    }
    require(identity["engine"] == "2.5.2" &&
                identity["build"] == ps_color_engine_identity_v1() &&
                identity["settings"] == "closed-rgb-v1",
            "OCIO engine/build/settings mismatch");
    require(!p->config.empty() && !p->spaces.empty(),
            "incomplete OCIO manifest");
  }
  if (q.file_data.size) {
    require(p->files.emplace(normalized(q.file), q.file_data).second,
            "duplicate file binding");
  }
  return p;
}
// Event validation bounds recursion before OCIO's recursive typed deserializer.
// YAML aliases are accepted only after the referenced node has closed.
class YamlLimits final : public YAML::EventHandler {
  std::vector<YAML::anchor_t> stack;
  std::set<YAML::anchor_t> active, complete;
  unsigned documents = 0;
  void leaf(YAML::anchor_t anchor) {
    ps_engine_cpp_tick(1);
    if (anchor) {
      complete.insert(anchor);
    }
  }
  void begin(YAML::anchor_t anchor) {
    require(stack.size() < 64, "OCIO YAML depth limit");
    ps_engine_cpp_tick(1);
    stack.push_back(anchor);
    if (anchor) {
      active.insert(anchor);
    }
  }
  void end() {
    auto anchor = stack.back();
    stack.pop_back();
    if (anchor) {
      active.erase(anchor);
      complete.insert(anchor);
    }
  }

 public:
  void OnDocumentStart(const YAML::Mark&) override {
    require(++documents == 1, "multiple YAML documents");
  }
  void OnDocumentEnd() override {}
  void OnNull(const YAML::Mark&, YAML::anchor_t a) override { leaf(a); }
  void OnAlias(const YAML::Mark&, YAML::anchor_t a) override {
    require(!active.count(a) && complete.count(a),
            "cyclic or unresolved YAML alias");
    ps_engine_cpp_tick(1);
  }
  void OnScalar(const YAML::Mark&, const std::string&, YAML::anchor_t a,
                const std::string&) override {
    leaf(a);
  }
  void OnSequenceStart(const YAML::Mark&, const std::string&, YAML::anchor_t a,
                       YAML::EmitterStyle::value) override {
    begin(a);
  }
  void OnSequenceEnd() override { end(); }
  void OnMapStart(const YAML::Mark&, const std::string&, YAML::anchor_t a,
                  YAML::EmitterStyle::value) override {
    begin(a);
  }
  void OnMapEnd() override { end(); }
};
void yaml_limits(const std::string& text) {
  ps_engine_cpp_tick(text.size());
  require(text.find('\0') == text.npos, "NUL in OCIO YAML");
  std::istringstream stream(text);
  YAML::Parser parser(stream);
  YamlLimits limits;
  while (parser.HandleNextDocument(limits)) {
  }
}
OCIO::ConstColorSpaceRcPtr space(const OCIO::Config& config,
                                 const std::string& selector) {
  auto split = selector.find(':');
  require(split != selector.npos,
          "ColorSpace selector needs name:, alias: or role:");
  const auto kind = selector.substr(0, split),
             value = selector.substr(split + 1);
  require(!value.empty(), "empty ColorSpace selector");
  OCIO::ConstColorSpaceRcPtr found;
  if (kind == "role") {
    bool exists = false;
    for (int i = 0; i < config.getNumRoles(); ++i) {
      if (value == config.getRoleName(i)) {
        exists = true;
      }
    }
    require(exists, "unknown exact OCIO role");
    return space(
        config, std::string("name:") + config.getRoleColorSpace(value.c_str()));
  }
  require(kind == "name" || kind == "alias",
          "invalid ColorSpace selector namespace");
  const int n = config.getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_ALL,
                                         OCIO::COLORSPACE_ALL);
  for (int i = 0; i < n; ++i) {
    auto s = config.getColorSpace(config.getColorSpaceNameByIndex(
        OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ALL, i));
    bool match = kind == "name" && value == s->getName();
    if (kind == "alias") {
      for (size_t a = 0; a < s->getNumAliases(); ++a) {
        match |= value == s->getAlias(a);
      }
    }
    if (match) {
      require(!found, "ambiguous exact ColorSpace selector");
      found = s;
    }
  }
  require(static_cast<bool>(found), "unknown exact OCIO ColorSpace");
  return found;
}
OCIO::ConstNamedTransformRcPtr named(const OCIO::Config& config,
                                     const std::string& selector) {
  auto colon = selector.find(':');
  require(colon != selector.npos,
          "NamedTransform selector needs name: or alias:");
  auto kind = selector.substr(0, colon), value = selector.substr(colon + 1);
  require(kind == "name" || kind == "alias", "invalid NamedTransform selector");
  OCIO::ConstNamedTransformRcPtr found;
  for (int i = 0; i < config.getNumNamedTransforms(OCIO::NAMEDTRANSFORM_ALL);
       ++i) {
    auto n = config.getNamedTransform(
        config.getNamedTransformNameByIndex(OCIO::NAMEDTRANSFORM_ALL, i));
    bool match = kind == "name" && value == n->getName();
    if (kind == "alias") {
      for (size_t a = 0; a < n->getNumAliases(); ++a) {
        match |= value == n->getAlias(a);
      }
    }
    if (match) {
      require(!found, "ambiguous NamedTransform selector");
      found = n;
    }
  }
  require(static_cast<bool>(found), "unknown exact NamedTransform");
  return found;
}
OCIO::Interpolation interpolation(int i) {
  switch (i) {
    case 0:
      return OCIO::INTERP_DEFAULT;
    case 1:
      return OCIO::INTERP_BEST;
    case 2:
      return OCIO::INTERP_NEAREST;
    case 3:
      return OCIO::INTERP_LINEAR;
    case 4:
      return OCIO::INTERP_TETRAHEDRAL;
    default:
      throw std::invalid_argument("unsupported interpolation");
  }
}
void looks_exist(const OCIO::Config& config, const std::string& value) {
  size_t first = 0;
  while (first < value.size()) {
    auto end = value.find_first_of(",|", first);
    auto token = value.substr(first, end - first);
    const auto b = token.find_first_not_of(" \t"),
               e = token.find_last_not_of(" \t");
    require(b != token.npos, "empty Look item");
    token = token.substr(b, e - b + 1);
    if (token[0] == '+' || token[0] == '-') {
      token.erase(0, 1);
    }
    bool found = false;
    for (int i = 0; i < config.getNumLooks(); ++i) {
      found |= token == config.getLookNameByIndex(i);
    }
    require(found, "unknown exact OCIO Look");
    if (end == value.npos) {
      break;
    }
    first = end + 1;
  }
}

template <class Curve>
void check_curve_structure(const Curve& curve) {
  require(static_cast<bool>(curve), "null OCIO grading curve");
  const auto n = curve->getNumControlPoints();
  require(n >= 0 && static_cast<uint64_t>(n) <= 1048576,
          "OCIO curve point limit");
}
void check_node_structure(const OCIO::ConstTransformRcPtr& node) {
  require(static_cast<bool>(node), "null OCIO transform");
  ps_engine_cpp_tick(1);
  if (dynamic_cast<const OCIO::GroupTransform*>(node.get()) ||
      dynamic_cast<const OCIO::MatrixTransform*>(node.get()) ||
      dynamic_cast<const OCIO::ExponentTransform*>(node.get()) ||
      dynamic_cast<const OCIO::ExponentWithLinearTransform*>(node.get()) ||
      dynamic_cast<const OCIO::ExposureContrastTransform*>(node.get()) ||
      dynamic_cast<const OCIO::LogCameraTransform*>(node.get()) ||
      dynamic_cast<const OCIO::LogAffineTransform*>(node.get()) ||
      dynamic_cast<const OCIO::LogTransform*>(node.get()) ||
      dynamic_cast<const OCIO::CDLTransform*>(node.get()) ||
      dynamic_cast<const OCIO::RangeTransform*>(node.get()) ||
      dynamic_cast<const OCIO::GradingPrimaryTransform*>(node.get()) ||
      dynamic_cast<const OCIO::GradingToneTransform*>(node.get())) {
    return;
  }
  if (auto* t = dynamic_cast<const OCIO::Lut1DTransform*>(node.get())) {
    require(t->getLength() <= 1048576, "OCIO 1D LUT length limit");
  } else if (auto* t = dynamic_cast<const OCIO::Lut3DTransform*>(node.get())) {
    require(t->getGridSize() <= 129, "OCIO 3D LUT grid limit");
  } else if (auto* t = dynamic_cast<const OCIO::FixedFunctionTransform*>(
                 node.get())) {
    require(t->getNumParams() <= 1048576,
            "OCIO fixed-function parameter limit");
  } else if (auto* t = dynamic_cast<const OCIO::GradingRGBCurveTransform*>(
                 node.get())) {
    const auto v = t->getValue();
    require(static_cast<bool>(v), "null RGB curves");
    for (int i = 0; i < OCIO::RGB_NUM_CURVES; ++i) {
      check_curve_structure(v->getCurve(static_cast<OCIO::RGBCurveType>(i)));
    }
  } else if (auto* t = dynamic_cast<const OCIO::GradingHueCurveTransform*>(
                 node.get())) {
    const auto v = t->getValue();
    require(static_cast<bool>(v), "null hue curves");
    for (int i = 0; i < OCIO::HUE_NUM_CURVES; ++i) {
      check_curve_structure(v->getCurve(static_cast<OCIO::HueCurveType>(i)));
    }
  } else {
    throw std::invalid_argument("unsupported OCIO transform type");
  }
}
void preflight(const OCIO::ConstTransformRcPtr& node, bool configured,
               unsigned& nodes, unsigned depth = 0) {
  require(node && depth < 64 && nodes++ < 4096,
          "OCIO authored transform depth/stage limit");
  if (auto* g = dynamic_cast<const OCIO::GroupTransform*>(node.get())) {
    for (int i = 0; i < g->getNumTransforms(); ++i) {
      preflight(g->getTransform(i), configured, nodes, depth + 1);
    }
  } else if (dynamic_cast<const OCIO::ColorSpaceTransform*>(node.get()) ||
             dynamic_cast<const OCIO::LookTransform*>(node.get()) ||
             dynamic_cast<const OCIO::DisplayViewTransform*>(node.get())) {
    require(configured,
            "symbolic OCIO transform requires an explicit config snapshot");

  } else if (auto* file =
                 dynamic_cast<const OCIO::FileTransform*>(node.get())) {
    const auto mode = file->getInterpolation();
    require(mode == OCIO::INTERP_DEFAULT || mode == OCIO::INTERP_BEST ||
                mode == OCIO::INTERP_LINEAR || mode == OCIO::INTERP_TETRAHEDRAL,
            "public interpolation is unsupported by pinned CPU LUT renderers");
    if (active_build && mode != OCIO::INTERP_DEFAULT &&
        mode != OCIO::INTERP_BEST) {
      active_build->strict_files.insert(file);
    }
  } else if (!dynamic_cast<const OCIO::BuiltinTransform*>(node.get())) {
    if (auto* lut = dynamic_cast<const OCIO::Lut1DTransform*>(node.get())) {
      require(lut->getInterpolation() == OCIO::INTERP_DEFAULT ||
                  lut->getInterpolation() == OCIO::INTERP_BEST ||
                  lut->getInterpolation() == OCIO::INTERP_LINEAR,
              "unsupported public 1D interpolation");
    }
    if (auto* lut = dynamic_cast<const OCIO::Lut3DTransform*>(node.get())) {
      require(lut->getInterpolation() == OCIO::INTERP_DEFAULT ||
                  lut->getInterpolation() == OCIO::INTERP_BEST ||
                  lut->getInterpolation() == OCIO::INTERP_LINEAR ||
                  lut->getInterpolation() == OCIO::INTERP_TETRAHEDRAL,
              "unsupported public 3D interpolation");
    }
    check_node_structure(node);
  }
}

OCIO::TransformRcPtr freeze(const OCIO::ConstTransformRcPtr& original,
                            unsigned& nodes, unsigned depth = 0) {
  require(original && depth < 64 && nodes++ < 4096,
          "OCIO transform depth/stage limit");
  ps_engine_cpp_tick(1);
  if (auto group = dynamic_cast<const OCIO::GroupTransform*>(original.get())) {
    auto out = OCIO::GroupTransform::Create();
    out->setDirection(group->getDirection());
    for (int i = 0; i < group->getNumTransforms(); ++i) {
      out->appendTransform(freeze(group->getTransform(i), nodes, depth + 1));
    }
    return out;
  }
  auto t = original->createEditableCopy();
  check_node_structure(t);
  if (auto* m = dynamic_cast<OCIO::MatrixTransform*>(t.get())) {
    double matrix[16], offset[4];
    m->getMatrix(matrix);
    m->getOffset(offset);
    require(matrix[3] == 0 && matrix[7] == 0 && matrix[11] == 0 &&
                matrix[12] == 0 && matrix[13] == 0 && matrix[14] == 0 &&
                matrix[15] == 1 && offset[3] == 0,
            "OCIO matrix couples or modifies alpha");
  } else if (auto* e = dynamic_cast<OCIO::ExponentTransform*>(t.get())) {
    double v[4];
    e->getValue(v);
    require(v[3] == 1 && e->getNegativeStyle() != OCIO::NEGATIVE_CLAMP,
            "OCIO exponent modifies alpha");
  } else if (auto* e =
                 dynamic_cast<OCIO::ExponentWithLinearTransform*>(t.get())) {
    double gamma[4], offset[4];
    e->getGamma(gamma);
    e->getOffset(offset);
    require(gamma[3] == 1 && offset[3] == 0,
            "OCIO linear exponent modifies alpha");
  } else if (auto* e =
                 dynamic_cast<OCIO::ExposureContrastTransform*>(t.get())) {
    e->makeExposureNonDynamic();
    e->makeContrastNonDynamic();
    e->makeGammaNonDynamic();
  } else if (auto* e = dynamic_cast<OCIO::GradingPrimaryTransform*>(t.get())) {
    e->makeNonDynamic();
  } else if (auto* e = dynamic_cast<OCIO::GradingRGBCurveTransform*>(t.get())) {
    e->makeNonDynamic();
  } else if (auto* e = dynamic_cast<OCIO::GradingToneTransform*>(t.get())) {
    e->makeNonDynamic();
  } else if (auto* e = dynamic_cast<OCIO::GradingHueCurveTransform*>(t.get())) {
    e->makeNonDynamic();
  } else if (!(dynamic_cast<OCIO::Lut1DTransform*>(t.get()) ||
               dynamic_cast<OCIO::Lut3DTransform*>(t.get()) ||
               dynamic_cast<OCIO::LogTransform*>(t.get()) ||
               dynamic_cast<OCIO::LogAffineTransform*>(t.get()) ||
               dynamic_cast<OCIO::LogCameraTransform*>(t.get()) ||
               dynamic_cast<OCIO::CDLTransform*>(t.get()) ||
               dynamic_cast<OCIO::RangeTransform*>(t.get()) ||
               dynamic_cast<OCIO::FixedFunctionTransform*>(t.get()))) {
    throw std::invalid_argument("Unknown alpha effect in resolved OCIO graph");
  }
  if (active_build) {
    std::ostringstream record;
    record.precision(17);
    if (auto* lut = dynamic_cast<OCIO::Lut1DTransform*>(t.get())) {
      record << "Lut1D length=" << lut->getLength()
             << " effective_interpolation=linear";
    } else if (auto* lut = dynamic_cast<OCIO::Lut3DTransform*>(t.get())) {
      record << "Lut3D grid=" << lut->getGridSize()
             << " effective_interpolation="
             << ((lut->getInterpolation() == OCIO::INTERP_BEST ||
                  lut->getInterpolation() == OCIO::INTERP_TETRAHEDRAL)
                     ? "tetrahedral"
                     : "linear");
    } else {
      record << *t;
    }
    active_build->record("resolved_static_node=" + record.str());
  }
  return t;
}
struct Engine final {
  OCIO::ConstCPUProcessorRcPtr cpu;
  ps_color_host_v1 host;
  ps_ocio_info_v1 info{};
  std::string trace;
};
void bootstrap() {
  static std::once_flag once;
  std::call_once(once, [] {
    // yaml-cpp's lexer owns lazy process-lifetime RegEx vectors. Initialize
    // every pinned expression outside an invocation, including syntax absent
    // from CreateRaw (anchors, tags, block scalars and quoted escapes).
    Scope process({}, nullptr, 0);
    (void)YAML::Exp::Empty();
    (void)YAML::Exp::Space();
    (void)YAML::Exp::Tab();
    (void)YAML::Exp::Blank();
    (void)YAML::Exp::Break();
    (void)YAML::Exp::BlankOrBreak();
    (void)YAML::Exp::Digit();
    (void)YAML::Exp::Alpha();
    (void)YAML::Exp::AlphaNumeric();
    (void)YAML::Exp::Word();
    (void)YAML::Exp::Hex();
    (void)YAML::Exp::NotPrintable();
    (void)YAML::Exp::Utf8_ByteOrderMark();
    (void)YAML::Exp::DocStart();
    (void)YAML::Exp::DocEnd();
    (void)YAML::Exp::DocIndicator();
    (void)YAML::Exp::BlockEntry();
    (void)YAML::Exp::Key();
    (void)YAML::Exp::KeyInFlow();
    (void)YAML::Exp::Value();
    (void)YAML::Exp::ValueInFlow();
    (void)YAML::Exp::ValueInJSONFlow();
    (void)YAML::Exp::Ampersand();
    (void)YAML::Exp::Comment();
    (void)YAML::Exp::Anchor();
    (void)YAML::Exp::AnchorEnd();
    (void)YAML::Exp::URI();
    (void)YAML::Exp::Tag();
    (void)YAML::Exp::PlainScalar();
    (void)YAML::Exp::PlainScalarInFlow();
    (void)YAML::Exp::EndScalar();
    (void)YAML::Exp::EndScalarInFlow();
    (void)YAML::Exp::ScanScalarEndInFlow();
    (void)YAML::Exp::ScanScalarEnd();
    (void)YAML::Exp::EscSingleQuote();
    (void)YAML::Exp::EscBreak();
    (void)YAML::Exp::ChompIndicator();
    (void)YAML::Exp::Chomp();
    // The emitter has two additional static expressions used by diagnostics.
    YAML::Emitter emitter;
    emitter << YAML::Flow << YAML::BeginSeq << "bootstrap" << YAML::EndSeq;
    OCIO::SetLoggingLevel(OCIO::LOGGING_LEVEL_NONE);
    (void)OCIO::FileTransform::GetNumFormats();
    (void)OCIO::BuiltinTransformRegistry::Get();
    (void)OCIO::BuiltinConfigRegistry::Get();
    auto config = OCIO::Config::CreateRaw();
    auto processor = config->getProcessor("raw", "raw");
    (void)processor->getOptimizedCPUProcessor(OCIO::OPTIMIZATION_NONE);
  });
}
std::unique_ptr<Engine> construct(const ps_ocio_request_v1& q) {
  auto p = proxy(q);
  yaml_limits(p->config);
  auto config = OCIO::Config::CreateFromConfigIOProxy(p)->createEditableCopy();
  config->setProcessorCacheFlags(OCIO::PROCESSOR_CACHE_OFF);
  auto context = config->getCurrentContext()->createEditableCopy();
  context->setWorkingDir("/");
  context->setConfigIOProxy(p);
  for (const auto& item : p->context) {
    context->setStringVar(item.first.c_str(), item.second.c_str());
  }
  for (const auto& item : p->spaces) {
    auto s = space(*config, "name:" + item.first);
    require(item.second ==
                (s->getReferenceSpaceType() == OCIO::REFERENCE_SPACE_SCENE
                     ? "scene"
                     : "display"),
            "OCIO declared reference space disagrees");
  }
  Build build;
  build.allow_bridge = q.allow_bridge;
  Observe observe(&build);
  build.record(std::string("resource=") + q.identity);
  build.record(std::string("direction=") + (q.inverse ? "inverse" : "forward"));
  for (int i = 0; i < context->getNumStringVars(); ++i) {
    const char* name = context->getStringVarNameByIndex(i);
    build.record(std::string("context=") + name + "=" +
                 context->getStringVar(name));
  }
  OCIO::ConstColorSpaceRcPtr source, destination;
  OCIO::ConstTransformRcPtr transform;
  const auto direction =
      q.inverse ? OCIO::TRANSFORM_DIR_INVERSE : OCIO::TRANSFORM_DIR_FORWARD;
  auto outer = direction;
  if (q.kind <= 2) {
    source = space(*config, q.source);
    if (q.kind != 1) {
      destination = space(*config, q.destination);
    }
    if (q.kind == 0) {
      auto t = OCIO::ColorSpaceTransform::Create();
      t->setSrc(source->getName());
      t->setDst(destination->getName());
      t->setDataBypass(q.data_bypass);
      transform = t;
    } else if (q.kind == 1) {
      bool display = false, view = false;
      for (int i = 0; i < config->getNumDisplaysAll(); ++i) {
        display |= std::string(q.display) == config->getDisplayAll(i);
      }
      require(display, "unknown exact OCIO display");
      for (int i = 0;
           i < config->getNumViews(OCIO::VIEW_DISPLAY_DEFINED, q.display);
           ++i) {
        view |= std::string(q.view) ==
                config->getView(OCIO::VIEW_DISPLAY_DEFINED, q.display, i);
      }
      // Shared views attached to the selected display are exposed by
      // getNumViews(display).
      for (int i = 0; i < config->getNumViews(q.display); ++i) {
        view |= std::string(q.view) == config->getView(q.display, i);
      }
      require(view, "unknown exact OCIO view");
      std::string output =
          config->getDisplayViewColorSpaceName(q.display, q.view);
      if (output == OCIO::OCIO_VIEW_USE_DISPLAY_NAME) {
        output = q.display;
      }
      destination = space(*config, "name:" + output);
      auto t = OCIO::DisplayViewTransform::Create();
      t->setSrc(source->getName());
      t->setDisplay(q.display);
      t->setView(q.view);
      t->setLooksBypass(q.looks_bypass);
      t->setDataBypass(q.data_bypass);
      transform = t;
      build.record(std::string("display=") + q.display + " view=" + q.view);
    } else {
      looks_exist(*config, q.looks);
      auto t = OCIO::LookTransform::Create();
      t->setSrc(source->getName());
      t->setDst(destination->getName());
      t->setLooks(q.looks);
      transform = t;
      build.record(std::string("looks=") + q.looks);
    }
    if (q.inverse) {
      std::swap(source, destination);
    }
  } else if (q.kind == 3) {
    auto n = named(*config, q.named);
    transform = OCIO::NamedTransform::GetTransform(n, direction);
    outer = OCIO::TRANSFORM_DIR_FORWARD;
    build.record(std::string("named=") + n->getName());
    build.record(std::string("named_definition=") +
                 (n->getTransform(direction)
                      ? (q.inverse ? "inverse" : "forward")
                      : (q.inverse ? "inverted_forward" : "inverted_inverse")));
  } else if (q.kind == 4) {
    auto t = OCIO::FileTransform::Create();
    t->setSrc(q.file);
    t->setInterpolation(interpolation(q.interpolation));
    t->setCCCId(q.ccc_id);
    transform = t;
    build.record(std::string("file=") + q.file + " ccc_id=" + q.ccc_id);
  } else if (q.kind == 5) {
    auto fragment = bytes_string(q.tree);
    require(!fragment.empty(), "empty transform tree text");
    yaml_limits(fragment);
    std::string wrapped = raw_config;
    wrapped +=
        "named_transforms:\n  - !<NamedTransform>\n    name: "
        "__photospider_tree__\n    transform: ";
    for (char c : fragment) {
      wrapped += c;
      if (c == '\n') {
        wrapped += "      ";
      }
    }
    wrapped += '\n';
    yaml_limits(wrapped);
    std::istringstream stream(wrapped);
    auto tree_config = OCIO::Config::CreateFromStream(stream);
    auto n = tree_config->getNamedTransform("__photospider_tree__");
    require(static_cast<bool>(n), "invalid typed transform tree");
    transform = n->getTransform(OCIO::TRANSFORM_DIR_FORWARD);
  } else {
    throw std::invalid_argument("invalid OCIO member");
  }
  if (q.kind >= 3) {
    if (q.expected_input && *q.expected_input) {
      source = space(*config, std::string("name:") + q.expected_input);
    }
    if (q.expected_output && *q.expected_output) {
      destination = space(*config, std::string("name:") + q.expected_output);
    }
  }
  auto check_endpoint = [&](const OCIO::ConstColorSpaceRcPtr& s,
                            const char* expected) {
    if (!s) {
      return;
    }
    if (expected && *expected) {
      require(std::string(expected) == s->getName(),
              "OCIO endpoint assertion disagrees");
    }
    if (q.semantic) {
      require(!s->isData(), "semantic OCIO endpoint isData");
    }
    if (q.snapshot.size) {
      require(p->spaces.count(s->getName()) != 0,
              "selected OCIO space missing from manifest declarations");
    }
  };
  check_endpoint(source, q.expected_input);
  check_endpoint(destination, q.expected_output);
  unsigned authored_nodes = 0;
  preflight(transform, q.snapshot.size != 0, authored_nodes);
  if (source) {
    build.record(std::string("canonical_input=") + source->getName());
  }
  if (destination) {
    build.record(std::string("canonical_output=") + destination->getName());
  }
  auto processor = config->getProcessor(context, transform, outer);
  auto resolved = processor->createGroupTransform();
  unsigned nodes = 0;
  auto frozen = freeze(resolved, nodes);
  auto fixed =
      config->getProcessor(context, frozen, OCIO::TRANSFORM_DIR_FORWARD);
  auto engine = std::make_unique<Engine>();
  engine->cpu = fixed->getOptimizedCPUProcessor(
      OCIO::BIT_DEPTH_F32, OCIO::BIT_DEPTH_F32,
      q.reference ? OCIO::OPTIMIZATION_NONE : OCIO::OPTIMIZATION_DEFAULT);
  require(!engine->cpu->isDynamic(), "unresolved OCIO dynamic state");
  if (source) {
    std::snprintf(engine->info.input, sizeof(engine->info.input), "%s",
                  source->getName());
    engine->info.input_reference = source->getReferenceSpaceType();
  }
  if (destination) {
    std::snprintf(engine->info.output, sizeof(engine->info.output), "%s",
                  destination->getName());
    engine->info.output_reference = destination->getReferenceSpaceType();
  }
  build.record(std::string("execution=") + (q.reference ? "NONE" : "DEFAULT"));
  build.record(std::string("cpu=") + engine->cpu->getCacheID());
  build.record("path=F32 absent-alpha point; per-instance static properties");
  engine->trace = std::move(build.trace);
  return engine;
}
}  // namespace
extern "C" void ps_engine_ocio_record(const char* key, const char* value) {
  if (active_build) {
    active_build->record(std::string(key) + "=" + value);
  }
}
extern "C" int ps_engine_ocio_file_enter(const void* file) {
  if (!active_build) {
    return 0;
  }
  const bool previous = active_build->strict_file;
  active_build->strict_file = active_build->strict_files.count(file) != 0;
  return previous;
}
extern "C" void ps_engine_ocio_file_leave(int previous) {
  if (active_build) {
    active_build->strict_file = previous;
  }
}
extern "C" int ps_engine_ocio_strict_interpolation() {
  return active_build && active_build->strict_file;
}
extern "C" void ps_engine_ocio_bridge(const char* name) {
  if (!active_build) {
    return;
  }
  active_build->record(std::string("implicit_reference_bridge=") + name);
  require(active_build->allow_bridge,
          "implicit OCIO reference-space bridge rejected");
}
extern "C" void* ps_color_ocio_create_v1(const ps_ocio_request_v1* q,
                                         const ps_color_host_v1* host,
                                         ps_ocio_info_v1* info, char* error,
                                         size_t size) {
  if (size) {
    error[0] = 0;
  }
  try {
    bootstrap();
    Scope scope(*host, error, size);
    auto e = construct(*q);
    if (host->work && !host->work(host->context, 0)) {
      return nullptr;
    }
    e->host = *host;
    *info = e->info;
    return e.release();
  } catch (const std::exception& exception) {
    if (size) {
      std::snprintf(error, size, "%s", exception.what());
    }
  } catch (...) {
    if (size) {
      std::snprintf(error, size, "OCIO construction failed");
    }
  }
  return nullptr;
}
extern "C" void ps_color_ocio_release_v1(void* p) {
  delete static_cast<Engine*>(p);
}
extern "C" int ps_color_ocio_apply_v1(void* p, float* planar, unsigned count) {
  const auto& e = *static_cast<Engine*>(p);
  try {
    Scope scope(e.host, nullptr, 0);
    scope.local.armed = 0;
    // Pinned CPUProcessor::applyRGB uses absent alpha +0, then exactly the same
    // F32 bit-depth ops and renderer sequence (numPixels=1) as the planar
    // oracle. It removes four allocations per point without changing the point
    // map.
    for (unsigned i = 0; i < count; ++i) {
      float pixel[3]{planar[i], planar[count + i], planar[2 * count + i]};
      e.cpu->applyRGB(pixel);
      for (unsigned c = 0; c < 3; ++c) {
        planar[c * count + i] = pixel[c];
      }
    }
    return !e.host.work || e.host.work(e.host.context, 0);
  } catch (...) {
    return 0;
  }
}
extern "C" const char* ps_color_ocio_trace_v1(const void* p) {
  return static_cast<const Engine*>(p)->trace.c_str();
}
