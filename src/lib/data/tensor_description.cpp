#include "photospider/data/tensor_description.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "plugin/utf8_validation.hpp"

namespace ps {
namespace {
constexpr char kKey[] = "photospider.tensor-description";
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
bool valid_text(const std::string& text) {
  return text.size() <= 128 &&
         (text.empty() || plugin_internal::valid_utf8_key(text));
}
bool endpoint_less(const TensorEndpoint& a, const TensorEndpoint& b) {
  if (a.index() == b.index())
    return a.index() ? std::get<double>(a) < std::get<double>(b)
                     : std::get<std::int64_t>(a) < std::get<std::int64_t>(b);
  const auto integer_less = [](std::int64_t i, double d) {
    if (d >= 0x1p63)
      return true;
    if (d < -0x1p63)
      return false;
    const auto whole = static_cast<std::int64_t>(d);
    return i < whole || (i == whole && d > static_cast<double>(whole));
  };
  if (auto* i = std::get_if<std::int64_t>(&a))
    return integer_less(*i, std::get<double>(b));
  auto d = std::get<double>(a);
  auto i = std::get<std::int64_t>(b);
  if (d < -0x1p63)
    return true;
  if (d >= 0x1p63)
    return false;
  const auto whole = static_cast<std::int64_t>(d);
  return whole < i || (whole == i && d < static_cast<double>(whole));
}
bool valid_encoding(const std::optional<TensorEncoding>& encoding) {
  if (!encoding)
    return true;
  for (const auto* pair : {&encoding->stored, &encoding->decoded})
    for (const auto& n : *pair)
      if (auto* f = std::get_if<double>(&n); f && !std::isfinite(*f))
        return false;
  return endpoint_less(encoding->stored[0], encoding->stored[1]) &&
         (endpoint_less(encoding->decoded[0], encoding->decoded[1]) ||
          endpoint_less(encoding->decoded[1], encoding->decoded[0]));
}
bool valid_sampling(const std::optional<TensorSampling>& sampling) {
  return !sampling || (valid_text(sampling->grid) && !sampling->grid.empty() &&
                       sampling->scale == std::array<double, 2>{1, 1} &&
                       sampling->offset == std::array<double, 2>{0, 0});
}
bool valid_extended(const std::string& convention,
                    const std::optional<TensorConfiguredSpace>& configured,
                    const std::optional<TensorAnalyticBinding>& binding,
                    bool profile) {
  if (convention != "relative-v1" && convention != "icc-native" &&
      convention != "ocio-native")
    return false;
  if ((convention == "ocio-native") != configured.has_value() ||
      (convention == "icc-native" && !profile) || (configured && profile))
    return false;
  if (configured &&
      (configured->config.byte_length == 0 || !valid_text(configured->space) ||
       configured->space.empty() ||
       (configured->reference_space != "scene" &&
        configured->reference_space != "display")))
    return false;
  if (binding) {
    if ((!profile && !configured) || binding->convention != "relative-v1" ||
        binding->roles.empty() || binding->roles.size() > 64 ||
        binding->roles.size() != binding->units.size()) {
      return false;
    }
    for (const auto* t : {&binding->model, &binding->primaries,
                          &binding->transfer, &binding->reference}) {
      if (!valid_text(*t)) {
        return false;
      }
    }
    for (const auto* list : {&binding->roles, &binding->units}) {
      for (const auto& t : *list) {
        if (!valid_text(t) || t.empty()) {
          return false;
        }
      }
    }
    if (binding->white) {
      for (auto n : *binding->white) {
        if (!std::isfinite(n)) {
          return false;
        }
      }
    }
    if (binding->primaries_xy) {
      for (auto n : *binding->primaries_xy) {
        if (!std::isfinite(n)) {
          return false;
        }
      }
    }
    if (binding->model == "rgb") {
      if (binding->transfer.empty() ||
          (binding->primaries.empty() &&
           (!binding->white || !binding->primaries_xy))) {
        return false;
      }
    } else if (binding->model == "xyz" || binding->model == "cielab" ||
               binding->model == "cielch") {
      if (!binding->white) {
        return false;
      }
    } else if (binding->model != "gray" && binding->model != "oklab" &&
               binding->model != "oklch") {
      return false;
    }
  }
  return true;
}
bool valid_interpretation(const TensorInterpretation& v) {
  for (const auto* text :
       {&v.model, &v.primaries, &v.transfer, &v.reference, &v.association}) {
    if (!valid_text(*text))
      return false;
  }
  if (v.white)
    for (auto number : *v.white)
      if (!std::isfinite(number))
        return false;
  if (v.primaries_xy)
    for (auto number : *v.primaries_xy)
      if (!std::isfinite(number))
        return false;
  return valid_extended(v.convention, v.configured, v.analytic_binding,
                        v.profile.has_value());
}
bool valid_channel(const TensorChannelDescription& channel) {
  return valid_encoding(channel.encoding) && valid_sampling(channel.sampling) &&
         valid_text(channel.name) && valid_text(channel.role) &&
         valid_text(channel.unit) &&
         (!channel.interpretation ||
          valid_interpretation(*channel.interpretation));
}
Status validate_structure(const TensorDescription& value) {
  if (!valid_encoding(value.encoding) || !valid_sampling(value.sampling) ||
      !valid_extended(value.convention, value.configured,
                      value.analytic_binding, value.profile.has_value()))
    return invalid("invalid encoding, sampling or configured interpretation");
  if ((value.channel_axis && *value.channel_axis >= 8) ||
      value.channels.size() > 65535 || value.axes.size() > 8 ||
      (!value.channel_axis && !value.channels.empty()) ||
      (value.component && !valid_channel(*value.component)))
    return invalid("invalid tensor description structure");
  for (const auto& channel : value.channels)
    if (!valid_channel(channel))
      return invalid("invalid tensor channel text");
  for (const auto& axis : value.axes)
    if (!valid_text(axis.name) || !valid_text(axis.unit) ||
        !std::isfinite(axis.origin) || !std::isfinite(axis.step) ||
        axis.step <= 0)
      return invalid("invalid tensor axis description");
  for (const auto* field : {&value.model, &value.primaries, &value.transfer,
                            &value.reference, &value.association})
    if (!valid_text(*field))
      return invalid("invalid tensor interpretation text");
  if (value.groups.size() > 128)
    return invalid("too many color groups");
  for (const auto& group : value.groups) {
    if (!valid_text(group.name) || group.name.empty() ||
        group.indices.empty() ||
        group.indices.size() != group.components.size() ||
        group.indices.size() > 64 ||
        !valid_interpretation(group.interpretation) ||
        group.interpretation.model.empty() ||
        (!group.interpretation.association.empty() &&
         group.interpretation.association != "straight"))
      return invalid("invalid explicit color group");
    const std::map<std::string, std::vector<std::string>> roles{
        {"rgb", {"red", "green", "blue"}},
        {"xyz", {"x", "y", "z"}},
        {"cielab", {"l", "a", "b"}},
        {"cielch", {"l", "c", "h"}},
        {"oklab", {"l", "a", "b"}},
        {"oklch", {"l", "c", "h"}},
        {"hsl", {"hue", "saturation", "lightness"}},
        {"hsv", {"hue", "saturation", "value"}},
        {"ycbcr", {"y", "cb", "cr"}},
        {"xyy", {"x", "y", "luminance"}},
        {"cmyk", {"cyan", "magenta", "yellow", "black"}},
        {"gray", {"gray"}},
        {"black_white", {"gray"}}};
    const auto expected = roles.find(group.interpretation.model);
    if (expected == roles.end() ||
        expected->second.size() != group.components.size())
      return invalid("unknown or incomplete color-group model");
    std::set<std::string> actual_roles;
    for (const auto& c : group.components)
      actual_roles.insert(c.role);
    if (actual_roles !=
        std::set<std::string>(expected->second.begin(), expected->second.end()))
      return invalid("incomplete color-group roles");
    const auto& interpretation = group.interpretation;
    if ((interpretation.model == "rgb" && !interpretation.profile &&
         !interpretation.configured &&
         (interpretation.transfer.empty() ||
          (interpretation.primaries.empty() &&
           (!interpretation.primaries_xy || !interpretation.white)))) ||
        ((interpretation.model == "cielab" ||
          interpretation.model == "cielch" || interpretation.model == "xyz") &&
         !interpretation.profile && !interpretation.configured &&
         !interpretation.white) ||
        (interpretation.model == "cmyk" && !interpretation.profile))
      return invalid("incomplete color-group interpretation");
    if (interpretation.analytic_binding) {
      const auto& b = *interpretation.analytic_binding;
      if (b.model != interpretation.model ||
          b.roles.size() != group.components.size())
        return invalid("analytic binding model/order mismatch");
      for (std::size_t i = 0; i < b.roles.size(); ++i)
        if (b.roles[i] != group.components[i].role ||
            (!group.components[i].unit.empty() &&
             b.units[i] != group.components[i].unit))
          return invalid("analytic binding component units mismatch");
    }
    std::optional<TensorSampling> sampling = value.sampling;
    for (std::size_t i = 0; i < group.components.size(); ++i) {
      auto current = group.components[i].sampling;
      if (!current && group.indices[i] < value.channels.size())
        current = value.channels[group.indices[i]].sampling;
      if (sampling && current && sampling->grid != current->grid)
        return invalid("group sampling grids disagree");
      if (current)
        sampling = current;
    }
    std::set<std::uint64_t> seen;
    for (std::size_t i = 0; i < group.indices.size(); ++i)
      if (!seen.insert(group.indices[i]).second ||
          !valid_channel(group.components[i]) ||
          group.components[i].interpretation ||
          (group.alpha && *group.alpha == group.indices[i]))
        return invalid("invalid group component indices");
  }
  const auto compatible_text = [](const std::string& a, const std::string& b) {
    return a.empty() || b.empty() || a == b;
  };
  const auto compatible_interpretation = [&](const TensorInterpretation& a,
                                             const TensorInterpretation& b) {
    return compatible_text(a.model, b.model) &&
           compatible_text(a.primaries, b.primaries) &&
           compatible_text(a.transfer, b.transfer) &&
           compatible_text(a.reference, b.reference) &&
           compatible_text(a.association, b.association) &&
           (!a.white || !b.white || *a.white == *b.white) &&
           (!a.primaries_xy || !b.primaries_xy ||
            *a.primaries_xy == *b.primaries_xy) &&
           (!a.profile || !b.profile || *a.profile == *b.profile) &&
           a.convention == b.convention &&
           (!a.configured || !b.configured || *a.configured == *b.configured) &&
           (!a.analytic_binding || !b.analytic_binding ||
            *a.analytic_binding == *b.analytic_binding);
  };
  std::map<std::uint64_t, TensorChannelDescription> assertions;
  for (std::size_t i = 0; i < value.channels.size(); ++i)
    assertions[i] = value.channels[i];
  for (const auto& group : value.groups)
    for (std::size_t i = 0; i < group.indices.size(); ++i) {
      auto c = group.components[i];
      c.interpretation = group.interpretation;
      auto& old = assertions[group.indices[i]];
      if ((old.encoding && c.encoding && !(*old.encoding == *c.encoding)) ||
          (old.sampling && c.sampling && !(*old.sampling == *c.sampling)) ||
          !compatible_text(old.name, c.name) ||
          !compatible_text(old.role, c.role) ||
          !compatible_text(old.unit, c.unit) ||
          (old.interpretation &&
           !compatible_interpretation(*old.interpretation, *c.interpretation)))
        return invalid("group conflicts with channel or overlapping group");
      if (!c.name.empty())
        old.name = c.name;
      if (!c.role.empty())
        old.role = c.role;
      if (!c.unit.empty())
        old.unit = c.unit;
      if (c.encoding)
        old.encoding = c.encoding;
      if (c.sampling)
        old.sampling = c.sampling;
      old.interpretation = c.interpretation;
    }
  return Status::success();
}
void put_u16(std::vector<std::uint8_t>* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value));
  bytes->push_back(static_cast<std::uint8_t>(value >> 8));
}
void put_u64(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    bytes->push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void put_f64(std::vector<std::uint8_t>* bytes, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  put_u64(bytes, bits);
}
void put_text(std::vector<std::uint8_t>* bytes, const std::string& value) {
  bytes->push_back(static_cast<std::uint8_t>(value.size()));
  bytes->insert(bytes->end(), value.begin(), value.end());
}
void put_identity(std::vector<std::uint8_t>* bytes,
                  const ColorProfileIdentity& p) {
  put_u64(bytes, p.byte_length);
  bytes->insert(bytes->end(), p.sha256.begin(), p.sha256.end());
}
void put_encoding(std::vector<std::uint8_t>* bytes,
                  const std::optional<TensorEncoding>& e) {
  bytes->push_back(e ? 1 : 0);
  if (!e)
    return;
  for (const auto* pair : {&e->stored, &e->decoded})
    for (const auto& n : *pair) {
      bytes->push_back(static_cast<std::uint8_t>(n.index()));
      if (auto* i = std::get_if<std::int64_t>(&n))
        put_u64(bytes, static_cast<std::uint64_t>(*i));
      else
        put_f64(bytes, std::get<double>(n));
    }
}
void put_sampling(std::vector<std::uint8_t>* bytes,
                  const std::optional<TensorSampling>& s) {
  bytes->push_back(s ? 1 : 0);
  if (!s)
    return;
  put_text(bytes, s->grid);
  for (auto n : s->scale)
    put_f64(bytes, n);
  for (auto n : s->offset)
    put_f64(bytes, n);
}
void put_extended_space(std::vector<std::uint8_t>* bytes,
                        const std::string& convention,
                        const std::optional<TensorConfiguredSpace>& configured,
                        const std::optional<TensorAnalyticBinding>& binding) {
  put_text(bytes, convention);
  bytes->push_back(configured ? 1 : 0);
  if (configured) {
    put_identity(bytes, configured->config);
    put_text(bytes, configured->space);
    put_text(bytes, configured->reference_space);
  }
  bytes->push_back(binding ? 1 : 0);
  if (binding) {
    for (const auto* t :
         {&binding->model, &binding->primaries, &binding->transfer,
          &binding->reference, &binding->convention})
      put_text(bytes, *t);
    bytes->push_back(binding->white ? 1 : 0);
    if (binding->white)
      for (auto n : *binding->white)
        put_f64(bytes, n);
    bytes->push_back(binding->primaries_xy ? 1 : 0);
    if (binding->primaries_xy)
      for (auto n : *binding->primaries_xy)
        put_f64(bytes, n);
    for (const auto* strings : {&binding->roles, &binding->units}) {
      put_u16(bytes, static_cast<std::uint16_t>(strings->size()));
      for (const auto& t : *strings)
        put_text(bytes, t);
    }
  }
}
void put_interpretation(std::vector<std::uint8_t>* bytes,
                        const TensorInterpretation& v) {
  for (const auto* text :
       {&v.model, &v.primaries, &v.transfer, &v.reference, &v.association})
    put_text(bytes, *text);
  bytes->push_back(v.white ? 1 : 0);
  if (v.white)
    for (auto n : *v.white)
      put_f64(bytes, n);
  bytes->push_back(v.primaries_xy ? 1 : 0);
  if (v.primaries_xy)
    for (auto n : *v.primaries_xy)
      put_f64(bytes, n);
  bytes->push_back(v.profile ? 1 : 0);
  if (v.profile) {
    put_u64(bytes, v.profile->byte_length);
    bytes->insert(bytes->end(), v.profile->sha256.begin(),
                  v.profile->sha256.end());
  }
  put_extended_space(bytes, v.convention, v.configured, v.analytic_binding);
}
void put_channel(std::vector<std::uint8_t>* bytes,
                 const TensorChannelDescription& value) {
  put_text(bytes, value.name);
  put_text(bytes, value.role);
  put_text(bytes, value.unit);
  bytes->push_back(value.interpretation ? 1 : 0);
  if (value.interpretation)
    put_interpretation(bytes, *value.interpretation);
  put_encoding(bytes, value.encoding);
  put_sampling(bytes, value.sampling);
}
struct Reader final {
  const std::vector<std::uint8_t>& bytes;
  std::size_t offset = 0;
  bool byte(std::uint8_t* value) {
    if (offset == bytes.size())
      return false;
    *value = bytes[offset++];
    return true;
  }
  bool u16(std::uint16_t* value) {
    std::uint8_t low = 0, high = 0;
    if (!byte(&low) || !byte(&high))
      return false;
    *value = static_cast<std::uint16_t>(low | (high << 8));
    return true;
  }
  bool f64(double* value) {
    std::uint64_t bits = 0;
    if (!u64(&bits))
      return false;
    std::memcpy(value, &bits, sizeof(bits));
    return true;
  }
  bool u64(std::uint64_t* value) {
    if (bytes.size() - offset < 8)
      return false;
    *value = 0;
    for (unsigned i = 0; i < 8; ++i)
      *value |= static_cast<std::uint64_t>(bytes[offset++]) << (8 * i);
    return true;
  }
  bool text(std::string* value) {
    std::uint8_t size = 0;
    if (!byte(&size) || size > 128 || bytes.size() - offset < size)
      return false;
    value->assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
    offset += size;
    return valid_text(*value);
  }
  bool marker(std::uint8_t* v) { return byte(v) && *v <= 1; }
  bool identity(ColorProfileIdentity* p) {
    if (!u64(&p->byte_length))
      return false;
    for (auto& b : p->sha256)
      if (!byte(&b))
        return false;
    return true;
  }
  bool encoding(std::optional<TensorEncoding>* out) {
    std::uint8_t present = 0;
    if (!marker(&present))
      return false;
    if (!present)
      return true;
    out->emplace();
    for (auto* pair : {&(*out)->stored, &(*out)->decoded})
      for (auto& n : *pair) {
        std::uint8_t kind = 0;
        std::uint64_t bits = 0;
        if (!marker(&kind) || !u64(&bits))
          return false;
        if (kind) {
          double v;
          std::memcpy(&v, &bits, 8);
          n = v;
        } else {
          std::int64_t v;
          std::memcpy(&v, &bits, 8);
          n = v;
        }
      }
    return true;
  }
  bool sampling(std::optional<TensorSampling>* out) {
    std::uint8_t present = 0;
    if (!marker(&present))
      return false;
    if (!present)
      return true;
    out->emplace();
    if (!text(&(*out)->grid))
      return false;
    for (auto& n : (*out)->scale)
      if (!f64(&n))
        return false;
    for (auto& n : (*out)->offset)
      if (!f64(&n))
        return false;
    return true;
  }
  bool extended_space(std::string* convention,
                      std::optional<TensorConfiguredSpace>* configured,
                      std::optional<TensorAnalyticBinding>* binding) {
    std::uint8_t present = 0;
    if (!text(convention) || !marker(&present))
      return false;
    if (present) {
      configured->emplace();
      if (!identity(&(*configured)->config) || !text(&(*configured)->space) ||
          !text(&(*configured)->reference_space))
        return false;
    }
    if (!marker(&present))
      return false;
    if (present) {
      binding->emplace();
      auto& b = **binding;
      for (auto* t :
           {&b.model, &b.primaries, &b.transfer, &b.reference, &b.convention})
        if (!text(t))
          return false;
      if (!marker(&present))
        return false;
      if (present) {
        b.white.emplace();
        for (auto& n : *b.white)
          if (!f64(&n))
            return false;
      }
      if (!marker(&present))
        return false;
      if (present) {
        b.primaries_xy.emplace();
        for (auto& n : *b.primaries_xy)
          if (!f64(&n))
            return false;
      }
      for (auto* strings : {&b.roles, &b.units}) {
        std::uint16_t count = 0;
        if (!u16(&count) || count > 64)
          return false;
        strings->resize(count);
        for (auto& t : *strings)
          if (!text(&t))
            return false;
      }
    }
    return true;
  }
  bool interpretation(TensorInterpretation* v) {
    for (auto* t : {&v->model, &v->primaries, &v->transfer, &v->reference,
                    &v->association})
      if (!text(t))
        return false;
    std::uint8_t present = 0;
    if (!byte(&present) || present > 1)
      return false;
    if (present) {
      v->white.emplace();
      for (auto& n : *v->white)
        if (!f64(&n))
          return false;
    }
    if (!byte(&present) || present > 1)
      return false;
    if (present) {
      v->primaries_xy.emplace();
      for (auto& n : *v->primaries_xy)
        if (!f64(&n))
          return false;
    }
    if (!byte(&present) || present > 1)
      return false;
    if (present) {
      v->profile.emplace();
      if (!u64(&v->profile->byte_length))
        return false;
      for (auto& n : v->profile->sha256)
        if (!byte(&n))
          return false;
    }
    return extended_space(&v->convention, &v->configured,
                          &v->analytic_binding) &&
           valid_interpretation(*v);
  }
  bool channel(TensorChannelDescription* value) {
    std::uint8_t present = 0;
    if (!text(&value->name) || !text(&value->role) || !text(&value->unit) ||
        !byte(&present) || present > 1)
      return false;
    if (present) {
      value->interpretation.emplace();
      if (!interpretation(&*value->interpretation))
        return false;
    }
    return encoding(&value->encoding) && sampling(&value->sampling);
  }
};
}  // namespace

