#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "fuzz/operation_contract_ir_fuzz.hpp"
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
  PS_CHECK(exercise_operation_contract_ir_input(valid.data(), valid.size()) ==
           OperationContractIrStage::CompilerAccepted);

  const std::vector<std::uint8_t> duplicate =
      read_seed(PS_OPERATION_DUPLICATE_SEED_PATH);
  PS_CHECK(exercise_operation_contract_ir_input(duplicate.data(),
                                                duplicate.size()) ==
           OperationContractIrStage::DuplicateSchemaRejected);
  return 0;
}
