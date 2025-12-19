#include "infra/PageMemory.hpp"
#include "infra/QueryMemory.hpp"
#include "infra/Scheduler.hpp"
#include "query/PlanImport.hpp"
#include "query/QueryPlan.hpp"
#include <iostream>
#include <fmt/core.h>
#include <plan.h>
#include <signal.h>
#include <unistd.h>
//---------------------------------------------------------------------------
namespace engine {
//---------------------------------------------------------------------------
ColumnarTable execute(QueryPlan plan, [[maybe_unused]] void* context) {
    Scheduler::start_query();
    pagememory::start_query();
    ColumnarTable output;
    {
        QueryPlan pp = std::move(plan);
        output = pp.run();
    }
    querymemory::end_query();
    Scheduler::end_query();
    return std::move(output);
}
//---------------------------------------------------------------------------
}

const inline void printColumnarTable(const ColumnarTable& result_table) {
    std::cout << "Result table: " << std::endl;
    for (const auto& row : result_table.columns) {
        for (const auto& page : row.pages) {
            for (const auto& byte : page->data) {
                std::cout << static_cast<int>(byte) << " ";
            }
        }
        std::cout << "\n";
    }
}

//---------------------------------------------------------------------------
namespace Contest {
//---------------------------------------------------------------------------
ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context) {
    engine::Scheduler::start_query();
    engine::pagememory::start_query();
    ColumnarTable output;
    {
        engine::DataSource ds;
        auto imported = engine::PlanImport::importPlan(ds, plan);
        output = imported.run();
    }
    engine::querymemory::end_query();
    engine::Scheduler::end_query();
    return std::move(output);
}
//---------------------------------------------------------------------------
void* build_context() {
    engine::Scheduler::setup();

    return nullptr;
}
//---------------------------------------------------------------------------
void destroy_context([[maybe_unused]] void* context) { engine::Scheduler::teardown(); }

static bool cleanup_registered = false;

// super cleanup function
void emergency_cleanup() {
    if (cleanup_registered) {
        engine::Scheduler::teardown();
        cleanup_registered = false;
    }
}

// engine-specific cleanup
void engine_cleanup_handler(int sig) {
    fmt::print(stderr, "Engine cleanup on signal {}\n", sig);
    emergency_cleanup();
}

void register_engine_cleanup() {
    if (!cleanup_registered) {
        signal(SIGINT, engine_cleanup_handler);
        signal(SIGTERM, engine_cleanup_handler);
        cleanup_registered = true;
    }
}
//---------------------------------------------------------------------------
} // namespace Contest
//---------------------------------------------------------------------------