bool operator==(const TensorEncoding& a, const TensorEncoding& b) {
  for (unsigned p = 0; p < 2; ++p)
    for (unsigned i = 0; i < 2; ++i) {
      const auto& x = p ? a.decoded[i] : a.stored[i];
      const auto& y = p ? b.decoded[i] : b.stored[i];
      if (x.index() != y.index())
        return false;
      if (auto* v = std::get_if<std::int64_t>(&x)) {
        if (*v != std::get<std::int64_t>(y))
          return false;
      } else {
        auto l = std::get<double>(x), r = std::get<double>(y);
        if (std::memcmp(&l, &r, 8))
          return false;
      }
    }
  return true;
}
bool operator==(const TensorSampling& a, const TensorSampling& b) {
  return a.grid == b.grid && a.scale == b.scale && a.offset == b.offset;
}
bool operator==(const TensorConfiguredSpace& a,
                const TensorConfiguredSpace& b) {
  return a.config == b.config && a.space == b.space &&
         a.reference_space == b.reference_space;
}
bool operator==(const TensorAnalyticBinding& a,
                const TensorAnalyticBinding& b) {
  return std::tie(a.model, a.primaries, a.transfer, a.reference, a.white,
                  a.primaries_xy, a.roles, a.units, a.convention) ==
         std::tie(b.model, b.primaries, b.transfer, b.reference, b.white,
                  b.primaries_xy, b.roles, b.units, b.convention);
}

