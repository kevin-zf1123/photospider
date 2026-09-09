#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "scene.hpp"  // NOLINT(build/include_subdir)

namespace s3 {
/** @brief Example application policy, deliberately outside the kernel API. */
class Coordinator final {
 public:
  Coordinator(Scene& scene, ps::ExecutionContext& execution)
      : scene_(scene), execution_(execution) {
    restart_preview();
  }
  /** @brief Pending numeric edits coalesce; accepted brush stamps remain FIFO.
   */
  void slider(float value) {
    require(std::isfinite(value) && value >= 0 && value <= 16, "invalid gain");
    pending_gain_ = value;
  }
  bool brush(Stamp stamp) {
    require(std::isfinite(stamp.x) && std::isfinite(stamp.y) &&
                std::isfinite(stamp.radius) &&
                stamp.radius >= std::numeric_limits<float>::min() &&
                std::isfinite(stamp.red) && std::isfinite(stamp.green) &&
                std::isfinite(stamp.blue) && stamp.red >= 0 &&
                stamp.green >= 0 && stamp.blue >= 0 &&
                std::isfinite(stamp.alpha) && stamp.alpha >= 0 &&
                stamp.alpha <= 1,
            "invalid brush event");
    if (stamps_.size() == 8)
      return false;
    stamps_.push_back(stamp);
    return true;
  }
  bool begin_export() {
    if (export_)
      return false;
    export_ = std::make_unique<TileRun>(scene_.freeze(execution_, gain));
    return true;
  }
  /** @brief One edit and one tile; preview/export alternate when both exist. */
  void tick() {
    bool edited = false;
    if (!stamps_.empty()) {
      scene_.stamp(execution_, stamps_.front());
      stamps_.pop_front();
      ++applied_stamps;
      edited = true;
    } else if (pending_gain_) {
      gain = *pending_gain_;
      pending_gain_.reset();
      edited = true;
    }
    if (edited) {
      ++version;
      restart_preview();
    }
    if (export_ && (!preview_ || export_turn_)) {
      export_->step(execution_);
      ++export_tiles;
      if (export_->done) {
        exported = std::move(export_->frame);
        export_.reset();
      }
    } else if (preview_) {
      preview_->step(execution_);
      ++preview_tiles;
      if (preview_->done) {
        publish(version, preview_quality_, "viewer", preview_->frame);
        if (preview_quality_ == 0) {
          preview_quality_ = 1;
          preview_ = std::make_unique<TileRun>(scene_.freeze(execution_, gain));
        } else {
          preview_.reset();
        }
      }
    }
    export_turn_ = !export_turn_;
  }
  bool idle() const {
    return !preview_ && !export_ && stamps_.empty() && !pending_gain_;
  }
  /** @brief Completion arbitration is shared by asynchronous UI integrations.
   */
  bool publish(std::uint64_t content, int quality, const std::string& target,
               const Frame& frame) {
    if (content != version || target != "viewer" || quality < 0 ||
        quality > 1 ||
        (displayed_version == content && quality < displayed_quality)) {
      ++rejected;
      return false;
    }
    displayed = frame;
    displayed_version = content;
    displayed_quality = quality;
    ++published;
    return true;
  }
  std::uint64_t version = 1, displayed_version = 0, applied_stamps = 0;
  std::uint64_t export_tiles = 0, preview_tiles = 0, published = 0,
                rejected = 0;
  int displayed_quality = -1;
  float gain = 2;
  Frame displayed, exported;

 private:
  void restart_preview() {
    preview_quality_ = 0;
    preview_ = std::make_unique<TileRun>(scene_.freeze(execution_, gain, 4));
  }
  Scene& scene_;
  ps::ExecutionContext& execution_;
  std::optional<float> pending_gain_;
  std::deque<Stamp> stamps_;
  std::unique_ptr<TileRun> preview_, export_;
  int preview_quality_ = 0;
  bool export_turn_ = false;
};
}  // namespace s3
