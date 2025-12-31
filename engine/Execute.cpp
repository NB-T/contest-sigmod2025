#include "infra/PageMemory.hpp"
#include "infra/QueryMemory.hpp"
#include "infra/Scheduler.hpp"
#include "query/PlanImport.hpp"
#include "query/QueryPlan.hpp"
#include <iostream>
#include <plan.h>
#include <unistd.h>
//---------------------------------------------------------------------------
namespace engine {
std::chrono::microseconds ignored_compile_time_default = std::chrono::microseconds{0};

//---------------------------------------------------------------------------
ColumnarTable execute(QueryPlan plan, [[maybe_unused]] void* context, std::chrono::microseconds& total_ignored_compile_time) {
    Scheduler::start_query();
    pagememory::start_query();
    ColumnarTable output;
    {
        QueryPlan pp = std::move(plan);
        output = pp.run(total_ignored_compile_time);
    }
    querymemory::end_query();
    Scheduler::end_query();
    return std::move(output);
}
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
namespace Contest {
//---------------------------------------------------------------------------
ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context, std::chrono::microseconds& total_ignored_compile_time) {
    std::cout << "cooking" << std::endl;

    engine::Scheduler::start_query();
    engine::pagememory::start_query();
    ColumnarTable output;
    {
        engine::DataSource ds;
        auto imported = engine::PlanImport::importPlan(ds, plan);
        output = imported.run(total_ignored_compile_time);
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
//---------------------------------------------------------------------------
} // namespace Contest
//---------------------------------------------------------------------------
