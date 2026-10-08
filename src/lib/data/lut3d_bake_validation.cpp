#include "data/lut3d_bake_validation.hpp"

#include <cstring>
#include <utility>

#include "data/typed_sample_validation.hpp"

namespace ps::input_internal {
Status validate_lut3d_bake_table(
    const ResultRef& result, const ResourceBudget& resources,
    std::uint64_t maximum_window, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume_work) {
  auto decoded = lut3d_bake_description(result.schema());
  if (!decoded.ok())
    return decoded.status();
  auto descriptor = result.descriptor();
  if (!descriptor.ok())
    return descriptor.status();
  if (descriptor.value().tensor_coverage(0).empty())
    return Status::success();
  const auto& spec = decoded.value();
  const auto width = Value::element_size(spec.table_dtype);
  if (maximum_window < width * 3)
    return {ErrorCode::ResourceExhausted,
            "bake table color exceeds validation window",
            FailureReason::CapacityLimit};
  const auto& tensor = result.schema().tensors[0];
  const ValueDescriptor logical{tensor.descriptor.element_type,
                                tensor.sample_shape()};
  auto acquired = result.acquire_tensor(
      descriptor.value(), 0, Region::whole(logical.shape), cancellation);
  if (!acquired.ok())
    return acquired.status();
  auto window = acquired.take_value();
  Status stopped;
  const auto stop = [&] {
    if (cancellation.cancelled()) {
      stopped = Status{ErrorCode::Cancelled, {}};
      return stopped.code;
    }
    auto work = resources.consume({256});
    if (work.ok() && consume_work)
      work = consume_work(256);
    if (!work.ok() && stopped.ok())
      stopped = work;
    return work.code;
  };
  auto checked = validate_color_samples(
      spec.output_description, logical, Region::whole(logical.shape),
      [&](const auto& at) -> Result<double> {
        auto run = window.row_run(at);
        if (!run.ok())
          return Result<double>(run.status());
        if (width == 4) {
          float value;
          std::memcpy(&value, run.value().data, 4);
          return Result<double>(static_cast<double>(value));
        }
        double value;
        std::memcpy(&value, run.value().data, 8);
        return Result<double>(value);
      },
      ErrorCode::OperationFailed, stop);
  return stopped.ok() ? checked : stopped;
}
}  // namespace ps::input_internal
