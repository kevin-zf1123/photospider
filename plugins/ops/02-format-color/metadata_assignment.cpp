#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_publication.hpp"
#include "01-numeric/sequence_profiles.hpp"
#include "execution/channel_assembly.hpp"
#include "photospider/format/metadata.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::metadata_internal {
namespace {
// This bounded tree is a transaction representation, not a second Value codec.
// Published semantics always use the canonical tensor-description-v4 facet.
struct Tree final {
  char kind = 'o';
  std::string bytes;
  std::map<std::string, Tree> fields;
};
struct Invalid final : std::runtime_error {
  using std::runtime_error::runtime_error;
};
Status invalid(const std::string& message) {
  return {ErrorCode::InvalidArgument,
          "metadata.assign: " + message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
std::uint64_t number(const std::string& text) {
  std::uint64_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() ||
      std::to_string(value) != text) {
    throw Invalid("expected canonical unsigned index");
  }
  return value;
}
Tree tree(const std::string& value) {
  return {'s', value, {}};
}
Tree tree(std::uint64_t value) {
  return {'u', std::to_string(value), {}};
}
Tree tree(std::int64_t value) {
  return {'i', std::to_string(value), {}};
}
Tree tree(double value) {
  if (!std::isfinite(value)) {
    throw Invalid("nonfinite descriptor number");
  }
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, 8);
  std::string bytes(8, '\0');
  for (unsigned i = 0; i < 8; ++i) {
    bytes[i] = static_cast<char>(bits >> (i * 8));
  }
  return {'d', std::move(bytes), {}};
}
Tree tree(const TensorRationalEndpoint& value) {
  std::string bytes(1, value.negative ? '\1' : '\0');
  for (const auto* words : {&value.numerator, &value.denominator}) {
    const auto count = static_cast<std::uint16_t>(words->size());
    bytes.push_back(static_cast<char>(count));
    bytes.push_back(static_cast<char>(count >> 8));
    for (const auto word : *words)
      for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(static_cast<char>(word >> (8 * i)));
  }
  return {'r', std::move(bytes), {}};
}
template <std::size_t N>
Tree tree(const std::array<double, N>& values);
Tree tree(const ColorProfileIdentity& p);
Tree tree(const TensorInterpretation& p);
Tree tree(const TensorChannelDescription& p);
Tree tree(const TensorEncoding& p);
Tree tree(const TensorSampling& p);
Tree tree(const TensorConfiguredSpace& p);
Tree tree(const TensorAnalyticBinding& p);
template <class T>
void optional(Tree* out, const char* key, const std::optional<T>& value) {
  if (value) {
    out->fields[key] = tree(*value);
  }
}
void text(Tree* out, const char* key, const std::string& value) {
  if (!value.empty()) {
    out->fields[key] = tree(value);
  }
}
template <std::size_t N>
Tree tree(const std::array<double, N>& values) {
  Tree out;
  for (std::size_t i = 0; i < N; ++i) {
    out.fields[std::to_string(i)] = tree(values[i]);
  }
  return out;
}
Tree tree(const ColorProfileIdentity& p) {
  Tree out;
  out.fields["byte_length"] = tree(p.byte_length);
  out.fields["sha256"] = {'b',
                          std::string(p.sha256.begin(), p.sha256.end()),
                          {}};
  return out;
}
Tree tree(const TensorEncoding& p) {
  Tree out;
  for (const auto& item : {std::make_pair("stored", &p.stored),
                           std::make_pair("decoded", &p.decoded)}) {
    Tree pair;
    for (std::size_t i = 0; i < 2; ++i) {
      pair.fields[std::to_string(i)] =
          std::visit([](const auto& n) { return tree(n); }, (*item.second)[i]);
    }
    out.fields[item.first] = std::move(pair);
  }
  return out;
}
Tree tree(const TensorSampling& p) {
  Tree out;
  out.fields["grid"] = tree(p.grid);
  out.fields["scale"] = tree(p.scale);
  out.fields["offset"] = tree(p.offset);
  return out;
}
Tree tree(const TensorConfiguredSpace& p) {
  Tree out;
  out.fields["config"] = tree(p.config);
  out.fields["space"] = tree(p.space);
  out.fields["reference_space"] = tree(p.reference_space);
  return out;
}
Tree tree(const TensorAnalyticBinding& p) {
  Tree out;
  text(&out, "model", p.model);
  text(&out, "primaries", p.primaries);
  text(&out, "transfer", p.transfer);
  text(&out, "reference", p.reference);
  optional(&out, "white", p.white);
  optional(&out, "primaries_xy", p.primaries_xy);
  out.fields["convention"] = tree(p.convention);
  for (const auto& entry :
       {std::make_pair("roles", &p.roles), std::make_pair("units", &p.units)}) {
    Tree list;
    for (std::size_t i = 0; i < entry.second->size(); ++i) {
      list.fields[std::to_string(i)] = tree((*entry.second)[i]);
    }
    out.fields[entry.first] = std::move(list);
  }
  return out;
}
Tree tree(const TensorInterpretation& p) {
  Tree out;
  text(&out, "model", p.model);
  text(&out, "primaries", p.primaries);
  text(&out, "transfer", p.transfer);
  text(&out, "reference", p.reference);
  text(&out, "association", p.association);
  optional(&out, "white", p.white);
  optional(&out, "primaries_xy", p.primaries_xy);
  optional(&out, "profile", p.profile);
  if (p.convention != "relative-v1") {
    out.fields["convention"] = tree(p.convention);
  }
  optional(&out, "configured", p.configured);
  optional(&out, "analytic_binding", p.analytic_binding);
  return out;
}
Tree tree(const TensorChannelDescription& p) {
  Tree out;
  text(&out, "name", p.name);
  text(&out, "role", p.role);
  text(&out, "unit", p.unit);
  optional(&out, "interpretation", p.interpretation);
  optional(&out, "encoding", p.encoding);
  optional(&out, "sampling", p.sampling);
  return out;
}
Tree tree(const TensorAxisDescription& p) {
  Tree out;
  text(&out, "name", p.name);
  text(&out, "unit", p.unit);
  out.fields["origin"] = tree(p.origin);
  out.fields["step"] = tree(p.step);
  return out;
}
template <class T>
Tree sequence(const std::vector<T>& values) {
  Tree out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    out.fields[std::to_string(i)] = tree(values[i]);
  }
  return out;
}
Tree tree(const TensorColorGroup& p) {
  Tree out;
  out.fields["name"] = tree(p.name);
  out.fields["indices"] = sequence(p.indices);
  out.fields["components"] = sequence(p.components);
  out.fields["interpretation"] = tree(p.interpretation);
  optional(&out, "alpha", p.alpha);
  return out;
}
Tree groups(const std::vector<TensorColorGroup>& values) {
  Tree out;
  for (const auto& value : values) {
    if (!out.fields.emplace(value.name, tree(value)).second) {
      throw Invalid("duplicate group name");
    }
  }
  return out;
}
Tree tree(const TensorDescription& p) {
  Tree out = tree(TensorInterpretation{p.model, p.primaries, p.transfer,
                                       p.reference, p.association, p.white,
                                       p.primaries_xy, p.profile, p.convention,
                                       p.configured, p.analytic_binding});
  if (p.channel_axis) {
    out.fields["channel_axis"] =
        tree(static_cast<std::uint64_t>(*p.channel_axis));
  }
  if (!p.channels.empty()) {
    out.fields["channels"] = sequence(p.channels);
  }
  optional(&out, "component", p.component);
  if (!p.axes.empty()) {
    out.fields["axes"] = sequence(p.axes);
  }
  if (!p.groups.empty()) {
    out.fields["groups"] = groups(p.groups);
  }
  optional(&out, "encoding", p.encoding);
  optional(&out, "sampling", p.sampling);
  return out;
}
Tree tree(const std::vector<std::uint64_t>& v) {
  return sequence(v);
}
Tree tree(const std::vector<TensorChannelDescription>& v) {
  return sequence(v);
}
Tree tree(const std::vector<TensorAxisDescription>& v) {
  return sequence(v);
}
Tree tree(const std::vector<TensorColorGroup>& v) {
  return groups(v);
}
Tree tree(const ValueFacet& v) {
  Tree out;
  out.fields["key"] = tree(v.key);
  out.fields["version"] = tree(static_cast<std::uint64_t>(v.version));
  out.fields["payload"] = {'b',
                           std::string(v.payload.begin(), v.payload.end()),
                           {}};
  return out;
}
void object(const Tree& t) {
  if (t.kind != 'o') {
    throw Invalid("expected typed record");
  }
}
const Tree* get(const Tree& t, const std::string& k) {
  object(t);
  auto i = t.fields.find(k);
  return i == t.fields.end() ? nullptr : &i->second;
}
void keys(const Tree& t, std::initializer_list<const char*> allowed) {
  object(t);
  for (const auto& f : t.fields) {
    if (std::none_of(allowed.begin(), allowed.end(),
                     [&](const char* key) { return f.first == key; })) {
      throw Invalid("unknown semantic field " + f.first);
    }
  }
}
std::string string(const Tree& t) {
  if (t.kind != 's') {
    throw Invalid("expected String");
  }
  return t.bytes;
}
std::uint64_t integer(const Tree& t) {
  if (t.kind != 'u') {
    throw Invalid("expected UInt64");
  }
  return number(t.bytes);
}
double real(const Tree& t) {
  if (t.kind != 'd' || t.bytes.size() != 8) {
    throw Invalid("expected Float64 bits");
  }
  std::uint64_t bits = 0;
  for (unsigned i = 0; i < 8; ++i) {
    bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(t.bytes[i]))
            << (i * 8);
  }
  double out = 0;
  std::memcpy(&out, &bits, 8);
  if (!std::isfinite(out)) {
    throw Invalid("nonfinite descriptor number");
  }
  return out;
}
std::string field(const Tree& t, const char* k) {
  const auto* p = get(t, k);
  return p ? string(*p) : "";
}
const Tree& required(const Tree& t, const char* k) {
  const auto* p = get(t, k);
  if (!p) {
    throw Invalid(std::string("missing field ") + k);
  }
  return *p;
}
template <std::size_t N>
std::array<double, N> array(const Tree& t) {
  object(t);
  if (t.fields.size() != N) {
    throw Invalid("wrong array size");
  }
  std::array<double, N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = real(required(t, std::to_string(i).c_str()));
  }
  return out;
}
ColorProfileIdentity identity(const Tree& t) {
  keys(t, {"byte_length", "sha256"});
  ColorProfileIdentity out;
  out.byte_length = integer(required(t, "byte_length"));
  const auto& bytes = required(t, "sha256");
  if (bytes.kind != 'b' || bytes.bytes.size() != 32) {
    throw Invalid("invalid resource identity");
  }
  std::copy(bytes.bytes.begin(), bytes.bytes.end(), out.sha256.begin());
  return out;
}
TensorEncoding encoding(const Tree& t) {
  keys(t, {"stored", "decoded"});
  TensorEncoding out;
  for (const auto& p : {std::make_pair("stored", &out.stored),
                        std::make_pair("decoded", &out.decoded)}) {
    const auto& pair = required(t, p.first);
    object(pair);
    if (pair.fields.size() != 2) {
      throw Invalid("encoding endpoint count");
    }
    for (std::size_t i = 0; i < 2; ++i) {
      const auto& n = required(pair, std::to_string(i).c_str());
      if (n.kind == 'i') {
        std::int64_t v = 0;
        auto r =
            std::from_chars(n.bytes.data(), n.bytes.data() + n.bytes.size(), v);
        if (r.ec != std::errc{} || r.ptr != n.bytes.data() + n.bytes.size() ||
            std::to_string(v) != n.bytes) {
          throw Invalid("invalid exact signed endpoint");
        }
        (*p.second)[i] = v;
      } else if (n.kind == 'r') {
        TensorRationalEndpoint exact;
        std::size_t at = 0;
        if (n.bytes.empty() || (n.bytes[0] != 0 && n.bytes[0] != 1))
          throw Invalid("invalid rational endpoint sign");
        exact.negative = n.bytes[at++] != 0;
        for (auto* words : {&exact.numerator, &exact.denominator}) {
          if (n.bytes.size() - at < 2)
            throw Invalid("short rational endpoint");
          const auto count = static_cast<std::uint16_t>(
              static_cast<std::uint8_t>(n.bytes[at]) |
              (static_cast<std::uint16_t>(
                   static_cast<std::uint8_t>(n.bytes[at + 1]))
               << 8));
          at += 2;
          if (!count || count > 128 || n.bytes.size() - at < count * 4)
            throw Invalid("invalid rational endpoint limbs");
          words->resize(count);
          for (auto& word : *words) {
            word = 0;
            for (unsigned shift = 0; shift < 4; ++shift)
              word |= static_cast<std::uint32_t>(
                          static_cast<std::uint8_t>(n.bytes[at++]))
                      << (8 * shift);
          }
        }
        if (at != n.bytes.size())
          throw Invalid("trailing rational endpoint bytes");
        (*p.second)[i] = std::move(exact);
      } else {
        (*p.second)[i] = real(n);
      }
    }
  }
  return out;
}
TensorSampling sampling(const Tree& t) {
  keys(t, {"grid", "scale", "offset"});
  return {string(required(t, "grid")), array<2>(required(t, "scale")),
          array<2>(required(t, "offset"))};
}
TensorConfiguredSpace configured(const Tree& t) {
  keys(t, {"config", "space", "reference_space"});
  return {identity(required(t, "config")), string(required(t, "space")),
          string(required(t, "reference_space"))};
}
TensorAnalyticBinding binding(const Tree& t) {
  keys(t, {"model", "primaries", "transfer", "reference", "white",
           "primaries_xy", "convention", "roles", "units"});
  TensorAnalyticBinding out;
  out.model = field(t, "model");
  out.primaries = field(t, "primaries");
  out.transfer = field(t, "transfer");
  out.reference = field(t, "reference");
  if (auto* p = get(t, "white")) {
    out.white = array<2>(*p);
  }
  if (auto* p = get(t, "primaries_xy")) {
    out.primaries_xy = array<6>(*p);
  }
  out.convention = string(required(t, "convention"));
  for (const auto& pair : {std::make_pair("roles", &out.roles),
                           std::make_pair("units", &out.units)}) {
    const auto& list = required(t, pair.first);
    object(list);
    for (std::size_t i = 0; i < list.fields.size(); ++i) {
      pair.second->push_back(string(required(list, std::to_string(i).c_str())));
    }
  }
  return out;
}
TensorInterpretation interpretation(const Tree& t, bool check = true) {
  if (check) {
    keys(t, {"model", "primaries", "transfer", "reference", "association",
             "white", "primaries_xy", "profile", "convention", "configured",
             "analytic_binding"});
  }
  TensorInterpretation out;
  out.model = field(t, "model");
  out.primaries = field(t, "primaries");
  out.transfer = field(t, "transfer");
  out.reference = field(t, "reference");
  out.association = field(t, "association");
  if (auto* p = get(t, "white")) {
    out.white = array<2>(*p);
  }
  if (auto* p = get(t, "primaries_xy")) {
    out.primaries_xy = array<6>(*p);
  }
  if (auto* p = get(t, "profile")) {
    keys(*p, {"byte_length", "sha256"});
    ColorProfileIdentity id;
    id.byte_length = integer(required(*p, "byte_length"));
    const auto& sha = required(*p, "sha256");
    if (sha.kind != 'b' || sha.bytes.size() != 32) {
      throw Invalid("invalid profile identity");
    }
    std::copy(sha.bytes.begin(), sha.bytes.end(), id.sha256.begin());
    out.profile = id;
  }
  if (auto* p = get(t, "convention")) {
    out.convention = string(*p);
  }
  if (auto* p = get(t, "configured")) {
    out.configured = configured(*p);
  }
  if (auto* p = get(t, "analytic_binding")) {
    out.analytic_binding = binding(*p);
  }
  return out;
}
TensorChannelDescription channel(const Tree& t) {
  keys(t, {"name", "role", "unit", "interpretation", "encoding", "sampling"});
  TensorChannelDescription out{field(t, "name"), field(t, "role"),
                               field(t, "unit")};
  if (auto* p = get(t, "interpretation")) {
    out.interpretation = interpretation(*p);
  }
  if (auto* p = get(t, "encoding")) {
    out.encoding = encoding(*p);
  }
  if (auto* p = get(t, "sampling")) {
    out.sampling = sampling(*p);
  }
  return out;
}
TensorAxisDescription axis(const Tree& t) {
  keys(t, {"name", "unit", "origin", "step"});
  TensorAxisDescription out;
  out.name = field(t, "name");
  out.unit = field(t, "unit");
  if (auto* p = get(t, "origin")) {
    out.origin = real(*p);
  }
  if (auto* p = get(t, "step")) {
    out.step = real(*p);
  }
  return out;
}
template <class F>
auto sequence(const Tree& t, F parse) {
  object(t);
  using T = decltype(parse(t));
  std::vector<T> out;
  for (std::size_t i = 0; i < t.fields.size(); ++i) {
    out.push_back(parse(required(t, std::to_string(i).c_str())));
  }
  return out;
}
TensorColorGroup group(const Tree& t) {
  keys(t, {"name", "indices", "components", "interpretation", "alpha"});
  TensorColorGroup out;
  out.name = string(required(t, "name"));
  out.indices = sequence(required(t, "indices"), integer);
  out.components = sequence(required(t, "components"), channel);
  out.interpretation = interpretation(required(t, "interpretation"));
  if (auto* p = get(t, "alpha")) {
    out.alpha = integer(*p);
  }
  return out;
}
TensorDescription description(const Tree& t) {
  keys(t, {"channel_axis", "channels", "component", "axes", "groups", "model",
           "primaries", "transfer", "reference", "association", "white",
           "primaries_xy", "profile", "convention", "configured",
           "analytic_binding", "encoding", "sampling"});
  auto i = interpretation(t, false);
  TensorDescription out;
  out.model = i.model;
  out.primaries = i.primaries;
  out.transfer = i.transfer;
  out.reference = i.reference;
  out.association = i.association;
  out.white = i.white;
  out.primaries_xy = i.primaries_xy;
  out.profile = i.profile;
  out.convention = i.convention;
  out.configured = i.configured;
  out.analytic_binding = i.analytic_binding;
  if (auto* p = get(t, "encoding")) {
    out.encoding = encoding(*p);
  }
  if (auto* p = get(t, "sampling")) {
    out.sampling = sampling(*p);
  }
  if (auto* p = get(t, "channel_axis")) {
    auto n = integer(*p);
    if (n >= 8) {
      throw Invalid("channel_axis outside rank limit");
    }
    out.channel_axis = static_cast<std::uint32_t>(n);
  }
  if (auto* p = get(t, "channels")) {
    out.channels = sequence(*p, channel);
  }
  if (auto* p = get(t, "component")) {
    out.component = channel(*p);
  }
  if (auto* p = get(t, "axes")) {
    out.axes = sequence(*p, axis);
  }
  if (auto* p = get(t, "groups")) {
    object(*p);
    for (const auto& entry : p->fields) {
      auto g = group(entry.second);
      if (g.name != entry.first) {
        throw Invalid("group identity disagrees with path");
      }
      out.groups.push_back(std::move(g));
    }
  }
  return out;
}
// Wire grammar: kind + canonical decimal byte length/count + ':' + contents.
// Ordered map keys, bounded bytes, depth and nodes make parsing deterministic.
void encode(const Tree& t, std::string* out) {
  *out += t.kind;
  *out +=
      std::to_string(t.kind == 'o' ? t.fields.size() : t.bytes.size()) + ':';
  if (t.kind == 'o') {
    for (const auto& f : t.fields) {
      encode(tree(f.first), out);
      encode(f.second, out);
    }
  } else {
    *out += t.bytes;
  }
  if (out->size() > 4096) {
    throw Invalid("edit transaction exceeds 4096 encoded bytes");
  }
}
Tree decode(const std::string& bytes, std::size_t* at, unsigned depth,
            unsigned* nodes) {
  if (depth > 12 || ++*nodes > 1024 || *at >= bytes.size()) {
    throw Invalid("bounded edit codec exhausted");
  }
  Tree t;
  t.kind = bytes[(*at)++];
  if (std::string("osiudbr").find(t.kind) == std::string::npos) {
    throw Invalid("unknown edit value type");
  }
  auto colon = bytes.find(':', *at);
  if (colon == std::string::npos) {
    throw Invalid("truncated edit header");
  }
  auto count = number(bytes.substr(*at, colon - *at));
  *at = colon + 1;
  if (count > bytes.size() - *at) {
    throw Invalid("truncated edit value");
  }
  if (t.kind == 'o') {
    std::string previous;
    for (std::uint64_t i = 0; i < count; ++i) {
      auto key = string(decode(bytes, at, depth + 1, nodes));
      if (key.empty() || (i && key <= previous)) {
        throw Invalid("noncanonical edit field order");
      }
      previous = key;
      t.fields.emplace(std::move(key), decode(bytes, at, depth + 1, nodes));
    }
  } else {
    t.bytes = bytes.substr(*at, count);
    *at += count;
  }
  return t;
}
std::string hex(const std::string& bytes) {
  const char* digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}
