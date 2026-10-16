// Indexes a MIDI file (zstd source, as the browser does for multi-GB files) and measures, at
// several points of the song, how long 1 s of playback costs each consumer (synth event
// stream, renderer sweep, keyboard snapshot) on one thread. Also prints the note-on rate
// profile from the index density. See HANDOFF sec. 70.
//   ./nuker_bench FILE.mid [points=10] [seconds=1]
#include "bpfa_midi_store.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace wasmidi;
using Clock = std::chrono::steady_clock;
static int g_fd = -1;
static bool readAt(void*, uint64_t off, uint8_t* dst, std::size_t n) {
    std::size_t done = 0;
    while (done < n) {
        ssize_t r = pread(g_fd, dst + done, n - done, off + done);
        if (r <= 0) return false;
        done += r;
    }
    return true;
}
static long rssMb() { rusage ru; getrusage(RUSAGE_SELF, &ru); return ru.ru_maxrss / 1024; }
static const char* g_stage = "";
static Clock::time_point g_t0, g_last;
static void progress(void*, int pct, const char* stage) {
    static int lastPct = -1;
    auto now = Clock::now();
    if (std::strcmp(stage, g_stage) != 0) {
        if (*g_stage) fprintf(stderr, "  %-34s %8.1f s\n", g_stage, std::chrono::duration<double>(now - g_last).count());
        g_last = now; g_stage = stage;
    }
    if (pct / 10 != lastPct / 10) { fprintf(stderr, "    %3d%% t=%.0fs rss=%ldMB\n", pct, std::chrono::duration<double>(now - g_t0).count(), rssMb()); lastPct = pct; }
}
static double ms(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: nuker_bench FILE.mid [points] [seconds]\n"); return 2; }
    const int points = argc > 2 ? atoi(argv[2]) : 10;
    const double span = argc > 3 ? atof(argv[3]) : 1.0;
    g_fd = open(argv[1], O_RDONLY);
    struct stat st; if (g_fd < 0 || fstat(g_fd, &st)) { perror("open"); return 1; }
    BpfaMidiStore store; MidiDocument meta; store.setRawSourceLimit(0);
    g_t0 = g_last = Clock::now();
    if (!store.index(uint64_t(st.st_size), readAt, nullptr, meta, progress, nullptr)) { fprintf(stderr, "index failed: %s\n", store.error()); return 1; }
    const MidiDocument& md = store.metadata();
    printf("INDEX %.0f s | store %.0f MB (source %.0f MB) | RSS %ld MB | notes %llu tracks %u ppq %u duration %.1f s\n",
        std::chrono::duration<double>(Clock::now() - g_t0).count(), store.memoryBytes() / 1048576.0,
        store.storedSourceBytes() / 1048576.0, rssMb(), (unsigned long long)md.noteCount, md.trackCount,
        md.ticksPerBeat, md.durationSeconds);

    // Note-ons per song second from the density buckets.
    const auto& dens = store.noteDensity();
    const uint32_t bucket = store.densityBucketTicks();
    std::vector<double> nps(size_t(std::ceil(md.durationSeconds)) + 1, 0.0);
    for (size_t i = 0; i < dens.size(); ++i) {
        const double s = md.tickToSeconds(uint32_t(std::min<uint64_t>(uint64_t(i) * bucket, 0xffffffffu)));
        const size_t sec = std::min(nps.size() - 1, size_t(s));
        nps[sec] += dens[i];
    }
    size_t peak = 0;
    for (size_t i = 0; i < nps.size(); ++i) if (nps[i] > nps[peak]) peak = i;
    printf("NPS peak %.0f at %zu s | profile (M note-ons/s per 10%%):", nps[peak], peak);
    for (int p = 0; p < 10; ++p) {
        size_t a = nps.size() * p / 10, b = nps.size() * (p + 1) / 10; double sum = 0, mx = 0;
        for (size_t i = a; i < b; ++i) { sum += nps[i]; mx = std::max(mx, nps[i]); }
        printf(" %.1f/%.1f", sum / std::max<size_t>(1, b - a) / 1e6, mx / 1e6);
    }
    printf("  (avg/max)\n");
    fflush(stdout);

    std::vector<double> starts;
    for (int p = 0; p < points; ++p) starts.push_back(md.durationSeconds * p / points);
    starts.push_back(std::max(0.0, double(peak)));
    for (double startSec : starts) {
        std::vector<BpfaMidiStore::EventWord> batch; std::vector<BpfaMidiStore::SysExBatchEvent> sx; std::vector<uint8_t> sxb;
        bool complete, hasNext; uint32_t next, base;
        auto t0 = Clock::now();
        store.resetEventCursor(uint32_t(md.secondsToTick(startSec)));
        const double resetMs = ms(t0);
        t0 = Clock::now();
        long events = 0, noteOns = 0;
        for (double t = startSec + 0.1; t <= startSec + span + 1e-9; t += 0.1) {
            const uint32_t end = uint32_t(std::ceil(md.secondsToTick(t)));
            do {
                store.buildEventBatch(end, 262144, batch, sx, sxb, complete, next, hasNext);
                events += long(batch.size());
                for (const auto& e : batch)
                    if ((e.packed & 0xf0u) == 0x90u && ((e.packed >> 16) & 0x7fu)) noteOns += 1 + long((e.packed >> 24) & 0xffu);
            } while (!complete);
        }
        const double synthMs = ms(t0);
        std::vector<VisualNote> app; std::vector<BpfaMidiStore::RenderClose> cl;
        t0 = Clock::now();
        store.resetRenderCursor(uint32_t(md.secondsToTick(startSec)), true);
        long ap = 0;
        for (double t = startSec + 0.1; t <= startSec + span + 1e-9; t += 0.1) {
            const uint32_t end = uint32_t(std::ceil(md.secondsToTick(t)));
            do { store.buildRenderSweep(end, 262144, app, cl, base, complete, next, hasNext); ap += long(app.size()); } while (!complete);
        }
        const double renderMs = ms(t0);
        printf("AT %7.1f s: synth reset %.0f ms, %ld words (%ld note-ons) in %.0f ms = %.2fx realtime | render %ld appends in %.0f ms = %.2fx\n",
            startSec, resetMs, events, noteOns, synthMs, synthMs / (span * 1000.0), ap, renderMs, renderMs / (span * 1000.0));
        // Synth feed with admission (HANDOFF sec. 70), for each voice count.
        for (uint32_t voices : {0u, 1024u, 12288u}) {
            BpfaMidiStore::SynthFeedConfig cfg;
            cfg.maxVoices = voices ? voices : 1024u;
            cfg.admissionPercent = voices ? 25u : 0u;
            store.setSynthFeedConfig(cfg);
            std::vector<double> times; double safe = 0; bool done = false;
            t0 = Clock::now();
            store.resetSynthFeed(uint32_t(md.secondsToTick(startSec)));
            long out = 0;
            do {
                store.buildSynthFeed(startSec + span, 1u << 20, batch, times, sx, sxb, safe, done);
                out += long(batch.size());
            } while (!done && safe < startSec + span);
            const double feedMs = ms(t0);
            const auto& fs = store.synthFeedStats();
            printf("    feed %5u voices: decoded %llu, emitted %ld (note-ons %llu, dropped %llu on / %llu off) in %.0f ms = %.2fx\n",
                voices, (unsigned long long)fs.decoded, out, (unsigned long long)fs.noteOns,
                (unsigned long long)fs.droppedNoteOns, (unsigned long long)fs.droppedNoteOffs, feedMs, feedMs / (span * 1000.0));
        }
        fflush(stdout);
    }
    return 0;
}
