#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_point.hpp"
#include "02-format-color/engine_api.h"
#include "photospider/core/resource_allocator.hpp"
#include "photospider/data/resource_set.hpp"
#include "photospider/ops/format/external_color.hpp"

namespace ps::plugin_internal::external_color {
using namespace color_point;  // NOLINT(build/namespaces)
inline int model_id(const std::string& name) {
  const char* names[] = {"gray", "rgb", "cmyk", "xyz", "cielab"};
  for (int i = 0; i < 5; ++i) {
    if (name == names[i]) {
      return i;
    }
  }
  require(false, "unsupported external color model");
  return 0;
}
inline const char* model_name(int m) {
  static const char* names[] = {"gray", "rgb", "cmyk", "xyz", "cielab"};
  require(m >= 0 && m < 5, "invalid model");
  return names[m];
}
inline std::vector<std::string> roles(int m) {
  if (m == 0) {
    return {"gray"};
  }
  if (m == 1) {
    return {"red", "green", "blue"};
  }
  if (m == 2) {
    return {"cyan", "magenta", "yellow", "black"};
  }
  if (m == 3) {
    return {"x", "y", "z"};
  }
  return {"l", "a", "b"};
}
inline std::uint32_t u32(ByteView bytes, unsigned offset) {
  require(offset <= bytes.size() && bytes.size() - offset >= 4,
          "truncated ICC header");
  std::uint32_t result = 0;
  for (unsigned k = 0; k < 4; ++k) {
    result = (result << 8) | bytes[offset + k];
  }
  return result;
}
inline std::string signature(ByteView bytes, unsigned at) {
  require(at <= bytes.size() && bytes.size() - at >= 4,
          "truncated ICC signature");
  return {reinterpret_cast<const char*>(bytes.data() + at), 4};
}
inline int signature_model(const std::string& s) {
  if (s == "GRAY") {
    return 0;
  }
  if (s == "RGB ") {
    return 1;
  }
  if (s == "CMYK") {
    return 2;
  }
  if (s == "XYZ ") {
    return 3;
  }
  if (s == "Lab ") {
    return 4;
  }
  require(false, "unsupported ICC endpoint signature");
  return 0;
}
inline std::string identity_text(const ResourceIdentity& id) {
  const char* hex = "0123456789abcdef";
  std::string s = std::to_string(id.byte_length) + ":";
  for (auto b : id.sha256) {
    s += hex[b >> 4];
    s += hex[b & 15];
  }
  return s;
}
inline ResourceIdentity identity_parse(const std::string& s) {
  auto colon = s.find(':');
  ResourceIdentity id;
  require(colon != std::string::npos, "missing resource length");
  auto parsed = std::from_chars(s.data(), s.data() + colon, id.byte_length);
  require(parsed.ec == std::errc{} && parsed.ptr == s.data() + colon &&
              id.byte_length &&
              s.substr(0, colon) == std::to_string(id.byte_length) &&
              s.size() == colon + 65,
          "invalid resource identity");
  for (unsigned k = 0; k < 32; ++k) {
    unsigned value = 0;
    for (unsigned j = 0; j < 2; ++j) {
      char c = s[colon + 1 + 2 * k + j];
      require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'),
              "noncanonical resource digest");
      value = (value << 4) | (c <= '9' ? c - '0' : c - 'a' + 10);
    }
    id.sha256[k] = static_cast<std::uint8_t>(value);
  }
  return id;
}
struct ProfileRef {
  ResourceIdentity id;
  int input = 0, output = 0;
  std::string cls;
};
inline ProfileRef profile_ref(const std::string& s) {
  require(s.rfind("icc-v1:", 0) == 0, "invalid ICC profile reference");
  const auto a = s.find(':', 7), b = s.find(':', a + 1), c = s.find(':', b + 1);
  require(a != s.npos && b != s.npos && c != s.npos,
          "truncated ICC profile reference");
  ProfileRef r;
  r.input = model_id(s.substr(7, a - 7));
  r.output = model_id(s.substr(a + 1, b - a - 1));
  r.cls = s.substr(b + 1, c - b - 1);
  require(r.cls.size() == 4, "invalid ICC class assertion");
  r.id = identity_parse(s.substr(c + 1));
  return r;
}
inline void verify_profile(const ProfileRef& ref, ByteView bytes) {
  require(bytes.size() == ref.id.byte_length && bytes.size() >= 132 &&
              signature_model(signature(bytes, 16)) == ref.input &&
              signature_model(signature(bytes, 20)) == ref.output &&
              signature(bytes, 12) == ref.cls,
          "ICC header assertions disagree");
}
inline ResourceIdentity resource_ref(const std::string& s) {
  require(s.rfind("resource-v1:", 0) == 0,
          "invalid immutable resource reference");
  return identity_parse(s.substr(12));
}
inline int intent(const std::string& s) {
  if (s == "perceptual") {
    return 0;
  }
  if (s == "relative_colorimetric") {
    return 1;
  }
  if (s == "saturation") {
    return 2;
  }
  if (s == "absolute_colorimetric") {
    return 3;
  }
  require(false, "invalid ICC intent");
  return 0;
}
inline int direction(const std::string& s) {
  if (s == "device_to_pcs") {
    return 0;
  }
  if (s == "pcs_to_device") {
    return 1;
  }
  if (s == "forward") {
    return 2;
  }
  require(false, "invalid ICC stage direction");
  return 0;
}
inline bool bpc_parameter(const Parameters& p, const char* name = "bpc") {
  const auto value = text(p, name, "off");
  require(value == "on" || value == "off", "BPC must be off/on");
  return value == "on";
}
inline ResourceBudget construction_budget(const Parameters& p) {
  ResourceLimits limits;
  auto number = [&](const char* name, std::int64_t fallback) {
    auto it = p.find(name);
    auto n = it == p.end() ? fallback : std::get<std::int64_t>(it->second);
    require(n > 0, "positive construction limit required");
    return static_cast<std::uint64_t>(n);
  };
  const auto bytes = number("construction_bytes", 256 * 1024 * 1024);
  limits.capacity[ResourceKind::Host] = bytes;
  limits.capacity[ResourceKind::Metadata] = bytes;
  limits.capacity[ResourceKind::Payload] = bytes;
  limits.maximum_work = number("construction_work", UINT64_C(1) << 32);
  return ResourceBudget(limits);
}
/** Intrusive ownership makes every C allocation reclaimable after an aborted
 * C construction stack. C++ destructors are never crossed by the C longjmp. */
