#include "execution/result_cache_proof.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "execution/result_blocks.hpp"

namespace ps::execution_internal {
Status ResultCacheProof::hash_facts(content_internal::Sha256& hash,
                                    const ResultRef& object,
                                    const ResultDescriptor& facts) {
  auto charged = work_(1 + object.schema().id.size() + facts.field_count() +
                       facts.tensor_count());
  if (!charged.ok())
    return charged;
  for (std::uint32_t slot = 0; slot < facts.tensor_count(); ++slot) {
    const auto& coverage = facts.tensor_coverage(slot);
    charged =
        work_(coverage.shape().size() +
              coverage.boxes().size() * (1 + 2 * coverage.shape().size()));
    if (!charged.ok())
      return charged;
  }
  hash.integer(facts.sealed());
  hash.integer(facts.field_count());
  for (std::uint32_t field = 0; field < facts.field_count(); ++field)
    hash.integer(facts.rows(field));
  hash.integer(facts.tensor_count());
  for (std::uint32_t slot = 0; slot < facts.tensor_count(); ++slot)
    append_block_footprint(&hash, facts.tensor_coverage(slot));
  hash.text(object.schema().id);
  hash.integer(object.schema().version);
  return Status::success();
}
Result<ResourceString> ResultCacheProof::supplied_facts(
    const ResultObjectInputs& results, const ResultTensorInputs& tensors) {
  content_internal::Sha256 hash;
  hash.text("photospider.result-cache-direct-facts.v2");
  for (const auto& input : results) {
    auto status = work_(1);
    if (!status.ok())
      return Result<ResourceString>(status);
    auto facts = input.second.descriptor(false);
    if (!facts.ok())
      return Result<ResourceString>(facts.status());
    hash.integer(input.first);
    status = hash_facts(hash, input.second, facts.value());
    if (!status.ok())
      return Result<ResourceString>(status);
  }
  for (const auto& input : tensors) {
    auto status = work_(1 + input.second.samples_.boxes().size());
    if (!status.ok())
      return Result<ResourceString>(status);
    hash.integer(input.first.first);
    hash.integer(input.first.second);
    hash.integer(input.second.pieces_.size());
    if (input.second.pieces_.empty()) {
      status = hash_facts(hash, input.second.result_, input.second.descriptor_);
      if (!status.ok())
        return Result<ResourceString>(status);
    } else {
      for (const auto& piece : input.second.pieces_) {
        status = hash_facts(hash, piece.result, piece.descriptor);
        if (!status.ok())
          return Result<ResourceString>(status);
        status = work_(1 + piece.samples.boxes().size() *
                               (1 + 2 * piece.samples.shape().size()));
        if (!status.ok())
          return Result<ResourceString>(status);
        append_block_footprint(&hash, piece.samples);
      }
    }
    append_block_footprint(&hash, input.second.samples_);
  }
  return Result<ResourceString>(
      ResourceString(hash.finish(), ResourceAllocator<char>(root_)));
}
Result<ResourceString> ResultCacheProof::source_digest(
    const std::vector<ExecutionBinding>& bindings,
    const ResourceVector<SourceObservation>& sources) {
  content_internal::Sha256 hash;
  hash.text("photospider.result-cache-sources.v1");
  auto initial = work_(1 + sources.size());
  if (!initial.ok())
    return Result<ResourceString>(initial);
  for (const auto& source : sources) {
    auto status = work_(bindings.size() + source.input.size() +
                        source.samples.boxes().size());
    if (!status.ok())
      return Result<ResourceString>(status);
    auto found = std::find_if(bindings.begin(), bindings.end(),
                              [&](const auto& binding) {
                                return std::string_view(binding.name) ==
                                       std::string_view(source.input);
                              });
    if (found == bindings.end())
      return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
    hash.text(source.input);
    hash.integer(static_cast<std::uint32_t>(source.target));
    hash.integer(source.slot);
    hash.integer(source.roles);
    append_block_footprint(&hash, source.samples);
    if (!found->result.valid())
      return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
    auto descriptor = found->result.descriptor(false);
    if (!descriptor.ok())
      return Result<ResourceString>(descriptor.status());
    if (source.target == ResultSupportTarget::Descriptor) {
      auto charged = work_(found->result.schema().canonical_size() + 1 +
                           descriptor.value().field_count() +
                           descriptor.value().tensor_count());
      if (!charged.ok())
        return Result<ResourceString>(charged);
      hash.text(found->result.schema().canonical());
      charged = hash_facts(hash, found->result, descriptor.value());
      if (!charged.ok())
        return Result<ResourceString>(charged);
      continue;
    }
    auto count = source.samples.element_count();
    if (!count.ok())
      return Result<ResourceString>(count.status());
    if (source.target == ResultSupportTarget::Field) {
      auto width = found->result.schema().row_bytes(source.slot);
      if (!width.ok() || width.value() > maximum_window_ ||
          (width.value() && count.value() > UINT64_MAX / width.value()))
        return Result<ResourceString>(Status{ErrorCode::ResourceExhausted, {}});
      status = work_(count.value() * width.value());
      if (!status.ok())
        return Result<ResourceString>(status);
      status = source.samples.visit(
          [&](const auto& at) {
            auto active = work_(0);
            if (!active.ok())
              return active;
            auto plan = found->result.prepare_read(descriptor.value(),
                                                   source.slot, at[0], 1);
            if (!plan.ok())
              return plan.status();
            auto bytes = plan.value().load(maximum_window_, cancellation_());
            if (!bytes.ok())
              return bytes.status();
            hash.bytes(bytes.value()->bytes().data(),
                       bytes.value()->bytes().size());
            return Status::success();
          },
          count.value(), cancellation_());
    } else if (source.target == ResultSupportTarget::Tensor) {
      if (source.slot >= found->result.schema().tensors.size())
        return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
      const auto width = Value::element_size(
          found->result.schema().tensors[source.slot].descriptor.element_type);
      status = work_(count.value());
      if (!status.ok())
        return Result<ResourceString>(status);
      for (const auto& box : source.samples.boxes()) {
        auto window = found->result.acquire_tensor(
            descriptor.value(), source.slot, box, cancellation_());
        if (!window.ok())
          return Result<ResourceString>(window.status());
        auto samples = Footprint::from_regions(source.samples.shape(), {box});
        if (!samples.ok())
          return Result<ResourceString>(samples.status());
        status = samples.value().visit(
            [&](const auto& at) {
              auto active = work_(0);
              if (!active.ok())
                return active;
              auto run = window.value().row_run(at);
              if (!run.ok())
                return run.status();
              hash.bytes(run.value().data, width);
              return Status::success();
            },
            count.value(), cancellation_());
        if (!status.ok())
          break;
      }
    } else {
      return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
    }
    if (!status.ok())
      return Result<ResourceString>(status);
  }
  auto active = work_(0);
  if (!active.ok())
    return Result<ResourceString>(active);
  return Result<ResourceString>(
      ResourceString(hash.finish(), ResourceAllocator<char>(root_)));
}
}  // namespace ps::execution_internal
