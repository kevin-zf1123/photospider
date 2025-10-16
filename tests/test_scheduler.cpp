#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "kernel/interaction.hpp"
#include "kernel/kernel.hpp"
#include "kernel/services/compute_service.hpp"
#include "graph_model.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>

TEST(SchedulerTest, ParallelLogToJson) {
    using ps::Kernel;
    using ps::InteractionService;

    Kernel kernel;
    InteractionService svc(kernel);

    svc.cmd_seed_builtin_ops();
    const std::string graph_name = "scheduler_ci_graph";
    const std::string graph_path = "util/testcases/full_ops.yaml";
    auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
    ASSERT_TRUE(loaded.has_value());

    auto endings = svc.cmd_ending_nodes(graph_name);
    ASSERT_TRUE(endings.has_value());
    ASSERT_FALSE(endings->empty());
    int node_id = (*endings)[0];

    kernel.runtime(graph_name).clear_scheduler_log();

    auto ok = svc.cmd_compute_async(graph_name, node_id, "int8",
                                    /*force*/true, /*timing*/false, /*parallel*/true);
    ASSERT_TRUE(ok.has_value());
    ASSERT_TRUE(ok->get());

    auto events = kernel.runtime(graph_name).get_scheduler_log();
    ASSERT_FALSE(events.empty());

    nlohmann::json j = nlohmann::json::array();
    for (const auto& e : events) {
        j.push_back({
            {"epoch", e.epoch},
            {"node_id", e.node_id},
            {"worker_id", e.worker_id},
            {"action", e.action == ps::GraphRuntime::SchedulerEvent::ASSIGN_INITIAL ? "ASSIGN_INITIAL" : "EXECUTE"},
            {"ts_us", std::chrono::duration_cast<std::chrono::microseconds>(e.timestamp.time_since_epoch()).count()}
        });
    }

    std::ofstream ofs("scheduler_log.json");
    ofs << std::setw(2) << j << std::endl;
    ofs.close();

    std::ifstream ifs("scheduler_log.json");
    ASSERT_TRUE(static_cast<bool>(ifs));
}
