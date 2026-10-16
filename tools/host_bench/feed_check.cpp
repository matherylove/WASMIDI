// Checks the synth feed (HANDOFF sec. 70) against buildEventBatch: with admission off both
// must give the same events per tick (stacked notes expanded); with admission on,
// every note-off must still pair with a kept note-on (no hanging or orphan notes per key).
//   ./feed_check FILE.mid [start...]
#include "bpfa_midi_store.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
using namespace wasmidi;
static int g_fd = -1;
static bool readAt(void*, uint64_t off, uint8_t* dst, std::size_t n) {
    std::size_t done = 0;
    while (done < n) { ssize_t r = pread(g_fd, dst + done, n - done, off + done); if (r <= 0) return false; done += r; }
    return true;
}
using EW = BpfaMidiStore::EventWord;
static void expand(const std::vector<EW>& in, std::vector<EW>& out) {
    for (const EW& e : in) {
        const uint32_t t = e.packed & 0xf0u;
        const uint32_t n = (t == 0x90u || t == 0x80u) ? ((e.packed >> 24) & 0xffu) + 1u : 1u;
        for (uint32_t i = 0; i < n; ++i) out.push_back(EW{e.tick, e.packed & 0x00ffffffu});
    }
}
// The feed folds same-tick copies of a note inside a track, so it is compared as a
// multiset per tick.
static void sortWithinTicks(std::vector<EW>& v) {
    std::size_t a = 0;
    while (a < v.size()) {
        std::size_t b = a;
        while (b < v.size() && v[b].tick == v[a].tick) ++b;
        std::sort(v.begin() + a, v.begin() + b, [](const EW& x, const EW& y) { return x.packed < y.packed; });
        a = b;
    }
}
int main(int argc, char** argv) {
    g_fd = open(argv[1], O_RDONLY);
    struct stat st; fstat(g_fd, &st);
    BpfaMidiStore store; MidiDocument meta; store.setRawSourceLimit(getenv("RAW") ? ~0ull : 0);
    if (!store.index(uint64_t(st.st_size), readAt, nullptr, meta, nullptr, nullptr)) { fprintf(stderr, "index failed\n"); return 1; }
    const MidiDocument& md = store.metadata();
    std::vector<double> starts;
    for (int a = 2; a < argc; ++a) starts.push_back(atof(argv[a]));
    if (starts.empty()) starts = {0.0, md.durationSeconds * 0.37};
    int failures = 0;
    for (double start : starts) {
        const double end = std::min<double>(start + 3.0, md.durationSeconds + 1.0);
        std::vector<EW> ref, got, batch; std::vector<BpfaMidiStore::SysExBatchEvent> sx; std::vector<uint8_t> sxb;
        std::vector<double> times; bool complete, hasNext, done; uint32_t next; double safe;
        const uint32_t startTick = uint32_t(md.secondsToTick(start));
        store.resetEventCursor(startTick);
        // Reference: every event with seconds < end (whole ticks).
        const uint32_t endTick = uint32_t(std::ceil(md.secondsToTick(end))) - 1u;
        do { store.buildEventBatch(endTick, 100000, batch, sx, sxb, complete, next, hasNext); expand(batch, ref); } while (!complete);
        BpfaMidiStore::SynthFeedConfig cfg; cfg.admissionPercent = 0;
        store.setSynthFeedConfig(cfg);
        store.resetSynthFeed(startTick);
        std::vector<EW> raw;
        do { store.buildSynthFeed(end, 50000, batch, times, sx, sxb, safe, done); expand(batch, raw); } while (!done && safe < end);
        for (const EW& e : raw) if (e.tick <= endTick) got.push_back(e);
        sortWithinTicks(ref);
        sortWithinTicks(got);
        std::size_t diff = 0; while (diff < ref.size() && diff < got.size() && ref[diff].tick == got[diff].tick && ref[diff].packed == got[diff].packed) ++diff;
        const bool same = ref.size() == got.size() && diff == ref.size();
        printf("start %.2f: reference %zu events, feed %zu -> %s", start, ref.size(), got.size(), same ? "IDENTICAL" : "DIFFER");
        if (!same) {
            printf(" (first difference at %zu)", diff); ++failures;
            if (getenv("DEBUG")) for (std::size_t k = diff > 3 ? diff - 3 : 0; k < diff + 4; ++k)
                printf("\n   %zu: ref %u %08x | feed %u %08x", k, k < ref.size() ? ref[k].tick : 0, k < ref.size() ? ref[k].packed : 0,
                       k < got.size() ? got[k].tick : 0, k < got.size() ? got[k].packed : 0);
        }
        printf("\n");
        // Admission on: per key, never more offs than ons, and balanced at the end of the song.
        for (uint32_t voices : {256u, 1024u}) {
            cfg.admissionPercent = 25; cfg.maxVoices = voices;
            store.setSynthFeedConfig(cfg);
            store.resetSynthFeed(startTick);
            std::vector<long> open(16 * 128, 0); long orphan = 0; unsigned long long ons = 0;
            do {
                store.buildSynthFeed(1e9, 1u << 20, batch, times, sx, sxb, safe, done);
                std::vector<EW> ex; expand(batch, ex);
                for (const EW& e : ex) {
                    const uint32_t k = ((e.packed & 15u) << 7) | ((e.packed >> 8) & 127u);
                    if ((e.packed & 0xf0u) == 0x90u) { ++open[k]; ++ons; }
                    else if ((e.packed & 0xf0u) == 0x80u) { if (open[k] > 0) --open[k]; else ++orphan; }
                }
            } while (!done);
            long hanging = 0; for (long o : open) hanging += o;
            // Reference counts of what the file itself leaves open/orphaned from this point.
            store.resetEventCursor(startTick);
            std::vector<long> ropen(16 * 128, 0); long rorphan = 0;
            do {
                store.buildEventBatch(md.maxTick, 200000, batch, sx, sxb, complete, next, hasNext);
                std::vector<EW> ex; expand(batch, ex);
                for (const EW& e : ex) {
                    const uint32_t k = ((e.packed & 15u) << 7) | ((e.packed >> 8) & 127u);
                    if ((e.packed & 0xf0u) == 0x90u) ++ropen[k];
                    else if ((e.packed & 0xf0u) == 0x80u) { if (ropen[k] > 0) --ropen[k]; else ++rorphan; }
                }
            } while (!complete);
            long rhanging = 0; for (long o : ropen) rhanging += o;
            const bool ok = hanging <= rhanging && orphan <= rorphan;
            printf("  admission %u voices: %llu note-ons kept, hanging %ld (file %ld), orphan offs %ld (file %ld) -> %s\n",
                voices, ons, hanging, rhanging, orphan, rorphan, ok ? "OK" : "FAIL");
            if (!ok) ++failures;
        }
    }
    return failures ? 1 : 0;
}
