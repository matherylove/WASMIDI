// Synth stream of the current mapped store vs BpfaMidiStore (HANDOFF sec. 43). Build with/without -DUSE_BPFA.
#include "midi_mapped_store.hpp"
#include "bpfa_midi_store.hpp"
#include <chrono>
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
    double startSec = atof(argv[2]), durSec = atof(argv[3]); int floorVel = atoi(argv[4]);
    FILE* out = fopen(argv[5], "wb");
    #ifdef USE_BPFA
    BpfaMidiStore store;
#else
    MidiMappedStore store;
#endif
#ifdef USE_BPFA
    if (getenv("BPFA_ZSTD")) store.setRawSourceLimit(0);
#endif
    MidiDocument meta;
    auto t0 = std::chrono::steady_clock::now();
    if (!store.index(g_size, readAt, nullptr, meta)) { fprintf(stderr, "index failed: %s\n", store.error()); return 1; }
    fprintf(stderr, "index %.0f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
#ifdef USE_BPFA
    fprintf(stderr, "bpfa notes %zu long %zu memory %.1f MB\n", store.noteRecordCount(), store.longNoteCount(), store.memoryBytes() / 1048576.0);
#endif
    const MidiDocument& md = store.metadata();
    uint32_t tick = (uint32_t)std::max(0.0, std::floor(md.secondsToTick(startSec)));
    store.resetEventCursor(tick);
    std::vector<decltype(store)::SysExBatchEvent> sxe; std::vector<uint8_t> sxb;
    store.buildHistoricalSysEx(tick, sxe, sxb);
    double histTime = md.tickToSeconds(tick);
    // record: type(1=msg,2=sysex) time msg / len bytes
    for (auto& e : sxe) { uint8_t ty = 2; fwrite(&ty,1,1,out); fwrite(&histTime,8,1,out); fwrite(&e.length,4,1,out); fwrite(sxb.data()+e.offset,1,e.length,out); }
    std::vector<decltype(store)::EventWord> sel; store.buildHistoricalSelectorState(tick, sel);
    for (auto& e : sel) { uint8_t ty = 1; uint32_t m = e.packed; fwrite(&ty,1,1,out); fwrite(&histTime,8,1,out); fwrite(&m,4,1,out); }
    std::vector<decltype(store)::EventWord> batch; bool complete; uint32_t nextTick; bool hasNext;
    double target = startSec; long total = 0;
    while (target < startSec + durSec) {
        target += 0.05;
        uint32_t endTick = (uint32_t)std::min<double>(md.maxTick, std::ceil(md.secondsToTick(target)));
        for (;;) {
            if (!store.buildEventBatch(endTick, 262144, batch, sxe, sxb, complete, nextTick, hasNext)) { fprintf(stderr,"batch fail\n"); return 1; }
            for (auto& e : sxe) { uint8_t ty = 2; double t = md.tickToSeconds(e.tick); fwrite(&ty,1,1,out); fwrite(&t,8,1,out); fwrite(&e.length,4,1,out); fwrite(sxb.data()+e.offset,1,e.length,out); }
            for (size_t i = 0; i < batch.size(); ++i) {
                uint32_t packed = batch[i].packed, status = packed & 255, cmd = status & 0xf0, d2 = (packed >> 16) & 127;
                if (cmd == 0x90 && d2 != 0 && (int)d2 < floorVel) continue;
                uint32_t word = packed & 0xffffff; uint32_t count = 1;
                if (cmd == 0x90 && d2 != 0) count = ((packed >> 24) & 255) + 1;
                double t = md.tickToSeconds(batch[i].tick);
                // expanded form: one record per stacked note
                for (uint32_t k = 0; k < count; ++k) { uint8_t ty = 1; fwrite(&ty,1,1,out); fwrite(&t,8,1,out); fwrite(&word,4,1,out); ++total; }
            }
            if (complete) break;
        }
    }
    fclose(out); fprintf(stderr, "ref events %ld tick %u maxTick %u\n", total, tick, md.maxTick);
    return 0;
}
