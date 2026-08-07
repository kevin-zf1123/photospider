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

compute::ComputeRunObservationCoordinate
ComputeRunObservationFanout::reserve_causal_coordinate() noexcept {
  return sequence_authority_->reserve_causal_coordinate();
}

void ComputeRunObservationFanout::on_current_generation(
    const compute::SupersessionIdentity& identity,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->on_current_generation(identity, coordinate);
  mirror_->on_current_generation(identity, coordinate);
}

bool ComputeRunObservationFanout::observes_task_semantics() const noexcept {
  return sequence_authority_->observes_task_semantics() ||
         mirror_->observes_task_semantics();
}

void ComputeRunObservationFanout::on_task_ready(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunTaskIdentity task_identity,
    const compute::ComputeRunTaskReadyObservation& observation,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->on_task_ready(descriptor, task_identity, observation,
                                     coordinate);
  mirror_->on_task_ready(descriptor, task_identity, observation, coordinate);
}

void ComputeRunObservationFanout::on_service_start(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunTaskIdentity task_identity, std::uint64_t service_charge,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->on_service_start(descriptor, task_identity,
                                        service_charge, coordinate);
  mirror_->on_service_start(descriptor, task_identity, service_charge,
                            coordinate);
}

void ComputeRunObservationFanout::on_task_terminal(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunTaskIdentity task_identity,
    compute::ComputeRunTaskTerminalKind kind,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->on_task_terminal(descriptor, task_identity, kind,
                                        coordinate);
  mirror_->on_task_terminal(descriptor, task_identity, kind, coordinate);
}

void ComputeRunObservationFanout::on_cancellation(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunCancellationReason reason,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->on_cancellation(descriptor, reason, coordinate);
  mirror_->on_cancellation(descriptor, reason, coordinate);
}

void ComputeRunObservationFanout::on_terminal(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunTerminalKind kind,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->on_terminal(descriptor, kind, coordinate);
  mirror_->on_terminal(descriptor, kind, coordinate);
}

void ComputeRunObservationFanout::on_current_visible(
    const compute::ComputeRunDescriptor& descriptor, Value output,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->on_current_visible(descriptor, output, coordinate);
  mirror_->on_current_visible(descriptor, std::move(output), coordinate);
}

void ComputeRunObservationFanout::on_progressive_final_triggered(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->on_progressive_final_triggered(descriptor, coordinate);
  mirror_->on_progressive_final_triggered(descriptor, coordinate);
}

void ComputeRunObservationFanout::on_run_quiescent(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->on_run_quiescent(descriptor, coordinate);
  mirror_->on_run_quiescent(descriptor, coordinate);
}

void ComputeRunObservationFanout::on_run_resource_settled(
    const compute::ComputeRunDescriptor& descriptor,
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->on_run_resource_settled(descriptor, coordinate);
  mirror_->on_run_resource_settled(descriptor, coordinate);
}

void ComputeRunObservationFanout::on_host_settled(
    compute::ComputeRunObservationCoordinate coordinate) noexcept {
  sequence_authority_->on_host_settled(coordinate);
  mirror_->on_host_settled(coordinate);
}

std::shared_ptr<compute::ComputeRunObservationSink>
make_compute_run_observation_fanout(
    std::shared_ptr<compute::ComputeRunObservationSink> sequence_authority,
    std::shared_ptr<compute::ComputeRunObservationSink> mirror) {
  return std::make_shared<ComputeRunObservationFanout>(
      std::move(sequence_authority), std::move(mirror));
}

}  // namespace ps::benchmark
