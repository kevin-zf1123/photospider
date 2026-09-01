#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/plugin/operation_plugin_api.h"

namespace {

/**
 * @brief Bounded byte reader used by the manual operation/IR fuzz harness.
 *
 * @note Exhaustion returns zero deterministically; it never reads past input.
 */
class ByteReader final {
 public:
  /**
   * @brief Binds one immutable fuzz input.
   * @param data Input bytes, nullable only when size is zero.
   * @param size Exact byte count.
   * @throws Nothing.
   */
  ByteReader(const std::uint8_t* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  /**
   * @brief Consumes one byte or returns zero at exhaustion.
   * @return Next byte or zero.
   * @throws Nothing.
   */
  std::uint8_t next() noexcept {
    return offset_ < size_ ? data_[offset_++] : 0U;
  }

 private:
  /** @brief Borrowed immutable fuzz bytes. */
  const std::uint8_t* data_ = nullptr;
  /** @brief Exact borrowed byte count. */
  std::size_t size_ = 0U;
  /** @brief Next unread byte offset. */
  std::size_t offset_ = 0U;
};

/**
 * @brief Maps one fuzz byte into a bounded source parameter value.
 * @param reader Bounded fuzz reader.
 * @param selector Variant selector byte.
 * @return One closed `ParameterValue` alternative.
 * @throws std::bad_alloc If a short string allocation fails.
 */
ps::ParameterValue parameter_value(ByteReader* reader, std::uint8_t selector) {
  switch (selector % 4U) {
    case 0U:
      return static_cast<std::int64_t>(reader->next());
    case 1U:
      return static_cast<double>(reader->next()) / 3.0;
    case 2U:
      return reader->next() % 2U != 0U;
    default:
      return std::string(reader->next() % 16U, 'x');
  }
}

}  // namespace

/**
 * @brief Fuzzes operation-v2 vocabulary, parameter schema, and typed IR gates.
 * @param data Arbitrary libFuzzer bytes.
 * @param size Exact byte count.
 * @return Always zero after bounded validation/compile attempts.
 * @throws Nothing across the libFuzzer C boundary.
 * @note This target is manual, owns no corpus output, and is never registered
 * with CTest; fixed DSO fixtures separately cover hostile raw pointers/sizes.
 */
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) noexcept {
  try {
    ByteReader reader(data, size);
    auto operations = std::make_shared<ps::OperationRegistry>();
    ps::OperationTraits traits;
    traits.version = reader.next() % 4U;
    traits.output_element_type = static_cast<ps::ElementType>(
        (reader.next() % 5U) + PS_OPERATION_ELEMENT_UINT8_V2);
    traits.shape_rule =
        static_cast<ps::OperationShapeRule>((reader.next() % 6U) + 1U);
    traits.region_rule =
        static_cast<ps::OperationRegionRule>((reader.next() % 5U) + 1U);
    traits.halo_radius = reader.next();
    if (traits.shape_rule == ps::OperationShapeRule::Fixed) {
      traits.fixed_output_shape = {
          static_cast<std::uint64_t>(reader.next() % 16U)};
    }
    const std::uint8_t schema_count = reader.next() % 5U;
    for (std::uint8_t index = 0U; index < schema_count; ++index) {
      const std::uint8_t key_selector = reader.next() % 4U;
      const std::string key =
          key_selector == 0U
              ? "value"
              : (key_selector == 1U
                     ? "duplicate"
                     : (key_selector == 2U ? std::string("bad\x01", 4U)
                                           : "parameter"));
      traits.parameter_schema.push_back(ps::OperationParameterSpec{
          key,
          static_cast<ps::OperationParameterType>((reader.next() % 6U) + 1U),
          reader.next() % 2U != 0U});
    }
    const ps::Status registered =
        operations->register_operation(ps::OperationDefinition{
            "fuzz.operation", std::move(traits),
            [](const ps::OperationInvocation&) -> ps::Result<ps::Value> {
              return ps::Result<ps::Value>(ps::Value::from_float64(0.0));
            }});
    if (!registered.ok()) {
      return 0;
    }
    operations->freeze();
    ps::WorkflowDocument document;
    ps::WorkflowNode node;
    node.id = reader.next() % 2U == 0U ? 1U : 0U;
    node.operation = reader.next() % 3U == 0U ? "unknown" : "fuzz.operation";
    const std::uint8_t parameter_count = reader.next() % 5U;
    for (std::uint8_t index = 0U; index < parameter_count; ++index) {
      const std::string key = reader.next() % 2U == 0U ? "value" : "unknown";
      node.parameters.insert_or_assign(key,
                                       parameter_value(&reader, reader.next()));
    }
    document.nodes.push_back(std::move(node));
    document.outputs = {ps::WorkflowOutput{"value", 1U, "value"}};
    ps::GraphContext graph(std::move(document));
    ps::Compiler compiler(std::move(operations));
    static_cast<void>(compiler.compile(graph));
  } catch (...) {
    return 0;
  }
  return 0;
}
