/**
 * @file observation_fanout.cpp
 * @brief Implements same-coordinate bounded product observation fanout.
 */
#include "benchmark/observation_fanout.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

#include "photospider/data/value.hpp"

namespace ps::benchmark {

/** @copydoc ComputeRunObservationFanout::ComputeRunObservationFanout */
ComputeRunObservationFanout::ComputeRunObservationFanout(
    std::shared_ptr<compute::ComputeRunObservationSink> sequence_authority,
    std::shared_ptr<compute::ComputeRunObservationSink> mirror)
    : sequence_authority_(std::move(sequence_authority)),
      mirror_(std::move(mirror)) {
  if (!sequence_authority_ || !mirror_ ||
      sequence_authority_.get() == mirror_.get()) {
    throw std::invalid_argument(
        "Observation fanout requires two distinct non-null sinks.");
  }
}

/** @copydoc ComputeRunObservationFanout::reserve_causal_coordinate */
compute::ComputeRunObservationCoordinate
ComputeRunObservationFanout::reserve_causal_coordinate() noexcept {
  return sequence_authority_->reserve_causal_coordinate();
}

/** @copydoc ComputeRunObservationFanout::abort_causal_coordinate */
void ComputeRunObservationFanout::abort_causal_coordinate(
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->abort_causal_coordinate(coordinate);
}

/** @copydoc ComputeRunObservationFanout::on_current_generation */
void ComputeRunObservationFanout::on_current_generation(
    const compute::SupersessionIdentity& identity,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  mirror_->on_current_generation(identity, coordinate);
  sequence_authority_->on_current_generation(identity, coordinate);
}

/** @copydoc ComputeRunObservationFanout::observes_task_semantics */
bool ComputeRunObservationFanout::observes_task_semantics() const noexcept {
  return sequence_authority_->observes_task_semantics() ||
         mirror_->observes_task_semantics();
}

/** @copydoc ComputeRunObservationFanout::on_task_ready */
void ComputeRunObservationFanout::on_task_ready(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunTaskIdentity task_identity,
    const compute::ComputeRunTaskReadyObservation& observation,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  mirror_->on_task_ready(descriptor, task_identity, observation, coordinate);
  sequence_authority_->on_task_ready(descriptor, task_identity, observation,
                                     coordinate);
}

/** @copydoc ComputeRunObservationFanout::on_service_start */
void ComputeRunObservationFanout::on_service_start(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunTaskIdentity task_identity, std::uint64_t service_charge,
    const compute::ComputeRunServiceStartObservation& observation,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  mirror_->on_service_start(descriptor, task_identity, service_charge,
                            observation, coordinate);
  sequence_authority_->on_service_start(
      descriptor, task_identity, service_charge, observation, coordinate);
}

/** @copydoc ComputeRunObservationFanout::on_task_terminal */
void ComputeRunObservationFanout::on_task_terminal(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunTaskIdentity task_identity,
    compute::ComputeRunTaskTerminalKind kind,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  mirror_->on_task_terminal(descriptor, task_identity, kind, coordinate);
  sequence_authority_->on_task_terminal(descriptor, task_identity, kind,
                                        coordinate);
}

/** @copydoc ComputeRunObservationFanout::on_cancellation */
void ComputeRunObservationFanout::on_cancellation(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunCancellationReason reason,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  mirror_->on_cancellation(descriptor, reason, coordinate);
  sequence_authority_->on_cancellation(descriptor, reason, coordinate);
}

/** @copydoc ComputeRunObservationFanout::on_terminal */
void ComputeRunObservationFanout::on_terminal(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunTerminalKind kind,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  mirror_->on_terminal(descriptor, kind, coordinate);
  sequence_authority_->on_terminal(descriptor, kind, coordinate);
}

/** @copydoc ComputeRunObservationFanout::on_current_visible */
void ComputeRunObservationFanout::on_current_visible(
    const compute::ComputeRunDescriptor& descriptor, Value output,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  mirror_->on_current_visible(descriptor, output, coordinate);
  sequence_authority_->on_current_visible(descriptor, std::move(output),
                                          coordinate);
}

/** @copydoc ComputeRunObservationFanout::on_progressive_final_triggered */
void ComputeRunObservationFanout::on_progressive_final_triggered(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  mirror_->on_progressive_final_triggered(descriptor, coordinate);
  sequence_authority_->on_progressive_final_triggered(descriptor, coordinate);
}

/** @copydoc ComputeRunObservationFanout::on_run_quiescent */
void ComputeRunObservationFanout::on_run_quiescent(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  mirror_->on_run_quiescent(descriptor, coordinate);
  sequence_authority_->on_run_quiescent(descriptor, coordinate);
}

/** @copydoc ComputeRunObservationFanout::on_run_resource_settled */
void ComputeRunObservationFanout::on_run_resource_settled(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  mirror_->on_run_resource_settled(descriptor, coordinate);
  sequence_authority_->on_run_resource_settled(descriptor, coordinate);
}

/** @copydoc ComputeRunObservationFanout::on_host_settled */
void ComputeRunObservationFanout::on_host_settled(
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  mirror_->on_host_settled(coordinate);
  sequence_authority_->on_host_settled(coordinate);
}

/** @copydoc make_compute_run_observation_fanout */
std::shared_ptr<compute::ComputeRunObservationSink>
make_compute_run_observation_fanout(
    std::shared_ptr<compute::ComputeRunObservationSink> sequence_authority,
    std::shared_ptr<compute::ComputeRunObservationSink> mirror) {
  return std::make_shared<ComputeRunObservationFanout>(
      std::move(sequence_authority), std::move(mirror));
}

}  // namespace ps::benchmark
