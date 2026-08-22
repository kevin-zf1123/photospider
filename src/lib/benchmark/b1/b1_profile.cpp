/**
 * @file b1_profile.cpp
 * @brief Implements the frozen B1 identity, digest, trace, and oracle model.
 */
#include "benchmark/b1/b1_profile.hpp"

#include <fenv.h>  // NOLINT(build/c++11)

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "photospider/data/image_view.hpp"

namespace ps::benchmark {
namespace {

/** @brief Exact M1 workload token accepted by the shared occurrence schema. */
constexpr char kM1WorkloadId[] = "M1-shared-v1";

/** @brief Fixed curve coefficients in candidate and oracle stage order. */
constexpr std::array<float, 4U> kB1CurveCoefficients{
    0.80F, 1.00F, 1.20F, 1.40F};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief One immutable lowercase-hex fixture row compiled into the product.
 * @throws Nothing for aggregate initialization.
 */
struct B1GoldenHex final {
  /** @brief Required source seed/job index. */
  std::uint64_t job_index;
  /** @brief Typed logical content digest bytes as lowercase hexadecimal. */
  std::string_view logical;
  /** @brief Raw tight little-endian payload digest as lowercase hexadecimal. */
  std::string_view raw;
};

/**
 * @brief Exact generated-once immutable goldens for the required B1 corpus.
 * @note These constants were independently generated through
 * `compute_b1_job_golden` and are verified against that oracle in tests. The
 * logical entries bind DenseTensor schema/Image facet structural version 2 and
 * normalized `[0,1]` Sample Domain facet structural version 1; raw-payload
 * entries are unchanged by descriptor framing.
 */
constexpr std::array<B1GoldenHex, 34U> kB1FrozenGoldens{{
    {0U, "f34bab95bb6a192fdcd648d88e5b4192b26325d8ed4c394f297f5c6471d6340a",
     "7b9a42e86a52426f1ff7b4bca35db272fcdb26c32c838991fbefd272e8562812"},
    {1U, "4eaa7c0d9bb9dd4adeef7046f9676269b2fcdb31edb9bf5b55ad76f73f70b3e8",
     "e739aaab34a8c6e782935b729791614015a85e606d8240f1e7c46ec260602793"},
    {2U, "29e01a3bad47c0d297ef071cf245a484acd249ae713f772c89b026a20359ebf4",
     "91bbf50085c42ac2445bbb4c0cd3f61b8e367609b69ec8b5bdb758b4f87e076f"},
    {3U, "81f63832069efe5e5372275edb0aa64fe8d964e6af6f96bf1147e8b990abbd20",
     "57972567c02c23a2ac00e4445ab92e13cbc43f52a83957ee7f83d2912abe5f60"},
    {4U, "526c7691a134bbff046fdfde9b5e2c3f5b1bb241ae555635f133be3cfcd9f3d7",
     "346bc873431f17f531d753e5c871be4edad794d7807081c017dfc6d672329a12"},
    {5U, "98b6038777bca6e16a1e21badc014914662bede4157943364668e456162952e4",
     "7c9c1afd04c2a9f4e4a8f61fb4382a3a626c6ac654c4787228dc308d2b1287d9"},
    {6U, "d9e45d8cae839ccdf275d9d2b7427d757a7a5796fef370a7ab0fdb136a786761",
     "09051222d3524a4d82c5eff549cc600ca107c512daf013ee5086a92d20b3c35a"},
    {7U, "aa4700ee932408ff4f51ec64045ff4eaa8222a264913be66cc6dc75942b0dfdd",
     "4faafcf41910b4c2340f6bca9c0e80fc403846593348bca468ac08e62b05ca11"},
    {8U, "817feaa6e4c6898c73e433a4e79b556c71093074960c1bd9183f794f5ddcf93f",
     "ddc068294e3cc07066339f7b17b17810a2930ca85a33fa5c5cf582393ca4282a"},
    {9U, "f3ab03c0608769fe0aa9e6815bcc0507b4520ee967a783bba189700738d97879",
     "788da5ab090ce04ec4c5ac9d4965f750b3aa43f6230c55a261af27a9db681042"},
    {10U, "286a3e39c44731e4941a5c5a86d2e16e2009d3bf96441b3ad06bd4973781cd1d",
     "af9c02c202b91d5c2b1f1e804b53f66af84894d94badec27c2dd8b0779d44121"},
    {11U, "deb3a94f842395a7921f0f225b2660da4adb005b268a3ad3c661726936301b61",
     "426accdf10eea1af785a32f4ac253da790c5e3802829f29705c16a4e49cdbce4"},
    {12U, "81ecbc027e53b50a6c299b8ed6798ca8a94f1100c684a9dd604d8ae9679e43bc",
     "262dcb21a97a38a6fe6e7c001e50130d60b91b7c026014974344e900c87fc913"},
    {13U, "fda694a5bab825b433f01598b42335c0c9a4593ac51f6deb1b0ee62b01b5e01c",
     "ef2bd6228e971be6336215eaca898ff0a8851dc259103cf33c745daed4f09c37"},
    {14U, "b50f244e7216de67ae51e5369d51e50d99caff6de950ac1b44e1094c29dbc04e",
     "e64612fa2cc2ee08017bdaca3f1c3d0919102ada8a0ce81bb545293464e24d5b"},
    {15U, "5f588c98ec4dccf9145a5ccc8b7e4025a7ec910b0903eec38f78606c7338ff95",
     "a747b7828290c75d239dc50329563cd925d8db8a07bee82acac044b1a3f20df9"},
    {16U, "e416aa05eead4d0fe14c25763d6c71383ef92a3bb6e28dfde1e131d46575a78b",
     "ca79395a5f2ed3b15e28417b9b889c7d15514ae0536eb56c6b4c17944192b8bc"},
    {17U, "358b6ba9533857f7a5263948b5eb7705da4bd97afbc4ef961563be758a479a35",
     "cc60add15fbc5db583b7f1d22b5003540b69c69a07b2c84b93c459b094801594"},
    {18U, "9dcfa7cc447216e2303a1fddc66c2ebe5f4206435356425027a06ce214816a64",
     "0f5f4656980129271c3e9775f95b9842f6f91be9e51b9bbd1271731db05a0efd"},
    {19U, "a111a5f4719743b0f4ea69e4703cd7e376aec3519b6cb35c215bc70da50d5e7e",
     "a51b437625f047837599c761de701d5983a0c5c2e22918f664b966b3ea79c964"},
    {20U, "01fc49e41a84f032da520662e59636ae99bab452391d1ab2c49b46e613dc2117",
     "2e35958d388fe07640970f0fda8dbc938a7112811f53b9d067fb5b255a936e97"},
    {21U, "c0d3cfa8b5b776c06f4dc608624a315deb30d8c8ce6280316ec1d2aacede2456",
     "2ba83af22837cece1286b5f1d194bede5faa3e1ca81bbe27ce77f4f0ce25f978"},
    {22U, "fc242c14562c3b5ed55136a7581fc4be91ed80641f0a2daae5304e06f8f6bf79",
     "841796843748b1157e039dc7f64bfc6e9e23a1cb36a6ba00a0483018c6752035"},
    {23U, "5d6b81d77390e1a811bdba463319e029a889fd3b4d33e0b0509c47cf9dfb68d9",
     "5c5e0654fc91dfcc232bd0addc51549b4a0d314ee1ce730ecad88d5d7efb0e5b"},
    {24U, "7c09f6b14f33d94c9bd2f93db0b839bfe44bc5203461b5b823276b538ded1f87",
     "8d21b113d0c133b94527842209c789f8de76b576fc54c997d566946c7d6eecff"},
    {25U, "69cda5f3aa875fb45e5bd667aeeb5e07d276ffd654a00a8febfdb614494bcfa7",
     "efb01d71f6cdfa58bd72df057d11752698541ec9c92bd219642a03e5851a7ab6"},
    {26U, "3002cda0301e841e0730db75c91b030f09a5e3a9a9fdc66bf1b6eb8e11749484",
     "f08d7c6888e1208a6ac25a539afac43c1b5e9d0761eabd1589281e3275a90092"},
    {27U, "00b25c92468e8f45161795b95c6f56edc668f408e18abfd35f11ec955c772587",
     "7e804f45f22e4acf03746b0780ea4aa744357182b574a9753d0f401690eb4eb1"},
    {28U, "d45c6e63078a3bf9260f97074ba3e27d54c6ed5f7d9bd7273a61426a5b6d6e59",
     "8c96207cd9fa84c6c4c358bf72f7526b61c5e2dfad60d870c6f8759afff4ae0c"},
    {29U, "34fa5625f0ced62e32f4259513d1f6b5c8725e624bb10ce3599ea098bb6a17c2",
     "2fc324176d11f46c60857ff580f6018d0a9645f6cc12c5da012d3c475239a4c2"},
    {252U, "0d57a1cdbe33713ef2b7b2b4d883fd499bcf965a1a0e20d358bc49fab72f3291",
     "b3510eb2251009bfbd6acd1866d285ea4f2f8c2817d2e79b6a91e5073e5e2ffe"},
    {253U, "755500b88d26980cb047048d72a014f1a02cb13daa5d718fd5d072e6004c34df",
     "fdc3fbae4b7f911b5fc96e380187e171efe30a3a6bfa0042035350d674505f7c"},
    {254U, "bbcf8089bcd1c9c8ceb3bae4b43dbb799908eec4286978d55002b78c9403dc31",
     "0da08a669853561f4bdaaba06321f8ba8da161f25ab863775f8687f22a000ee6"},
    {255U, "2277a67645f7f92981568cdc5da7a1455a9e42db865801035d62fa8219e4ca3e",
     "9d526df7b9103dbab571f04ecc93a654d05ddaadc9d3f1f41634774911d4587d"},
}};

/**
 * @brief Returns the canonical sort rank of one phase.
 * @param phase Valid phase.
 * @return Cold zero, warmup one, or measured two.
 * @throws std::invalid_argument for an invalid enum representation.
 */
int phase_rank(B1JobPhase phase) {
  switch (phase) {
    case B1JobPhase::Cold:
      return 0;
    case B1JobPhase::Warmup:
      return 1;
    case B1JobPhase::Measured:
      return 2;
  }
  throw std::invalid_argument("B1 job phase is invalid.");
}

/**
 * @brief Returns the canonical sort rank of one Graph role.
 * @param role Valid role.
 * @return Graph A zero or Graph B one.
 * @throws std::invalid_argument for an invalid enum representation.
 */
int graph_rank(B1GraphRole role) {
  switch (role) {
    case B1GraphRole::A:
      return 0;
    case B1GraphRole::B:
      return 1;
  }
  throw std::invalid_argument("B1 Graph role is invalid.");
}

/**
 * @brief Returns the canonical sort rank of one semantic action.
 * @param action Valid action.
 * @return Ready zero, start one, or terminal two.
 * @throws std::invalid_argument for an invalid enum representation.
 */
int action_rank(B1SemanticAction action) {
  switch (action) {
    case B1SemanticAction::Ready:
      return 0;
    case B1SemanticAction::Start:
      return 1;
    case B1SemanticAction::Terminal:
      return 2;
  }
  throw std::invalid_argument("B1 semantic action is invalid.");
}

/**
 * @brief Returns the exact canonical token for one semantic action.
 * @param action Valid action.
 * @return Process-lifetime token.
 * @throws std::invalid_argument for an invalid enum representation.
 */
const char* action_name(B1SemanticAction action) {
  switch (action) {
    case B1SemanticAction::Ready:
      return "ready";
    case B1SemanticAction::Start:
      return "start";
    case B1SemanticAction::Terminal:
      return "terminal";
  }
  throw std::invalid_argument("B1 semantic action is invalid.");
}

/**
 * @brief Parses one exact canonical semantic action token.
 * @param token Candidate token.
 * @return Parsed action.
 * @throws std::invalid_argument for an unknown token.
 */
B1SemanticAction parse_action(std::string_view token) {
  if (token == "ready") {
    return B1SemanticAction::Ready;
  }
  if (token == "start") {
    return B1SemanticAction::Start;
  }
  if (token == "terminal") {
    return B1SemanticAction::Terminal;
  }
  throw std::invalid_argument("B1 semantic action token is invalid.");
}

/**
 * @brief Parses one exact unpadded uint64 token.
 * @param text Candidate ASCII token.
 * @return Parsed unsigned value.
 * @throws std::invalid_argument for empty, padded, malformed, or overflowed
 * text.
 */
std::uint64_t parse_canonical_uint64(std::string_view text) {
  if (text.empty() || (text.size() > 1U && text.front() == '0')) {
    throw std::invalid_argument("B1 uint64 spelling is not canonical.");
  }
  std::uint64_t value = 0U;
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    throw std::invalid_argument("B1 uint64 spelling is invalid.");
  }
  return value;
}

/**
 * @brief Appends one exact length frame to an output string.
 * @param payload Borrowed frame payload.
 * @param output Non-null destination.
 * @return Nothing.
 * @throws std::bad_alloc when destination growth cannot allocate.
 */
void append_frame(std::string_view payload, std::string* output) {
  output->append(std::to_string(payload.size()));
  output->push_back(':');
  output->append(payload);
}

/**
 * @brief Rotates one SHA-256 word right.
 * @param value Input word.
 * @param bits Rotation count in one through thirty-one.
 * @return Rotated word.
 * @throws Nothing.
 */
constexpr std::uint32_t rotate_right(std::uint32_t value,
                                     std::uint32_t bits) noexcept {
  return (value >> bits) | (value << (32U - bits));
}

/**
 * @brief Parses one exact prefixed field from a semantic trace line.
 * @param line Complete line without LF.
 * @param cursor In/out byte cursor.
 * @param prefix Required field name including equals or leading semicolon.
 * @param last Whether the field must consume the rest of the line.
 * @return Borrowed field payload.
 * @throws std::invalid_argument for missing, empty, or reordered framing.
 */
std::string_view parse_trace_field(std::string_view line, std::size_t* cursor,
                                   std::string_view prefix, bool last) {
  if (cursor == nullptr || *cursor > line.size() ||
      line.substr(*cursor, prefix.size()) != prefix) {
    throw std::invalid_argument("B1 semantic trace field order is invalid.");
  }
  *cursor += prefix.size();
  const std::size_t end = last ? line.size() : line.find(';', *cursor);
  if (end == std::string_view::npos || end == *cursor) {
    throw std::invalid_argument("B1 semantic trace field payload is invalid.");
  }
  const std::string_view value = line.substr(*cursor, end - *cursor);
  *cursor = end;
  return value;
}

/**
 * @brief Compares two deterministic task descriptions exactly.
 * @param lhs First task.
 * @param rhs Second task.
 * @return True when every field and dependency matches.
 * @throws Nothing.
 */
bool equal_semantic_task(const B1SemanticTask& lhs,
                         const B1SemanticTask& rhs) noexcept {
  return lhs.job_index == rhs.job_index && lhs.graph == rhs.graph &&
         lhs.task_ordinal == rhs.task_ordinal &&
         lhs.dependencies == rhs.dependencies && lhs.resources == rhs.resources;
}

/**
 * @brief Validates that records form exact frozen plans and action triplets.
 * @param sorted_records Records already in canonical order.
 * @return Nothing for a complete valid trace.
 * @throws std::invalid_argument for any task, action, or outcome drift.
 */
void validate_semantic_records(
    const std::vector<B1SemanticRecord>& sorted_records) {
  if (sorted_records.empty() ||
      sorted_records.size() % (kB1TasksPerJob * 3U) != 0U) {
    throw std::invalid_argument(
        "B1 semantic trace does not contain complete plans.");
  }
  std::size_t cursor = 0U;
  while (cursor < sorted_records.size()) {
    const std::uint64_t job = sorted_records[cursor].task.job_index;
    const std::vector<B1SemanticTask> expected = b1_frozen_semantic_plan(job);
    for (const B1SemanticTask& task : expected) {
      for (int rank = 0; rank < 3; ++rank) {
        if (cursor >= sorted_records.size()) {
          throw std::invalid_argument("B1 semantic trace record is missing.");
        }
        const B1SemanticRecord& record = sorted_records[cursor++];
        if (!equal_semantic_task(record.task, task) ||
            action_rank(record.action) != rank) {
          throw std::invalid_argument(
              "B1 semantic trace task/action identity drifted.");
        }
        const B1SemanticOutcome expected_outcome =
            rank == 2 ? B1SemanticOutcome::Succeeded
                      : B1SemanticOutcome::NotApplicable;
        if (record.outcome != expected_outcome) {
          throw std::invalid_argument("B1 semantic trace outcome is invalid.");
        }
      }
    }
  }
}

/**
 * @brief Owns and restores one complete floating-point environment.
 * @throws std::runtime_error when capture or RNE installation fails.
 * @note Destruction restores the complete prior environment or terminates.
 */
class ScopedB1Binary32RoundToNearest final {
 public:
  /**
   * @brief Captures the caller environment and installs RNE.
   * @throws std::runtime_error on capture, install, or recovery failure.
   */
  ScopedB1Binary32RoundToNearest() {
    if (fegetenv(&previous_) != 0) {
      throw std::runtime_error(
          "B1 oracle cannot capture the floating-point environment.");
    }
    if (fesetround(FE_TONEAREST) != 0) {
      if (fesetenv(&previous_) != 0) {
        throw std::runtime_error(
            "B1 oracle cannot install RNE or restore its environment.");
      }
      throw std::runtime_error("B1 oracle cannot install binary32 RNE.");
    }
  }

