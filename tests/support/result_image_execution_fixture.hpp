#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <utility>

#include "support/result_image_operation_fixture.hpp"

namespace ps::test_image {
inline void install_scalar_member(
    OperationDefinition& operation, bool consumer, float literal,
    std::shared_ptr<std::atomic<unsigned>> entries) {
  operation.lower = [consumer](const OperationLowering& query) {
    OperationProgramBuilder builder(query);
    for (const auto& member : query.requests[0].members)
      for (auto tile : member.second) {
        auto task =
            take(builder.add_task(consumer ? "gain" : "constant", "member-v1"));
        if (consumer)
          require(builder.read_input(task, 0, 2, 0).ok(), "named scalar input");
        require(builder.write_output(task, 0, member.first, tile).ok(),
                "scalar output");
      }
    return builder.finish();
  };
  operation.prepare_kernel =
      [consumer, literal, entries](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    const auto scalar = [](const KernelView& view) {
      return view.domain == physical::Domain::Host && view.bytes == 4 &&
             view.layout.element_type == ElementType::Float32 &&
             view.layout.rank == 1 && view.layout.shape[0] == 1;
    };
    if (placement.backend != physical::Backend::Cpu ||
        kernel.contract != "member-v1" ||
        kernel.symbol != (consumer ? "gain" : "constant") ||
        query.writes.size() != 1 || !scalar(query.writes[0]) ||
        query.reads.size() != (consumer ? 1U : 0U) ||
        (consumer && (!scalar(query.reads[0]) || query.reads[0].port != 0 ||
                      query.reads[0].member != 2)))
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::InvalidArgument, "named member fixture signature"});
    struct State {
      bool consumer;
      std::array<std::uint8_t, 4> literal;
      std::shared_ptr<std::atomic<unsigned>> entries;
    };
    auto state = std::make_unique<State>();
    state->consumer = consumer;
    std::memcpy(state->literal.data(), &literal, sizeof(literal));
    state->entries = entries;
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = 8;
    result.context = state.release();
    result.release = [](const void* raw) {
      delete static_cast<const State*>(raw);
    };
    result.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      const auto& state =
          *static_cast<const State*>(invocation->prepared_context);
      ++*state.entries;
      const auto* source = state.consumer
                               ? static_cast<const std::uint8_t*>(
                                     invocation->inputs[0].allocation) +
                                     invocation->inputs[0].byte_offset
                               : state.literal.data();
      auto* output =
          static_cast<std::uint8_t*>(invocation->outputs[0].allocation) +
          invocation->outputs[0].byte_offset;
      std::memcpy(output, source, 4);
      *status = {};
    };
    return Result<ps_prepared_kernel_v8>(result);
  };
}
}  // namespace ps::test_image
