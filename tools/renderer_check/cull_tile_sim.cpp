// CPU model of the tile path (HANDOFF sec. 38): draws a dense synthetic stream raw and
// through grid-aligned culled tiles with pixel-centre scissors, and counts pixels
// whose colour differs. Aligned and misaligned frames must give 0 (HANDOFF sec. 73).
// c++ -O2 -std=c++17 -Isrc -o /tmp/cullsim tools/renderer_check/cull_tile_sim.cpp src/renderer/note_raster_compositor.cpp
#include "renderer/note_raster_compositor.hpp"
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
#include <algorithm>
using namespace wasmidi;
// CPU model of the GL draw: pixel column c gets a note if the column centre lies in
// [x0, x1) with x = (tick - viewStart) * W / span; last drawn wins.
static void draw(std::vector<uint32_t>& img, int W, uint32_t viewStart, uint32_t span,
                 const std::vector<CompositorNote>& notes, int px0, int px1) {
    const uint32_t viewEnd = viewStart + span;
    for (size_t i = 0; i < notes.size(); ++i) {
        const auto& n = notes[i];
        const uint32_t end = n.endTick ? n.endTick : viewEnd;
        const double x0 = (double(n.startTick) - viewStart) * W / span, x1 = (double(end) - viewStart) * W / span;
        const int key = (n.packedData >> 8) & 0xff;
        int c0 = std::max(px0, (int)std::ceil(x0 - 0.5)), c1 = std::min(px1, (int)std::ceil(x1 - 0.5));
        for (int c = c0; c < c1; ++c) img[key * W + c] = ((n.packedData >> 16) & 0xffu) + 1u + (uint32_t(n.startTick) << 8);
    }
}
int main() {
    std::mt19937 rng(3);
    const uint32_t span = 1920; const int W = 1000;
    std::vector<CompositorNote> all;
    for (uint32_t t = 0; t < 40000; t += 3) {
        int n = 20 + rng() % 60;
        for (int i = 0; i < n; ++i) {
            uint32_t key = rng() % 128, len = 1 + rng() % (rng() % 4 ? 30 : 3000);
            uint32_t color = rng() % 16;
            int dups = rng() % 3 ? 1 : 1 + rng() % 5;
            for (int d = 0; d < dups; ++d) all.push_back({t, t + len, (key << 8) | (color << 16) | 100});
        }
    }
    CompositorScratch scratch; CompositorResult res;
    long diffs = 0, pixels = 0, frames = 0; size_t kept = 0, total = 0;
    std::vector<std::vector<CompositorNote>> tiles;
    for (uint32_t k = 0; k * span < 40000; ++k) {
        std::vector<CompositorNote> in;
        for (auto& n : all) { if (n.startTick >= (k + 1) * span) break; if (n.endTick && n.endTick <= k * span) continue; in.push_back(n); }
        CompositorSettings s; s.startTick = k * span; s.spanTicks = span; s.rasterWidth = W; s.viewEndTick = (k + 1) * span;
        CullViewport(in.data(), in.size(), s, scratch, res);
        tiles.push_back(res.notes); kept += res.notes.size(); total += in.size();
    }
    for (uint32_t vs = 1000; vs + span < 38000; vs += 97) {
        std::vector<uint32_t> a(128 * W, 0), b(128 * W, 0);
        std::vector<CompositorNote> vis;
        for (auto& n : all) { if (n.startTick > vs + span) break; if (n.endTick && n.endTick <= vs) continue; vis.push_back(n); }
        draw(a, W, vs, span, vis, 0, W);
        for (uint32_t k = vs / span; k <= (vs + span) / span; ++k) {
            int p0 = std::clamp((int)std::ceil((double(k * span) - vs) * W / span - 0.5), 0, W);
            int p1 = std::clamp((int)std::ceil((double((k + 1) * span) - vs) * W / span - 0.5), 0, W);
            if (p1 > p0) draw(b, W, vs, span, tiles[k], p0, p1);
        }
        for (int i = 0; i < 128 * W; ++i) { pixels++; if ((a[i] & 0xffu) != (b[i] & 0xffu)) ++diffs; }
        ++frames;
    }
    printf("tiles kept %zu of %zu tile-inputs (%.1f%%); frames %ld, differing pixels %ld of %ld (%.4f%%)\n",
           kept, total, 100.0 * kept / total, frames, diffs, pixels, 100.0 * diffs / pixels);

    // Partial tiles (HANDOFF sec. 81): the notes are known only through tick C >= viewEnd
    // (later starts missing, later closes still open) and the visible tiles are culled
    // with that coverage, as GLRenderer::buildCullTile does when the roll's lead is short.
    long pdiffs = 0, ppixels = 0, pframes = 0;
    for (uint32_t vs = 1000; vs + span < 38000; vs += 89) {
        const uint32_t ve = vs + span, cover = ve + rng() % (span + 1);
        std::vector<uint32_t> a(128 * W, 0), b(128 * W, 0);
        std::vector<CompositorNote> vis;
        for (auto& n : all) { if (n.startTick > ve) break; if (n.endTick && n.endTick <= vs) continue; vis.push_back(n); }
        draw(a, W, vs, span, vis, 0, W);
        for (uint32_t k = vs / span; k <= ve / span; ++k) {
            const uint32_t tileEnd = (k + 1) * span, coverage = std::min(tileEnd, cover);
            const uint32_t collectEnd = coverage == tileEnd ? tileEnd : coverage + 1;
            std::vector<CompositorNote> in;
            for (auto n : all) {
                if (n.startTick >= collectEnd) break;
                if (n.endTick > coverage) n.endTick = 0;
                if (n.endTick && n.endTick <= k * span) continue;
                in.push_back(n);
            }
            CompositorSettings s; s.startTick = k * span; s.spanTicks = span; s.rasterWidth = W; s.viewEndTick = coverage;
            CullViewport(in.data(), in.size(), s, scratch, res);
            int p0 = std::clamp((int)std::ceil((double(k * span) - vs) * W / span - 0.5), 0, W);
            int p1 = std::clamp((int)std::ceil((double(tileEnd) - vs) * W / span - 0.5), 0, W);
            if (p1 > p0) draw(b, W, vs, span, res.notes, p0, p1);
        }
        for (int i = 0; i < 128 * W; ++i) { ppixels++; if ((a[i] & 0xffu) != (b[i] & 0xffu)) ++pdiffs; }
        ++pframes;
    }
    printf("partial tiles: frames %ld, differing pixels %ld of %ld (%.4f%%)\n",
           pframes, pdiffs, ppixels, 100.0 * pdiffs / ppixels);
    return diffs || pdiffs ? 1 : 0;
}