  /**
   * @brief Restores the complete captured environment.
   * @throws Nothing; restoration failure terminates.
   */
  ~ScopedB1Binary32RoundToNearest() noexcept {
    if (fesetenv(&previous_) != 0) {
      std::terminate();
    }
  }

  /** @brief Disables duplicate restoration ownership. */
  ScopedB1Binary32RoundToNearest(const ScopedB1Binary32RoundToNearest&) =
      delete;

  /** @brief Disables replacement of restoration ownership. */
  ScopedB1Binary32RoundToNearest& operator=(
      const ScopedB1Binary32RoundToNearest&) = delete;

 private:
  /** @brief Complete caller environment captured before RNE installation. */
  fenv_t previous_{};
};

/**
 * @brief Independently rounds one byte fraction to IEEE binary32 RNE.
 * @param numerator Unsigned numerator in `[0,255]`.
 * @return Exact representation of `numerator / 255`.
 * @throws Nothing.
 */
float b1_reference_byte_fraction(std::uint8_t numerator) noexcept {
  if (numerator == 0U) {
    return 0.0F;
  }
  if (numerator == 255U) {
    const std::uint32_t bits = 0x3f800000U;
    float result = 0.0F;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
  }
  unsigned int highest_bit = 0U;
  for (std::uint32_t value = numerator; value > 1U; value >>= 1U) {
    ++highest_bit;
  }
  int exponent = static_cast<int>(highest_bit) - 8;
  const unsigned int shift = static_cast<unsigned int>(23 - exponent);
  const std::uint64_t scaled = static_cast<std::uint64_t>(numerator) << shift;
  std::uint64_t significand = scaled / 255U;
  const std::uint64_t remainder = scaled % 255U;
  if (remainder * 2U > 255U ||
      (remainder * 2U == 255U && (significand & 1U) != 0U)) {
    ++significand;
  }
  if (significand == (std::uint64_t{1U} << 24U)) {
    significand >>= 1U;
    ++exponent;
  }
  const std::uint32_t bits =
      (static_cast<std::uint32_t>(exponent + 127) << 23U) |
      static_cast<std::uint32_t>(significand - (std::uint64_t{1U} << 23U));
  float result = 0.0F;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

/**
 * @brief Applies one independently specified three-cut binary32 curve stage.
 * @param input Exact binary32 input.
 * @param coefficient Exact binary32 coefficient.
 * @return `RNE(1/RNE(1+RNE(input*coefficient)))`.
 * @throws Nothing.
 */
float b1_reference_curve(float input, float coefficient) noexcept {
  volatile float product = input * coefficient;
  volatile float denominator = 1.0F + product;
  volatile float result = 1.0F / denominator;
  return result;
}

/**
 * @brief Updates a raw hash with one float in exact little-endian byte order.
 * @param value Candidate binary32 sample.
 * @param hash Non-null incremental digest owner.
 * @return Nothing.
 * @throws As `B1Sha256::update`.
 */
void hash_little_endian_float(float value, B1Sha256* hash) {
  static_assert(sizeof(float) == sizeof(std::uint32_t));
  std::uint32_t bits = 0U;
  std::memcpy(&bits, &value, sizeof(bits));
  std::array<std::byte, 4U> bytes{
      static_cast<std::byte>(bits & 0xffU),
      static_cast<std::byte>((bits >> 8U) & 0xffU),
      static_cast<std::byte>((bits >> 16U) & 0xffU),
      static_cast<std::byte>((bits >> 24U) & 0xffU)};
  hash->update(bytes.data(), bytes.size());
}

}  // namespace

bool B1JobInstance::operator==(const B1JobInstance& other) const noexcept {
  return row_workload_id == other.row_workload_id &&
         replicate_ordinal == other.replicate_ordinal && phase == other.phase &&
         cycle_ordinal == other.cycle_ordinal && job_index == other.job_index &&
         run_cap == other.run_cap;
}

bool B1JobInstance::operator<(const B1JobInstance& other) const noexcept {
  try {
    return std::tuple(row_workload_id, replicate_ordinal, phase_rank(phase),
                      cycle_ordinal, job_index, run_cap) <
           std::tuple(other.row_workload_id, other.replicate_ordinal,
                      phase_rank(other.phase), other.cycle_ordinal,
                      other.job_index, other.run_cap);
  } catch (...) {
    std::terminate();
  }
}

bool B1IoTaskIdentity::operator==(
    const B1IoTaskIdentity& other) const noexcept {
  return job == other.job && stage == other.stage && attempt == other.attempt;
}

bool B1Sha256Digest::operator==(const B1Sha256Digest& other) const noexcept {
  return bytes == other.bytes;
}

B1Sha256::B1Sha256() noexcept
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
             0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {
}  // NOLINT(whitespace/indent_namespace)

void B1Sha256::update(const std::byte* data, std::size_t size) {
  if (finished_) {
    throw std::invalid_argument("B1 SHA-256 is already finalized.");
  }
  if (size != 0U && data == nullptr) {
    throw std::invalid_argument("B1 SHA-256 input is null and nonempty.");
  }
  if (size > (std::numeric_limits<std::uint64_t>::max() / 8U) - total_bytes_) {
    throw std::overflow_error("B1 SHA-256 bit length overflowed.");
  }
  total_bytes_ += static_cast<std::uint64_t>(size);
  while (size != 0U) {
    const std::size_t copied = std::min(size, buffer_.size() - buffered_);
    std::memcpy(buffer_.data() + buffered_, data, copied);
    buffered_ += copied;
    data += copied;
    size -= copied;
    if (buffered_ == buffer_.size()) {
      compress(buffer_.data());
      buffered_ = 0U;
    }
  }
}

void B1Sha256::update(std::string_view text) {
  update(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

B1Sha256Digest B1Sha256::finish() {
  if (finished_) {
    throw std::logic_error("B1 SHA-256 was finalized more than once.");
  }
  finished_ = true;
  const std::uint64_t bit_length = total_bytes_ * 8U;
  buffer_[buffered_++] = std::byte{0x80};
  if (buffered_ > 56U) {
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
              buffer_.end(), std::byte{0});
    compress(buffer_.data());
    buffered_ = 0U;
  }
  std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
            buffer_.begin() + 56, std::byte{0});
  for (std::size_t index = 0U; index < 8U; ++index) {
    buffer_[56U + index] =
        static_cast<std::byte>((bit_length >> ((7U - index) * 8U)) & 0xffU);
  }
  compress(buffer_.data());

  B1Sha256Digest digest;
  for (std::size_t word = 0U; word < state_.size(); ++word) {
    for (std::size_t octet = 0U; octet < 4U; ++octet) {
      digest.bytes[word * 4U + octet] =
          static_cast<std::byte>((state_[word] >> ((3U - octet) * 8U)) & 0xffU);
    }
  }
  return digest;
}

void B1Sha256::compress(const std::byte* block) noexcept {
  static constexpr std::array<std::uint32_t, 64U> kRoundConstants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
  std::array<std::uint32_t, 64U> schedule{};
  for (std::size_t index = 0U; index < 16U; ++index) {
    schedule[index] =
        (static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(block[index * 4U]))
         << 24U) |
        (static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(block[index * 4U + 1U]))
         << 16U) |
        (static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(block[index * 4U + 2U]))
         << 8U) |
        static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(block[index * 4U + 3U]));
  }
  for (std::size_t index = 16U; index < schedule.size(); ++index) {
    const std::uint32_t before15 = schedule[index - 15U];
    const std::uint32_t before2 = schedule[index - 2U];
    const std::uint32_t sigma0 = rotate_right(before15, 7U) ^
                                 rotate_right(before15, 18U) ^ (before15 >> 3U);
    const std::uint32_t sigma1 = rotate_right(before2, 17U) ^
                                 rotate_right(before2, 19U) ^ (before2 >> 10U);
    schedule[index] =
        schedule[index - 16U] + sigma0 + schedule[index - 7U] + sigma1;
  }
  std::uint32_t a = state_[0U];
  std::uint32_t b = state_[1U];
  std::uint32_t c = state_[2U];
  std::uint32_t d = state_[3U];
  std::uint32_t e = state_[4U];
  std::uint32_t f = state_[5U];
  std::uint32_t g = state_[6U];
  std::uint32_t h = state_[7U];
  for (std::size_t index = 0U; index < schedule.size(); ++index) {
    const std::uint32_t sum1 =
        rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temporary1 =
        h + sum1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t sum0 =
        rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  state_[0U] += a;
  state_[1U] += b;
  state_[2U] += c;
  state_[3U] += d;
  state_[4U] += e;
  state_[5U] += f;
  state_[6U] += g;
  state_[7U] += h;
}

