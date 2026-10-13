// Streams a raw MIDI from stdin into BpfaMidiStore (zstd source), then times 1 s of synth,
// renderer and keyboard work at the start and at mid-song (HANDOFF sec. 56).
// Example: python3 -c "<lzma raw stream of a .7z>" | ./large_midi_bench <uncompressed size>
#include "bpfa_midi_store.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sys/resource.h>
using namespace wasmidi;
static unsigned long long g_decompressCalls = 0; // instrumented copies of the store can export a real counter
static uint64_t g_next = 0;
static bool readAt(void*, uint64_t off, uint8_t* dst, std::size_t n) {
    if (off != g_next) { fprintf(stderr, "non-sequential read %llu (expected %llu)\n", (unsigned long long)off, (unsigned long long)g_next); return false; }
    std::size_t done = 0;
    while (done < n) { ssize_t r = read(0, dst + done, n - done); if (r <= 0) return false; done += r; }
    g_next += n; return true;
}
static std::chrono::steady_clock::time_point g_t0, g_last; static const char* g_stage = "";
static double cpuMs() { timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static long rssMb() { rusage ru; getrusage(RUSAGE_SELF, &ru); return ru.ru_maxrss / 1024; }
static void progress(void*, int pct, const char* stage) {
    static int lastPct = -1;
    auto now = std::chrono::steady_clock::now();
    if (std::strcmp(stage, g_stage) != 0) {
        if (*g_stage) fprintf(stderr, "  %-28s %8.0f ms (peak RSS %ld MB)\n", g_stage, std::chrono::duration<double, std::milli>(now - g_last).count(), rssMb());
        g_last = now; g_stage = stage;
    }
    if (pct / 5 != lastPct / 5) { fprintf(stderr, "    %3d%% %-26s t=%.0fs rss=%ldMB\n", pct, stage, std::chrono::duration<double>(now - g_t0).count(), rssMb()); lastPct = pct; }
}
int main(int argc, char** argv) {
    const uint64_t size = strtoull(argv[1], nullptr, 10);
    BpfaMidiStore store; MidiDocument meta; store.setRawSourceLimit(0);
    g_t0 = g_last = std::chrono::steady_clock::now();
    if (!store.index(size, readAt, nullptr, meta, progress, nullptr)) { fprintf(stderr, "index failed: %s\n", store.error()); return 1; }
    const MidiDocument& md = store.metadata();
    fprintf(stderr, "INDEX total %.0f s | store %.0f MB (compressed source %.0f MB) | peak RSS %ld MB | notes %llu tracks %u duration %.1f s\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count(), store.memoryBytes() / 1048576.0,
        store.storedSourceBytes() / 1048576.0, rssMb(), (unsigned long long)md.noteCount, md.trackCount, md.durationSeconds);
    for (double startSec : {0.0, md.durationSeconds * 0.5}) {
        std::vector<BpfaMidiStore::EventWord> batch; std::vector<BpfaMidiStore::SysExBatchEvent> sx; std::vector<uint8_t> sxb;
        std::vector<VisualNote> app; std::vector<BpfaMidiStore::RenderClose> cl; bool complete, hasNext; uint32_t next, base;
        auto t0 = std::chrono::steady_clock::now(); unsigned long long d0 = g_decompressCalls;
        store.resetEventCursor(uint32_t(md.secondsToTick(startSec)));
        long events = 0;
        for (double t = startSec + 0.1; t <= startSec + 1.0; t += 0.1) {
            const uint32_t end = uint32_t(std::ceil(md.secondsToTick(t)));
            do { store.buildEventBatch(end, 262144, batch, sx, sxb, complete, next, hasNext); events += batch.size(); } while (!complete);
        }
        double synthMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); unsigned long long dSynth = g_decompressCalls - d0;
        t0 = std::chrono::steady_clock::now(); d0 = g_decompressCalls;
        store.resetRenderCursor(uint32_t(md.secondsToTick(startSec)), true); long ap = 0;
        for (double t = startSec + 0.1; t <= startSec + 1.0; t += 0.1) {
            const uint32_t end = uint32_t(std::ceil(md.secondsToTick(t)));
            do { store.buildRenderSweep(end, 262144, app, cl, base, complete, next, hasNext); ap += app.size(); } while (!complete);
        }
        double renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); unsigned long long dRender = g_decompressCalls - d0;
        t0 = std::chrono::steady_clock::now(); d0 = g_decompressCalls;
        MidiMappedStore::LiveSnapshot ls; const double tk = md.secondsToTick(startSec + 1.0);
        store.buildLiveSnapshot(tk, md.secondsToTick(startSec + 0.75), md.secondsToTick(startSec), ls, true);
        double liveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); unsigned long long dLive = g_decompressCalls - d0;
        fprintf(stderr, "PLAY 1 s from %.1f s: synth %ld events in %.0f ms (%llu block decompressions) | render %ld appends in %.0f ms (%llu) | keyboard rebuild %.0f ms (%llu)\n",
            startSec, events, synthMs, dSynth, ap, renderMs, dRender, liveMs, dLive);
    }
    return 0;
}
