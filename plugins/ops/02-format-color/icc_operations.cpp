#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "02-format-color/external_common.hpp"
#include "common/domain/icc_validation.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::icc_ops {
using namespace external_color;  // NOLINT(build/namespaces)
enum class Kind { Profiles, Link, Chain, Proof, Gamut };
struct State final : Plan {
  Kind kind = Kind::Profiles;
  Parameters parameters;
  std::vector<ProfileRef> profiles;
  std::optional<ResourceIdentity> chain;
  int source_model = 1, target_model = 1;
  bool need_entry = false, need_exit = false;
  std::optional<Endpoint> entry, exit;
  int rendering_intent = 1, proofing_intent = 3;
  bool bpc = false, reference = false;
};
std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> out;
  std::size_t first = 0;
  do {
    auto end = value.find(delimiter, first);
    out.push_back(value.substr(first, end - first));
    if (end == value.npos) {
      break;
    }
    first = end + 1;
  } while (true);
  return out;
}
// ICC public coordinates always use the family native scale, including raw
// endpoint assertions. Percent, code values, L*=0..100 and absolute nits need
// an explicit preceding conversion. Empty semantic units inherit that scale.
void native_units(int model, const std::vector<std::string>& units) {
  require(units.size() == roles(model).size(), "ICC unit count mismatch");
  for (unsigned c = 0; c < units.size(); ++c) {
    const auto& unit = units[c];
    require(unit.empty() || unit == "1" || unit == "relative" ||
                (model == 4 &&
                 ((c == 1 && unit == "a*") || (c == 2 && unit == "b*"))),
            "ICC coordinates require native units");
  }
}
void semantic(const State& s) {
  if (!s.selection.semantic) {
    return;
  }
  const auto& i = s.selection.interpretation;
  native_units(s.source_model, s.selection.units);
  require(i.model == model_name(s.source_model), "ICC input model mismatch");
  require(!i.coordinates || i.coordinates->scale != "absolute",
          "normalize absolute coordinates before ICC");
  if (s.entry) {
    const auto& asserted = s.entry->interpretation;
    if (i.profile && asserted.profile) {
      require(*i.profile == *asserted.profile, "entry profile disagrees");
    }
    if (i.configured && asserted.configured) {
      require(*i.configured == *asserted.configured,
              "entry configured space disagrees");
    }
    if (!i.profile && !i.configured) {
      require(asserted.analytic_binding.has_value(),
              "analytic ICC source requires explicit endpoint binding");
      const auto& b = *asserted.analytic_binding;
      require(b.roles == roles(s.source_model) && b.model == i.model &&
                  b.primaries == i.primaries && b.transfer == i.transfer &&
                  b.reference == i.reference && b.white == i.white &&
                  b.primaries_xy == i.primaries_xy &&
                  b.convention == i.convention,
              "analytic ICC entry assertion disagrees");
    }
  } else {
    require(!s.profiles.empty() && i.profile && *i.profile == s.profiles[0].id,
            "ICC semantic input requires its explicit profile identity or "
            "entry binding");
  }
  if (!s.profiles.empty() && s.kind != Kind::Link && s.entry) {
    require(s.entry->interpretation.profile &&
                *s.entry->interpretation.profile == s.profiles[0].id,
            "entry declaration must name source profile");
  }
}
OperationPreparation prepare(const std::vector<OperationMetadata>& inputs,
                             const Parameters& p, Kind kind) {
  auto s = std::make_shared<State>();
  s->kind = kind;
  s->parameters = p;
  require(inputs.size() == 1, "ICC requires one input");
  const auto layout = text(p, "layout", "auto");
  require(layout == "auto" || layout == "materialize",
          "ICC requires materialization");
  (void)construction_budget(p);
  if (kind == Kind::Chain) {
    auto parts = split(text(p, "stages"), ':');
    require(parts.size() == 7 && parts[0] == "chain-v1",
            "invalid ICC chain reference");
    s->source_model = model_id(parts[1]);
    s->target_model = model_id(parts[2]);
    require((parts[3] == "0" || parts[3] == "1") &&
                (parts[4] == "0" || parts[4] == "1"),
            "invalid chain endpoints");
    s->need_entry = parts[3] == "1";
    s->need_exit = parts[4] == "1";
    s->chain = identity_parse(parts[5] + ":" + parts[6]);
  } else {
    if (kind == Kind::Link) {
      s->profiles.push_back(profile_ref(text(p, "link_profile")));
      require(s->profiles[0].cls == "link", "DeviceLink profile required");
      s->source_model = s->profiles[0].input;
      s->target_model = s->profiles[0].output;
      s->need_entry = s->need_exit = true;
    } else {
      s->profiles.push_back(profile_ref(text(p, "source_profile")));
      s->source_model = s->profiles[0].input;
      if (kind == Kind::Profiles) {
        s->profiles.push_back(profile_ref(text(p, "destination_profile")));
        s->target_model = s->profiles.back().input;
      } else {
        s->profiles.push_back(profile_ref(text(p, "proof_profile")));
        if (kind == Kind::Proof) {
          s->profiles.push_back(profile_ref(text(p, "display_profile")));
          s->target_model = s->profiles.back().input;
        } else {
          s->target_model = 0;
        }
      }
      for (const auto& ref : s->profiles) {
        require(ref.cls == "scnr" || ref.cls == "mntr" || ref.cls == "prtr" ||
                    ref.cls == "spac",
                "ordinary ICC profile class required");
      }
    }
  }
  if (p.count("entry_space")) {
    s->entry = endpoint(text(p, "entry_space"), s->source_model);
  }
  if (p.count("exit_space")) {
    s->exit = endpoint(text(p, "exit_space"), s->target_model);
  }
  require(!s->need_entry || s->entry.has_value(),
          "complete entry_space required");
  require(!s->need_exit || s->exit.has_value(), "complete exit_space required");
  s->selection = select(inputs[0], p, roles(s->source_model), false);
  semantic(*s);
  for (auto item : {std::pair{&s->entry, s->source_model},
                    std::pair{&s->exit, s->target_model}}) {
    if (*item.first) {
      const auto& endpoint = **item.first;
      require(!endpoint.interpretation.coordinates ||
                  endpoint.interpretation.coordinates->scale != "absolute",
              "ICC endpoints require relative coordinates");
      native_units(item.second, endpoint.units);
      if (endpoint.interpretation.analytic_binding) {
        native_units(item.second,
                     endpoint.interpretation.analytic_binding->units);
      }
    }
  }
  if (kind == Kind::Gamut) {
    if (s->selection.axis < 0) {
      same_shape(s.get(), inputs[0]);
      s->output_components = 1;
    } else {
      mask_shape(s.get(), inputs[0], boolean(p, "keepdims"));
    }
  } else {
    replace_shape(s.get(), inputs[0],
                  static_cast<unsigned>(roles(s->target_model).size()), p);
  }
  const auto execution = text(p, "execution", "optimized");
  require(execution == "reference" || execution == "optimized",
          "invalid execution profile");
  s->reference = execution == "reference";
  if (kind == Kind::Profiles || kind == Kind::Chain || kind == Kind::Proof) {
    s->rendering_intent =
        intent(text(p, kind == Kind::Proof ? "rendering_intent" : "intent",
                    "relative_colorimetric"));
    s->bpc = bpc_parameter(p);
    require(s->rendering_intent != 3 || !s->bpc, "absolute intent rejects BPC");
  }
  if (kind == Kind::Proof) {
    s->proofing_intent =
        intent(text(p, "proofing_intent", "absolute_colorimetric"));
    require(s->proofing_intent == 1 || s->proofing_intent == 3,
            "proofing intent must be relative/absolute");
  }
  if (kind == Kind::Gamut) {
    s->rendering_intent =
        intent(text(p, "source_intent", "relative_colorimetric"));
  }
  s->symbol = "ps.color.icc.v1";
  s->contract = "lcms2.19.1.host2";
  s->scratch = 8 * 64 * sizeof(double);
  OperationPreparation result;
  for (const auto& ref : s->profiles) {
    result.resources.push_back(ref.id);
  }
  if (s->chain) {
    result.resources.push_back(*s->chain);
  }
  for (const auto* e : {&s->entry, &s->exit}) {
    if (*e) {
      const auto& i = (**e).interpretation;
      if (i.profile) {
        result.resources.push_back(*i.profile);
      }
      if (i.configured) {
        result.resources.push_back(i.configured->config);
      }
    }
  }
  result.outputs.push_back({OperationMetadata{s->output_schema}, {}});
  result.state = std::move(s);
  return result;
}
struct Engine final {
  EngineMemory memory;
  void* handle = nullptr;
  std::mutex mutex;
  explicit Engine(ResourceBudget budget) : memory(std::move(budget)) {}
  ~Engine() {
    if (handle) {
      ps_color_icc_release_v1(handle);
    }
  }
};
std::unique_ptr<Engine> build(const State& s, const ResourceSet& resources,
                              const ResourceBudget& budget) {
  auto engine = std::make_unique<Engine>(budget);
  const auto host = engine->memory.host();
  ResourceVector<ImmutableResource> owners{ResourceAllocator<ImmutableResource>(
      budget, ResourceAllocationKind::Metadata)};
  ResourceVector<ps_icc_stage_v1> stages{ResourceAllocator<ps_icc_stage_v1>(
      budget, ResourceAllocationKind::Metadata)};
  const auto validate = [&](ByteView bytes) {
    const auto work = [&](std::uint64_t n) { return budget.consume({n}); };
    checked(color_internal::validate_icc_resource(bytes, budget, work));
  };
  if (s.chain) {
    owners.push_back(checked(resources.find(*s.chain)));
    const auto bytes = owners.back().bytes();
    std::size_t offset = 0;
    const auto read = [&](unsigned n) {
      require(offset <= bytes.size() && n <= bytes.size() - offset,
              "truncated ICC chain");
      std::uint64_t v = 0;
      for (unsigned k = 0; k < n; ++k) {
        v |= static_cast<std::uint64_t>(bytes[offset++]) << (k * 8);
      }
      return v;
    };
    require(bytes.size() >= 10 && !std::memcmp(bytes.data(), "PSICC1", 6),
            "invalid ICC chain bytes");
    offset = 6;
    require(read(1) == static_cast<unsigned>(s.source_model) &&
                read(1) == static_cast<unsigned>(s.target_model),
            "ICC chain model assertion mismatch");
    const auto count = read(2);
    require(count >= 2 && count <= 255, "invalid ICC chain count");
    stages.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      const auto d = read(1), intent_code = read(1), bpc = read(1),
                 size = read(8);
      require(d <= 2 && (intent_code <= 3 || intent_code == 255) && bpc <= 2 &&
                  size <= bytes.size() - offset,
              "invalid ICC chain stage");
      ByteView profile(bytes.data() + offset, size);
      offset += size;
      validate(profile);
      stages.push_back({{profile.data(), profile.size()},
                        static_cast<int>(d),
                        intent_code == 255 ? -1 : static_cast<int>(intent_code),
                        bpc == 2 ? -1 : static_cast<int>(bpc)});
    }
    require(offset == bytes.size(), "trailing ICC chain data");
    const auto first =
        signature(ByteView(stages[0].profile.data, stages[0].profile.size), 12);
    const auto last = signature(
        ByteView(stages.back().profile.data, stages.back().profile.size), 12);
    require(
        s.need_entry == (first == "link") && s.need_exit == (last == "link"),
        "chain endpoint declaration mismatch");
  } else {
    owners.reserve(s.profiles.size());
    stages.reserve(s.profiles.size());
    for (unsigned i = 0; i < s.profiles.size(); ++i) {
      owners.push_back(checked(resources.find(s.profiles[i].id)));
      auto bytes = owners.back().bytes();
      verify_profile(s.profiles[i], bytes);
      validate(bytes);
      stages.push_back({{bytes.data(), bytes.size()},
                        s.kind == Kind::Link ? 2
                        : i == 0             ? 0
                                             : 1,
                        s.kind == Kind::Link ? -1 : s.rendering_intent,
                        s.kind == Kind::Link ? -1
                        : i == 0             ? -1
                                             : static_cast<int>(s.bpc)});
    }
  }
  ps_icc_request_v1 request{};
  request.kind = static_cast<int>(s.kind);
  request.input_model = s.source_model;
  request.output_model = s.target_model;
  request.reference = s.reference;
  request.rendering_intent = s.rendering_intent;
  request.proofing_intent = s.proofing_intent;
  request.bpc = s.bpc;
  request.stages = stages.data();
  request.count = stages.size();
  std::array<char, 1024> error{};
  engine->handle =
      ps_color_icc_create_v1(&request, &host, error.data(), error.size());
  checked(engine->memory.status());
  require(engine->handle && error[0] == 0,
          std::string("ICC construction: ") + error.data());
  return engine;
}
OperationDefinition definition(Kind kind, const char* key);
Result<std::string> construction_trace(const OperationMetadata& input,
                                       const Parameters& parameters,
                                       unsigned member,
                                       const ResourceSet& resources) {
  try {
    require(member < 5, "invalid external color member");
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
        std::string(ps_color_icc_trace_v1(engine->handle)));
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
                      ? static_cast<double*>(q->scratch[0].allocation)
                      : nullptr;
  double* source_values = scratch;
  double* target = scratch ? scratch + 256 : nullptr;
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
          if (s.source_model == 2 || (s.source_model == 4 && c == 0)) {
            value = value * 100.0;
          }
          source_values[c * count + p] = value;
        }
      }
      if (!ps_color_icc_apply_v1(prepared.engine->handle, source_values, target,
                                 count)) {
        if (!cancelled()) {
          *status = kernel_failure(ErrorCode::OperationFailed,
                                   "ICC engine execution failed");
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
          if (s.kind != Kind::Gamut &&
              (s.target_model == 2 || (s.target_model == 4 && c == 0))) {
            value = value / 100.0;
          }
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
  op.traits = traits(kind == Kind::Gamut ? "mask" : "values");
  auto& fields = op.traits.parameter_schema;
  for (auto name : {"entry_space", "exit_space"}) {
    fields.push_back({name, OperationParameterType::String, false});
  }
  for (auto name : {"construction_bytes", "construction_work"}) {
    fields.push_back({name, OperationParameterType::Int64, false});
  }
  if (kind != Kind::Gamut) {
    fields.push_back({"execution", OperationParameterType::String, false});
    fields.push_back({"output_axis", OperationParameterType::Int64, false});
  }
  if (kind == Kind::Profiles) {
    for (auto name : {"source_profile", "destination_profile"}) {
      fields.push_back({name, OperationParameterType::String, true});
    }
  } else if (kind == Kind::Link) {
    fields.push_back({"link_profile", OperationParameterType::String, true});
  } else if (kind == Kind::Chain) {
    fields.push_back({"stages", OperationParameterType::String, true});
  } else {
    for (auto name : {"source_profile", "proof_profile"}) {
      fields.push_back({name, OperationParameterType::String, true});
    }
    if (kind == Kind::Proof) {
      fields.push_back(
          {"display_profile", OperationParameterType::String, true});
    } else {
      fields.push_back(
          {"source_intent", OperationParameterType::String, false});
      fields.push_back({"keepdims", OperationParameterType::Bool, false});
    }
  }
  if (kind == Kind::Profiles || kind == Kind::Proof) {
    fields.push_back({kind == Kind::Proof ? "rendering_intent" : "intent",
                      OperationParameterType::String, false});
    fields.push_back({"bpc", OperationParameterType::String, false});
  }
  if (kind == Kind::Proof) {
    fields.push_back(
        {"proofing_intent", OperationParameterType::String, false});
  }
  op.prepare_static = [kind](const auto& inputs,
                             const auto& p) -> Result<OperationPreparation> {
    try {
      return Result<OperationPreparation>(prepare(inputs, p, kind));
    } catch (const basis_ops::CheckedFailure& f) {
      return Result<OperationPreparation>(f.status);
    }
  };
  op.validate_resources = [](const void* state, const ResourceSet& resources) {
    try {
      const auto& s = *static_cast<const State*>(state);
      FloatingEnvironment environment;
      require(environment.active(), "ICC floating environment unavailable");
      auto budget = construction_budget(s.parameters);
      auto engine = build(s, resources, budget);
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
        return Answer(Status{ErrorCode::TypeMismatch, "invalid ICC kernel"});
      }
      if (place.backend != physical::Backend::Cpu ||
          place.domain != physical::Domain::Host) {
        return Answer(
            Status{ErrorCode::BackendUnavailable, "ICC requires CPU Host"});
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
      require(environment.active(), "ICC floating environment unavailable");
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
}  // namespace ps::plugin_internal::icc_ops
namespace ps::plugin_internal {
Status register_icc_color(OperationRegistry* registry) {
  using icc_ops::Kind;
  for (const auto& item : std::array<std::pair<Kind, const char*>, 5>{
           {{Kind::Profiles, "color.icc_transform_lcms2_19_1_cpu"},
            {Kind::Link, "color.icc_apply_devicelink_lcms2_19_1_cpu"},
            {Kind::Chain, "color.icc_transform_chain_lcms2_19_1_cpu"},
            {Kind::Proof, "color.softproof_lcms2_19_1_cpu"},
            {Kind::Gamut,
             "color.gamut_alarm_lcms2_19_1_source_dims_v1_cpu"}}}) {
    auto status = registry->register_operation(
        icc_ops::definition(item.first, item.second));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