bool B1SemanticResourceVector::operator==(
    const B1SemanticResourceVector& other) const noexcept {
  return work_units == other.work_units &&
         ready_entries == other.ready_entries &&
         ready_bytes == other.ready_bytes && cpu_slots == other.cpu_slots &&
         host_retained_bytes == other.host_retained_bytes &&
         host_scratch_bytes == other.host_scratch_bytes &&
         device_memory_bytes == other.device_memory_bytes &&
         device_scratch_bytes == other.device_scratch_bytes;
}

const char* b1_job_phase_name(B1JobPhase phase) {
  switch (phase) {
    case B1JobPhase::Cold:
      return "cold";
    case B1JobPhase::Warmup:
      return "warmup";
    case B1JobPhase::Measured:
      return "measured";
  }
  throw std::invalid_argument("B1 job phase is invalid.");
}

const char* b1_graph_role_name(B1GraphRole role) {
  switch (role) {
    case B1GraphRole::A:
      return "A";
    case B1GraphRole::B:
      return "B";
  }
  throw std::invalid_argument("B1 Graph role is invalid.");
}

const char* b1_io_stage_name(B1IoStage stage) {
  switch (stage) {
    case B1IoStage::PayloadStage:
      return "payload-stage";
    case B1IoStage::ManifestCommit:
      return "manifest-commit";
  }
  throw std::invalid_argument("B1 Compute I/O stage is invalid.");
}

