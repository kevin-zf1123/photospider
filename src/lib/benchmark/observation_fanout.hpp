/**
 * @file observation_fanout.hpp
 * @brief Declares one source-private same-coordinate observation fanout.
 */
#pragma once

#include <memory>

#include "compute/compute_run.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {

/**
 * @brief Forwards one product observation stream to two bounded collectors.
 *
 * The sequence-authority sink alone reserves every product coordinate. Every
 * subsequent callback is delivered to both sinks with that exact coordinate,
 * allowing a workload-specific collector and the shared M1 collector to
 * observe the same Run without inventing a second causal clock.
 *
 * @throws std::invalid_argument when either sink is null or both aliases are
 * identical.
 * @throws Nothing from product callbacks after construction.
 * @note Both child sinks must be bounded, allocation-free, nonblocking, and
 * observation-only on every callback. This adapter owns no Run, task, Value,
 * cancellation, queue, resource, or scheduler authority.
 */
class ComputeRunObservationFanout final
    : public compute::ComputeRunObservationSink {
 public:
  /**
   * @brief Binds one coordinate authority and one same-coordinate mirror.
   * @param sequence_authority Sink whose allocator defines every coordinate.
   * @param mirror Workload-specific sink receiving the identical coordinate.
   * @throws std::invalid_argument for null or aliased sinks.
   */
  ComputeRunObservationFanout(
      std::shared_ptr<compute::ComputeRunObservationSink> sequence_authority,
      std::shared_ptr<compute::ComputeRunObservationSink> mirror);

  /** @brief Releases both observation-only sink owners. @throws Nothing. */
  ~ComputeRunObservationFanout() noexcept override = default;

  /** @brief Prevents duplicating one observer fanout identity. */
  ComputeRunObservationFanout(const ComputeRunObservationFanout&) = delete;

  /** @brief Prevents replacing one observer fanout identity. */
  ComputeRunObservationFanout& operator=(const ComputeRunObservationFanout&) =
      delete;

  /** @copydoc compute::ComputeRunObservationSink::reserve_causal_coordinate */
  compute::ComputeRunObservationCoordinate reserve_causal_coordinate() noexcept
      override;

  /** @copydoc compute::ComputeRunObservationSink::abort_causal_coordinate */
  void abort_causal_coordinate(
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

  /** @copydoc compute::ComputeRunObservationSink::on_current_generation */
  void on_current_generation(
      const compute::SupersessionIdentity& identity,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

  /** @copydoc compute::ComputeRunObservationSink::observes_task_semantics */
  bool observes_task_semantics() const noexcept override;

  /** @copydoc compute::ComputeRunObservationSink::on_task_ready */
  void on_task_ready(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      const compute::ComputeRunTaskReadyObservation& observation,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

  /** @copydoc compute::ComputeRunObservationSink::on_service_start */
  void on_service_start(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      std::uint64_t service_charge,
      const compute::ComputeRunServiceStartObservation& observation,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

  /** @copydoc compute::ComputeRunObservationSink::on_task_terminal */
  void on_task_terminal(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTaskIdentity task_identity,
      compute::ComputeRunTaskTerminalKind kind,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

  /** @copydoc compute::ComputeRunObservationSink::on_cancellation */
  void on_cancellation(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunCancellationReason reason,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

  /** @copydoc compute::ComputeRunObservationSink::on_terminal */
  void on_terminal(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunTerminalKind kind,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

  /** @copydoc compute::ComputeRunObservationSink::on_current_visible */
  void on_current_visible(
      const compute::ComputeRunDescriptor& descriptor, Value output,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

  /**
   * @copydoc compute::ComputeRunObservationSink::on_progressive_final_triggered
   */
  void on_progressive_final_triggered(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

  /** @copydoc compute::ComputeRunObservationSink::on_run_quiescent */
  void on_run_quiescent(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

  /** @copydoc compute::ComputeRunObservationSink::on_run_resource_settled */
  void on_run_resource_settled(
      const compute::ComputeRunDescriptor& descriptor,
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

  /** @copydoc compute::ComputeRunObservationSink::on_host_settled */
  void on_host_settled(
      compute::ComputeRunObservationCoordinate coordinate) noexcept override;

 private:
  /** @brief Sole causal-coordinate allocator and first callback recipient. */
  std::shared_ptr<compute::ComputeRunObservationSink> sequence_authority_;

  /** @brief Second bounded recipient of each authority-owned coordinate. */
  std::shared_ptr<compute::ComputeRunObservationSink> mirror_;
};

/**
 * @brief Creates one shared same-coordinate observation fanout.
 * @param sequence_authority Sink defining the shared causal coordinate.
 * @param mirror Workload-specific same-coordinate observer.
 * @return Shared sink suitable for one source-private Host request.
 * @throws std::invalid_argument for null or aliased sinks.
 * @throws std::bad_alloc when shared fanout ownership allocates.
 */
std::shared_ptr<compute::ComputeRunObservationSink>
make_compute_run_observation_fanout(
    std::shared_ptr<compute::ComputeRunObservationSink> sequence_authority,
    std::shared_ptr<compute::ComputeRunObservationSink> mirror);

}  // namespace ps::benchmark
