#include "benchmark_fixture.h"
#include "core/clipping.h"
#include "core/constraints.h"
#include "core/diagnostics.h"
#include "core/raster.h"
#include "core/serialize.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>

using namespace rig;
static volatile size_t checksum = 0;

template<class F> static bool measure(const char* name, int count, int grid, bool clipping, const char* stage,
                                      int samples, F action) {
    for (int i = 0; i < 10; ++i) action(i);
    std::vector<double> times;
    for (int i = 0; i < samples; ++i) {
        const auto start = std::chrono::steady_clock::now();
        action(i);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!std::isfinite(ms) || ms < 0) return false;
        times.push_back(ms);
    }
    std::sort(times.begin(), times.end());
    double mean = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
    std::printf("%s,%d,%d,%d,%d,%d,%s,%d,%.6f,%.6f,%.6f,%.6f,%.6f\n", name, count + 1,
                count + (clipping ? 1 : 0), count * grid * grid, count * (grid - 1) * (grid - 1) * 2,
                (count + 15) / 16, stage, samples, mean, times[(times.size() - 1) / 2],
                times[(size_t)std::ceil(times.size() * 0.95) - 1], times.front(), times.back());
    return true;
}

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && std::string(argv[1]) == "--smoke";
    if (argc > 1 && !smoke) { std::fprintf(stderr, "usage: rigcore_bench [--smoke]\n"); return 1; }
    const int samples = smoke ? 3 : 120;
    std::puts("fixture,bones,slots,mesh_vertices,mesh_triangles,constraints,stage,samples,mean_ms,p50_ms,p95_ms,min_ms,max_ms");
    for (int count : {32, 128, 512}) {
        int grid = count == 32 ? 3 : 5;
        bool clipping = count == 512;
        const char* name = count == 32 ? "small" : count == 128 ? "medium" : "large_clipped";
        Skeleton sk = bench::fixture(count, grid, clipping);
        std::string error;
        if (!validateSkeleton(sk, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        Pose pose = makeAnimatedPose(sk, &sk.animations[0], 0.25f);
        std::vector<Geometry> geometries(sk.slots.size());
        const ClipTriangles clip = clipTrianglesOf({{0, 0}, {256, 0}, {256, 128}, {128, 128}, {128, 256}, {0, 256}});
        Geometry clipped;
        const unsigned char white[] = {255, 255, 255, 255};
        std::vector<unsigned char> pixels;
        bool ok = measure(name, count, grid, clipping, "pose", samples, [&](int i) {
            pose = makeAnimatedPose(sk, &sk.animations[0], float(i % 120) / 120);
            checksum += pose.world.size();
        });
        ok &= measure(name, count, grid, clipping, "geometry", samples, [&](int) {
            for (size_t s = clipping ? 1 : 0; s < sk.slots.size(); ++s) {
                computeGeometry(sk, pose, sk.slots[s], sk.slots[s].attachments[0], geometries[s]);
                checksum += geometries[s].positions.size();
            }
        });
        if (clipping) ok &= measure(name, count, grid, clipping, "clipping", samples, [&](int) {
            for (const auto& geometry : geometries) {
                clipGeometry(geometry, clip, clipped);
                checksum += clipped.positions.size();
            }
        });
        ok &= measure(name, count, grid, clipping, "raster_256", samples, [&](int) {
            pixels.assign(256 * 256 * 4, 0);
            renderPose(sk, pose, {{1, 1, white}}, Affine{1, 0, 0, -1, 0, 256}, 256, 256, pixels);
            checksum += pixels[(128 * 256 + 128) * 4 + 3];
        });
        ok &= measure(name, count, grid, clipping, "snapshot_json", samples, [&](int) { checksum += toJson(sk, -1).size(); });
        if (!ok || pixels.size() != 256 * 256 * 4 || std::none_of(pixels.begin(), pixels.end(), [](unsigned char v) { return v != 0; })) return 1;
    }
    std::fprintf(stderr, "checksum=%zu; warmup=10; samples=%d; raster=256x256; GPU/IO excluded\n", size_t(checksum), samples);
    return checksum ? 0 : 1;
}
