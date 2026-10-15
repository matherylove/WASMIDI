// Renderer sweep: current mapped store vs BpfaMidiStore (HANDOFF sec. 44). Build with/without -DUSE_BPFA.
#include "midi_mapped_store.hpp"
#include "bpfa_midi_store.hpp"
#include <cstdio>
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
    if (!store.index(g_size, readAt, nullptr, meta)) { fprintf(stderr, "index failed\n"); return 1; }
    const MidiDocument& md = store.metadata();
    uint32_t tick = (uint32_t)std::max(0.0, std::floor(md.secondsToTick(startSec)));
    store.resetRenderCursor(tick, perTrack != 0);
    std::vector<VisualNote> appends; std::vector<MidiMappedStore::RenderClose> closes;
    uint32_t base, nextTick; bool complete, hasNext; double target = startSec; long na = 0, nc = 0;
    while (target < startSec + durSec) {
        target += 0.05;
        uint32_t endTick = (uint32_t)std::min<double>(md.maxTick, std::ceil(md.secondsToTick(target)));
        for (;;) {
            if (!store.buildRenderSweep(endTick, 262144, appends, closes, base, complete, nextTick, hasNext)) { fprintf(stderr, "sweep fail\n"); return 1; }
            fwrite(&base, 4, 1, out);
            uint32_t n = appends.size(); fwrite(&n, 4, 1, out); if (n) fwrite(appends.data(), sizeof(VisualNote), n, out);
            n = closes.size(); fwrite(&n, 4, 1, out); if (n) fwrite(closes.data(), sizeof(MidiMappedStore::RenderClose), n, out);
            na += appends.size(); nc += closes.size();
            if (complete) break;
        }
    }
    fclose(out); fprintf(stderr, "appends %ld closes %ld\n", na, nc);
    return 0;
}
