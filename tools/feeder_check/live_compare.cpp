// Live/keyboard snapshots with playback frames and seeks: current store vs BpfaMidiStore (HANDOFF sec. 44).
#include "midi_mapped_store.hpp"
#include "bpfa_midi_store.hpp"
#include "bpfa_parallel_sim.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
using namespace wasmidi;
static const uint8_t* g_data; static uint64_t g_size;
static bool readAt(void*, uint64_t off, uint8_t* dst, std::size_t n) { if (off + n > g_size) return false; memcpy(dst, g_data + off, n); return true; }
int main(int argc, char** argv) {
    int fd = open(argv[1], O_RDONLY); struct stat st; fstat(fd, &st);
    g_size = st.st_size; g_data = (const uint8_t*)mmap(nullptr, g_size, PROT_READ, MAP_PRIVATE, fd, 0);
    FILE* out = fopen(argv[2], "wb");
#ifdef USE_BPFA
    BpfaMidiStore store;
#else
    MidiMappedStore store;
#endif
#ifdef USE_BPFA
    if (getenv("BPFA_ZSTD")) store.setRawSourceLimit(0);
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
    double ms = 0;
    auto frame = [&](double sec, bool force) {
        const double t = md.secondsToTick(sec), n = md.secondsToTick(std::max(0.0, sec - 0.25)), c = md.secondsToTick(std::max(0.0, sec - 1.0));
        MidiMappedStore::LiveSnapshot s;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = store.buildLiveSnapshot(t, n, c, s, force);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        fwrite(&ok, 1, 1, out); fwrite(&s, sizeof(s), 1, out);
    };
    for (int i = 1; i < argc - 2; ++i) {
        double start = atof(argv[i + 2]);
        frame(start, false);
        for (int f = 1; f <= 120; ++f) frame(start + f / 60.0, false);
    }
    fclose(out); fprintf(stderr, "snapshot time %.0f ms\n", ms);
    return 0;
}
