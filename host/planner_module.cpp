#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "host/provider_library.hpp"
#include "photospider/compiler/planner.hpp"
#include "photospider/core/callback_environment.hpp"
#include "photospider/plugin/planner_module_api.h"
#include "sdk/program/artifact_text.hpp"

namespace ps {
namespace {
using Id = physical::Id;
using Key = std::tuple<Id, Id, std::uint64_t>;
template <class T>
T get(Result<T> r) {
  if (!r.ok())
    throw r.status();
  return r.take_value();
}
void require(bool value) {
  if (!value)
    throw Status{ErrorCode::InvalidArgument, "invalid planner builder request"};
}
template <class T>
void span(const T* p, uint32_t n, uint64_t limit) {
  require(n <= limit &&
          (!n || (p && reinterpret_cast<uintptr_t>(p) % alignof(T) == 0)));
}
struct Builder {
  const ValidatedIR& ir;
  PlanBuilder& builder;
  const PlanningOptions& options;
  Status failure;
  std::map<Key, Id> tiles;
  std::optional<physical::Program> pending;
  physical::ValidationLimits limits() const {
    return {options.maximum_records, options.maximum_dependency_edges,
            options.maximum_work};
  }
  LoweringSession session{limits()};
  std::mutex mutex;
  const std::thread::id thread = std::this_thread::get_id();
  void bytes(const physical::Program& p,
             const std::vector<PlanCallBinding>& bindings,
             ps_plan_bytes_v1* out) {
    auto storage = std::make_unique<std::string>(
        get(compiler_internal::encode_physical_tables(
            p, bindings,
            compiler_internal::physical_transfer_limit(limits()))));
    *out = {storage->data(), storage->size(), storage.get(),
            [](void* raw) { delete static_cast<std::string*>(raw); }};
    storage.release();
  }
  Builder(const ValidatedIR& i, PlanBuilder& b, const PlanningOptions& o)
      : ir(i), builder(b), options(o) {}
  void records(uint64_t extra) {
    const auto& p = builder.program();
    uint64_t used = p.schemas.size() + p.values.size() + p.calls.size() +
                    p.blocks.size() + p.layouts.size() + p.tiles.size() +
                    p.placements.size() + p.kernels.size() + p.tasks.size();
    if (used > options.maximum_records ||
        extra > options.maximum_records - used || used >= physical::none)
      throw Status{ErrorCode::ResourceExhausted, "planner record limit"};
  }
  std::uint64_t work = 0, edges = 0;
  void charge(uint64_t amount) {
    if (amount > options.maximum_work - work)
      throw Status{ErrorCode::ResourceExhausted,
                   "provider planning work exhausted"};
    work += amount;
  }
  template <class F>
  static ps_kernel_status_v8 with(void* ctx, F fn) noexcept {
    auto& self = *static_cast<Builder*>(ctx);
    auto record = [&](Status failure) {
      std::lock_guard<std::mutex> lock(self.mutex);
      if (self.failure.ok())
        self.failure = std::move(failure);
      return kernel_failure(self.failure);
    };
    if (self.thread != std::this_thread::get_id())
      return record({ErrorCode::InvalidArgument,
                     "planner service called on foreign thread"});
    {
      std::lock_guard<std::mutex> lock(self.mutex);
      if (!self.failure.ok())
        return kernel_failure(self.failure);
    }
    try {
      require(!self.pending);
      self.charge(1);
      fn(self);
    } catch (Status& s) {
      return record(std::move(s));
    } catch (const std::bad_alloc&) {
      return record({ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      return record({ErrorCode::InvalidArgument, {}});
    }
    return {};
  }
  TileGrid grid(Id v, Id m) {
    const auto& p = builder.program();
    require(v < p.values.size());
    return get(TileGrid::from_schema(*p.schemas.at(p.values[v].schema),
                                     p.tile_extent, m));
  }
  Id tile(const ps_plan_access_v1& a, Id producer) {
    auto& p = builder.program();
    Key key{a.value, a.member, a.tile};
    require(a.domain <= 2);
    auto old = tiles.find(key);
    if (old != tiles.end()) {
      auto& t = p.tiles[old->second];
      require(t.domain == static_cast<physical::Domain>(a.domain));
      if (producer != physical::none) {
        require(t.producer == physical::none);
        t.producer = producer;
      }
      return old->second;
    }
    records(2);
    auto r = get(grid(a.value, a.member).region(a.tile));
    const auto& tensor = p.schemas[p.values[a.value].schema]->tensors[a.member];
    auto storage = get(tensor_tile_storage(tensor, r, p.tile_extent));
    physical::Layout l;
    l.element_type = tensor.descriptor.element_type;
    l.rank = r.rank();
    l.strides = storage.strides;
    for (unsigned i = 0; i < l.rank; ++i) {
      l.origin[i] = r.dimensions()[i].offset;
      l.shape[i] = r.dimensions()[i].extent;
    }
    require(p.tiles.size() < options.maximum_records &&
            p.layouts.size() < physical::none);
    const Id id = p.tiles.size();
    p.tiles.push_back({a.value, a.member, static_cast<Id>(p.layouts.size()),
                       producer, static_cast<physical::Domain>(a.domain),
                       a.tile, storage.bytes,
                       TensorSpan::element_size(l.element_type)});
    p.layouts.push_back(l);
    p.values[a.value].tiles.push_back(id);
    tiles.emplace(key, id);
    return id;
  }
  void append(const ps_plan_task_v1& t, Id* id) {
    auto& p = builder.program();
    require(!pending && id && t.struct_size == sizeof(t) &&
            t.call < p.calls.size() && t.kind <= 2 && t.placement.domain <= 2 &&
            t.placement.backend <= 1 &&
            (t.invocation_abi == 8 || t.invocation_abi == 9));
    auto n = static_cast<uint64_t>(t.read_count) + t.write_count +
             t.schema_read_count + t.predecessor_count;
    if (n > options.maximum_dependency_edges - edges ||
        p.tasks.size() >= options.maximum_tasks)
      throw Status{ErrorCode::ResourceExhausted, "provider task or edge limit"};
    records(3);
    edges += n;
    charge(n + t.parameter_bytes);
    span(t.reads, t.read_count, n);
    span(t.writes, t.write_count, n);
    span(t.schema_reads, t.schema_read_count, n);
    span(t.predecessors, t.predecessor_count, n);
    span(t.parameters, t.parameter_bytes, options.maximum_work);
    physical::TaskDesc task;
    task.call = t.call;
    task.kind = static_cast<physical::TaskKind>(t.kind);
    const Id ti = p.tasks.size();
    task.placement = p.placements.size();
    p.placements.push_back(
        {static_cast<physical::Backend>(t.placement.backend),
         static_cast<physical::Domain>(t.placement.domain),
         get(host_internal::provider_text(t.placement.profile)),
         get(host_internal::provider_text(t.placement.queue, true))});
    if (task.kind != physical::TaskKind::Copy) {
      task.kernel = p.kernels.size();
      p.kernels.push_back({builder.calls()[t.call].operation,
                           get(host_internal::provider_text(t.symbol)),
                           get(host_internal::provider_text(t.contract)),
                           t.invocation_abi});
    }
    std::copy_n(t.scratch, 3, task.scratch.begin());
    for (unsigned i = 0; i < t.read_count; ++i) {
      auto v = tile(t.reads[i], physical::none);
      task.reads.push_back({t.reads[i].port, v, p.tiles[v].layout});
    }
    for (unsigned i = 0; i < t.write_count; ++i)
      task.writes.push_back(tile(t.writes[i], ti));
    if (t.schema_read_count)
      task.schema_reads.assign(t.schema_reads,
                               t.schema_reads + t.schema_read_count);
    if (t.predecessor_count)
      task.explicit_dependencies.assign(t.predecessors,
                                        t.predecessors + t.predecessor_count);
    if (t.parameter_bytes)
      task.parameters.assign(t.parameters, t.parameters + t.parameter_bytes);
    if (task.kind == physical::TaskKind::Effect) {
      require(p.calls[t.call].effect_task == physical::none);
      p.calls[t.call].effect_task = ti;
    }
    p.tasks.push_back(std::move(task));
    p.calls[t.call].tasks.push_back(ti);
    p.blocks[p.calls[t.call].block].tasks.push_back(ti);
    *id = ti;
  }
};
Status invoke(const ps_planner_module_v1& mod, const ValidatedIR& ir,
              PlanBuilder& b, const PlanningOptions& o) {
  Builder state(ir, b, o);
  auto& p = b.program();
  ps_planner_host_v1 host{
      sizeof(host),
      1,
      static_cast<uint32_t>(ir.form()),
      static_cast<uint32_t>(o.execution_mode),
      p.tile_extent,
      o.maximum_tasks,
      o.maximum_records,
      o.maximum_dependency_edges,
      o.maximum_work,
      static_cast<Id>(p.calls.size()),
      static_cast<Id>(p.values.size()),
      static_cast<Id>(p.blocks.size()),
      &state,
      [](void* ctx, Id i, ps_plan_call_v1* out) {
        return Builder::with(ctx, [&](Builder& s) {
          require(out && i < s.builder.calls().size());
          const auto& c = s.builder.program().calls[i];
          const auto& binding = s.builder.calls()[i];
          *out = {c.source_call,
                  c.block,
                  c.ordinal,
                  static_cast<Id>(binding.inputs.size()),
                  static_cast<Id>(binding.outputs.size()),
                  {binding.operation.data(),
                   static_cast<Id>(binding.operation.size())},
                  binding.inputs.data(),
                  binding.outputs.data()};
        });
      },
      [](void* ctx, Id value, Id member, ps_tile_grid_v8* out) {
        return Builder::with(ctx, [&](Builder& s) {
          require(out);
          auto g = s.grid(value, member);
          *out = {};
          out->port = value;
          out->member = member;
          out->rank = g.rank();
          out->batch_axis_mask = g.batch_axis_mask();
          out->height_axis = g.height_axis();
          out->width_axis = g.width_axis();
          out->tile_count = g.tile_count();
          out->batch_count = g.batch_count();
          out->rows = g.tile_rows();
          out->columns = g.tile_columns();
          std::copy_n(g.shape().data(), 8, out->shape);
        });
      },
      [](void* ctx, Id block, Id output, Id member, uint64_t tile,
         Id* selected) {
        return Builder::with(ctx, [&](Builder& s) {
          const auto& p = s.builder.program();
          require(selected && block < p.blocks.size() &&
                  output < p.blocks[block].return_values.size());
          *selected = 0;
          for (const auto& item : s.builder.outputs())
            if (item.second == output) {
              const auto& m =
                  s.builder.output_requests().at(item.first).members;
              auto found = m.find(member);
              *selected = found != m.end() && found->second.contains(tile);
            }
        });
      },
      [](void* ctx, const ps_plan_task_v1* task, Id* id) {
        return Builder::with(ctx, [&](Builder& s) {
          require(task);
          s.append(*task, id);
        });
      },
      [](void* ctx, ps_plan_bytes_v1* bytes) {
        return Builder::with(ctx, [&](Builder& s) {
          require(bytes);
          s.bytes(s.builder.program(), s.builder.calls(), bytes);
        });
      },
      [](void* ctx, Id block, void* sink, ps_plan_interval_fn_v1 emit) {
        return Builder::with(ctx, [&](Builder& s) {
          require(emit && block < s.builder.program().blocks.size());
          for (const auto& o : s.builder.outputs()) {
            ps_string_v8 name{o.first.data(), static_cast<Id>(o.first.size())};
            auto status =
                kernel_status(emit(sink, name, o.second, physical::none, 0, 0));
            if (!status.ok())
              throw status;
            for (const auto& member :
                 s.builder.output_requests().at(o.first).members)
              for (auto interval : member.second.intervals()) {
                status = kernel_status(emit(sink, name, o.second, member.first,
                                            interval.begin, interval.end));
                if (!status.ok())
                  throw status;
              }
          }
        });
      },
      [](void* ctx, Id di, Id* domain) {
        return Builder::with(ctx, [&](Builder& s) {
          require(domain && di < s.builder.input_declarations().size());
          auto f = s.options.input_domains.find(
              s.builder.input_declarations()[di].name);
          *domain = f == s.options.input_domains.end()
                        ? 0
                        : static_cast<Id>(f->second);
        });
      },
      [](void* ctx, Id ci, Id* pure, Id* gpu) {
        return Builder::with(ctx, [&](Builder& s) {
          require(ci < s.builder.calls().size() && pure && gpu);
          auto& t = s.builder.calls()[ci].prepared->traits();
          *pure = t.side_effect_free;
          *gpu = t.supports_gpu;
        });
      },
      [](void* ctx, Id ci, const ps_tile_request_v8* r, Id count, Id backend,
         ps_plan_bytes_v1* out) {
        return Builder::with(ctx, [&](Builder& s) {
          require(out && ci < s.builder.calls().size() && backend <= 1);
          span(r, count, s.options.maximum_records);
          const auto& op = s.builder.calls()[ci];
          std::vector<TileRequest> requests(op.outputs.size());
          std::map<std::pair<Id, Id>, std::vector<TileInterval>> ranges;
          for (Id i = 0; i < count; ++i) {
            require(r[i].output < requests.size());
            ranges[{r[i].output, r[i].member}].push_back(
                {r[i].begin, r[i].end});
          }
          for (auto& e : ranges)
            requests[e.first.first].members.emplace(
                e.first.second,
                get(TileSet::from_intervals(s.session.budget(), e.second.data(),
                                            e.second.size())));
          auto limits = s.limits();
          limits.maximum_records =
              std::max(limits.maximum_records,
                       physical::ValidationLimits{}.maximum_records);
          auto local = get(op.prepared->lower(
              requests, s.builder.program().tile_extent,
              static_cast<physical::Backend>(backend), limits, &s.session));
          PlanCallBinding binding;
          binding.inputs = local.program.inputs;
          binding.outputs = local.outputs;
          s.bytes(local.program, {std::move(binding)}, out);
        });
      },
      [](void* ctx, const char* data, uint64_t count) {
        return Builder::with(ctx, [&](Builder& s) {
          require(!s.pending && s.tiles.empty() && data && count <= SIZE_MAX);
          auto parsed = get(compiler_internal::decode_physical_tables(
              {data, static_cast<size_t>(count)},
              s.builder.program().tile_extent, s.limits(),
              compiler_internal::physical_transfer_limit(s.limits())));
          require(parsed.bindings.size() == s.builder.calls().size());
          for (Id i = 0; i < parsed.bindings.size(); ++i)
            require(parsed.bindings[i].inputs == s.builder.calls()[i].inputs &&
                    parsed.bindings[i].outputs == s.builder.calls()[i].outputs);
          s.pending = std::move(parsed.physical);
        });
      },
      [](void* ctx, uint64_t amount) {
        return Builder::with(ctx, [&](Builder& s) {
          auto status = s.session.budget().consume({amount});
          if (!status.ok())
            throw status;
        });
      },
      [](void* ctx, Id call, Id value) {
        return Builder::with(ctx, [&](Builder& s) {
          require(call < s.builder.calls().size());
          const auto& outs = s.builder.calls()[call].outputs;
          require(std::find(outs.begin(), outs.end(), value) != outs.end());
          auto& exports = s.builder.program().calls[call].returns;
          require(std::find(exports.begin(), exports.end(), value) ==
                  exports.end());
          exports.push_back(value);
        });
      }};
  auto result = kernel_status(mod.plan(mod.context, &host));
  if (!state.failure.ok())
    return state.failure;
  if (!result.ok())
    return result;
  if (state.pending) {
    p = std::move(*state.pending);
    return {};
  }
  for (auto& v : p.values)
    std::sort(v.tiles.begin(), v.tiles.end(), [&](Id a, Id b) {
      const auto& x = p.tiles[a];
      const auto& y = p.tiles[b];
      return std::tie(x.member, x.tile_index) <
             std::tie(y.member, y.tile_index);
    });
  return physical::finalize_program(
      &p, {o.maximum_records, o.maximum_dependency_edges, o.maximum_work});
}
}  // namespace
Status PlannerRegistry::load_plugin(const std::string& path) try {
  core_internal::CallbackEnvironment env;
  if (frozen())
    return {ErrorCode::InvalidArgument, "planner registry frozen"};
  auto owner = get(host_internal::ProviderLibrary::open(path));
  auto* m = get(host_internal::query_provider<ps_planner_module_v1>(
      *owner, "ps_planner_module_get_api_v1"));
  require(m->plan && m->ir_forms && !(m->ir_forms & ~3U));
  PlannerDefinition d;
  d.name = get(host_internal::provider_text(m->name));
  d.version = get(host_internal::provider_text(m->version));
  d.description = get(host_internal::provider_text(m->description, true));
  d.supported_ir_forms.clear();
  for (unsigned f = 0; f < 2; ++f)
    if (m->ir_forms & (1U << f))
      d.supported_ir_forms.push_back(static_cast<IRForm>(f));
  d.plan = [owner, m](const auto& ir, auto& b, const auto& o) {
    return invoke(*m, ir, b, o);
  };
  return add(std::move(d));
} catch (const Status& s) {
  return s;
} catch (const std::bad_alloc&) {
  return {ErrorCode::ResourceExhausted, {}};
} catch (...) {
  return {ErrorCode::OperationFailed, "planner module entry failed"};
}
}  // namespace ps