void validate_b1_job_instance(const B1JobInstance& job) {
  if (job.row_workload_id != kB1WorkloadId &&
      job.row_workload_id != kM1WorkloadId) {
    throw std::invalid_argument("B1 job workload identity is invalid.");
  }
  if (job.replicate_ordinal == 0U ||
      job.replicate_ordinal > kB1ReplicateCount) {
    throw std::invalid_argument("B1 replicate ordinal is outside [1,3].");
  }
  if (job.job_index > 255U) {
    throw std::invalid_argument("B1 job index is outside [0,255].");
  }
  if (job.run_cap != 1U && job.run_cap != 8U) {
    throw std::invalid_argument("B1 Run cap is neither one nor eight.");
  }
  if (job.row_workload_id == kM1WorkloadId && job.run_cap != 8U) {
    throw std::invalid_argument("M1 B1 jobs require Run cap eight.");
  }
  switch (job.phase) {
    case B1JobPhase::Cold:
      if (job.cycle_ordinal != 0U || job.job_index != kB1ColdJobIndex) {
        throw std::invalid_argument(
            "B1 cold identity is not seed 252 cycle 0.");
      }
      return;
    case B1JobPhase::Warmup:
      if (job.cycle_ordinal != 0U || job.job_index < 253U ||
          job.job_index > 255U) {
        throw std::invalid_argument(
            "B1 warmup identity is not seed 253..255 cycle 0.");
      }
      return;
    case B1JobPhase::Measured:
      if (job.job_index >= kB1MeasuredJobCount ||
          (job.row_workload_id == kB1WorkloadId && job.cycle_ordinal != 0U)) {
        throw std::invalid_argument("B1 measured occurrence is invalid.");
      }
      return;
  }
  throw std::invalid_argument("B1 job phase is invalid.");
}

