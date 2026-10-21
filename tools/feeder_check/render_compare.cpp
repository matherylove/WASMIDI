// Renderer sweep: current mapped store vs BpfaMidiStore (HANDOFF sec. 44). Build with/without -DUSE_BPFA.
#include "midi_mapped_store.hpp"
#include "bpfa_midi_store.hpp"
#include "bpfa_parallel_sim.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <vector>
using namespace wasmidi;
static const uint8_t* g_data; static uint64_t g_size;
static bool readAt(void*, uint64_t off, uint8_t* dst, std::size_t n) { if (off + n > g_size) return false; memcpy(dst, g_data + off, n); return true; }
int main(int argc, char** argv) {
    int fd = open(argv[1], O_RDONLY); struct stat st; fstat(fd, &st);
    g_size = st.st_size; g_data = (const uint8_t*)mmap(nullptr, g_size, PROT_READ, MAP_PRIVATE, fd, 0);
    double startSec = atof(argv[2]), durSec = atof(argv[3]); int perTrack = atoi(argv[4]);
    FILE* out = fopen(argv[5], "wb");
#ifdef USE_BPFA
    BpfaMidiStore store;
#else
    MidiMappedStore store;
#endif
    MidiDocument meta;
    #ifdef USE_BPFA
    const char* parallelEnv = getenv("BPFA_PARALLEL");
    const bool indexed = parallelEnv
        ? bpfaParallelIndex(store, g_size, readAt, nullptr, meta, unsigned(atoi(parallelEnv)), getenv("BPFA_ZSTD") != nullptr)
        : store.index(g_size, readAt, nullptr, meta);
#else
    const bool indexed = store.index(g_size, readAt, nullptr, meta);
#endif
    if (!indexed) { fprintf(stderr, "index failed\n"); return 1; }
    const MidiDocument& md = store.metadata();
    uint32_t tick = (uint32_t)std::max(0.0, std::floor(md.secondsToTick(startSec)));
    // FOLD=W:L (HANDOFF sec. 108): BPFA sweeps with the fold, the old store filters by the same pixel
    // centres afterwards; both write only the decided notes, numbered in order.
    uint32_t foldW = 0, foldL = 0;
    if (const char* fold = getenv("FOLD")) sscanf(fold, "%u:%u", &foldW, &foldL);
#ifdef USE_BPFA
    store.setRenderFold(foldW, foldL);
#endif
    store.resetRenderCursor(tick, perTrack != 0);
    std::vector<VisualNote> allAppends; std::vector<MidiMappedStore::RenderClose> allCloses; uint32_t firstBase = 0, lastEnd = 0;
    std::vector<VisualNote> appends; std::vector<MidiMappedStore::RenderClose> closes;
    uint32_t base, nextTick; bool complete, hasNext; double target = startSec; long na = 0, nc = 0;
    while (target < startSec + durSec) {
        target += 0.05;
        uint32_t endTick = (uint32_t)std::min<double>(md.maxTick, std::ceil(md.secondsToTick(target)));
        for (;;) {
            if (!store.buildRenderSweep(endTick, 262144, appends, closes, base, complete, nextTick, hasNext)) { fprintf(stderr, "sweep fail\n"); return 1; }
            lastEnd = endTick;
            if (foldW) {
                if (allAppends.empty() && !appends.empty()) firstBase = base;
                allAppends.insert(allAppends.end(), appends.begin(), appends.end());
                allCloses.insert(allCloses.end(), closes.begin(), closes.end());
                if (complete) break;
                continue;
            }
            fwrite(&base, 4, 1, out);
            uint32_t n = appends.size(); fwrite(&n, 4, 1, out); if (n) fwrite(appends.data(), sizeof(VisualNote), n, out);
            n = closes.size(); fwrite(&n, 4, 1, out); if (n) fwrite(closes.data(), sizeof(MidiMappedStore::RenderClose), n, out);
            na += appends.size(); nc += closes.size();
            if (complete) break;
        }
    }
    if (foldW) {
        const auto column = [&](uint64_t d) -> int64_t {
            const int64_t num = int64_t(d * 2u * foldL) - int64_t(foldW);
            return num <= 0 ? 0 : (num + 2 * int64_t(foldW) - 1) / (2 * int64_t(foldW));
        };
        std::vector<uint32_t> ends(allAppends.size(), 0), number(allAppends.size(), 0);
        for (const auto& c : allCloses) if (c.noteId >= firstBase && c.noteId - firstBase < ends.size()) ends[c.noteId - firstBase] = c.endTick;
        const uint32_t known = lastEnd > foldW / foldL + 2 ? lastEnd - foldW / foldL - 2 : 0;
        uint32_t n = 0;
        for (std::size_t i = 0; i < allAppends.size(); ++i) {
            const uint32_t s = allAppends[i].startTick, e = ends[i] ? ends[i] : 0xffffffffu;
#ifndef USE_BPFA
            if (column(e) <= column(s)) continue;
#endif
            if (s >= known) continue;
            number[i] = ++n;
            fwrite(&allAppends[i].startTick, 4, 1, out); fwrite(&allAppends[i].packedData, 4, 1, out);
        }
        for (const auto& c : allCloses) {
            if (c.noteId < firstBase || c.noteId - firstBase >= number.size() || !number[c.noteId - firstBase]) continue;
            fwrite(&number[c.noteId - firstBase], 4, 1, out); fwrite(&c.endTick, 4, 1, out);
        }
        na = n;
    }
    fclose(out); fprintf(stderr, "appends %ld closes %ld\n", na, nc);
    return 0;
}
