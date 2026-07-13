#pragma once

#include <algorithm>
#include <cstdint>
#include <opencv2/core.hpp>

#include "compute/compute_geometry.hpp"
#include "compute/dirty_region_planner.hpp"

namespace ps::compute::detail {

/**
 * @brief Checks whether an output extent can contain dirty work.
 *
 * @param size Candidate image extent.
 * @return True when both dimensions are positive.
 * @throws Nothing.
 * @note Dirty planning erases entries with invalid extents before snapshot
 * materialization.
 */
inline bool has_valid_size(const cv::Size& size) {
  return size.width > 0 && size.height > 0;
}

/**
 * @brief Builds a full-output ROI for an already validated extent.
 *
 * @param size Positive image extent.
 * @return Rect covering the whole extent from origin.
 * @throws Nothing.
 * @note Callers are responsible for validating size before using the ROI.
 */
inline cv::Rect full_extent_roi(const cv::Size& size) {
  return cv::Rect(0, 0, size.width, size.height);
}

/**
 * @brief Converts an HP halo radius to RT proxy-space radius.
 *
 * @param halo_hp Non-negative HP-space halo radius.
 * @return RT proxy-space halo rounded up to preserve coverage.
 * @throws Nothing.
 * @note This mirrors the existing RT dirty executor contract where RT work is a
 * conservative projection of HP-space dirty demand.
 */
inline int scale_halo_to_rt(int halo_hp) {
  const std::int64_t bounded_halo = std::max(0, halo_hp);
  return static_cast<int>((bounded_halo + kRtDownscaleFactor - 1) /
                          kRtDownscaleFactor);
}

/**
 * @brief Domain policy for high-precision dirty planning.
 *
 * The policy supplies the HP entry type, HP alignment constants, snapshot
 * domain, and tile ROI selection used by the shared dirty-planning template.
 *
 * @note All ROI fields stay in HP coordinates and no RT projection is produced.
 */
struct HighPrecisionDirtyPolicy {
  /** @brief Plan type produced by the policy. */
  using Plan = HighPrecisionDirtyPlan;

  /** @brief Per-node entry type produced by the policy. */
  using Entry = HpPlanEntry;

  /** @brief Snapshot domain represented by this policy. */
  static constexpr DirtyDomain kDomain = DirtyDomain::HighPrecision;

  /** @brief Short intent label used in validation errors. */
  static constexpr const char* kIntentLabel = "HP";

  /** @brief Error message used when all HP entries are erased. */
  static constexpr const char* kEmptyPlanMessage =
      "HP planner produced empty execution set.";

  /**
   * @brief Refreshes derived size fields after HP extent changes.
   *
   * @param entry Entry whose HP extent was refreshed.
   * @throws Nothing.
   * @note HP entries do not have derived RT size fields.
   */
  static void refresh_size_fields(Entry& entry) { (void)entry; }

  /**
   * @brief Refreshes derived halo fields after HP halo changes.
   *
   * @param entry Entry whose HP halo was refreshed.
   * @throws Nothing.
   * @note HP entries do not have derived RT halo fields.
   */
  static void refresh_halo_fields(Entry& entry) { (void)entry; }

  /**
   * @brief Converts the target dirty ROI into final HP work coordinates.
   *
   * @param dirty_roi Incoming HP-space dirty ROI.
   * @param entry Target entry containing the HP output extent.
   * @return Aligned and clipped HP dirty ROI.
   * @throws Nothing.
   * @note HP planning aligns directly to HP micro tiles.
   */
  static cv::Rect target_hp_roi(const cv::Rect& dirty_roi, const Entry& entry) {
    return clip_rect(align_rect(dirty_roi, kHpMicroTileSize), entry.hp_size);
  }

  /**
   * @brief Refreshes domain-local projection after roi_hp changes.
   *
   * @param entry Entry whose roi_hp was updated.
   * @return Always true for HP entries.
   * @throws Nothing.
   * @note HP domain work uses roi_hp directly.
   */
  static bool refresh_domain_roi(Entry& entry) {
    return !is_rect_empty(entry.roi_hp);
  }

  /**
   * @brief Error text for an impossible domain projection.
   *
   * @return Unused HP projection error text.
   * @throws Nothing.
   * @note The shared template calls this only if refresh_domain_roi() fails.
   */
  static const char* domain_roi_empty_message() {
    return "Dirty ROI does not intersect node output.";
  }