Result<ValueFacet> encode_tensor_description(
    const TensorDescription& description) {
  auto status = validate_structure(description);
  if (!status.ok())
    return Result<ValueFacet>(status);
  ValueFacet facet;
  facet.key = kKey;
  facet.version = 3;
  auto& bytes = facet.payload;
  bytes.insert(bytes.end(), {'T', 'D', 'M', '3'});
  bytes.push_back(description.channel_axis
                      ? static_cast<std::uint8_t>(*description.channel_axis)
                      : 255);
  put_u16(&bytes, static_cast<std::uint16_t>(description.channels.size()));
  for (const auto& channel : description.channels)
    put_channel(&bytes, channel);
  bytes.push_back(description.component ? 1 : 0);
  if (description.component)
    put_channel(&bytes, *description.component);
  bytes.push_back(static_cast<std::uint8_t>(description.axes.size()));
  for (const auto& axis : description.axes) {
    put_text(&bytes, axis.name);
    put_text(&bytes, axis.unit);
    put_f64(&bytes, axis.origin);
    put_f64(&bytes, axis.step);
  }
  for (const auto* field :
       {&description.model, &description.primaries, &description.transfer,
        &description.reference, &description.association})
    put_text(&bytes, *field);
  bytes.push_back(description.white ? 1 : 0);
  if (description.white)
    for (const auto number : *description.white)
      put_f64(&bytes, number);
  bytes.push_back(description.primaries_xy ? 1 : 0);
  if (description.primaries_xy)
    for (const auto number : *description.primaries_xy)
      put_f64(&bytes, number);
  bytes.push_back(description.profile ? 1 : 0);
  if (description.profile) {
    put_u64(&bytes, description.profile->byte_length);
    bytes.insert(bytes.end(), description.profile->sha256.begin(),
                 description.profile->sha256.end());
  }
  put_u16(&bytes, static_cast<std::uint16_t>(description.groups.size()));
  for (const auto& group : description.groups) {
    put_text(&bytes, group.name);
    put_u16(&bytes, static_cast<std::uint16_t>(group.indices.size()));
    for (std::size_t i = 0; i < group.indices.size(); ++i) {
      put_u64(&bytes, group.indices[i]);
      put_channel(&bytes, group.components[i]);
    }
    put_interpretation(&bytes, group.interpretation);
    bytes.push_back(group.alpha ? 1 : 0);
    if (group.alpha)
      put_u64(&bytes, *group.alpha);
  }
  put_encoding(&bytes, description.encoding);
  put_sampling(&bytes, description.sampling);
  put_extended_space(&bytes, description.convention, description.configured,
                     description.analytic_binding);
  if (bytes.size() > 4096)
    return Result<ValueFacet>(
        invalid("tensor description exceeds facet bound"));
  return Result<ValueFacet>(std::move(facet));
}