void validate_b1_io_task_identity(const B1IoTaskIdentity& task) {
  validate_b1_job_instance(task.job);
  static_cast<void>(b1_io_stage_name(task.stage));
}

std::string encode_b1_job_instance(const B1JobInstance& job) {
  validate_b1_job_instance(job);
  std::string output;
  output.reserve(96U);
  append_frame(job.row_workload_id, &output);
  append_frame(std::to_string(job.replicate_ordinal), &output);
  append_frame(b1_job_phase_name(job.phase), &output);
  append_frame(std::to_string(job.cycle_ordinal), &output);
  append_frame(std::to_string(job.job_index), &output);
  append_frame(std::to_string(job.run_cap), &output);
  return output;
}

B1GraphRole b1_graph_for_job(std::uint64_t job_index) {
  if (job_index > 255U) {
    throw std::out_of_range("B1 job index is outside [0,255].");
  }
  return job_index % 2U == 0U ? B1GraphRole::A : B1GraphRole::B;
}

std::uint64_t b1_manifest_length(std::uint64_t job_index) {
  if (job_index > 255U) {
    throw std::out_of_range("B1 job index is outside [0,255].");
  }
  if (job_index < 10U) {
    return 243U;
  }
  if (job_index < 100U) {
    return 244U;
  }
  return 245U;
}

std::string b1_digest_hex(const B1Sha256Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output(64U, '0');
  for (std::size_t index = 0U; index < digest.bytes.size(); ++index) {
    const std::uint8_t value =
        std::to_integer<std::uint8_t>(digest.bytes[index]);
    output[index * 2U] = kHex[value >> 4U];
    output[index * 2U + 1U] = kHex[value & 0x0fU];
  }
  return output;
}

