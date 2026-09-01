#pragma once

#include <cstddef>
#include <cstdint>

namespace ps::fuzz_testing {

/** @brief Deepest named boundary reached by one operation/IR fuzz input. */
enum class OperationContractIrStage : std::uint32_t {
  /** @brief Operation registration rejected before duplicate-schema proof. */
  RegistrationRejected = 1U,
  /** @brief A constructed duplicate schema reached registry validation. */
  DuplicateSchemaRejected = 2U,
  /** @brief Valid registration reached compiler validation and was rejected. */
  CompilerRejected = 3U,
  /** @brief Valid registration reached and passed the compiler path. */
  CompilerAccepted = 4U,
};

/**
 * @brief Exercises the maintained bounded operation/typed-IR fuzz pipeline.
 * @param data Arbitrary input bytes, nullable only when size is zero.
 * @param size Exact input byte count.
 * @return Deepest named validation/compiler boundary reached.
 * @throws std::bad_alloc If bounded test/harness storage allocation fails.
 * @note The function has no corpus output and owns no persistent registry.
 */
OperationContractIrStage exercise_operation_contract_ir_input(
    const std::uint8_t* data, std::size_t size);

}  // namespace ps::fuzz_testing