  /**
   * @brief Normalizes an upstream demand ROI before parent-edge clipping.
   *
   * @param upstream_roi_hp HP-space upstream ROI returned by propagation.
   * @param current_entry Current node entry.
   * @return HP micro-aligned upstream ROI.
   * @throws Nothing.
   * @note HP preserves the previous behavior of aligning before parent
   * clipping.
   */
  static cv::Rect normalize_upstream_roi(const cv::Rect& upstream_roi_hp,
                                         const Entry& current_entry) {
    (void)current_entry;
    return align_rect(upstream_roi_hp, kHpMicroTileSize);
  }

  /**
   * @brief Decides whether an upstream ROI should skip parent entry creation.
   *
   * @param upstream_roi_hp Normalized upstream ROI.
   * @return Always false for HP planning.
   * @throws Nothing.
   * @note HP keeps the prior side effect of creating then erasing empty parent
   * entries when propagation returns no area.
   */
  static bool skip_empty_upstream_roi(const cv::Rect& upstream_roi_hp) {
    (void)upstream_roi_hp;
    return false;
  }

  /**
   * @brief Clips normalized upstream demand to a parent HP extent.
   *
   * @param upstream_roi_hp Normalized upstream ROI.
   * @param parent_entry Parent entry containing the HP extent.
   * @return Parent HP ROI, or empty when demand does not intersect the parent.
   * @throws Nothing.
   */
  static cv::Rect parent_hp_roi(const cv::Rect& upstream_roi_hp,
                                const Entry& parent_entry) {
    return clip_rect(upstream_roi_hp, parent_entry.hp_size);
  }

  /**
   * @brief Finalizes an accumulated HP ROI before snapshot materialization.
   *
   * @param roi_hp Accumulated HP-space ROI.
   * @param hp_size HP output extent.
   * @return HP micro-aligned and clipped ROI.
   * @throws Nothing.
   */
  static cv::Rect finalize_hp_roi(const cv::Rect& roi_hp,
                                  const cv::Size& hp_size) {
    return clip_rect(align_rect(roi_hp, kHpMicroTileSize), hp_size);
  }

  /**
   * @brief Escalates one HP entry to full-output monolithic dirty work.
   *
   * @param entry Entry to mutate.
   * @throws Nothing.
   * @note Monolithic HP snapshot records use the full HP output ROI.
   */
  static void promote_monolithic(Entry& entry) {
    entry.roi_hp = full_extent_roi(entry.hp_size);
  }

  /**
   * @brief Returns the domain-local ROI stored in tile and monolithic records.
   *
   * @param entry Finalized HP entry.
   * @return HP work ROI.
   * @throws Nothing.
   */
  static cv::Rect snapshot_work_roi(const Entry& entry) { return entry.roi_hp; }

  /**
   * @brief Returns the tile size used for HP dirty micro tiles.
   *
   * @return HP micro tile size in HP pixels.
   * @throws Nothing.
   */
  static int tile_size() { return kHpMicroTileSize; }
};

/**
 * @brief Domain policy for real-time dirty planning.
 *
 * The policy keeps HP-space propagation semantics while deriving RT proxy-space
 * extents, halos, ROIs, and tile records for execution.
 *
 * @note HP and RT task pools remain separate; this policy only projects HP
 * dirty demand into the RT domain for the RT plan.
 */
struct RealTimeDirtyPolicy {
  /** @brief Plan type produced by the policy. */
  using Plan = RealTimeDirtyPlan;

  /** @brief Per-node entry type produced by the policy. */
  using Entry = RtPlanEntry;

  /** @brief Snapshot domain represented by this policy. */
  static constexpr DirtyDomain kDomain = DirtyDomain::RealTime;

  /** @brief Short intent label used in validation errors. */
  static constexpr const char* kIntentLabel = "RT";

  /** @brief Error message used when all RT entries are erased. */
  static constexpr const char* kEmptyPlanMessage =
      "RT planner produced empty execution set.";

  /**
   * @brief Refreshes RT extent after HP extent changes.
   *
   * @param entry Entry whose hp_size was refreshed.
   * @throws Nothing.
   * @note RT extent rounds up so proxy space conservatively covers HP output.
   */
  static void refresh_size_fields(Entry& entry) {
    entry.rt_size = scale_down_size(entry.hp_size, kRtDownscaleFactor);
  }

  /**
   * @brief Refreshes RT halo after HP halo changes.
   *
   * @param entry Entry whose halo_hp was refreshed.
   * @throws Nothing.
   * @note The conversion rounds up to avoid dropping neighborhood influence.
   */
  static void refresh_halo_fields(Entry& entry) {
    entry.halo_rt = scale_halo_to_rt(entry.halo_hp);
  }