B1Sha256Digest parse_b1_digest(std::string_view text) {
  if (text.size() != 64U) {
    throw std::invalid_argument("B1 SHA-256 spelling is not 64 bytes.");
  }
  B1Sha256Digest digest;
  for (std::size_t index = 0U; index < digest.bytes.size(); ++index) {
    const auto nibble = [](char character) -> std::uint8_t {
      if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
      }
      if (character >= 'a' && character <= 'f') {
        return static_cast<std::uint8_t>(character - 'a' + 10);
      }
      throw std::invalid_argument("B1 SHA-256 spelling is not lowercase hex.");
    };
    digest.bytes[index] = static_cast<std::byte>(
        (nibble(text[index * 2U]) << 4U) | nibble(text[index * 2U + 1U]));
  }
  return digest;
}

B1Sha256Digest b1_sha256(const std::byte* data, std::size_t size) {
  B1Sha256 hash;
  hash.update(data, size);
  return hash.finish();
}

B1Sha256Digest b1_sha256(std::string_view text) {
  B1Sha256 hash;
  hash.update(text);
  return hash.finish();
}

std::string b1_frozen_graph_yaml(std::uint64_t job_index) {
  if (job_index > 255U) {
    throw std::out_of_range("B1 job index is outside [0,255].");
  }
  std::ostringstream output;
  output << "- id: 0\n"
         << "  name: b1_coordinate_pattern\n"
         << "  type: image_generator\n"
         << "  subtype: coordinate_pattern\n"
         << "  parameters:\n"
         << "    width: 2048\n"
         << "    height: 2048\n"
         << "    channels: 4\n"
         << "    seed: " << job_index << '\n'
         << "- id: 1\n"
         << "  name: b1_curve_one\n"
         << "  type: image_process\n"
         << "  subtype: curve_transform\n"
         << "  image_inputs:\n"
         << "    - from_node_id: 0\n"
         << "  parameters:\n"
         << "    k: 0.80\n"
         << "- id: 2\n"
         << "  name: b1_curve_two\n"
         << "  type: image_process\n"
         << "  subtype: curve_transform\n"
         << "  image_inputs:\n"
         << "    - from_node_id: 1\n"
         << "  parameters:\n"
         << "    k: 1.00\n"
         << "- id: 3\n"
         << "  name: b1_curve_three\n"
         << "  type: image_process\n"
         << "  subtype: curve_transform\n"
         << "  image_inputs:\n"
         << "    - from_node_id: 2\n"
         << "  parameters:\n"
         << "    k: 1.20\n"
         << "- id: 4\n"
         << "  name: b1_curve_four\n"
         << "  type: image_process\n"
         << "  subtype: curve_transform\n"
         << "  image_inputs:\n"
         << "    - from_node_id: 3\n"
         << "  parameters:\n"
         << "    k: 1.40\n";
  return output.str();
}

std::string b1_source_node_yaml(std::uint64_t job_index) {
  if (job_index > 255U) {
    throw std::out_of_range("B1 job index is outside [0,255].");
  }
  std::ostringstream output;
  output << "id: 0\n"
         << "name: b1_coordinate_pattern\n"
         << "type: image_generator\n"
         << "subtype: coordinate_pattern\n"
         << "parameters:\n"
         << "  width: 2048\n"
         << "  height: 2048\n"
         << "  channels: 4\n"
         << "  seed: " << job_index << '\n';
  return output.str();
}

HostComputeRequest make_b1_host_compute_request(const GraphSessionId& session,
                                                std::uint64_t run_cap) {
  if (run_cap != 1U && run_cap != 8U) {
    throw std::invalid_argument("B1 Run cap is neither one nor eight.");
  }
  HostComputeRequest request;
  request.session = session;
  request.node = NodeId{4};
  request.cache.precision = "fp32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  request.execution.quiet = true;
  request.execution.maximum_parallelism = static_cast<std::uint32_t>(run_cap);
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.dirty_roi = std::nullopt;
  return request;
}

std::vector<B1SemanticTask> b1_frozen_semantic_plan(std::uint64_t job_index) {
  const B1GraphRole graph = b1_graph_for_job(job_index);
  std::vector<B1SemanticTask> tasks;
  tasks.reserve(kB1TasksPerJob);
  tasks.push_back(
      B1SemanticTask{job_index,
                     graph,
                     0U,
                     {},
                     B1SemanticResourceVector{1U, 1U, 0U, 1U, 0U, 0U, 0U, 0U}});
  for (std::size_t stage = 0U; stage < kB1CurveCoefficients.size(); ++stage) {
    for (std::size_t tile = 0U; tile < kB1TilesPerCurveStage; ++tile) {
      const std::uint64_t ordinal =
          static_cast<std::uint64_t>(1U + stage * kB1TilesPerCurveStage + tile);
      const std::uint64_t dependency =
          stage == 0U ? 0U
                      : static_cast<std::uint64_t>(
                            1U + (stage - 1U) * kB1TilesPerCurveStage + tile);
      tasks.push_back(
          B1SemanticTask{job_index,
                         graph,
                         ordinal,
                         {dependency},
                         B1SemanticResourceVector{1U, 1U, kB1CurveTileBytes, 1U,
                                                  0U, 0U, 0U, 0U}});
    }
  }
  return tasks;
}

std::vector<B1SemanticRecord> make_b1_success_semantic_records(
    const std::vector<B1SemanticTask>& tasks) {
  if (tasks.empty()) {
    throw std::invalid_argument("B1 semantic plan is empty.");
  }
  const std::vector<B1SemanticTask> expected =
      b1_frozen_semantic_plan(tasks.front().job_index);
  if (tasks.size() != expected.size()) {
    throw std::invalid_argument("B1 semantic plan task count drifted.");
  }
  std::vector<B1SemanticRecord> records;
  records.reserve(tasks.size() * 3U);
  for (std::size_t index = 0U; index < tasks.size(); ++index) {
    if (!equal_semantic_task(tasks[index], expected[index])) {
      throw std::invalid_argument("B1 semantic plan task drifted.");
    }
    records.push_back(B1SemanticRecord{tasks[index], B1SemanticAction::Ready,
                                       B1SemanticOutcome::NotApplicable});
    records.push_back(B1SemanticRecord{tasks[index], B1SemanticAction::Start,
                                       B1SemanticOutcome::NotApplicable});
    records.push_back(B1SemanticRecord{tasks[index], B1SemanticAction::Terminal,
                                       B1SemanticOutcome::Succeeded});
  }
  return records;
}

