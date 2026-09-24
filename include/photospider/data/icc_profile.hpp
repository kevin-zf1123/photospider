#pragma once

#include <memory>
#include <string>
#include <utility>

#include "photospider/data/color_profile_identity.hpp"
#include "photospider/execution/cancellation.hpp"
#include "photospider/execution/resources.hpp"

namespace ps {
/**
 * @brief Immutable, validated ICC v2/v4 RGB/Gray/CMYK/XYZ/Lab resource.
 * @note Copies share accepted bytes and accounting. Identity is SHA-256 plus
 * length; paths, addresses and the ICC MD5 Profile ID are not resource
 * identity. Structural admission does not execute a CMM or certify print
 * quality. Handles are immutable/thread-safe and may outlive the budget's
 * caller.
 */
class PHOTOSPIDER_API IccProfile final {
 public:
  IccProfile() noexcept = default;
  /** @brief Copies and validates explicitly supplied profile bytes.
   * @param bytes Borrowed immutable range for this call; never interpreted as a
   * pathname. Caller must not mutate concurrently. No later file I/O occurs.
   * @param resources Root admitting copied bytes, parser scratch and owners.
   * @param cancellation Observed while copying, hashing and parsing.
   * @param maximum_work Additional per-import work cap; root work also applies.
   * @return Owning frozen profile; InvalidArgument/InvalidDomain for invalid
   * structure, ResourceExhausted for admission/work limits, or Cancelled.
   * @throws std::bad_alloc For error diagnostic/control-block allocation.
   * @note Failure publishes no resource and releases all temporary capacity.
   */
  static Result<IccProfile> import(ByteView bytes,
                                   const ResourceBudget& resources,
                                   const CancellationToken& cancellation = {},
                                   std::uint64_t maximum_work = 64 * 1024 *
                                                                1024);
  /** @brief True only for an admitted resource; default handles are invalid. */
  bool valid() const noexcept { return impl_ != nullptr; }
  /** @brief Content identity, borrowed while this handle remains valid.
   * @throws std::logic_error For an invalid handle.
   */
  const ColorProfileIdentity& identity() const;
  /** @brief Frozen profile bytes, borrowed while this handle remains valid.
   * @throws std::logic_error For an invalid handle.
   */
  ByteView bytes() const;
  /** @brief Canonical model from the admitted ICC header, never a name guess.
   */
  std::string model() const;
  /** @brief Actual immutable allocation; owners may outlive this handle.
   * @throws std::logic_error For an invalid handle. No copy of payload bytes.
   */
  const std::shared_ptr<const CpuStorage>& storage() const;
  /** @brief Shares accepted bytes under another root's reference accounting.
   * @return New owning handle, InvalidArgument if invalid, ResourceExhausted on
   * admission failure. Same allocation identity is deduplicated by the root.
   * @throws std::bad_alloc For owner allocation. No parsing or I/O is repeated.
   */
  Result<IccProfile> reference(const ResourceBudget& resources) const;

 private:
  struct Impl;
  explicit IccProfile(std::shared_ptr<const Impl> impl)
      : impl_(std::move(impl)) {}
  std::shared_ptr<const Impl> impl_;
};
}  // namespace ps