  /**
   * @brief Converts the target dirty ROI into HP-aligned RT planning input.
   *
   * @param dirty_roi Incoming HP-space dirty ROI.
   * @param entry Target entry containing the HP output extent.
   * @return HP ROI aligned to the RT projection grid.
   * @throws Nothing.
   * @note The HP alignment equals the RT tile size scaled into HP space.
   */
  static cv::Rect target_hp_roi(const cv::Rect& dirty_roi, const Entry& entry) {
    return clip_rect(align_rect(dirty_roi, kHpAlignment), entry.hp_size);
  }

  /**
   * @brief Refreshes RT proxy ROI after roi_hp or rt_size changes.
   *
   * @param entry Entry whose domain projection is refreshed.
   * @return True when the RT projection contains executable work.
   * @throws Nothing.
   * @note The RT ROI is scaled down, aligned to RT tiles, then clipped to
   * rt_size.
   */
  static bool refresh_domain_roi(Entry& entry) {
    entry.roi_rt =
        clip_rect(align_rect(scale_down_rect(entry.roi_hp, kRtDownscaleFactor),
                             kRtTileSize),
                  entry.rt_size);
    return !is_rect_empty(entry.roi_rt);
  }

  /**
   * @brief Error text for a dirty ROI that disappears in RT proxy space.
   *
   * @return RT projection error text.
   * @throws Nothing.
   */
  static const char* domain_roi_empty_message() {
    return "Dirty ROI collapses after RT scaling.";
  }

  /**
   * @brief Normalizes an upstream demand ROI before parent-edge clipping.
   *
   * @param upstream_roi_hp HP-space upstream ROI returned by propagation.
   * @param current_entry Current node entry containing the HP extent.
   * @return Upstream ROI clipped to the current HP output extent.
   * @throws Nothing.
   * @note RT preserves the previous behavior of clipping before parent
   * alignment.
   */
  static cv::Rect normalize_upstream_roi(const cv::Rect& upstream_roi_hp,
                                         const Entry& current_entry) {
    return clip_rect(upstream_roi_hp, current_entry.hp_size);
  }

  /**
   * @brief Decides whether an upstream ROI should skip parent entry creation.
   *
   * @param upstream_roi_hp Normalized upstream ROI.
   * @return True when the normalized upstream ROI is empty.
   * @throws Nothing.
   * @note RT keeps the prior early-exit behavior after current-node clipping.
   */
  static bool skip_empty_upstream_roi(const cv::Rect& upstream_roi_hp) {
    return is_rect_empty(upstream_roi_hp);
  }

  /**
   * @brief Clips RT-projected upstream demand to a parent HP extent.
   *
   * @param upstream_roi_hp Normalized upstream ROI.
   * @param parent_entry Parent entry containing the HP extent.
   * @return Parent HP ROI aligned to the HP representation of RT tiles.
   * @throws Nothing.
   */
  static cv::Rect parent_hp_roi(const cv::Rect& upstream_roi_hp,
                                const Entry& parent_entry) {
    return clip_rect(align_rect(upstream_roi_hp, kHpAlignment),
                     parent_entry.hp_size);
  }

  /**
   * @brief Finalizes an accumulated RT plan ROI in HP coordinates.
   *
   * @param roi_hp Accumulated HP-space ROI.
   * @param hp_size HP output extent.
   * @return HP ROI aligned to the RT projection grid and clipped to hp_size.
   * @throws Nothing.
   */
  static cv::Rect finalize_hp_roi(const cv::Rect& roi_hp,
                                  const cv::Size& hp_size) {
    return clip_rect(align_rect(roi_hp, kHpAlignment), hp_size);
  }

  /**
   * @brief Escalates one RT entry to full-output monolithic dirty work.
   *
   * @param entry Entry to mutate.
   * @throws Nothing.
   * @note HP ROI remains full output for inspection; RT ROI drives execution.
   */
  static void promote_monolithic(Entry& entry) {
    entry.roi_hp = full_extent_roi(entry.hp_size);
    entry.roi_rt = full_extent_roi(entry.rt_size);
  }

  /**
   * @brief Returns the domain-local ROI stored in tile and monolithic records.
   *
   * @param entry Finalized RT entry.
   * @return RT proxy work ROI.
   * @throws Nothing.
   */
  static cv::Rect snapshot_work_roi(const Entry& entry) { return entry.roi_rt; }

  /**
   * @brief Returns the tile size used for RT dirty micro tiles.
   *
   * @return RT micro tile size in proxy pixels.
   * @throws Nothing.
   */
  static int tile_size() { return kRtTileSize; }
};

}  // namespace ps::compute::detail