Tree parse(const std::string& param) {
  if (param.size() < 6 || param.size() > 8192 || param.size() % 2) {
    throw Invalid("invalid edit codec size");
  }
  std::string bytes;
  const std::string digits = "0123456789abcdef";
  for (std::size_t i = 0; i < param.size(); i += 2) {
    auto a = digits.find(param[i]), b = digits.find(param[i + 1]);
    if (a == std::string::npos || b == std::string::npos) {
      throw Invalid("noncanonical edit hex");
    }
    bytes += static_cast<char>((a << 4) | b);
  }
  std::size_t at = 0;
  unsigned nodes = 0;
  auto out = decode(bytes, &at, 0, &nodes);
  if (at != bytes.size()) {
    throw Invalid("trailing edit data");
  }
  return out;
}
std::vector<std::string> path(const std::string& text) {
  if (text.empty() || text[0] != '/' || text.size() > 1024) {
    throw Invalid("invalid path " + text);
  }
  std::vector<std::string> out;
  for (std::size_t i = 1; i <= text.size();) {
    auto end = text.find('/', i);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string part;
    for (; i < end; ++i) {
      char c = text[i];
      if (c == '~') {
        if (++i == end || (text[i] != '0' && text[i] != '1')) {
          throw Invalid("invalid path escape");
        }
        c = text[i] == '0' ? '~' : '/';
      }
      part += c;
    }
    if (part.empty()) {
      throw Invalid("empty path segment");
    }
    out.push_back(std::move(part));
    i = end + 1;
  }
  if (out.size() > 9) {
    throw Invalid("path depth limit");
  }
  return out;
}
bool contains(std::initializer_list<const char*> keys,
              const std::string& value) {
  return std::any_of(keys.begin(), keys.end(),
                     [&](const char* s) { return s == value; });
}
void schema_path(const std::vector<std::string>& p) {
  if (p[0] == "annotations") {
    if (p.size() != 2 || p[1].size() > 256 ||
        std::any_of(p[1].begin(), p[1].end(),
                    [](unsigned char c) { return c < 0x21 || c > 0x7e; }) ||
        p[1].compare(0, 12, "photospider.") == 0) {
      throw Invalid("annotation path must name an opaque facet key");
    }
    return;
  }
  if (p[0] != "semantic") {
    throw Invalid("unknown path namespace");
  }
  std::string kind = "description";
  for (std::size_t i = 1; i < p.size(); ++i) {
    const auto& f = p[i];
    if (kind == "description" || kind == "interpretation") {
      if (contains(
              {"model", "primaries", "transfer", "reference", "association",
               "white", "primaries_xy", "profile", "convention"},
              f)) {
        kind = "leaf";
      } else if (contains({"configured", "analytic_binding"}, f)) {
        kind = f;
      } else if (kind == "description" &&
                 contains({"encoding", "sampling"}, f)) {
        kind = f;
      } else if (kind == "description" && f == "channel_axis") {
        kind = "leaf";
      } else if (kind == "description" && f == "component") {
        kind = "channel";
      } else if (kind == "description" &&
                 contains({"channels", "axes", "groups"}, f)) {
        kind = f;
      } else {
        throw Invalid("unknown semantic field " + f);
      }
    } else if (kind == "channels" || kind == "axes" || kind == "groups" ||
               kind == "components") {
      if (kind == "axes" || kind == "components") {
        number(f);
      }
      if (kind == "channels") {
        const auto colon = f.find(':');
        if (colon == std::string::npos || colon + 1 == f.size())
          throw Invalid("invalid channel selector");
        const auto match = f.substr(0, colon);
        if (match == "index")
          number(f.substr(colon + 1));
        else if (match != "name" && match != "role")
          throw Invalid("unknown selector namespace");
      }
      kind = kind == "axes" ? "axis" : kind == "groups" ? "group" : "channel";
    } else if (kind == "channel") {
      if (contains({"name", "role", "unit"}, f)) {
        kind = "leaf";
      } else if (f == "interpretation") {
        kind = "interpretation";
      } else if (contains({"encoding", "sampling"}, f)) {
        kind = f;
      } else {
        throw Invalid("unknown channel field " + f);
      }
    } else if (kind == "axis") {
      if (!contains({"name", "unit", "origin", "step"}, f)) {
        throw Invalid("unknown axis field " + f);
      }
      kind = "leaf";
    } else if (kind == "group") {
      if (f == "interpretation") {
        kind = "interpretation";
      } else if (f == "components") {
        kind = "components";
      } else if (contains({"name", "indices", "alpha"}, f)) {
        kind = "leaf";
      } else {
        throw Invalid("unknown group field " + f);
      }
    } else if (kind == "encoding") {
      if (!contains({"stored", "decoded"}, f)) {
        throw Invalid("unknown encoding field");
      }
      kind = "endpoints";
    } else if (kind == "endpoints") {
      if (f != "0" && f != "1") {
        throw Invalid("invalid endpoint index");
      }
      kind = "leaf";
    } else if (kind == "sampling") {
      if (!contains({"grid", "scale", "offset"}, f)) {
        throw Invalid("unknown sampling field");
      }
      kind = "leaf";
    } else if (kind == "configured") {
      if (!contains({"config", "space", "reference_space"}, f)) {
        throw Invalid("unknown configured field");
      }
      kind = "leaf";
    } else if (kind == "analytic_binding") {
      if (!contains({"model", "primaries", "transfer", "reference", "white",
                     "primaries_xy", "convention", "roles", "units"},
                    f)) {
        throw Invalid("unknown analytic binding field");
      }
      kind = "leaf";
    } else {
      throw Invalid("path descends through atomic field");
    }
  }
}
std::vector<std::string> resolve(std::vector<std::string> p,
                                 const TensorDescription& source,
                                 const ValueDescriptor& d,
                                 bool ignore_absent = false) {
  schema_path(p);
  if (p.size() >= 3 && p[0] == "semantic" && p[1] == "channels") {
    const auto& selector = p[2];
    const auto colon = selector.find(':');
    if (colon == std::string::npos) {
      throw Invalid("channel selector needs index:/name:/role:");
    }
    auto kind = selector.substr(0, colon), value = selector.substr(colon + 1);
    std::uint64_t index = 0;
    if (kind == "index") {
      index = number(value);
    } else if (kind == "name" || kind == "role") {
      unsigned matches = 0;
      for (std::size_t i = 0; i < source.channels.size(); ++i) {
        if ((kind == "name" ? source.channels[i].name
                            : source.channels[i].role) == value) {
          index = i;
          ++matches;
        }
      }
      if (!value.empty() && matches == 0 && ignore_absent) {
        return {};
      }
      if (value.empty() || matches != 1) {
        throw Invalid("missing or ambiguous original channel selector");
      }
    } else {
      throw Invalid("unknown channel selector namespace");
    }
    if (!source.channel_axis || index >= d.shape[*source.channel_axis]) {
      throw Invalid("channel selector outside original axis");
    }
    p[2] = std::to_string(index);
  }
  if (p.size() >= 3 && p[0] == "semantic" && p[1] == "axes" &&
      number(p[2]) >= d.shape.size()) {
    throw Invalid("axis selector outside rank");
  }
  return p;
}
bool prefix(const std::vector<std::string>& a,
            const std::vector<std::string>& b) {
  return a.size() <= b.size() && std::equal(a.begin(), a.end(), b.begin());
}
struct Edit final {
  std::vector<std::string> path;
  const Tree* value = nullptr;
};
void apply(Tree* root, const Edit& edit, bool ignore) {
  Tree* parent = root;
  for (std::size_t i = 0; i + 1 < edit.path.size(); ++i) {
    object(*parent);
    auto found = parent->fields.find(edit.path[i]);
    if (found == parent->fields.end()) {
      if (!edit.value && ignore) {
        return;
      }
      // Empty collections are legal parents, but incomplete records are not.
      if (i == 1 && edit.path[0] == "semantic" && edit.path[i] == "groups" &&
          edit.path.size() == 3 && edit.value) {
        found = parent->fields.emplace(edit.path[i], Tree{}).first;
      } else {
        throw Invalid("missing edit parent " + edit.path[i]);
      }
    }
    parent = &found->second;
  }
  object(*parent);
  const auto& leaf = edit.path.back();
  if (edit.value) {
    parent->fields[leaf] = *edit.value;
    return;
  }
  if (!parent->fields.count(leaf)) {
    if (ignore) {
      return;
    }
    throw Invalid("missing deletion target " + leaf);
  }
  if (edit.path.size() == 1 ||
      (edit.path.size() == 3 && edit.path[0] == "semantic" &&
       (edit.path[1] == "channels" || edit.path[1] == "axes"))) {
    parent->fields[leaf] = Tree{};
  } else {
    parent->fields.erase(leaf);
  }
}
std::string option(const std::map<std::string, ParameterValue>& p,
                   const char* key, const char* fallback) {
  auto i = p.find(key);
  return i == p.end() ? fallback : std::get<std::string>(i->second);
}
void options(const std::map<std::string, ParameterValue>& p) {
  if (!contains({"patch", "replace"}, option(p, "mode", "patch")) ||
      !contains({"error", "cascade"}, option(p, "dependencies", "error")) ||
      !contains({"error", "ignore"}, option(p, "missing", "error")) ||
      !contains({"auto", "view", "materialize"}, option(p, "layout", "auto"))) {
    throw Invalid("invalid mode/dependencies/missing/layout");
  }
}
void static_value(const std::vector<std::string>& p, const Tree& value) {
  schema_path(p);
  const auto& leaf = p.back();
  if (p[0] == "annotations") {
    keys(value, {"key", "version", "payload"});
    if (string(required(value, "key")) != p[1] ||
        integer(required(value, "version")) == 0 ||
        integer(required(value, "version")) > UINT32_MAX ||
        required(value, "payload").kind != 'b') {
      throw Invalid("invalid annotation value");
    }
  } else if (p.size() == 1) {
    auto parsed = description(value);
    auto encoded = encode_tensor_description(parsed);
    if (!encoded.ok()) {
      throw Invalid(encoded.status().message);
    }
  } else if (leaf == "channels") {
    sequence(value, channel);
  } else if (leaf == "axes") {
    sequence(value, axis);
  } else if (leaf == "groups") {
    object(value);
    for (const auto& g : value.fields) {
      group(g.second);
    }
  } else if (leaf == "component" || (p.size() == 3 && p[1] == "channels") ||
             (p.size() == 5 && p[1] == "groups" && p[3] == "components")) {
    channel(value);
  } else if (p.size() == 3 && p[1] == "axes") {
    axis(value);
  } else if (p.size() == 3 && p[1] == "groups") {
    auto g = group(value);
    if (g.name != leaf) {
      throw Invalid("group identity disagrees with path");
    }
  } else if (leaf == "components") {
    sequence(value, channel);
  } else if (leaf == "indices") {
    sequence(value, integer);
  } else if (leaf == "interpretation") {
    interpretation(value);
  } else if (leaf == "encoding") {
    encoding(value);
  } else if (leaf == "sampling") {
    sampling(value);
  } else if (leaf == "configured") {
    configured(value);
  } else if (leaf == "analytic_binding") {
    binding(value);
  } else if (leaf == "channel_axis" || leaf == "alpha") {
    integer(value);
  } else if (leaf == "origin" || leaf == "step") {
    real(value);
  } else if (leaf == "white" || leaf == "scale" || leaf == "offset") {
    array<2>(value);
  } else if (leaf == "primaries_xy") {
    array<6>(value);
  } else if (leaf == "profile" || leaf == "config") {
    identity(value);
  } else if (leaf == "stored" || leaf == "decoded") {
    Tree e;
    e.fields["stored"] = value;
    e.fields["decoded"] = value;
    encoding(e);
  } else if (p.size() > 2 &&
             (p[p.size() - 2] == "stored" || p[p.size() - 2] == "decoded")) {
    if (value.kind != 'i' && value.kind != 'd' && value.kind != 'r') {
      throw Invalid("endpoint must be exact Int64, Float64 or rational");
    }
  } else if (leaf == "roles" || leaf == "units") {
    sequence(value, string);
  } else {
    string(value);
  }
}
Tree transaction(const format::MetadataOptions& o) {
  Tree out;
  out.fields["version"] = tree(std::uint64_t{1});
  Tree sets, removes;
  for (const auto& entry : o.set) {
    auto value = std::visit([](const auto& v) { return tree(v); }, entry.value);
    static_value(path(entry.path), value);
    if (!sets.fields.emplace(entry.path, std::move(value)).second) {
      throw Invalid("duplicate set path");
    }
  }
  for (const auto& entry : o.remove) {
    schema_path(path(entry));
    if (!removes.fields.emplace(entry, Tree{}).second) {
      throw Invalid("duplicate remove path");
    }
  }
  out.fields["set"] = std::move(sets);
  out.fields["remove"] = std::move(removes);
  if (o.description) {
    out.fields["description"] = tree(*o.description);
  }
  return out;
}
}  // namespace
namespace {
Result<OperationPreparation> prepare(
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& params,
    plugin_internal::numeric_ops::SequenceProfile profile) try {
  using Answer = Result<OperationPreparation>;
  auto available =
      plugin_internal::numeric_ops::sequence_profile_available(profile);
  if (!available.ok()) {
    return Answer(available);
  }
  options(params);
  const auto& input = inputs.at(0);
  TensorDescription source;
  Tree original;
  original.fields["annotations"] = Tree{};
  for (const auto& facet : input.facets) {
    if (facet.key == "photospider.tensor-description") {
      auto decoded = decode_tensor_description(facet);
      if (!decoded.ok()) {
        return Answer(decoded.status());
      }
      source = decoded.take_value();
    } else if (facet.key.compare(0, 12, "photospider.") != 0) {
      original.fields["annotations"].fields[facet.key] = tree(facet);
    } else {
      return Answer(invalid(
          "registered legacy facet requires explicit import: " + facet.key));
    }
  }
  auto status = validate_tensor_description(source, input.descriptor);
  if (!status.ok()) {
    return Answer(status);
  }
  original.fields["semantic"] = tree(source);
  const auto encoded = params.find("edits");
  Tree edits = encoded == params.end()
                   ? transaction({})
                   : parse(std::get<std::string>(encoded->second));
  keys(edits, {"version", "set", "remove", "description"});
  if (integer(required(edits, "version")) != 1) {
    throw Invalid("unsupported edit codec version");
  }
  const bool replace = option(params, "mode", "patch") == "replace";
  if (replace != (get(edits, "description") != nullptr)) {
    throw Invalid("description required exactly in replace mode");
  }
  Tree candidate = original;
  if (replace) {
    candidate.fields["semantic"] = required(edits, "description");
  }
  std::vector<Edit> operations;
  for (const auto* action : {"set", "remove"}) {
    const auto& entries = required(edits, action);
    object(entries);
    for (const auto& entry : entries.fields) {
      const bool setting = std::string(action) == "set";
      auto p =
          resolve(path(entry.first), source, input.descriptor,
                  !setting && option(params, "missing", "error") == "ignore");
      if (p.empty()) {
        continue;
      }
      if (replace && p[0] != "annotations") {
        throw Invalid("replace edits must name annotations");
      }
      if (setting) {
        static_value(path(entry.first), entry.second);
      }
      if (!setting &&
          (entry.second.kind != 'o' || !entry.second.fields.empty())) {
        throw Invalid("remove record must be empty");
      }
      operations.push_back({std::move(p), setting ? &entry.second : nullptr});
    }
  }
  std::sort(operations.begin(), operations.end(),
            [](const auto& a, const auto& b) { return a.path < b.path; });
  for (std::size_t i = 1; i < operations.size(); ++i) {
    if (prefix(operations[i - 1].path, operations[i].path)) {
      throw Invalid("duplicate or overlapping resolved edit paths");
    }
  }
  for (const auto& edit : operations) {
    apply(&candidate, edit, option(params, "missing", "error") == "ignore");
  }
  auto& semantics = candidate.fields.at("semantic");
  std::set<std::vector<std::string>> changed_paths, assigned_paths;
  for (const auto& e : operations) {
    changed_paths.insert(e.path);
    if (e.value) {
      assigned_paths.insert(e.path);
    }
  }
  const auto intersects = [](const auto& paths,
                             const std::vector<std::string>& p) {
    auto next = paths.lower_bound(p);
    if (next != paths.end() && prefix(p, *next)) {
      return true;
    }
    auto ancestor = p;
    while (!ancestor.empty()) {
      if (paths.count(ancestor)) {
        return true;
      }
      ancestor.pop_back();
    }
    return false;
  };
  const auto protected_path = [&](const std::vector<std::string>& p) {
    return replace || intersects(assigned_paths, p);
  };
  const bool cascade = option(params, "dependencies", "error") == "cascade";
  if (cascade) {
    // Closed v2 dependency graph: channel-axis -> channel table/groups;
    // group required fields/component assertions -> that complete group.
    // Groups do not reference each other, so one monotone pass is sufficient.
    if (!get(semantics, "channel_axis")) {
      for (const auto* key : {"channels", "groups"}) {
        if (get(semantics, key)) {
          if (protected_path({"semantic", key})) {
            throw Invalid("cascade would remove explicit target");
          }
          semantics.fields.erase(key);
        }
      }
    }
    const auto affected = [&](const std::vector<std::string>& p) {
      return intersects(changed_paths, p);
    };
    const auto cleanup = [&](Tree* record,
                             const std::vector<std::string>& location,
                             const char* field_name) {
      auto found = record->fields.find(field_name);
      if (found == record->fields.end()) {
        return;
      }
      auto p = location;
      p.push_back(field_name);
      if (!affected(p)) {
        return;
      }
      bool valid = true;
      try {
        TensorDescription check;
        check.component.emplace();
        if (std::string(field_name) == "encoding") {
          check.component->encoding = encoding(found->second);
        } else if (std::string(field_name) == "sampling") {
          check.component->sampling = sampling(found->second);
        } else {
          check.component->interpretation = interpretation(found->second);
        }
        valid = validate_tensor_description(check, input.descriptor).ok();
      } catch (const Invalid&) {
        valid = false;
      }
      if (!valid) {
        if (protected_path(p)) {
          throw Invalid("cascade would remove explicit " +
                        std::string(field_name));
        }
        record->fields.erase(found);
      }
    };
    cleanup(&semantics, {"semantic"}, "encoding");
    cleanup(&semantics, {"semantic"}, "sampling");
    const std::vector<std::string> interpretation_fields{
        "model",       "primaries",  "transfer",        "reference",
        "association", "white",      "primaries_xy",    "profile",
        "convention",  "configured", "analytic_binding"};
    bool changed_interpretation = false;
    Tree global_interpretation;
    for (const auto& key : interpretation_fields) {
      changed_interpretation =
          changed_interpretation || affected({"semantic", key});
      auto at = semantics.fields.find(key);
      if (at != semantics.fields.end()) {
        global_interpretation.fields.emplace(*at);
      }
    }
    if (changed_interpretation) {
      bool valid = true;
      try {
        TensorDescription check;
        check.component.emplace();
        check.component->interpretation = interpretation(global_interpretation);
        valid = validate_tensor_description(check, input.descriptor).ok();
      } catch (const Invalid&) {
        valid = false;
      }
      if (!valid) {
        for (const auto& key : interpretation_fields) {
          if (protected_path({"semantic", key})) {
            throw Invalid("cascade would remove explicit root interpretation");
          }
        }
        for (const auto& key : interpretation_fields) {
          semantics.fields.erase(key);
        }
      }
    }

    for (const auto* collection : {"channels", "component"}) {
      auto at = semantics.fields.find(collection);
      if (at == semantics.fields.end()) {
        continue;
      }
      if (std::string(collection) == "component") {
        for (const auto* field : {"encoding", "sampling", "interpretation"}) {
          cleanup(&at->second, {"semantic", "component"}, field);
        }
      } else {
        object(at->second);
        for (auto& c : at->second.fields) {
          for (const auto* field : {"encoding", "sampling", "interpretation"}) {
            cleanup(&c.second, {"semantic", "channels", c.first}, field);
          }
        }
      }
    }
    // Each complete group depends only on its own record, common code/grid
    // declarations, its referenced channels, and intersecting explicit groups.
    // Build those edges once; visit each affected group once. Validation uses a
    // compact component table rather than copying all M source records per
    // group.
    auto groups_it = semantics.fields.find("groups");
    if (groups_it != semantics.fields.end()) {
      object(groups_it->second);
      Tree independent = semantics;
      independent.fields.erase("groups");
      auto base = description(independent);
      auto old_groups = get(original.fields.at("semantic"), "groups");
      std::map<std::string, TensorColorGroup> assigned_groups;
      std::map<std::uint64_t, std::set<std::string>> assigned_channels;
      for (const auto& entry : groups_it->second.fields) {
        if (!protected_path({"semantic", "groups", entry.first})) {
          continue;
        }
        auto g = group(entry.second);
        for (auto index : g.indices) {
          assigned_channels[index].insert(entry.first);
        }
        assigned_groups.emplace(entry.first, std::move(g));
      }

      for (auto it = groups_it->second.fields.begin();
           it != groups_it->second.fields.end();) {
        const std::vector<std::string> group_path{"semantic", "groups",
                                                  it->first};
        bool touched = affected(group_path) ||
                       affected({"semantic", "encoding"}) ||
                       affected({"semantic", "sampling"}) ||
                       affected({"semantic", "channel_axis"});
        TensorColorGroup g;
        bool valid = true;
        try {
          g = group(it->second);
        } catch (const Invalid&) {
          valid = false;
        }
        if (valid) {
          for (auto index : g.indices) {
            touched = touched ||
                      affected({"semantic", "channels", std::to_string(index)});
          }
        }
        std::set<std::string> peers;
        if (valid) {
          for (auto index : g.indices) {
            auto found = assigned_channels.find(index);
            if (found == assigned_channels.end()) {
              continue;
            }
            for (const auto& peer : found->second) {
              if (peer != it->first) {
                peers.insert(peer);
              }
            }
          }
        }
        touched = touched || !peers.empty();
        if (!touched) {
          ++it;
          continue;
        }
        if (valid) {
          TensorDescription check;
          check.channel_axis = 0;
          check.encoding = base.encoding;
          check.sampling = base.sampling;
          const auto extent = base.channel_axis
                                  ? input.descriptor.shape[*base.channel_axis]
                                  : 0;
          std::vector<TensorColorGroup> related{std::move(g)};
          for (const auto& peer : peers) {
            related.push_back(assigned_groups.at(peer));
          }
          std::map<std::uint64_t, std::uint64_t> local;
          const auto add = [&](std::uint64_t index) {
            if (index >= extent) {
              valid = false;
              return;
            }
            if (local.count(index)) {
              return;
            }
            local[index] = check.channels.size();
            check.channels.push_back(index < base.channels.size()
                                         ? base.channels[index]
                                         : TensorChannelDescription{});
          };
          for (auto& r : related) {
            std::set<std::uint64_t> unique(r.indices.begin(), r.indices.end());
            if (unique.size() != r.indices.size() ||
                (r.alpha && unique.count(*r.alpha))) {
              valid = false;
            }
            for (auto index : r.indices) {
              add(index);
            }
            if (r.alpha) {
              add(*r.alpha);
            }
          }
          if (valid) {
            for (auto& r : related) {
              for (auto& index : r.indices) {
                index = local.at(index);
              }
              if (r.alpha) {
                r.alpha = local.at(*r.alpha);
              }
            }
            check.groups = std::move(related);
            valid = validate_tensor_description(check,
                                                {input.descriptor.element_type,
                                                 {check.channels.size()}})
                        .ok();
          }
        }
        if (valid) {
          ++it;
          continue;
        }
        if (!old_groups || !get(*old_groups, it->first) ||
            protected_path(group_path)) {
          throw Invalid("cascade would remove explicit/unknown group " +
                        it->first);
        }
        it = groups_it->second.fields.erase(it);
      }
    }
  }

  auto target = description(semantics);
  if (!replace) {
    std::map<std::string, std::size_t> order;
    for (std::size_t i = 0; i < source.groups.size(); ++i) {
      order[source.groups[i].name] = i;
    }
    std::stable_sort(
        target.groups.begin(), target.groups.end(),
        [&](const auto& a, const auto& b) {
          auto ai = order.find(a.name), bi = order.find(b.name);
          return (ai == order.end() ? source.groups.size() : ai->second) <
                 (bi == order.end() ? source.groups.size() : bi->second);
        });
  }
  status = validate_tensor_description(target, input.descriptor);
  if (!status.ok()) {
    return Answer(status);
  }
  if (input.planar_layout && target.channel_axis &&
      target.channel_axis != input.planar_layout->channel_axis) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         "metadata.assign: target channel axis disagrees with "
                         "physical planar layout"});
  }
  auto facet = encode_tensor_description(target);
  if (!facet.ok()) {
    return Answer(facet.status());
  }
  OperationOutputSpecialization output;
  output.metadata = input;
  output.metadata.facets.clear();
  if (!semantics.fields.empty()) {
    output.metadata.facets.push_back(facet.take_value());
  }
  const auto& annotations = candidate.fields.at("annotations");
  object(annotations);
  for (const auto& entry : annotations.fields) {
    keys(entry.second, {"key", "version", "payload"});
    ValueFacet annotation;
    annotation.key = string(required(entry.second, "key"));
    const auto version = integer(required(entry.second, "version"));
    const auto& payload = required(entry.second, "payload");
    if (annotation.key != entry.first ||
        annotation.key.compare(0, 12, "photospider.") == 0 || version == 0 ||
        version > UINT32_MAX || payload.kind != 'b') {
      throw Invalid("invalid opaque annotation");
    }
    annotation.version = static_cast<std::uint32_t>(version);
    annotation.payload.assign(payload.bytes.begin(), payload.bytes.end());
    output.metadata.facets.push_back(std::move(annotation));
  }
  std::sort(output.metadata.facets.begin(), output.metadata.facets.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  DependencyMappedNeed need;
  need.port = 0;
  need.roles = static_cast<std::uint32_t>(DependencyRole::Data);
  for (std::size_t a = 0; a < input.descriptor.shape.size(); ++a) {
    DependencyAxis axis;
    axis.observation_axis = static_cast<std::int32_t>(a);
    need.axes.push_back(axis);
  }
  auto all = Footprint::all(input.descriptor.shape);
  if (!all.ok()) {
    return Answer(all.status());
  }
  output.static_dependency_pieces =
      std::vector<DependencyMapPiece>{{all.take_value(), {std::move(need)}}};
  output.regional_atomic = true;
  output.preserve_output_views =
      option(params, "layout", "auto") != "materialize";
  if (output.preserve_output_views) {
    output.maximum_output_payload_bytes = 0;
  }
  OperationPreparation prepared;
  prepared.outputs.push_back(std::move(output));
  return Answer(std::move(prepared));
} catch (const Invalid& e) {
  return Result<OperationPreparation>(invalid(e.what()));
}

