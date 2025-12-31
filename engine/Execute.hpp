#pragma once
//---------------------------------------------------------------------------
#include <chrono>
#include <plan.h>
//---------------------------------------------------------------------------
namespace engine {
//---------------------------------------------------------------------------
class QueryPlan;
static std::chrono::microseconds ignored_compile_time_default;
//---------------------------------------------------------------------------
ColumnarTable execute(QueryPlan plan, [[maybe_unused]] void* context, std::chrono::microseconds& total_ignored_compile_time = ignored_compile_time_default);
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------