std::string encode_b1_semantic_trace(
    const std::vector<B1SemanticRecord>& records) {
  std::vector<B1SemanticRecord> sorted = records;
  std::sort(sorted.begin(), sorted.end(),
            [](const B1SemanticRecord& lhs, const B1SemanticRecord& rhs) {
              return std::tuple(lhs.task.job_index, graph_rank(lhs.task.graph),
                                lhs.task.task_ordinal,
                                action_rank(lhs.action)) <
                     std::tuple(rhs.task.job_index, graph_rank(rhs.task.graph),
                                rhs.task.task_ordinal, action_rank(rhs.action));
            });
  validate_semantic_records(sorted);

  std::ostringstream output;
  output << "execution-profile-semantic-trace-v1\n";
  for (const B1SemanticRecord& record : sorted) {
    output << "job=" << record.task.job_index
           << ";graph=" << b1_graph_role_name(record.task.graph)
           << ";task=" << record.task.task_ordinal
           << ";action=" << action_name(record.action) << ";deps=";
    if (record.task.dependencies.empty()) {
      output << '-';
    } else {
      for (std::size_t index = 0U; index < record.task.dependencies.size();
           ++index) {
        if (index != 0U) {
          output << ',';
        }
        output << record.task.dependencies[index];
      }
    }
    output << ";outcome="
           << (record.outcome == B1SemanticOutcome::Succeeded ? "succeeded"
                                                              : "-")
           << ";work=" << record.task.resources.work_units
           << ";ready-entries=" << record.task.resources.ready_entries
           << ";ready-bytes=" << record.task.resources.ready_bytes
           << ";cpu=" << record.task.resources.cpu_slots
           << ";host-retained=" << record.task.resources.host_retained_bytes
           << ";host-scratch=" << record.task.resources.host_scratch_bytes
           << ";device-memory=" << record.task.resources.device_memory_bytes
           << ";device-scratch=" << record.task.resources.device_scratch_bytes
           << '\n';
  }
  return output.str();
}

std::vector<B1SemanticRecord> parse_b1_semantic_trace(std::string_view bytes) {
  constexpr std::string_view kHeader = "execution-profile-semantic-trace-v1\n";
  if (bytes.substr(0U, kHeader.size()) != kHeader ||
      bytes.size() == kHeader.size() || bytes.back() != '\n' ||
      bytes.find('\r') != std::string_view::npos ||
      bytes.find('\0') != std::string_view::npos) {
    throw std::invalid_argument("B1 semantic trace envelope is invalid.");
  }
  std::vector<B1SemanticRecord> records;
  std::size_t line_start = kHeader.size();
  while (line_start < bytes.size()) {
    const std::size_t line_end = bytes.find('\n', line_start);
    if (line_end == std::string_view::npos || line_end == line_start) {
      throw std::invalid_argument("B1 semantic trace line framing is invalid.");
    }
    const std::string_view line =
        bytes.substr(line_start, line_end - line_start);
    std::size_t cursor = 0U;
    B1SemanticRecord record;
    record.task.job_index =
        parse_canonical_uint64(parse_trace_field(line, &cursor, "job=", false));
    const std::string_view graph =
        parse_trace_field(line, &cursor, ";graph=", false);
    if (graph == "A") {
      record.task.graph = B1GraphRole::A;
    } else if (graph == "B") {
      record.task.graph = B1GraphRole::B;
    } else {
      throw std::invalid_argument("B1 semantic trace Graph token is invalid.");
    }
    record.task.task_ordinal = parse_canonical_uint64(
        parse_trace_field(line, &cursor, ";task=", false));
    record.action =
        parse_action(parse_trace_field(line, &cursor, ";action=", false));
    const std::string_view deps =
        parse_trace_field(line, &cursor, ";deps=", false);
    if (deps != "-") {
      std::size_t dep_start = 0U;
      while (dep_start < deps.size()) {
        const std::size_t dep_end = deps.find(',', dep_start);
        const std::string_view dep =
            deps.substr(dep_start, dep_end == std::string_view::npos
                                       ? deps.size() - dep_start
                                       : dep_end - dep_start);
        const std::uint64_t value = parse_canonical_uint64(dep);
        if (!record.task.dependencies.empty() &&
            value <= record.task.dependencies.back()) {
          throw std::invalid_argument(
              "B1 semantic dependencies are not strictly sorted.");
        }
        record.task.dependencies.push_back(value);
        if (dep_end == std::string_view::npos) {
          break;
        }
        dep_start = dep_end + 1U;
      }
    }
    const std::string_view outcome =
        parse_trace_field(line, &cursor, ";outcome=", false);
    if (outcome == "-") {
      record.outcome = B1SemanticOutcome::NotApplicable;
    } else if (outcome == "succeeded") {
      record.outcome = B1SemanticOutcome::Succeeded;
    } else {
      throw std::invalid_argument("B1 semantic trace outcome is invalid.");
    }
    record.task.resources.work_units = parse_canonical_uint64(
        parse_trace_field(line, &cursor, ";work=", false));
    record.task.resources.ready_entries = parse_canonical_uint64(
        parse_trace_field(line, &cursor, ";ready-entries=", false));
    record.task.resources.ready_bytes = parse_canonical_uint64(
        parse_trace_field(line, &cursor, ";ready-bytes=", false));
    record.task.resources.cpu_slots = parse_canonical_uint64(
        parse_trace_field(line, &cursor, ";cpu=", false));
    record.task.resources.host_retained_bytes = parse_canonical_uint64(
        parse_trace_field(line, &cursor, ";host-retained=", false));
    record.task.resources.host_scratch_bytes = parse_canonical_uint64(
        parse_trace_field(line, &cursor, ";host-scratch=", false));
    record.task.resources.device_memory_bytes = parse_canonical_uint64(
        parse_trace_field(line, &cursor, ";device-memory=", false));
    record.task.resources.device_scratch_bytes = parse_canonical_uint64(
        parse_trace_field(line, &cursor, ";device-scratch=", true));
    if (cursor != line.size()) {
      throw std::invalid_argument("B1 semantic trace has trailing fields.");
    }
    records.push_back(std::move(record));
    line_start = line_end + 1U;
  }

  std::vector<B1SemanticRecord> sorted = records;
  std::sort(sorted.begin(), sorted.end(),
            [](const B1SemanticRecord& lhs, const B1SemanticRecord& rhs) {
              return std::tuple(lhs.task.job_index, graph_rank(lhs.task.graph),
                                lhs.task.task_ordinal,
                                action_rank(lhs.action)) <
                     std::tuple(rhs.task.job_index, graph_rank(rhs.task.graph),
                                rhs.task.task_ordinal, action_rank(rhs.action));
            });
  if (sorted.size() != records.size() ||
      !std::equal(sorted.begin(), sorted.end(), records.begin(),
                  [](const B1SemanticRecord& lhs, const B1SemanticRecord& rhs) {
                    return equal_semantic_task(lhs.task, rhs.task) &&
                           lhs.action == rhs.action &&
                           lhs.outcome == rhs.outcome;
                  })) {
    throw std::invalid_argument("B1 semantic trace is not canonically sorted.");
  }
  validate_semantic_records(records);
  return records;
}

