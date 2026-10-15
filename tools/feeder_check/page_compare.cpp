// Visual pages (sequential 64-screen warming plus seeks): current store vs BpfaMidiStore (HANDOFF sec. 45).
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
    FILE* out = fopen(argv[2], "wb");
#ifdef USE_BPFA
    BpfaMidiStore store;
#else
    MidiMappedStore store;
#endif
    MidiDocument meta;
    if (!store.index(g_size, readAt, nullptr, meta)) { fprintf(stderr, "index failed\n"); return 1; }
    const MidiDocument& md = store.metadata();
    const uint32_t span = (uint32_t)std::max(1.0, std::ceil(md.secondsToTick(0.1) - md.secondsToTick(0.0)));
    long notes = 0; std::vector<VisualNote> page;
    for (int i = 3; i < argc; ++i) {
        const uint32_t start = (uint32_t)std::floor(md.secondsToTick(atof(argv[i])));
        for (int k = 0; k < 64; ++k) {
            const uint32_t a = start + k * span, b = a + span - 1;
            bool ok = store.buildVisualPage(a, b, page);
            uint32_t n = page.size();
            fwrite(&ok, 1, 1, out); fwrite(&n, 4, 1, out); if (n) fwrite(page.data(), sizeof(VisualNote), n, out);
            notes += n;
        }
    }
    fclose(out); fprintf(stderr, "page notes %ld\n", notes);
    return 0;
}
