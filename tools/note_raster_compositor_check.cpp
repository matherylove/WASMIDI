// Host check for src/renderer/note_raster_compositor.cpp. No Qt, no GL.
//
//   c++ -O2 -std=c++17 -Isrc/renderer -o /tmp/cull \
//       tools/note_raster_compositor_check.cpp \
//       src/renderer/note_raster_compositor.cpp && /tmp/cull
//
// The central claim is that culling cannot change a rendered pixel. This proves
// it by rasterizing the full note set and the culled set through a reference
// model of the note shader's own depth rule, then comparing every cell.

#include "note_raster_compositor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

using namespace wasmidi;

static int g_fails = 0;
#define CHECK(c, ...) do { if(!(c)){ std::printf("  FAIL: "); \
    std::printf(__VA_ARGS__); std::printf("\n"); ++g_fails; } } while(0)

static std::uint32_t Pack(int velocity, int pitch, int slot) {
    return (std::uint32_t)(velocity & 0x7f) |
        ((std::uint32_t)(pitch & 0xff) << 8) |
        ((std::uint32_t)(slot & 0x0f) << 16);
}

// What the GPU ends up showing in each cell: the note that survives GL_LESS on
// z = (endTick - startTick), with instances submitted in startTick order. The
// stored value is everything the fragment shader can distinguish: palette slot,
// velocity (opacity), and the tick range (glow test).
struct Shown {
    std::uint32_t slot = 0xffffffffu;
    std::uint32_t velocity = 0;
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint32_t duration = 0xffffffffu;
    std::uint32_t order = 0xffffffffu;
    bool empty = true;
    bool operator!=(const Shown& o) const {
        if (empty != o.empty) return true;
        if (empty) return false;
        return slot != o.slot || velocity != o.velocity ||
               start != o.start || end != o.end;
    }
};

static std::vector<Shown> Rasterize(const std::vector<CompositorNote>& notes,
    const CompositorSettings& s) {
    const int w = s.rasterWidth, k = s.keyCount;
    std::vector<Shown> cells((std::size_t)w * k);
    const double span = (double)std::max<std::uint32_t>(1u, s.spanTicks);
    const std::int64_t viewStart = s.startTick;
    const std::int64_t viewEnd = s.viewEndTick ? s.viewEndTick
        : viewStart + (std::int64_t)s.spanTicks;
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const CompositorNote& n = notes[i];
        const int pitch = (int)((n.packedData >> 8) & 0xff);
        if (pitch < s.firstKey || pitch >= s.firstKey + k) continue;
        const std::int64_t ns = n.startTick;
        const std::int64_t ne = n.endTick ? (std::int64_t)n.endTick : viewEnd;
        if (ne <= viewStart || ns >= viewEnd) continue;
        // GPU model: a pixel column is drawn when its centre lies in [x0, x1).
        int left = (int)std::ceil((double)w * ((double)(ns - viewStart) / span) - 0.5);
        int right = (int)std::ceil((double)w * ((double)(ne - viewStart) / span) - 0.5);
        left = std::max(0, left); right = std::min(w, right);
        const std::uint32_t dur = (std::uint32_t)std::max<std::int64_t>(0, ne - ns);
        const std::uint32_t order = (std::uint32_t)i;
        for (int c = left; c < right; ++c) {
            Shown& cell = cells[(std::size_t)(pitch - s.firstKey) * w + c];
            // BPFA: later start on top, later source order breaks the tie.
            const bool wins = cell.empty ||
                (n.startTick != cell.start ? n.startTick >= cell.start
                                           : order > cell.order);
            if (!wins) continue;
            cell.slot = (n.packedData >> 16) & 0x0f;
            cell.velocity = n.packedData & 0x7f;
            cell.start = n.startTick;
            cell.end = n.endTick;
            cell.duration = dur; cell.order = order; cell.empty = false;
        }
    }
    return cells;
}

static CompositorSettings MakeSettings(int w) {
    CompositorSettings s;
    s.startTick = 0; s.spanTicks = 3840; s.rasterWidth = w;
    s.firstKey = 0; s.keyCount = 128; s.viewEndTick = 3840;
    return s;
}
static CompositorScratch g_scratch;

