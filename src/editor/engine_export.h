#pragma once
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

struct EngineClipOption {
    std::string name;
    bool selected = true, loop = true;
};

struct EngineExportOptions {
    float fps = 30, scale = 1, pixelsPerUnit = 100;
    int supersample = 2, padding = 8;
    std::string path;
    std::vector<EngineClipOption> clips;
};

struct EngineExportBudget {
    int animations = 0, frames = 0, width = 0, height = 0, pages = 0;
    uint64_t atlasBytes = 0, rasterBytes = 0;
};

struct EngineExportProgress {
    enum Gate { Running, Cancelled, Committing };
    enum Stage { Analyze, Render, Write, Commit };
    std::atomic<int> gate{Running}, stage{Analyze}, completed{0}, total{1};
    bool cancel() { int expected = Running; return gate.compare_exchange_strong(expected, Cancelled); }
};

struct EngineExportResult {
    EngineExportBudget budget;
    std::string error;
    bool cancelled = false;
};

struct EngineExportJob {
    std::shared_ptr<EngineExportProgress> progress = std::make_shared<EngineExportProgress>();
    std::future<EngineExportResult> result;
    std::string path;
    bool estimate = false;
};