Result<TensorDescription> decode_tensor_description(const ValueFacet& facet) {
  using Answer = Result<TensorDescription>;
  if (facet.key != kKey || facet.version != 3 || facet.payload.size() < 9 ||
      facet.payload.size() > 4096 || facet.payload[0] != 'T' ||
      facet.payload[1] != 'D' || facet.payload[2] != 'M' ||
      facet.payload[3] != '3')
    return Answer(invalid("invalid tensor description facet"));
  Reader reader{facet.payload, 4};
  TensorDescription value;
  std::uint8_t axis = 0, has_component = 0, axes = 0;
  std::uint16_t count = 0;
  if (!reader.byte(&axis) || (axis != 255 && axis >= 8) || !reader.u16(&count))
    return Answer(invalid("invalid tensor description header"));
  if (axis != 255)
    value.channel_axis = axis;
  if (count > (facet.payload.size() - reader.offset) / 3)
    return Answer(invalid("truncated tensor channel table"));
  value.channels.resize(count);
  for (auto& channel : value.channels)
    if (!reader.channel(&channel))
      return Answer(invalid("invalid tensor channel table"));
  if (!reader.byte(&has_component) || has_component > 1)
    return Answer(invalid("invalid tensor component marker"));
  if (has_component) {
    value.component.emplace();
    if (!reader.channel(&*value.component))
      return Answer(invalid("invalid tensor component"));
  }
  if (!reader.byte(&axes) || axes > 8)
    return Answer(invalid("invalid tensor axis count"));
  value.axes.resize(axes);
  for (auto& described : value.axes)
    if (!reader.text(&described.name) || !reader.text(&described.unit) ||
        !reader.f64(&described.origin) || !reader.f64(&described.step))
      return Answer(invalid("invalid tensor axis data"));
  for (auto* field : {&value.model, &value.primaries, &value.transfer,
                      &value.reference, &value.association})
    if (!reader.text(field))
      return Answer(invalid("invalid tensor interpretation"));
  std::uint8_t present = 0;
  if (!reader.byte(&present) || present > 1)
    return Answer(invalid("invalid tensor white marker"));
  if (present) {
    value.white.emplace();
    for (auto& number : *value.white)
      if (!reader.f64(&number))
        return Answer(invalid("truncated tensor white"));
  }
  if (!reader.byte(&present) || present > 1)
    return Answer(invalid("invalid tensor primaries marker"));
  if (present) {
    value.primaries_xy.emplace();
    for (auto& number : *value.primaries_xy)
      if (!reader.f64(&number))
        return Answer(invalid("truncated tensor primaries"));
  }
  if (!reader.byte(&present) || present > 1)
    return Answer(invalid("invalid tensor profile marker"));
  if (present) {
    value.profile.emplace();
    if (!reader.u64(&value.profile->byte_length) ||
        facet.payload.size() - reader.offset < value.profile->sha256.size())
      return Answer(invalid("truncated tensor profile identity"));
    for (auto& byte : value.profile->sha256)
      if (!reader.byte(&byte))
        return Answer(invalid("truncated tensor profile identity"));
  }
  std::uint16_t group_count = 0;
  if (!reader.u16(&group_count) || group_count > 128)
    return Answer(invalid("invalid group count"));
  value.groups.resize(group_count);
  for (auto& group : value.groups) {
    std::uint16_t count = 0;
    if (!reader.text(&group.name) || !reader.u16(&count) || count > 64)
      return Answer(invalid("invalid group header"));
    group.indices.resize(count);
    group.components.resize(count);
    for (std::size_t i = 0; i < count; ++i)
      if (!reader.u64(&group.indices[i]) ||
          !reader.channel(&group.components[i]))
        return Answer(invalid("invalid group components"));
    if (!reader.interpretation(&group.interpretation) ||
        !reader.byte(&present) || present > 1)
      return Answer(invalid("invalid group interpretation"));
    if (present) {
      group.alpha.emplace();
      if (!reader.u64(&*group.alpha))
        return Answer(invalid("invalid group alpha"));
    }
  }
  if (!reader.encoding(&value.encoding) || !reader.sampling(&value.sampling) ||
      !reader.extended_space(&value.convention, &value.configured,
                             &value.analytic_binding))
    return Answer(invalid("invalid v3 extended descriptions"));
  if (reader.offset != facet.payload.size() || !validate_structure(value).ok())
    return Answer(invalid("noncanonical tensor description"));
  auto canonical = encode_tensor_description(value);
  if (!canonical.ok() || canonical.value().payload != facet.payload)
    return Answer(invalid("noncanonical tensor description bytes"));
  return Answer(std::move(value));
}

