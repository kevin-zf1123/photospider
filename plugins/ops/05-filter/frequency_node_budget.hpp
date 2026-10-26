#pragma once

#include <cstring>
#include <limits>
#include <queue>
#include <utility>

#include "05-filter/frequency.hpp"

namespace ps::plugin_internal::frequency {
namespace node_budget_internal {
// A versioned trailer is consumed only by this wrapper. Original task bytes
// remain a prefix, with their original lifetime and encoding intact.
inline constexpr std::uint64_t magic = UINT64_C(0x4652514255440001);
inline void append(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    bytes->push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
inline std::uint64_t decode(const std::vector<std::uint8_t>& bytes,
                            std::size_t offset) {
  std::uint64_t result = 0;
  for (unsigned i = 0; i < 8; ++i)
    result |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
  return result;
}
struct Owner {
  ps_prepared_kernel_v9 inner{};
  std::uint64_t limit = 0, parameter_bytes = 0;
  bool incoming = false, outgoing = false;
  ~Owner() {
    if (inner.release)
      inner.release(inner.context);
  }
};
struct Proxy {
  ps_kernel_work_v9 host{};
  ps_kernel_cancellation_v8 cancellation{};
  std::uint64_t remaining = 0;
  ps_kernel_status_v8 failure{};
  ps_kernel_status_v8 consume(std::uint64_t work, std::uint64_t stages) {
    // Match Work: first failure is sticky; the executor separately retains
    // cancellation's terminal priority. No host requests follow a refusal.
    if (failure.code)
      return failure;
    if (cancellation.is_cancelled &&
        cancellation.is_cancelled(cancellation.context))
      failure = kernel_failure(ErrorCode::Cancelled, "FRQ node cancelled",
                                FailureReason::Cancelled);
    else if (work > remaining)
      failure = kernel_failure(ErrorCode::ResourceExhausted,
                                "FRQ node work limit", FailureReason::WorkLimit);
    else {
      const auto status = host.consume(host.context, work, stages);
      if (status.code > PS_KERNEL_INTERNAL_V8 ||
          status.reason > PS_KERNEL_REASON_VIEW_UNAVAILABLE_V8)
        failure = kernel_failure(ErrorCode::Internal,
                                  "FRQ node malformed host status",
                                  FailureReason::MalformedEnvelope);
      else if (status.code)
        failure = status;
      else
        remaining -= work;
    }
    return failure;
  }
};
inline void invoke(const ps_kernel_invocation_v9* invocation,
                    ps_kernel_status_v8* status) {
  const auto& owner =
      *static_cast<const Owner*>(invocation->views.prepared_context);
  std::uint64_t remaining = owner.limit;
  if (owner.incoming) {
    const auto& token = invocation->views.inputs[invocation->views.input_count-1];
    std::memcpy(&remaining, static_cast<const std::uint8_t*>(token.allocation) +
                                token.byte_offset, sizeof(remaining));
  }
  Proxy proxy{invocation->work, invocation->views.cancellation, remaining, {}};
  auto mapped = *invocation;
  mapped.views.prepared_context = owner.inner.context;
  mapped.views.parameter_bytes = owner.parameter_bytes;
  mapped.views.input_count -= owner.incoming;
  mapped.views.output_count -= owner.outgoing;
  mapped.work.context = &proxy;
  mapped.work.consume = [](void* context, std::uint64_t work,
                           std::uint64_t stages) {
    return static_cast<Proxy*>(context)->consume(work, stages);
  };
  owner.inner.invoke_cpu(&mapped, status);
  if (proxy.failure.code)
    *status = proxy.failure;
  if (status->code == 0 && owner.outgoing) {
    const auto& token = invocation->views.outputs[invocation->views.output_count-1];
    std::memcpy(static_cast<std::uint8_t*>(token.allocation) + token.byte_offset,
                &proxy.remaining, sizeof(proxy.remaining));
  }
}
inline void validate_token(const KernelView& view) {
  require(view.port == physical::none && view.member == 0 &&
              view.domain == physical::Domain::Host && view.bytes == 8 &&
              view.layout.element_type == ElementType::Int64 &&
              view.layout.rank == 1 && view.layout.shape[0] == 1 &&
              view.layout.origin[0] == 0 && view.layout.strides[0] == 8 &&
              view.layout.byte_offset == 0,
          "FRQ node token layout");
}
}  // namespace node_budget_internal

inline OperationDefinition node_budget_operation(OperationDefinition def) {
  using namespace node_budget_internal;  // NOLINT(build/namespaces)
  const auto lower = def.lower;
  def.lower = [lower](const OperationLowering& q) -> Result<OperationProgram> {
    try {
      const auto& state = *static_cast<const Frequency*>(q.static_state);
      auto result = lower(q);
      if (!result.ok() || state.work_limit == INT64_MAX)
        return result;
      const auto old = result.take_value();
      const auto& p = old.program;
      OperationProgramBuilder builder(q);
      std::vector<physical::Id> tasks(p.tasks.size(), physical::none);
      std::vector<physical::Id> values(p.values.size(), physical::none);
      std::vector<physical::Id> tiles(p.tiles.size(), physical::none);
      std::vector<physical::Id> output_ports(p.values.size(), physical::none);
      for (unsigned i = 0; i < old.outputs.size(); ++i)
        output_ports[old.outputs[i]] = i;
      for (unsigned i = 0; i < p.values.size(); ++i)
        if (p.values[i].kind == physical::ValueKind::Produced &&
            output_ports[i] == physical::none)
          values[i] = take(builder.add_temporary(p.schemas[p.values[i].schema]));
      std::vector<std::size_t> pending(p.tasks.size());
      std::priority_queue<physical::Id, std::vector<physical::Id>,
                          std::greater<physical::Id>> ready;
      for (physical::Id i = 0; i < p.tasks.size(); ++i) {
        pending[i] = p.tasks[i].predecessors.size();
        if (!pending[i])
          ready.push(i);
      }
      const auto token_schema = std::make_shared<SchemaTemplate>(
          generation::tensor_schema(ElementType::Int64, {1}));
      physical::Id previous_token = physical::none;
      std::size_t visited = 0;
      while (!ready.empty()) {
        const auto id = ready.top();
        ready.pop();
        const auto& source = p.tasks[id];
        require(source.kind == physical::TaskKind::Kernel,
                "FRQ node budget CPU tasks");
        const auto& kernel = p.kernels[source.kernel];
        const auto task = take(builder.add_task(
            kernel.symbol, kernel.contract, p.placements[source.placement],
            source.scratch));
        tasks[id] = task;
        builder.parameters(task) = source.parameters;
        for (const auto& read : source.reads) {
          const auto& tile = p.tiles[read.tile];
          if (p.values[tile.value].kind == physical::ValueKind::Input)
            check(builder.read_input(task, read.port, tile.member,
                                      tile.tile_index, tile.domain));
          else {
            require(tiles[read.tile] != physical::none,
                    "FRQ node private producer order");
            check(builder.read_tile(task, tiles[read.tile], read.port));
          }
        }
        for (const auto value : source.schema_reads) {
          require(p.values[value].kind == physical::ValueKind::Input,
                  "FRQ node schema input");
          check(builder.read_input_schema(task, p.values[value].owner));
        }
        for (const auto tile_id : source.writes) {
          const auto& tile = p.tiles[tile_id];
          if (output_ports[tile.value] != physical::none)
            check(builder.write_output(task, output_ports[tile.value],
                                        tile.member, tile.tile_index));
          else
            tiles[tile_id] = take(builder.write_temporary(
                task, values[tile.value], tile.member, tile.tile_index));
        }
        for (const auto predecessor : source.explicit_dependencies)
          check(builder.depends_on(task, tasks[predecessor]));
        if (previous_token != physical::none)
          check(builder.read_tile(task, previous_token, physical::none));
        const bool incoming = previous_token != physical::none;
        const bool outgoing = ++visited < p.tasks.size();
        if (outgoing) {
          const auto value = take(builder.add_temporary(token_schema));
          previous_token = take(builder.write_temporary(task, value, 0, 0));
        }
        auto& parameters = builder.parameters(task);
        const auto original_bytes = parameters.size();
        append(&parameters, magic);
        append(&parameters, original_bytes);
        append(&parameters, incoming | (static_cast<std::uint64_t>(outgoing)<<1));
        for (const auto next : source.successors)
          if (--pending[next] == 0)
            ready.push(next);
      }
      require(visited == p.tasks.size(), "FRQ node topological order");
      auto wrapped = take(builder.finish());
      for (auto& kernel : wrapped.program.kernels)
        kernel.abi_version = 9;
      check(physical::finalize_program(&wrapped.program, q.limits));
      return Result<OperationProgram>(std::move(wrapped));
    } catch (const Status& failure) {
      return Result<OperationProgram>(failure);
    }
  };
  const auto prepare = def.prepare_kernel_v9;
  def.prepare_kernel_v9 =
      [prepare](const auto& desc, const auto& placement,
                 const KernelPreparation& q, const auto& resources,
                 const auto* native) -> Result<ps_prepared_kernel_v9> {
    try {
      const auto limit = static_cast<const Frequency*>(q.static_state)->work_limit;
      if (limit == INT64_MAX)
        return prepare(desc, placement, q, resources, native);
      require(q.task_parameters.size() >= 24, "FRQ node token trailer");
      const auto offset = q.task_parameters.size()-24;
      require(decode(q.task_parameters, offset) == magic &&
                  decode(q.task_parameters, offset+8) == offset,
              "FRQ node token version");
      const auto flags = decode(q.task_parameters, offset+16);
      require(flags <= 3, "FRQ node token flags");
      auto owner = std::make_unique<Owner>();
      owner->limit = limit;
      owner->parameter_bytes = offset;
      owner->incoming = flags & 1;
      owner->outgoing = flags & 2;
      if (owner->incoming) {
        require(!q.reads.empty(), "FRQ node input token");
        validate_token(q.reads.back());
      }
      if (owner->outgoing) {
        require(!q.writes.empty(), "FRQ node output token");
        validate_token(q.writes.back());
      }
      const std::vector<std::uint8_t> original(q.task_parameters.begin(),
                                              q.task_parameters.begin()+offset);
      KernelPreparation inner(q.operation, q.inputs, q.parameters, original);
      inner.outputs = q.outputs;
      inner.tile_extent = q.tile_extent;
      inner.static_state = q.static_state;
      inner.scratch = q.scratch;
      inner.reads.assign(q.reads.begin(), q.reads.end()-owner->incoming);
      inner.writes.assign(q.writes.begin(), q.writes.end()-owner->outgoing);
      owner->inner = take(prepare(desc, placement, inner, resources, native));
      require(owner->inner.invoke_cpu != nullptr, "FRQ node CPU callback");
      auto kernel = owner->inner;
      kernel.context = owner.release();
      kernel.invoke_cpu = invoke;
      kernel.release = [](const void* p) { delete static_cast<const Owner*>(p); };
      return Result<ps_prepared_kernel_v9>(kernel);
    } catch (const Status& failure) {
      return Result<ps_prepared_kernel_v9>(failure);
    }
  };
  return def;
}
}  // namespace ps::plugin_internal::frequency
