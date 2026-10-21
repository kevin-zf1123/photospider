#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/color_authoring.hpp"
#include "02-format-color/external_common.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::ocio_ops {
using namespace external_color;  // NOLINT(build/namespaces)
enum class Kind { Space, View, Looks, Named, File, Tree };
struct State final : Plan {
  Kind kind = Kind::Space;
  Parameters parameters;
  std::optional<ResourceIdentity> config, file, tree;
  std::string file_name, source, destination, display, view, looks, named,
      ccc_id;
  std::string expected_input, expected_output;
  std::optional<Endpoint> entry, exit;
  bool inverse = false, reference = false, data_bypass = true,
       looks_bypass = false, allow_bridge = false;
  int interpolation = 0;
};
std::pair<std::string, ResourceIdentity> file_ref(const std::string& s) {
  require(s.rfind("file-v1:", 0) == 0,
          "standalone OCIO file needs a frozen file binding");
  auto end = s.find(':', 8);
  require(end != s.npos, "invalid file binding");
  std::string name;
  auto hex = s.substr(8, end - 8);
  require(!hex.empty() && hex.size() % 2 == 0, "invalid logical filename");
  auto digit = [](char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : -1;
  };
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    int a = digit(hex[i]), b = digit(hex[i + 1]);
    require(a >= 0 && b >= 0, "invalid logical filename");
    name.push_back(static_cast<char>(a * 16 + b));
  }
  return {name, identity_parse(s.substr(end + 1))};
}
void match_entry(const Selection& selection, const Endpoint& entry) {
  const auto& a = selection.interpretation;
  const auto& b = entry.interpretation;
  if (a.configured) {
    require(b.configured && *a.configured == *b.configured,
            "OCIO configured entry disagrees");
  } else if (a.profile) {
    require(b.profile && *a.profile == *b.profile,
            "OCIO profile entry disagrees");
  } else if (b.configured || b.profile) {
    require(b.analytic_binding.has_value(),
            "analytic source needs explicit endpoint binding");
    const auto& n = *b.analytic_binding;
    require(n.model == a.model && n.primaries == a.primaries &&
                n.transfer == a.transfer && n.reference == a.reference &&
                n.white == a.white && n.primaries_xy == a.primaries_xy &&
                n.convention == a.convention && n.units == selection.units,
            "OCIO analytic entry binding disagrees");
    return;
  } else {
    require(a.model == b.model && a.primaries == b.primaries &&
                a.transfer == b.transfer && a.reference == b.reference &&
                a.white == b.white && a.primaries_xy == b.primaries_xy &&
                a.convention == b.convention,
            "OCIO native entry disagrees");
  }
  require(selection.units == entry.units, "OCIO entry units disagree");
}
OperationPreparation prepare(const std::vector<OperationMetadata>& inputs,
                             const Parameters& p, Kind kind) {
  require(inputs.size() == 1, "OCIO requires one input");
  auto s = std::make_shared<State>();
  s->kind = kind;
  s->parameters = p;
  (void)construction_budget(p);
  require(
      text(p, "layout", "auto") == "auto" || text(p, "layout") == "materialize",
      "OCIO requires materialization");
  const auto direction = text(p, "direction", "forward");
  require(direction == "forward" || direction == "inverse",
          "invalid OCIO direction");
  s->inverse = direction == "inverse";
  const auto execution = text(p, "execution", "optimized");
  require(execution == "optimized" || execution == "reference",
          "invalid OCIO execution");
  s->reference = execution == "reference";
  const auto bridge = text(p, "reference_bridge", "reject");
  require(bridge == "reject" || bridge == "config_default",
          "invalid reference bridge");
  s->allow_bridge = bridge == "config_default";
  if (p.count("config")) {
    s->config = resource_ref(text(p, "config"));
  }
  require(s->config || kind == Kind::File || kind == Kind::Tree,
          "explicit OCIO config required");
  s->source = text(p, "source_space");
  s->destination = text(p, "destination_space");
  s->display = text(p, "display");
  s->view = text(p, "view");
  s->looks = text(p, "looks");
  s->named = text(p, "named_transform");
  s->ccc_id = text(p, "ccc_id");
  s->data_bypass = boolean(p, "data_bypass", true);
  s->looks_bypass = boolean(p, "looks_bypass");
  if (kind == Kind::File) {
    auto value = text(p, "file");
    if (value.rfind("file-v1:", 0) == 0) {
      auto ref = file_ref(value);
      s->file_name = ref.first;
      s->file = ref.second;
    } else {
      require(s->config.has_value(),
              "logical file needs an explicit resource mapping");
      s->file_name = value;
    }
    const auto interpolation = text(p, "interpolation", "default");
    std::array<const char*, 5> names{"default", "best", "nearest", "linear",
                                     "tetrahedral"};
    auto at = std::find(names.begin(), names.end(), interpolation);
    require(at != names.end(), "unsupported concrete interpolation");
    s->interpolation = static_cast<int>(at - names.begin());
  }
  if (kind == Kind::Tree) {
    s->tree = resource_ref(text(p, "tree"));
  }
  if (p.count("entry_space")) {
    s->entry = endpoint(text(p, "entry_space"), 1);
  }
  if (p.count("exit_space")) {
    s->exit = endpoint(text(p, "exit_space"), 1);
  }
  if (static_cast<int>(kind) >= 3) {
    require(s->entry && s->exit, "complete OCIO forward endpoints required");
  }
  if (s->inverse) {
    std::swap(s->entry, s->exit);
  }
  s->selection = select(inputs[0], p, roles(1), false);
  if (s->selection.semantic) {
    const auto& i = s->selection.interpretation;
    require(i.model == "rgb", "OCIO requires RGB coordinates");
    if (s->entry) {
      match_entry(s->selection, *s->entry);
    } else {
      require(
          i.configured && s->config && i.configured->config == *s->config,
          "semantic OCIO input needs the selected config or explicit binding");
    }
    if (i.configured) {
      s->expected_input = i.configured->space;
    }
  }
  for (auto item : {std::pair{&s->entry, &s->expected_input},
                    std::pair{&s->exit, &s->expected_output}}) {
    if (*item.first && (**item.first).interpretation.configured) {
      const auto& c = *(**item.first).interpretation.configured;
      require(s->config && c.config == *s->config,
              "OCIO endpoint config identity disagrees");
      if (!item.second->empty()) {
        require(*item.second == c.space, "OCIO endpoint space disagrees");
      }
      *item.second = c.space;
    }
  }
  same_shape(s.get(), inputs[0]);
  s->symbol = "ps.color.ocio.v1";
  s->contract = "ocio2.5.2.host2";
  s->scratch = 3 * 64 * sizeof(float);
  OperationPreparation result;
  result.state = s;
  result.outputs.push_back({OperationMetadata{s->output_schema}, {}});
  for (const auto* identity : {&s->config, &s->file, &s->tree}) {
    if (*identity) {
      result.resources.push_back(**identity);
    }
  }
  for (const auto* endpoint : {&s->entry, &s->exit}) {
    if (*endpoint) {
      const auto& i = (**endpoint).interpretation;
      if (i.profile) {
        result.resources.push_back(*i.profile);
      }
      if (i.configured) {
        result.resources.push_back(i.configured->config);
      }
    }
  }
  return result;
}
struct Engine final {
  EngineMemory memory;
  void* handle = nullptr;
  ps_ocio_info_v1 info{};
  std::mutex mutex;
  explicit Engine(ResourceBudget budget) : memory(std::move(budget)) {}
  ~Engine() {
    if (handle) {
      ps_color_ocio_release_v1(handle);
    }
  }
};
std::unique_ptr<Engine> build(const State& s, const ResourceSet& resources,
                              const ResourceBudget& budget) {
  auto e = std::make_unique<Engine>(budget);
  auto host = e->memory.host();
  std::array<ImmutableResource, 3> owners;
  auto bytes = [&](const auto& identity, unsigned i) {
    if (!identity) {
      return ps_color_bytes_v1{};
    }
    owners[i] = checked(resources.find(*identity));
    auto b = owners[i].bytes();
    return ps_color_bytes_v1{b.data(), b.size()};
  };
  auto identity = s.config ? identity_text(*s.config)
                  : s.file ? identity_text(*s.file)
                           : identity_text(*s.tree);
  ps_ocio_request_v1 q{};
  q.kind = static_cast<int>(s.kind);
  q.inverse = s.inverse;
  q.reference = s.reference;
  q.data_bypass = s.data_bypass;
  q.looks_bypass = s.looks_bypass;
  q.allow_bridge = s.allow_bridge;
  q.semantic = s.selection.semantic;
  q.interpolation = s.interpolation;
  q.snapshot = bytes(s.config, 0);
  q.file_data = bytes(s.file, 1);
  q.tree = bytes(s.tree, 2);
  q.identity = identity.c_str();
  q.source = s.source.c_str();
  q.destination = s.destination.c_str();
  q.display = s.display.c_str();
  q.view = s.view.c_str();
  q.looks = s.looks.c_str();
  q.named = s.named.c_str();
  q.file = s.file_name.c_str();
  q.ccc_id = s.ccc_id.c_str();
  q.expected_input = s.expected_input.c_str();
  q.expected_output = s.expected_output.c_str();
  std::array<char, 1024> error{};
  e->handle =
      ps_color_ocio_create_v1(&q, &host, &e->info, error.data(), error.size());
  checked(e->memory.status());
  require(e->handle && error[0] == 0,
          std::string("OCIO construction: ") + error.data());
  const auto check = [&](const std::optional<Endpoint>& endpoint,
                         int reference) {
    if (endpoint && endpoint->interpretation.configured) {
      require(endpoint->interpretation.configured->reference_space ==
                  (reference == 0 ? "scene" : "display"),
              "OCIO reference-space assertion disagrees");
    }
  };
  check(s.entry, e->info.input_reference);
  check(s.exit, e->info.output_reference);
  if (s.selection.semantic && s.selection.interpretation.configured) {
    require(s.selection.interpretation.configured->reference_space ==
                (e->info.input_reference == 0 ? "scene" : "display"),
            "OCIO input reference-space assertion disagrees");
  }
  return e;
}
Result<std::string> output_endpoint(const OperationMetadata& input,
                                    const Parameters& parameters,
                                    unsigned member,
                                    const ResourceSet& resources) {
  try {
    require(member < 6, "invalid OCIO view member");
    FloatingEnvironment environment;
    require(environment.active(), "OCIO floating environment unavailable");
    auto prepared = prepare({input}, parameters, static_cast<Kind>(member));
    const auto& state = *static_cast<const State*>(prepared.state.get());
    require(state.exit.has_value(),
            "view stage needs a complete effective exit");
    if (member <= 2) {
      require(state.exit->interpretation.configured.has_value(),
              "resolved OCIO route needs a configured exit assertion");
    }
    auto engine = build(state, resources, construction_budget(parameters));
    return format::color_endpoint_parameter(state.exit->interpretation,
                                            state.exit->units);
  } catch (const basis_ops::CheckedFailure& f) {
    return Result<std::string>(f.status);
  }
}
OperationDefinition definition(Kind kind, const char* key);
Result<std::string> construction_trace(const OperationMetadata& input,
                                       const Parameters& parameters,
                                       unsigned member,
                                       const ResourceSet& resources) {
  try {
    require(member < 6, "invalid external color member");
    FloatingEnvironment environment;
    require(environment.active(), "color floating environment unavailable");
    auto published =
        definition(static_cast<Kind>(member), "external.color.inspection")
            .traits;
    std::sort(published.parameter_schema.begin(),
              published.parameter_schema.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });
    checked(validate_operation_parameters(published, parameters));
    auto prepared = prepare({input}, parameters, static_cast<Kind>(member));
    const auto& state = *static_cast<const State*>(prepared.state.get());
    auto engine = build(state, resources, construction_budget(parameters));
    return Result<std::string>(
        std::string(ps_color_ocio_trace_v1(engine->handle)));
  } catch (const basis_ops::CheckedFailure& f) {
    return Result<std::string>(f.status);
  } catch (const std::bad_alloc&) {
    return Result<std::string>(
        Status{ErrorCode::ResourceExhausted, {}, FailureReason::CapacityLimit});
  }
}