Result<ValueFragments> evaluate(const DependencyPhase& phase,
                                bool materialize) {
  using Answer = Result<ValueFragments>;
  const auto& out = phase.query.output;
  const auto width = Value::element_size(out.descriptor.element_type);
  const auto& fragments = phase.inputs[0].fragments();
  std::uint64_t facet_bytes = out.facets.capacity() * sizeof(ValueFacet);
  for (const auto& facet : out.facets) {
    facet_bytes += facet.key.capacity() + facet.payload.capacity();
  }
  plugin_internal::numeric_ops::ArrayPublication publication(
      phase.query.outputs.boxes().size() * fragments.size(),
      out.descriptor.shape.size(), facet_bytes);
  std::vector<Value> values;
  for (const auto& box : phase.query.outputs.boxes()) {
    for (const auto& fragment : fragments) {
      auto dims = box.dimensions();
      bool hit = true;
      for (std::size_t d = 0; d < dims.size(); ++d) {
        const auto s = fragment.region().dimensions()[d];
        const auto begin = std::max(dims[d].offset, s.offset);
        const auto end =
            std::min(dims[d].offset + dims[d].extent, s.offset + s.extent);
        if (begin >= end) {
          hit = false;
          break;
        }
        dims[d] = {begin, end - begin};
      }
      if (!hit) {
        continue;
      }
      auto status = phase.consume_work(dims.size() + 1);
      if (!status.ok()) {
        return Answer(status);
      }
      Region region(dims);
      auto view = Value::from_storage(out.descriptor, region, fragment.layout(),
                                      fragment.storage(), out.facets,
                                      phase.query.resources);
      if (!view.ok()) {
        return Answer(view.status());
      }
      Value value = view.take_value();
      if (materialize) {
        auto allocated =
            MutableValue::allocate(out.descriptor, region, phase.allocator);
        if (!allocated.ok()) {
          return Answer(allocated.status());
        }
        auto writer = allocated.take_value();
        // Coalesce only provably contiguous trailing axes. The admitted
        // fragment already validates every address in this rectangle; each run
        // stays inside its intersection with the requested coverage. Generic
        // reversed/broadcast rows keep their signed stride without converting
        // samples or touching padding. One host stop/work check per <=1024
        // copied samples replaces per-element heap-backed coordinate visits.
        std::size_t first = dims.size();
        std::uint64_t run = 1;
        for (std::size_t axis = dims.size(); axis-- > 0;) {
          if (dims[axis].extent != 1 &&
              value.layout().byte_strides[axis] !=
                  static_cast<std::int64_t>(run * width)) {
            break;
          }
          run *= dims[axis].extent;
          first = axis;
        }
        std::int64_t stride = width;
        if (first == dims.size()) {
          first = dims.size() - 1;
          run = dims.back().extent;
          stride = value.layout().byte_strides.back();
        }
        std::vector<std::uint64_t> at;
        for (const auto& d : dims) {
          at.push_back(d.offset);
        }
        const auto count = region.element_count();
        if (!count.ok()) {
          return Answer(count.status());
        }
        std::uint64_t written = 0;
        for (std::uint64_t row = 0; row < count.value() / run; ++row) {
          auto address = value.byte_address(at);
          if (!address.ok()) {
            return Answer(address.status());
          }
          const auto* source = value.bytes().data() + address.value();
          for (std::uint64_t x = 0; x < run;) {
            const auto n = std::min<std::uint64_t>(1024, run - x);
            status = phase.consume_work(n * (dims.size() + width));
            if (!status.ok()) {
              return Answer(status);
            }
            auto* destination = writer.data() + written * width;
            const auto* from = source + static_cast<std::int64_t>(x) * stride;
            if (stride == static_cast<std::int64_t>(width)) {
              std::memcpy(destination, from, n * width);
            } else if (stride == 0) {
              std::memcpy(destination, from, width);
              for (std::uint64_t filled = 1; filled < n;) {
                const auto more = std::min(filled, n - filled);
                std::memcpy(destination + filled * width, destination,
                            more * width);
                filled += more;
              }
            } else {
              for (std::uint64_t i = 0; i < n; ++i) {
                std::memcpy(destination + i * width,
                            from + static_cast<std::int64_t>(i) * stride,
                            width);
              }
            }
            written += n;
            x += n;
          }
          for (std::size_t axis = first; axis-- > 0;) {
            if (++at[axis] < dims[axis].offset + dims[axis].extent) {
              break;
            }
            at[axis] = dims[axis].offset;
          }
        }
        auto published =
            std::move(writer).publish(out.facets, phase.query.resources);
        if (!published.ok()) {
          return Answer(published.status());
        }
        value = published.take_value();
      }
      auto retained = publication.retain(std::move(value));
      if (!retained.ok()) {
        return Answer(retained.status());
      }
      values.push_back(retained.take_value());
    }
  }
  return publication.finish(out.descriptor, phase.query.outputs, values.data(),
                            values.size(), phase.sets, out.facets,
                            phase.query.resources);
}
struct State final {
  bool materialize = false, requested = false;
  explicit State(bool copy) : materialize(copy) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    if (!requested) {
      requested = true;
      DependencyNeedBatch batch;
      batch.static_mapping = true;
      return Result<DependencyPoll>(std::move(batch));
    }
    auto result = evaluate(phase, materialize);
    return result.ok() ? Result<DependencyPoll>(result.take_value())
                       : Result<DependencyPoll>(result.status());
  }
};
OperationDefinition definition(
    const std::string& key,
    plugin_internal::numeric_ops::SequenceProfile profile) {
  OperationDefinition op;
  op.key = key;
  auto& t = op.traits;
  t.input_count = 1;
  t.input_schema.resize(1);
  t.planar_storage_capable = true;
  t.cacheable = false;
  t.requires_metadata_specialization = true;
  for (const auto* p : {"mode", "edits", "dependencies", "missing", "layout"}) {
    t.parameter_schema.push_back({p, OperationParameterType::String, false});
  }
  auto& out = t.outputs[0];
  out.key = "values";
  out.shape_rule = OperationShapeRule::Fixed;
  out.fixed_output_shape = {1};
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 1;
  out.continuation_bytes = sizeof(State);
  out.maximum_dependency_stages = 2;
  op.prepare_static = [profile](const auto& inputs, const auto& params) {
    return prepare(inputs, params, profile);
  };
  op.start_dependency = [](const DependencyQuery& q,
                           const BufferAllocator& allocator) {
    return DependencyContinuation::make<State>(
        allocator, option(q.parameters, "layout", "auto") == "materialize");
  };
  op.planar_callback = [](const PlanarOperationInvocation& call) {
    if (option(call.parameters, "layout", "auto") == "view") {
      return invalid(
          "ViewUnavailable: direct planar output has a caller-owned writer");
    }
    const auto& c = call.inputs[0].config();
    PlanarImageLayout layout{c.order,        c.height_axis,     c.width_axis,
                             c.channel_axis, c.row_pitch_bytes, c.groups};
    DependencyMappedNeed map;
    for (std::size_t a = 0; a < call.output_region.rank(); ++a) {
      DependencyAxis axis;
      axis.observation_axis = static_cast<std::int32_t>(a);
      map.axes.push_back(axis);
    }
    return execution_internal::copy_channel_piece(
        call.output_region, map, &call.inputs[0], nullptr, call.output, layout,
        Value::element_size(call.inputs[0].descriptor().element_type),
        call.cancellation);
  };
  return op;
}
}  // namespace
}  // namespace ps::metadata_internal

