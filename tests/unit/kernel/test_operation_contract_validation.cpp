#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "fuzz/operation_contract_ir_fuzz.hpp"
#include "photospider/plugin/operation_registry.hpp"
#include "support/test_support.hpp"

#ifndef PS_OPERATION_VALID_SEED_PATH
#error "PS_OPERATION_VALID_SEED_PATH must name the maintained valid seed"
#endif
#ifndef PS_OPERATION_DUPLICATE_SEED_PATH
#error \
    "PS_OPERATION_DUPLICATE_SEED_PATH must name the maintained duplicate seed"
#endif

namespace {

/**
 * @brief Reads one exact committed fuzz seed without generating corpus data.
 * @param path Source-tree seed path supplied by CMake.
 * @return Complete seed bytes.
 * @throws std::runtime_error If the seed cannot be opened.
 * @throws std::bad_alloc If byte storage allocation fails.
 * @note The file is read-only and no libFuzzer output directory is involved.
 */
std::vector<std::uint8_t> read_seed(const char* path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("could not open maintained fuzz seed");
  }
  const std::string bytes{std::istreambuf_iterator<char>(stream),
                          std::istreambuf_iterator<char>()};
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

}  // namespace

/**
 * @brief Proves both maintained seeds reach their named long-lived stages.
 * @return Zero when valid/compiler and duplicate-schema stages are exact.
 * @throws std::runtime_error If a committed seed cannot be opened.
 * @throws std::bad_alloc If bounded harness allocation fails.
 * @note This ordinary CTest seam generates no corpus; the manual target still
 * supplies libFuzzer execution coverage.
 */
int main() {
  using ps::fuzz_testing::exercise_operation_contract_ir_input;
  using ps::fuzz_testing::OperationContractIrStage;

  const std::vector<std::uint8_t> valid =
      read_seed(PS_OPERATION_VALID_SEED_PATH);
  PS_CHECK(!valid.empty() && valid[0] == ps::OperationTraits{}.version);
  PS_CHECK(exercise_operation_contract_ir_input(valid.data(), valid.size()) ==
           OperationContractIrStage::CompilerAccepted);
  // Physical lowering handles all scalar types and both free and atomic
  // rank-one members. Invalid atomic rank fails before lowering.
  for (std::uint8_t type = 0; type < 7; ++type) {
    for (std::uint8_t atomic = 0; atomic < 2; ++atomic) {
      auto tensor = valid;
      tensor[1] = type;
      tensor[2] = 15;  // Largest bounded source extent.
      tensor[3] = atomic;
      PS_CHECK(
          exercise_operation_contract_ir_input(tensor.data(), tensor.size()) ==
          OperationContractIrStage::CompilerAccepted);
    }
  }
  auto invalid_rank = valid;
  invalid_rank[3] = 2;
  PS_CHECK(exercise_operation_contract_ir_input(invalid_rank.data(),
                                                invalid_rank.size()) ==
           OperationContractIrStage::RegistrationRejected);

  auto invalid_workflow = valid;
  invalid_workflow[6] =
      0;  // Select an unknown operation after valid registration.
  PS_CHECK(exercise_operation_contract_ir_input(invalid_workflow.data(),
                                                invalid_workflow.size()) ==
           OperationContractIrStage::CompilerRejected);
  PS_CHECK(exercise_operation_contract_ir_input(nullptr, 0) ==
           OperationContractIrStage::RegistrationRejected);
  const auto current_version = ps::OperationTraits{}.version;
  for (const auto version : {0U, current_version - 1U, current_version + 1U}) {
    auto unsupported = valid;
    unsupported[0] = static_cast<std::uint8_t>(version);
    PS_CHECK(exercise_operation_contract_ir_input(unsupported.data(),
                                                  unsupported.size()) ==
             OperationContractIrStage::RegistrationRejected);
  }

  const std::vector<std::uint8_t> duplicate =
      read_seed(PS_OPERATION_DUPLICATE_SEED_PATH);
  PS_CHECK(!duplicate.empty() && duplicate[0] == ps::OperationTraits{}.version);
  PS_CHECK(exercise_operation_contract_ir_input(duplicate.data(),
                                                duplicate.size()) ==
           OperationContractIrStage::DuplicateSchemaRejected);
  return 0;
}