Result<std::string> tensor_description_parameter(
    const TensorDescription& description) {
  auto encoded = encode_tensor_description(description);
  if (!encoded.ok())
    return Result<std::string>(encoded.status());
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(encoded.value().payload.size() * 2);
  for (const auto byte : encoded.value().payload) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 15]);
  }
  return Result<std::string>(std::move(result));
}

Result<TensorDescription> tensor_description_from_parameter(
    const std::string& parameter) {
  if (parameter.size() < 18 || parameter.size() > 8192 || parameter.size() % 2)
    return Result<TensorDescription>(invalid("invalid tensor override length"));
  ValueFacet facet;
  facet.key = kKey;
  facet.version = 3;
  facet.payload.reserve(parameter.size() / 2);
  const auto nibble = [](char digit) -> int {
    if (digit >= '0' && digit <= '9')
      return digit - '0';
    if (digit >= 'a' && digit <= 'f')
      return digit - 'a' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < parameter.size(); i += 2) {
    const auto high = nibble(parameter[i]), low = nibble(parameter[i + 1]);
    if (high < 0 || low < 0)
      return Result<TensorDescription>(invalid("invalid tensor override hex"));
    facet.payload.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return decode_tensor_description(facet);
}

Status validate_tensor_description(const TensorDescription& description,
                                   const ValueDescriptor& descriptor) {
  auto status = validate_structure(description);
  if (!status.ok())
    return status;
  const auto rank = descriptor.shape.size();
  if (rank == 0 || rank > 8 ||
      (description.channel_axis && *description.channel_axis >= rank) ||
      (!description.axes.empty() && description.axes.size() != rank))
    return invalid("tensor description disagrees with shape");
  if (!description.channels.empty() &&
      description.channels.size() !=
          descriptor.shape[*description.channel_axis])
    return {ErrorCode::TypeMismatch,
            "tensor channel table length disagrees with shape",
            FailureReason::None,
            {FailureOrigin::Schema, FailureScope::Unspecified}};
  const auto integer_dtype = descriptor.element_type != ElementType::Float32 &&
                             descriptor.element_type != ElementType::Float64;
  TensorEndpoint low = std::int64_t{0}, high = std::int64_t{255};
  switch (descriptor.element_type) {
    case ElementType::UInt8:
      break;
    case ElementType::UInt16:
      high = std::int64_t{65535};
      break;
    case ElementType::Int8:
      low = std::int64_t{-128};
      high = std::int64_t{127};
      break;
    case ElementType::Int16:
      low = std::int64_t{-32768};
      high = std::int64_t{32767};
      break;
    case ElementType::Int64:
      low = INT64_MIN;
      high = INT64_MAX;
      break;
    case ElementType::Float32:
      low = -static_cast<double>(std::numeric_limits<float>::max());
      high = static_cast<double>(std::numeric_limits<float>::max());
      break;
    case ElementType::Float64:
      low = -std::numeric_limits<double>::max();
      high = std::numeric_limits<double>::max();
      break;
  }
  const auto compatible_encoding = [&](const std::optional<TensorEncoding>& e) {
    return !e || (!endpoint_less(e->stored[0], low) &&
                  !endpoint_less(high, e->stored[1]));
  };
  if (!compatible_encoding(description.encoding))
    return {ErrorCode::TypeMismatch, "encoding interval exceeds dtype"};
  for (const auto& c : description.channels)
    if (!compatible_encoding(c.encoding))
      return {ErrorCode::TypeMismatch, "channel encoding exceeds dtype"};
  if (description.component &&
      !compatible_encoding(description.component->encoding))
    return {ErrorCode::TypeMismatch, "component encoding exceeds dtype"};
  for (const auto& g : description.groups)
    for (std::size_t i = 0; i < g.components.size(); ++i) {
      auto e = g.components[i].encoding;
      if (!e && g.indices[i] < description.channels.size())
        e = description.channels[g.indices[i]].encoding;
      if (!e)
        e = description.encoding;
      if (!compatible_encoding(e))
        return {ErrorCode::TypeMismatch, "group encoding exceeds dtype"};
      if (integer_dtype && !e)
        return invalid(
            "complete integer color group requires explicit decoder");
    }
  std::set<std::string> names;
  for (const auto& group : description.groups) {
    if (!description.channel_axis || !names.insert(group.name).second)
      return invalid("group requires channel axis and unique name");
    const auto count = descriptor.shape[*description.channel_axis];
    for (auto index : group.indices)
      if (index >= count)
        return invalid("group index exceeds channel count");
    if (group.alpha && *group.alpha >= count)
      return invalid("group alpha exceeds channel count");
  }
  return Status::success();
}
}  // namespace ps
