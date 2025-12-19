#pragma once
//---------------------------------------------------------------------------
#include "infra/Util.hpp"
#include "infra/helper/Span.hpp"
#include "op/ScanBase.hpp"
#include "op/TargetBase.hpp"
#include "pipeline/PipelineConcepts.hpp"
#include <cassert>
#include <string_view>
//---------------------------------------------------------------------------
namespace engine {
//---------------------------------------------------------------------------
class BT;
class BTBuild;
class BTProbe;
class TableScan;
class TableTarget;
//---------------------------------------------------------------------------
using DefaultProbe = BTProbe;
//---------------------------------------------------------------------------
using DefaultProbeParameter = const BT*;
//---------------------------------------------------------------------------
using PipelineFunction = void (*)(TargetBase& target, ScanBase& scan, engine::span<const DefaultProbeParameter> probes, engine::span<const unsigned> keyOffsets, engine::span<const unsigned> outputAttributeOffsets);
//---------------------------------------------------------------------------
class JoinPipelineBase {
    virtual ~JoinPipelineBase() noexcept = default;
};
//---------------------------------------------------------------------------
struct PipelineFunctions {
    static PipelineFunction compilePipeline(std::string_view name);

    template <typename Target, typename Scan, size_t NumJoins, typename Keys, typename Attrs>
    static void runPipeline(TargetBase& target, ScanBase& scan, engine::span<const DefaultProbeParameter> probeParams, engine::span<const unsigned> keyOffsets, engine::span<const unsigned> attrOffsets);
};
//---------------------------------------------------------------------------
}