class EngineMemory final {
  struct alignas(std::max_align_t) Block {
    ResourceLease lease;
    EngineMemory* owner;
    Block* previous;
    Block* next;
    std::size_t size;
  };

 public:
  explicit EngineMemory(ResourceBudget budget) : budget_(std::move(budget)) {}
  ~EngineMemory() {
    if (head_) {
      ps_color_release_allocations_v1(this);
    }
    while (head_) {
      release(head_ + 1);
    }
  }
  EngineMemory(const EngineMemory&) = delete;
  EngineMemory& operator=(const EngineMemory&) = delete;
  ps_color_host_v1 host() { return {this, allocate, release, work}; }
  const Status& status() const { return failure_; }

 private:
  static void* allocate(void* pointer, std::size_t n,
                        std::size_t alignment) noexcept {
    auto& self = *static_cast<EngineMemory*>(pointer);
    try {
      if (!self.failure_.ok()) {
        return nullptr;
      }
      if (alignment > alignof(Block) || n > SIZE_MAX - sizeof(Block)) {
        self.fail();
        return nullptr;
      }
      const auto bytes = n + sizeof(Block);
      auto capacity = ResourceCapacity::host(bytes, bytes);
      capacity[ResourceKind::Entries] = 1;
      auto admitted = self.budget_.reserve(capacity);
      if (!admitted.ok()) {
        self.failure_ = admitted.status();
        return nullptr;
      }
      auto lease = admitted.take_value();
      auto* storage = std::malloc(bytes);
      if (!storage) {
        self.fail();
        return nullptr;
      }
      auto* b = new (storage)
          Block{std::move(lease), &self, nullptr, self.head_, bytes};
      if (self.head_) {
        self.head_->previous = b;
      }
      self.head_ = b;
      resource_internal::commit_metadata(b->lease, bytes);
      return b + 1;
    } catch (...) {
      self.fail();
      return nullptr;
    }
  }
  static void release(void* p) noexcept {
    if (!p) {
      return;
    }
    auto* b = static_cast<Block*>(p) - 1;
    if (b->previous) {
      b->previous->next = b->next;
    } else {
      b->owner->head_ = b->next;
    }
    if (b->next) {
      b->next->previous = b->previous;
    }
    auto lease = std::move(b->lease);
    b->~Block();
    std::free(b);
  }
  static int work(void* p, std::uint64_t n) noexcept {
    auto& self = *static_cast<EngineMemory*>(p);
    if (!self.failure_.ok()) {
      return 0;
    }
    try {
      return self.budget_.try_consume({n}, self.failure_) ? 1 : 0;
    } catch (...) {
      self.fail();
      return 0;
    }
  }
  void fail() noexcept {
    if (failure_.ok()) {
      failure_ = Status{ErrorCode::ResourceExhausted,
                        {},
                        FailureReason::CapacityLimit};
    }
  }
  ResourceBudget budget_;
  Block* head_ = nullptr;
  Status failure_;
};
inline void replace_shape(Plan* p, const OperationMetadata& input,
                          unsigned count, const Parameters& parameters) {
  same_shape(p, input);
  if (count == p->selection.channels.size()) {
    require(!parameters.count("output_axis"),
            "output_axis only applies to axis-free expansion");
    return;
  }
  auto schema = *p->output_schema;
  auto& t = schema.tensors[0];
  p->output_components = count;
  p->spans.clear();
  if (p->selection.axis < 0) {
    require(parameters.count("output_axis"),
            "axis-free color expansion requires output_axis");
    auto a = std::get<std::int64_t>(parameters.at("output_axis"));
    require(a >= 0 && static_cast<std::size_t>(a) <= t.descriptor.shape.size(),
            "invalid output_axis");
    typed(t.sample_shape().size() < 8, "expanded color rank exceeds 8");
    t.descriptor.shape.insert(t.descriptor.shape.begin() + a, count);
    p->output_axis = static_cast<int>(a + p->selection.batch_rank);
    if (t.layout.spatial) {
      if (t.layout.height_axis >= a) {
        ++t.layout.height_axis;
      }
      if (t.layout.width_axis >= a) {
        ++t.layout.width_axis;
      }
      t.layout.channel_axis = static_cast<std::uint32_t>(a);
    }
    for (unsigned c = 0; c < count; ++c) {
      p->spans.push_back({c, 1, 0, static_cast<int>(c)});
    }
  } else {
    require(!parameters.count("output_axis"),
            "existing channel axis is preserved");
    auto sorted = p->selection.channels;
    std::sort(sorted.begin(), sorted.end());
    auto axis = p->selection.axis - p->selection.batch_rank;
    const auto original = t.descriptor.shape[axis];
    require(original - sorted.size() <= UINT64_MAX - count,
            "channel count overflow");
    t.descriptor.shape[axis] = original - sorted.size() + count;
    std::uint64_t old = 0, out = 0;
    for (auto selected : sorted) {
      if (old < selected) {
        p->spans.push_back({out, selected - old, old, -1});
        out += selected - old;
      }
      if (selected == sorted.front()) {
        for (unsigned c = 0; c < count; ++c) {
          p->spans.push_back({out++, 1, 0, static_cast<int>(c)});
        }
      }
      old = selected + 1;
    }
    if (old < original) {
      p->spans.push_back({out, original - old, old, -1});
    }
  }
  p->output_schema = std::make_shared<const SchemaTemplate>(std::move(schema));
}
struct Endpoint {
  TensorInterpretation interpretation;
  std::vector<std::string> units;
};
inline Endpoint endpoint(const std::string& parameter, int expected) {
  auto d = checked(tensor_description_from_parameter(parameter));
  require(d.channel_axis && *d.channel_axis == 0 && d.groups.size() == 1,
          "endpoint requires canonical color group");
  TensorDescriptor descriptor;
  descriptor.element_type = ElementType::Float64;
  descriptor.shape = {roles(expected).size()};
  checked(validate_tensor_description(d, descriptor));
  const auto& g = d.groups[0];
  Endpoint e{global(d), {}};
  merge(&e.interpretation, g.interpretation);
  require(e.interpretation.model == model_name(expected),
          "endpoint model disagrees");
  const auto expected_roles = roles(expected);
  require(g.indices.size() == expected_roles.size(),
          "endpoint component count disagrees");
  for (unsigned i = 0; i < expected_roles.size(); ++i) {
    require(g.indices[i] == i && g.components[i].role == expected_roles[i] &&
                !g.components[i].unit.empty(),
            "endpoint requires ordered roles and units");
    e.units.push_back(g.components[i].unit);
  }
  return e;
}
}  // namespace ps::plugin_internal::external_color
