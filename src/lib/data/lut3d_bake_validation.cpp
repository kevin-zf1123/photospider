#include "data/lut3d_bake_validation.hpp"

#include <algorithm>
#include <utility>

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
  const auto& spec = decoded.value();
  const auto width = Value::element_size(spec.table_dtype);
  const auto batch_limit =
      std::min<std::uint64_t>(maximum_window, 4096) / (width * 3);
  if (!batch_limit)
    return {ErrorCode::ResourceExhausted,
            "bake table color exceeds validation window",
            FailureReason::CapacityLimit};
  const auto count = descriptor.value().rows(0);
  for (std::uint64_t row = 0; row < count;) {
    if (cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    const auto batch = std::min(batch_limit, count - row);
    auto charged = resources.consume({batch * 3 + 1});
    if (charged.ok() && consume_work)
      charged = consume_work(batch * 3 + 1);
    if (!charged.ok())
      return charged;
    auto read = result.prepare_read(descriptor.value(), 0, row, batch);
    if (!read.ok())
      return read.status();
    auto window = read.value().load(maximum_window, cancellation);
    if (!window.ok())
      return window.status();
    auto value = Value::from_storage({spec.table_dtype, {count, 3}},
                                     Region({{row, batch}, {0, 3}}),
                                     {0,
                                      {static_cast<std::int64_t>(width * 3),
                                       static_cast<std::int64_t>(width)},
                                      {row, 0}},
                                     window.take_value());
    if (!value.ok())
      return value.status();
    auto checked = validate_color_array_value(
        spec.output_description, value.value(), ErrorCode::OperationFailed,
        [&] {
          return cancellation.cancelled() ? ErrorCode::Cancelled
                                          : ErrorCode::Ok;
        });
    if (!checked.ok())
      return checked;
    row += batch;
  }
  return Status::success();
}
}  // namespace ps::input_internal
