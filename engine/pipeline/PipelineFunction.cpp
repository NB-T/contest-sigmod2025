#include "pipeline/PipelineFunction.hpp"
#include "JITOptions.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <dlfcn.h>
//---------------------------------------------------------------------------
namespace engine {
//---------------------------------------------------------------------------
namespace {
//---------------------------------------------------------------------------
/// Handle to dynamic library
struct DLHandle {
    /// Result of dlopen
    void* handle = nullptr;

    /// Constructor
    explicit DLHandle(void* handle) : handle(handle) {}
    /// Move
    DLHandle(DLHandle&& other) noexcept { *this = std::move(other); }
    /// Move
    DLHandle& operator=(DLHandle&& other) noexcept {
        reset();
        std::swap(handle, other.handle);
        return *this;
    }
    /// Reset
    void reset() {
        if (handle) {
            dlclose(handle);
            handle = nullptr;
        }
    }
    /// Destructor
    ~DLHandle() {
        reset();
    }
};
//---------------------------------------------------------------------------
struct CompiledFunction {
    /// The handle to the shared library
    std::shared_ptr<DLHandle> handle;
    /// The function
    PipelineFunction func;
};
//---------------------------------------------------------------------------
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HAS_ASAN
#endif
#endif
//---------------------------------------------------------------------------
CompiledFunction compileFunction(std::string_view name) {
    using namespace std::string_literals;
    auto dumpdir = std::filesystem::path("dump");
    std::filesystem::create_directories(dumpdir);
    std::string filename{name};
    for (char& c : filename) {
        if (c == '<' || c == '>' || c == '"' || c == ':' || c == ',')
            c = '_';
    }

    auto basePath = (dumpdir / filename).string();
    auto cppPath = basePath + ".cpp";
    auto soPath = basePath;
#ifdef NDEBUG
    soPath += "_ndebug";
#endif
#ifdef HAS_ASAN
    soPath += "_asan";
#endif
    auto oPath = soPath + ".o";
    soPath += ".so";
    std::ofstream out(cppPath);
    out <<
R"(#include "pipeline/PipelineGen.hpp"
using namespace engine;
extern "C" __attribute__((visibility("default"))) void pipelineEntry(TargetBase& target,ScanBase& scan,engine::span<const DefaultProbeParameter> probes,engine::span<const unsigned> keyOffsets,engine::span<const unsigned> outputAttributeOffsets) {
    PipelineFunctions::runPipeline<)"
        << name <<
R"(>(target, scan, probes, keyOffsets, outputAttributeOffsets);
}
)";
    out.flush();
    if (!out)
        throw std::runtime_error("Failed while writing " + filename + ".cpp");
    out.close();

    std::string cmd;
    cmd.reserve(512);
    cmd += jit::compileCommand;
    cmd += " -c ";
    cmd += cppPath;
    cmd += " -o ";
    cmd += oPath;

    // std::cerr << cmd << std::endl;
    int rc = std::system(cmd.c_str());
    if (rc != 0)
        throw std::runtime_error("Compilation failed with command " + cmd);

    cmd = "";
    cmd += jit::linkCommand;
    cmd += oPath;
    cmd += " -o ";
    cmd += soPath;

    // std::cerr << cmd << std::endl;
    rc = std::system(cmd.c_str());
    if (rc != 0)
        throw std::runtime_error("Linking failed with command " + cmd);

    void* handle = dlopen(soPath.c_str(), RTLD_NOW);
    if (!handle)
        throw std::runtime_error("dlopen failed: "s + dlerror());

    auto dhl = std::make_unique<DLHandle>(handle);

    void* sym = dlsym(handle, "pipelineEntry");
    if (!sym)
        throw std::runtime_error("dlsym failed: "s + dlerror());
    auto* func = reinterpret_cast<PipelineFunction>(sym);
    return {std::move(dhl), func};
}
//---------------------------------------------------------------------------
std::unordered_map<std::string, CompiledFunction> compiledFunctions;
//---------------------------------------------------------------------------
}
//---------------------------------------------------------------------------
PipelineFunction PipelineFunctions::compilePipeline(std::string_view nameRaw) {
    std::string name{nameRaw};
    if (auto it = compiledFunctions.find(name); it != compiledFunctions.end()) {
        return it->second.func;
    } else {
        auto cf = compileFunction(nameRaw);
        auto func = cf.func;
        compiledFunctions.emplace_hint(it, name, std::move(cf));
        return func;
    }
}
//---------------------------------------------------------------------------
}