static void SortByStart(std::vector<CompositorNote>& v) {
    std::stable_sort(v.begin(), v.end(),
        [](const CompositorNote& a, const CompositorNote& b) {
            return a.startTick < b.startTick; });
}

static void PixelIdentical(const char* name,
    const std::vector<CompositorNote>& notes, const CompositorSettings& s) {
    CompositorResult r;
    const auto t0 = std::chrono::steady_clock::now();
    CullViewport(notes.data(), notes.size(), s, g_scratch, r);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    const std::vector<Shown> before = Rasterize(notes, s);
    const std::vector<Shown> after = Rasterize(r.notes, s);
    std::size_t diff = 0;
    for (std::size_t i = 0; i < before.size(); ++i)
        if (before[i] != after[i]) ++diff;
    CHECK(diff == 0, "%s: %zu cells differ after culling", name, diff);

    // survivors must stay in submission order
    for (std::size_t i = 1; i < r.notes.size(); ++i)
        CHECK(r.notes[i - 1].startTick <= r.notes[i].startTick,
            "%s: survivors lost startTick order at %zu", name, i);

    // Sliced over several calls (rev. 88) the survivors are the same notes.
    CompositorScratch slicedScratch;
    CompositorProgress progress;
    CompositorResult sliced;
    CullBegin(notes.data(), notes.size(), s, slicedScratch, progress);
    while (!CullStep(slicedScratch, progress, 7919)) {}
    CullFinish(progress, sliced);
    bool same = sliced.notes.size() == r.notes.size();
    for (std::size_t i = 0; same && i < r.notes.size(); ++i)
        same = sliced.notes[i].startTick == r.notes[i].startTick &&
            sliced.notes[i].endTick == r.notes[i].endTick &&
            sliced.notes[i].packedData == r.notes[i].packedData;
    CHECK(same, "%s: sliced cull kept different notes", name);

    std::printf("   %-26s %8zu -> %8zu inst (%5.1f%% menos)  %6.2f ms"
        "  fila-llena %llu\n",
        name, notes.size(), r.notes.size(),
        notes.empty() ? 0.0 :
            100.0 * (double)r.culledNotes / (double)notes.size(), ms,
        (unsigned long long)r.rejectedByFullRow);
}

