#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "allocation_profile.hpp"  // NOLINT(build/include_subdir)
#include "copy_profile.hpp"        // NOLINT(build/include_subdir)
#include "photospider/photospider.hpp"
#include "result_workflow.hpp"  // NOLINT(build/include_subdir)

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using Clock = std::chrono::steady_clock;
template <class T>
T take(Result<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().message);
  return value.take_value();
}
void check(Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
double us(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start)
      .count();
}
double percentile(std::vector<double> values, double p) {
  std::sort(values.begin(), values.end());
  return values[static_cast<std::size_t>((values.size() - 1) * p)];
}
}  // namespace
int main(int argc, char** argv) try {
  copy_profile::initialize();
  const unsigned runs = argc > 1 ? std::stoul(argv[1]) : 1000;
  const unsigned nodes = argc > 2 ? std::stoul(argv[2]) : 8;
  const unsigned workers = argc > 3 ? std::stoul(argv[3]) : 4;
  const unsigned clients = argc > 4 ? std::stoul(argv[4]) : 1;
  const unsigned timing = argc > 5 ? std::stoul(argv[5]) : 0;
  if (timing > 1 || !runs || runs > 1000000 || !nodes || nodes > 4096 ||
      !workers || workers > 64 || !clients || clients > 64 || clients > runs)
    throw std::runtime_error(
        "usage: runs[1..1000000] nodes[1..4096] workers[1..64] "
        "clients[1..min(64,runs)] timing[0|1]");
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->register_operation(scheduler_result::operation(true)));
  check(registry->freeze());
  ExecutionContextConfig config;
  config.cpu_workers = workers;
  config.gpu_enabled = false;
  config.result_cache_bytes = 0;
  config.collect_scheduler_timing = timing != 0;
  ExecutionContext execution(registry, config);
  Compiler compiler(registry);
  const auto root = take(execution.resource_budget());
  struct Sample {
    double construct, compile, execute, total;
  };
  const auto request = [&](std::int64_t seed) {
    const auto start = Clock::now();
    WorkflowDocument document;
    auto input = scheduler_result::source(
        root, scheduler_result::schema(ElementType::Int64, {1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&seed), sizeof(seed)));
    document.inputs = {scheduler_result::declaration(input)};
    for (unsigned i = 0; i < nodes; ++i) {
      WorkflowNode node;
      node.id = i + 1;
      node.operation = "benchmark.increment";
      node.inputs = {i ? WorkflowInput{WorkflowNodeOutput{i, "value"}}
                       : WorkflowInput{WorkflowInputReference{1}}};
      document.nodes.push_back(std::move(node));
    }
    document.outputs = {{"output", nodes, "value"}};
    GraphContext graph(std::move(document));
    ExecutionBindings bindings{{{"input", std::move(input)}}};
    const double construct_us = us(start);
    auto t = Clock::now();
    auto plan = take(compiler.compile(graph));
    const double compile_us = us(t);
    t = Clock::now();
    auto result = take(execution.execute(plan.plan, std::move(bindings)));
    const double execute_us = us(t);
    std::int64_t observed;
    const auto& output = result.results.at("output");
    check(output.read_tensor(take(output.descriptor()), 0, {0}, &observed, 8));
    if (observed != seed + nodes)
      throw std::runtime_error("incorrect result");
    return Sample{construct_us, compile_us, execute_us, us(start)};
  };
  struct Client {
    std::vector<Sample> samples;
    std::exception_ptr failure;
  };
  std::vector<Client> results(clients);
  for (unsigned i = 0; i < clients; ++i)
    results[i].samples.reserve(runs / clients + (i < runs % clients));
  std::mutex mutex;
  std::condition_variable changed;
  unsigned ready = 0;
  bool released = false;
  std::vector<std::thread> threads;
  threads.reserve(clients);
  const auto wall = Clock::now();
  try {
    for (unsigned client = 0; client < clients; ++client) {
      threads.emplace_back([&, client] {
        auto& result = results[client];
        try {
          for (unsigned i = 0; i < 20; ++i)
            request(-1 - static_cast<std::int64_t>(20 * client + i));
        } catch (...) {
          result.failure = std::current_exception();
        }
        {
          std::unique_lock<std::mutex> lock(mutex);
          ++ready;
          changed.notify_all();
          changed.wait(lock, [&] { return released; });
        }
        if (result.failure)
          return;
        try {
          for (unsigned i = client; i < runs; i += clients)
            result.samples.push_back(request(i));
        } catch (...) {
          result.failure = std::current_exception();
        }
      });
    }
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      released = true;
    }
    changed.notify_all();
    for (auto& thread : threads)
      thread.join();
    throw;
  }
  SchedulerStatistics before;
  Clock::time_point measured;
  {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [&] { return ready == clients; });
    before = execution.scheduler_statistics();
    allocation_profile::begin();
    copy_profile::begin();
    measured = Clock::now();
    released = true;
  }
  changed.notify_all();
  for (auto& thread : threads)
    thread.join();
  const auto allocations = allocation_profile::end();
  const auto copies = copy_profile::end();
  const double measured_wall_us = us(measured), whole_wall_us = us(wall);
  const auto after = execution.scheduler_statistics();
  if (after.cpu.saturated || after.gpu.saturated)
    throw std::runtime_error("scheduler observation saturated");
  if (timing && (after.cpu.accepted_callbacks - before.cpu.accepted_callbacks !=
                     std::uint64_t{runs} * nodes * 3 ||
                 after.cpu.started_callbacks != after.cpu.accepted_callbacks))
    throw std::runtime_error(
        "Result start/Need/publication callback count mismatch");
  std::vector<double> construct, compile, execute, total;
  construct.reserve(runs);
  compile.reserve(runs);
  execute.reserve(runs);
  total.reserve(runs);
  for (const auto& result : results) {
    if (result.failure)
      std::rethrow_exception(result.failure);
    for (const auto& sample : result.samples) {
      construct.push_back(sample.construct);
      compile.push_back(sample.compile);
      execute.push_back(sample.execute);
      total.push_back(sample.total);
    }
  }
  if (total.size() != runs)
    throw std::runtime_error("incomplete measured requests");
  double sum = 0;
  for (double value : total)
    sum += value;
  std::cout << "{\"io_contract\":\"Result\",\"callbacks_per_node\":3,\"runs\":"
            << runs << ",\"nodes\":" << nodes << ",\"workers\":" << workers
            << ",\"clients\":" << clients
            << ",\"warmup\":20,\"warmup_per_client\":20,\"cache\":false,\"new_"
               "graphs\":true"
            << ",\"oracle\":\"int64_chain_increment\",\"qps\":"
            << runs * 1e6 / sum
            << ",\"wall_qps\":" << runs * 1e6 / measured_wall_us
            << ",\"measured_wall_us\":" << measured_wall_us
            << ",\"summed_latency_us\":" << sum
            << ",\"construct_p50_us\":" << percentile(construct, .50)
            << ",\"compile_p50_us\":" << percentile(compile, .50)
            << ",\"execute_p50_us\":" << percentile(execute, .50)
            << ",\"p50_us\":" << percentile(total, .50)
            << ",\"p95_us\":" << percentile(total, .95)
            << ",\"p99_us\":" << percentile(total, .99)
            << ",\"scheduler_timing\":" << (timing ? "true" : "false")
            << ",\"cpu_callbacks\":"
            << after.cpu.started_callbacks - before.cpu.started_callbacks
            << ",\"cpu_submission_ns\":"
            << after.cpu.submission_ns - before.cpu.submission_ns
            << ",\"cpu_queue_wait_ns\":"
            << after.cpu.queue_wait_ns - before.cpu.queue_wait_ns
            << ",\"cpu_queue_wait_max_ns_including_warmup\":"
            << after.cpu.maximum_queue_wait_ns
            << ",\"cpu_queued_peak_including_warmup\":"
            << after.cpu.maximum_queued_callbacks
            << ",\"wall_including_warmup_us\":" << whole_wall_us;
  allocation_profile::write(std::cout, allocations, runs);
  copy_profile::write(std::cout, copies);
  std::cout << "}\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