struct Prepared final {
  State state;
  fixed_tensor::PointInputs<1> inputs;
  std::unique_ptr<Engine> engine;
  unsigned requested = 0;
};
// Static dtype specialization removes variable-width byte copies from the
// point gather/scatter loops while preserving unaligned and bypass bits.
template <bool Narrow>
void invoke(const ps_kernel_invocation_v8* q,
            ps_kernel_status_v8* status) noexcept {
  auto& prepared =
      *const_cast<Prepared*>(static_cast<const Prepared*>(q->prepared_context));
  const auto& s = prepared.state;
  const auto cancelled = [&] {
    if (q->cancellation.is_cancelled &&
        q->cancellation.is_cancelled(q->cancellation.context)) {
      *status =
          kernel_failure(ErrorCode::Cancelled, "color execution cancelled");
      return true;
    }
    return false;
  };
  if (cancelled()) {
    return;
  }
  std::unique_lock<std::mutex> lock(prepared.engine->mutex);
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "color floating environment");
    return;
  }
  fixed_tensor::PointReader<1> reader{prepared.inputs, *q};
  const auto& output = q->outputs[0];
  const auto& layout = *output.layout;
  constexpr unsigned width = Narrow ? 4 : 8;
  std::uint64_t points = 1, writes = 0;
  for (unsigned a = 0; a < layout.rank; ++a) {
    if (static_cast<int>(a) != s.output_axis) {
      points *= layout.shape[a];
    }
  }
  auto position = [&](std::uint64_t p) {
    std::array<std::uint64_t, 8> at{};
    for (unsigned a = layout.rank; a-- > 0;) {
      if (static_cast<int>(a) != s.output_axis) {
        at[a] = layout.origin[a] + p % layout.shape[a];
        p /= layout.shape[a];
      } else {
        at[a] = layout.origin[a];
      }
    }
    return at;
  };
  auto* scratch = q->scratch_count
                      ? static_cast<float*>(q->scratch[0].allocation)
                      : nullptr;
  float* source_values = scratch;
  float* target = scratch;
  for (std::uint64_t base = 0; base < points; base += 64) {
    if (cancelled()) {
      return;
    }
    const auto count =
        static_cast<unsigned>(std::min<std::uint64_t>(64, points - base));
    if (prepared.requested) {
      for (unsigned p = 0; p < count; ++p) {
        auto source = source_position(s, position(base + p), layout.rank);
        for (unsigned c = 0; c < s.selection.channels.size(); ++c) {
          if (s.selection.axis >= 0) {
            source[s.selection.axis] = s.selection.channels[c];
          }
          const auto bits = reader.bits(0, source, width);
          double value;
          if constexpr (Narrow) {
            std::uint32_t u = static_cast<std::uint32_t>(bits);
            float f;
            std::memcpy(&f, &u, 4);
            value = f;
          } else {
            std::memcpy(&value, &bits, 8);
          }
          float narrowed = static_cast<float>(value);
          source_values[c * count + p] = narrowed;
        }
      }
      if (!ps_color_ocio_apply_v1(prepared.engine->handle, source_values,
                                  count)) {
        if (!cancelled()) {
          *status = kernel_failure(ErrorCode::OperationFailed,
                                   "OCIO engine execution failed");
        }
        return;
      }
      if (cancelled()) {
        return;
      }
    }
    for (unsigned p = 0; p < count; ++p) {
      auto at = position(base + p);
      auto source = source_position(s, at, layout.rank);
      std::array<std::uint64_t, 4> values{};
      for (unsigned c = 0; c < s.output_components; ++c) {
        if (prepared.requested & (1U << c)) {
          double value = target[c * count + p];
          if constexpr (Narrow) {
            float f = static_cast<float>(value);
            std::uint32_t bits;
            std::memcpy(&bits, &f, 4);
            values[c] = bits;
          } else {
            std::memcpy(&values[c], &value, 8);
          }
        }
      }
      for (const auto& span : s.spans) {
        auto first = span.first, end = span.first + span.count;
        if (s.output_axis >= 0) {
          first = std::max(first, layout.origin[s.output_axis]);
          end = std::min(
              end, layout.origin[s.output_axis] + layout.shape[s.output_axis]);
        }
        for (auto c = first; c < end; ++c) {
          if ((writes++ % 256 == 0) && cancelled()) {
            return;
          }
          if (s.output_axis >= 0) {
            at[s.output_axis] = c;
          }
          std::uint64_t bits;
          if (span.component >= 0) {
            bits = values[span.component];
          } else {
            source[s.selection.axis] = span.source + c - span.first;
            bits = reader.bits(0, source, width);
          }
          std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                          offset(output, at),
                      &bits, width);
        }
      }
    }
  }
  *status = {};
}
OperationDefinition definition(Kind kind, const char* key) {
  OperationDefinition op;
  op.key = key;
  op.traits = traits("values");
  auto& fields = op.traits.parameter_schema;
  for (auto name : {"direction", "execution", "reference_bridge", "entry_space",
                    "exit_space"}) {
    fields.push_back(
        {name, OperationParameterType::String,
         (static_cast<int>(kind) >= 3) && (std::string(name) == "entry_space" ||
                                           std::string(name) == "exit_space")});
  }
  fields.push_back(
      {"config", OperationParameterType::String, static_cast<int>(kind) < 4});
  for (auto name : {"construction_bytes", "construction_work"}) {
    fields.push_back({name, OperationParameterType::Int64, false});
  }
  if (static_cast<int>(kind) <= 2) {
    fields.push_back({"source_space", OperationParameterType::String, true});
    if (kind != Kind::View) {
      fields.push_back(
          {"destination_space", OperationParameterType::String, true});
    }
  }
  if (kind == Kind::Space || kind == Kind::View) {
    fields.push_back({"data_bypass", OperationParameterType::Bool, false});
  }
  if (kind == Kind::View) {
    for (auto name : {"display", "view"}) {
      fields.push_back({name, OperationParameterType::String, true});
    }
    fields.push_back({"looks_bypass", OperationParameterType::Bool, false});
  }
  if (kind == Kind::Looks) {
    fields.push_back({"looks", OperationParameterType::String, true});
  }
  if (kind == Kind::Named) {
    fields.push_back({"named_transform", OperationParameterType::String, true});
  }
  if (kind == Kind::File) {
    fields.push_back({"file", OperationParameterType::String, true});
    for (auto name : {"ccc_id", "interpolation"}) {
      fields.push_back({name, OperationParameterType::String, false});
    }
  }
  if (kind == Kind::Tree) {
    fields.push_back({"tree", OperationParameterType::String, true});
  }
  op.prepare_static = [kind](const auto& in,
                             const auto& p) -> Result<OperationPreparation> {
    try {
      return Result<OperationPreparation>(prepare(in, p, kind));
    } catch (const basis_ops::CheckedFailure& f) {
      return Result<OperationPreparation>(f.status);
    }
  };
  op.validate_resources = [](const void* state, const ResourceSet& resources) {
    try {
      const auto& s = *static_cast<const State*>(state);
      FloatingEnvironment environment;
      require(environment.active(), "OCIO floating environment unavailable");
      auto e = build(s, resources, construction_budget(s.parameters));
      return Status::success();
    } catch (const basis_ops::CheckedFailure& f) {
      return f.status;
    }
  };
  op.lower = lower<State>;
  op.prepare_kernel =
      [](const physical::KernelDesc& k, const physical::Placement& place,
         const KernelPreparation& q, const ResourceSet& resources,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    try {
      const auto* s = static_cast<const State*>(q.static_state);
      if (!s || k.symbol != s->symbol || k.contract != s->contract) {
        return Answer(Status{ErrorCode::TypeMismatch, "invalid OCIO kernel"});
      }
      if (place.backend != physical::Backend::Cpu ||
          place.domain != physical::Domain::Host) {
        return Answer(
            Status{ErrorCode::BackendUnavailable, "OCIO requires CPU Host"});
      }
      auto p = std::make_unique<Prepared>();
      p->state = *s;
      checked(prepare_inputs(*s, q, &p->inputs));
      const auto region = fixed_tensor::view_region(q.writes[0].layout);
      for (const auto& span : s->spans) {
        if (span.component >= 0 && intersects(*s, span, region)) {
          p->requested |= 1U << span.component;
        }
      }
      const auto* current = resource_internal::metadata_budget();
      auto budget = current ? *current : construction_budget(s->parameters);
      FloatingEnvironment environment;
      require(environment.active(), "OCIO floating environment unavailable");
      p->engine = build(*s, resources, budget);
      ps_prepared_kernel_v8 result{};
      result.struct_size = sizeof(result);
      result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
      result.context = p.release();
      result.invoke_cpu = s->narrow ? invoke<true> : invoke<false>;
      result.release = [](const void* ptr) {
        delete static_cast<const Prepared*>(ptr);
      };
      return Answer(result);
    } catch (const basis_ops::CheckedFailure& f) {
      return Answer(f.status);
    }
  };
  return op;
}
}  // namespace ps::plugin_internal::ocio_ops
namespace ps::plugin_internal {
Status register_ocio_color(OperationRegistry* registry) {
  using ocio_ops::Kind;
  for (const auto& item : std::array<std::pair<Kind, const char*>, 6>{
           {{Kind::Space, "color.ocio_convert_space_2_5_2_cpu"},
            {Kind::View, "color.ocio_display_view_2_5_2_cpu"},
            {Kind::Looks, "color.ocio_looks_2_5_2_cpu"},
            {Kind::Named, "color.ocio_named_transform_2_5_2_cpu"},
            {Kind::File, "color.ocio_file_transform_2_5_2_cpu"},
            {Kind::Tree, "color.ocio_transform_tree_2_5_2_cpu"}}}) {
    auto status = registry->register_operation(
        ocio_ops::definition(item.first, item.second));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