static int RunAll() {
    std::printf("1. casos base\n");
    {
        CompositorSettings s = MakeSettings(1920);
        CompositorResult r;
        CullViewport(nullptr, 0, s, g_scratch, r);
        CHECK(r.notes.empty(), "empty input produced notes");

        std::vector<CompositorNote> one = { { 480u, 1440u, Pack(100, 60, 3) } };
        CullViewport(one.data(), one.size(), s, g_scratch, r);
        CHECK(r.notes.size() == 1, "a lone note was culled");

        // A sub-pixel note that covers a pixel centre is drawn and must survive
        // (x in [50.5, 51) at 2 ticks per pixel: the centre 50.5 is on its left edge).
        std::vector<CompositorNote> tiny = { { 101u, 102u, Pack(90, 64, 1) } };
        CullViewport(tiny.data(), tiny.size(), s, g_scratch, r);
        CHECK(r.notes.size() == 1, "sub-pixel note covering a pixel centre was culled");
        // One that covers no centre on this grid (x in [50, 50.5)) still draws in a
        // tile shifted by a fraction of a pixel, so it survives.
        std::vector<CompositorNote> none = { { 100u, 101u, Pack(90, 64, 1) } };
        CullViewport(none.data(), none.size(), s, g_scratch, r);
        CHECK(r.notes.size() == 1, "a sub-pixel note was culled");
        // A later note over half of a column does not hide the one under the other
        // half (the hole of HANDOFF sec. 73).
        std::vector<CompositorNote> half = {
            { 100u, 102u, Pack(90, 64, 1) }, { 101u, 102u, Pack(90, 64, 2) } };
        CullViewport(half.data(), half.size(), s, g_scratch, r);
        CHECK(r.notes.size() == 2, "a note half covered inside one column was culled");
        // Two notes in front that together cover the column do hide it (equal start:
        // the later index is in front).
        std::vector<CompositorNote> split = { { 100u, 102u, Pack(90, 64, 1) },
            { 100u, 101u, Pack(90, 64, 2) }, { 101u, 102u, Pack(90, 64, 3) } };
        CullViewport(split.data(), split.size(), s, g_scratch, r);
        CHECK(r.notes.size() == 2 && r.notes[0].endTick == 101u,
              "notes covering a column between them did not hide the one behind");

        // open note (endTick == 0) stretches to the view edge
        std::vector<CompositorNote> open = { { 100u, 0u, Pack(90, 70, 1) } };
        CullViewport(open.data(), open.size(), s, g_scratch, r);
        CHECK(r.notes.size() == 1, "open note was culled");
    }

    std::printf("2. la nota posterior gana la celda (regla BPFA)\n");
    {
        CompositorSettings s = MakeSettings(1920);
        std::vector<CompositorNote> notes = {
            { 0u, 3840u, Pack(100, 60, 1) },     // long, underneath
            { 480u, 600u, Pack(100, 60, 7) } };  // short, on top
        CompositorResult r;
        CullViewport(notes.data(), notes.size(), s, g_scratch, r);
        CHECK(r.notes.size() == 2, "both notes are visible somewhere");

        // fully covered by a shorter note spanning the whole view
        std::vector<CompositorNote> covered = {
            { 0u, 3840u, Pack(100, 61, 1) },
            { 0u, 3840u, Pack(100, 61, 7) } };
        CullViewport(covered.data(), covered.size(), s, g_scratch, r);
        CHECK(r.notes.size() == 1, "an entirely hidden note survived");
        const std::uint32_t expected = covered[1].packedData;  // later order wins
        CHECK(r.notes[0].packedData == expected,
            "the wrong note survived for this layering rule");
    }

    std::printf("3. identidad de pixel sobre material sintetico\n");
    {
        struct Case { const char* name; int notes; int keys; int len; int colors; };
        const Case cases[] = {
            { "disperso 10k",        10000,  80, 200, 4 },
            { "denso 100k",         100000,  80,  60, 4 },
            { "crashpoint 1M",     1000000, 128,  20, 8 },
            { "acorde apilado",     200000,   1, 800, 2 },
            { "notas largas",        50000, 128, 3000, 3 },
        };
        for (const Case& c : cases) {
            std::mt19937 rng(4321);
            std::uniform_int_distribution<int> pitch(0, c.keys - 1);
            std::uniform_int_distribution<int> start(0, 3839);
            std::uniform_int_distribution<int> len(1, c.len);
            std::vector<CompositorNote> notes;
            notes.reserve(c.notes);
            for (int i = 0; i < c.notes; ++i) {
                const int st = start(rng);
                notes.push_back({ (std::uint32_t)st,
                    (std::uint32_t)(st + len(rng)),
                    Pack(60 + (i % 60), pitch(rng), i % c.colors) });
            }
            SortByStart(notes);
            PixelIdentical(c.name, notes, MakeSettings(1920));
        }
    }

    std::printf("4. identidad de pixel a varias resoluciones\n");
    {
        std::mt19937 rng(55);
        std::uniform_int_distribution<int> pitch(0, 127), start(0, 3839), len(1, 80);
        std::vector<CompositorNote> notes;
        for (int i = 0; i < 300000; ++i) {
            const int st = start(rng);
            notes.push_back({ (std::uint32_t)st, (std::uint32_t)(st + len(rng)),
                Pack(100, pitch(rng), i % 6) });
        }
        SortByStart(notes);
        for (int w : { 640, 1280, 1920, 3840 }) {
            char label[32];
            std::snprintf(label, sizeof(label), "raster %d", w);
            PixelIdentical(label, notes, MakeSettings(w));
        }
    }

    return 0;
}

int main() {
    std::printf("=== orden BPFA: gana la que empieza despues ===\n");
    RunAll();
    std::printf("\n%s (%d)\n", g_fails ? "FALLOS" : "TODO OK", g_fails);
    return g_fails ? 1 : 0;
}