Value generate_b1_oracle_image(std::uint64_t job_index) {
  if (job_index > 255U) {
    throw std::out_of_range("B1 job index is outside [0,255].");
  }
  ScopedB1Binary32RoundToNearest rounding_scope;
  std::array<float, 256U> transformed_samples{};
  for (std::size_t numerator = 0U; numerator < transformed_samples.size();
       ++numerator) {
    float sample =
        b1_reference_byte_fraction(static_cast<std::uint8_t>(numerator));
    for (const float coefficient : kB1CurveCoefficients) {
      sample = b1_reference_curve(sample, coefficient);
    }
    transformed_samples[numerator] = sample;
  }
  DenseTensorDescriptor descriptor{{static_cast<std::size_t>(kB1ImageEdge),
                                    static_cast<std::size_t>(kB1ImageEdge),
                                    static_cast<std::size_t>(kB1ChannelCount)},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  facet.sample_domain =
      SampleDomainFacet{1U,
                        SampleEncoding{1U, SampleEncodingKind::Normalized},
                        SampleDomain{SampleDomainKind::Normalized, 0.0, 1.0},
                        {}};
  StridedLayout layout{
      {static_cast<std::ptrdiff_t>(kB1PayloadRowBytes),
       static_cast<std::ptrdiff_t>(kB1ChannelCount * sizeof(float)),
       static_cast<std::ptrdiff_t>(sizeof(float))}};
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      descriptor, facet, layout, static_cast<std::size_t>(kB1PayloadBytes));
  {
    WriteLease write = builder.acquire_write();
    float* const samples = reinterpret_cast<float*>(write.data());
    std::size_t offset = 0U;
    for (std::uint64_t y = 0U; y < kB1ImageEdge; ++y) {
      for (std::uint64_t x = 0U; x < kB1ImageEdge; ++x) {
        for (std::uint64_t channel = 0U; channel < kB1ChannelCount; ++channel) {
          const std::uint8_t numerator = static_cast<std::uint8_t>(
              (17U * x + 31U * y + 47U * channel + job_index) & 255U);
          samples[offset++] = transformed_samples[numerator];
        }
      }
    }
  }
  return builder.seal();
}

B1JobGolden compute_b1_job_golden(std::uint64_t job_index) {
  const Value image = generate_b1_oracle_image(job_index);
  const ContentDigestResult logical = compute_content_digest(image);
  if (logical.state != ContentDigestState::Available ||
      !logical.digest.has_value()) {
    throw std::runtime_error("B1 oracle logical digest is unavailable: " +
                             logical.diagnostic);
  }
  const ImageView view(image);
  B1Sha256 raw_hash;
  for (std::size_t y = 0U; y < view.height(); ++y) {
    for (std::size_t x = 0U; x < view.width(); ++x) {
      for (std::size_t channel = 0U; channel < view.channels(); ++channel) {
        float sample = 0.0F;
        std::memcpy(&sample, view.channel_data(x, y, channel), sizeof(sample));
        hash_little_endian_float(sample, &raw_hash);
      }
    }
  }
  return B1JobGolden{job_index, *logical.digest, raw_hash.finish()};
}

B1JobGolden b1_frozen_job_golden(std::uint64_t job_index) {
  const auto found = std::lower_bound(
      kB1FrozenGoldens.begin(), kB1FrozenGoldens.end(), job_index,
      [](const B1GoldenHex& golden, std::uint64_t candidate) {
        return golden.job_index < candidate;
      });
  if (found == kB1FrozenGoldens.end() || found->job_index != job_index) {
    throw std::out_of_range(
        "B1 frozen golden exists only for jobs 0..29 and 252..255.");
  }
  try {
    const B1Sha256Digest logical_bytes = parse_b1_digest(found->logical);
    ContentDigest logical;
    logical.bytes = logical_bytes.bytes;
    return B1JobGolden{job_index, logical, parse_b1_digest(found->raw)};
  } catch (const std::invalid_argument& error) {
    throw std::logic_error(std::string("Compiled B1 golden is corrupt: ") +
                           error.what());
  }
}

std::string b1_artifact_manifest(std::uint64_t job_index,
                                 const B1Sha256Digest& payload_digest) {
  if (job_index > 255U) {
    throw std::out_of_range("B1 job index is outside [0,255].");
  }
  std::ostringstream output;
  output << "schema=execution-profile-artifact-v1\n"
         << "job=" << job_index << '\n'
         << "width=2048\n"
         << "height=2048\n"
         << "channels=RGBA\n"
         << "scalar=ieee754-binary32\n"
         << "byte-order=little\n"
         << "row-stride=32768\n"
         << "payload=output.rgba32le\n"
         << "payload-sha256=" << b1_digest_hex(payload_digest) << '\n';
  std::string manifest = output.str();
  if (manifest.size() != b1_manifest_length(job_index)) {
    throw std::logic_error("B1 artifact manifest length invariant drifted.");
  }
  return manifest;
}

}  // namespace ps::benchmark