namespace ps::format {
Result<WorkflowNodeOutput> assign_metadata(WorkflowDocument& document,
                                           WorkflowInput input,
                                           const MetadataOptions& opts) try {
  using namespace metadata_internal;  // NOLINT(build/namespaces)
  if (!contains({"strict", "accelerated_apple_silicon", "accelerated_x86_64"},
                opts.profile)) {
    throw Invalid("invalid CPU profile");
  }
  std::map<std::string, ParameterValue> params{
      {"mode", opts.mode},
      {"dependencies", opts.dependencies},
      {"missing", opts.missing},
      {"layout", opts.layout}};
  options(params);
  if ((opts.mode == "replace") != opts.description.has_value()) {
    throw Invalid("description required exactly in replace mode");
  }
  std::vector<std::vector<std::string>> paths;
  for (const auto& e : opts.set) {
    paths.push_back(path(e.path));
  }
  for (const auto& e : opts.remove) {
    paths.push_back(path(e));
  }
  std::sort(paths.begin(), paths.end());
  for (std::size_t i = 0; i < paths.size(); ++i) {
    if (opts.mode == "replace" && paths[i][0] != "annotations") {
      throw Invalid("replace edits must name annotations");
    }
    if (i && prefix(paths[i - 1], paths[i])) {
      throw Invalid("overlapping edit paths");
    }
  }
  std::string bytes;
  encode(transaction(opts), &bytes);
  params["edits"] = hex(bytes);
  auto ids = numeric::available_workflow_node_ids(document, 1, {input});
  if (!ids.ok()) {
    return Result<WorkflowNodeOutput>(ids.status());
  }
  WorkflowNode node{ids.value()[0],
                    "metadata.assign_" + opts.profile,
                    {std::move(input)},
                    std::move(params)};
  auto edge = WorkflowNodeOutput{node.id, "values"};
  document.nodes.push_back(std::move(node));
  return Result<WorkflowNodeOutput>(std::move(edge));
} catch (const metadata_internal::Invalid& e) {
  return Result<WorkflowNodeOutput>(metadata_internal::invalid(e.what()));
} catch (const std::bad_alloc&) {
  return Result<WorkflowNodeOutput>(
      Status{ErrorCode::ResourceExhausted, "metadata authoring capacity"});
}
Result<WorkflowNodeOutput> remove_metadata(
    WorkflowDocument& document, WorkflowInput input,
    const std::vector<std::string>& targets,
    const MetadataOptions& options) try {
  if (options.mode != "patch" || !options.set.empty() || options.description ||
      !options.remove.empty()) {
    return Result<WorkflowNodeOutput>(metadata_internal::invalid(
        "remove helper accepts deletion-only options"));
  }
  auto edits = options;
  edits.remove = targets;
  return assign_metadata(document, std::move(input), edits);
} catch (const std::bad_alloc&) {
  return Result<WorkflowNodeOutput>(
      Status{ErrorCode::ResourceExhausted, "metadata authoring capacity"});
}
}  // namespace ps::format

namespace ps::plugin_internal {
Status register_metadata_assignment(OperationRegistry* registry) {
  using numeric_ops::SequenceProfile;
  for (const auto& p :
       {std::make_pair("strict", SequenceProfile::Strict),
        std::make_pair("accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    auto status = registry->register_operation(metadata_internal::definition(
        "metadata.assign_" + std::string(p.first), p.second));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
