// Cuts [start, end) seconds out of a (huge) MIDI into a small valid MIDI for quick tests
// (HANDOFF sec. 70). Tempo and controller/program/pitch events before the cut are kept at
// tick 0 of the slice (last value per channel/controller); notes before the cut are dropped.
//   ./midi_slice IN.mid OUT.mid START END
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

typedef struct { uint64_t tick; uint32_t us; } tempo_pt;
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint32_t vlq(const uint8_t** p) { uint32_t v = 0; uint8_t c; do { c = *(*p)++; v = (v << 7) | (c & 0x7f); } while (c & 0x80); return v; }
typedef struct { uint8_t* d; size_t n, cap; } buf;
static void put(buf* b, const void* s, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 1024; b->d = realloc(b->d, b->cap); }
    memcpy(b->d + b->n, s, n); b->n += n;
}
static void putc8(buf* b, uint8_t c) { put(b, &c, 1); }
static void putvlq(buf* b, uint32_t v) {
    uint8_t t[5]; int n = 0; t[n++] = v & 0x7f;
    while (v >>= 7) t[n++] = 0x80 | (v & 0x7f);
    while (n) putc8(b, t[--n]);
}
static int cmp_tempo(const void* a, const void* b) {
    const tempo_pt *x = a, *y = b; return x->tick < y->tick ? -1 : x->tick > y->tick;
}

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: midi_slice IN OUT START END\n"); return 2; }
    int fd = open(argv[1], O_RDONLY); struct stat st; if (fd < 0 || fstat(fd, &st)) return 1;
    const uint8_t* base = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    const double s0 = atof(argv[3]), s1 = atof(argv[4]);
    const int ntrk = (base[10] << 8) | base[11], div = (base[12] << 8) | base[13];
    const uint8_t** tb = calloc(ntrk, sizeof *tb); const uint8_t** te = calloc(ntrk, sizeof *te);
    const uint8_t* p = base + 8 + be32(base + 4);
    for (int i = 0; i < ntrk; ++i) {
        while (memcmp(p, "MTrk", 4)) p += 8 + be32(p + 4);
        tb[i] = p + 8; te[i] = p + 8 + be32(p + 4); p = te[i];
    }
    // Tempo map from every track.
    size_t ntp = 1, cap = 1024; tempo_pt* tp = malloc(cap * sizeof *tp); tp[0] = (tempo_pt){0, 500000};
    for (int i = 0; i < ntrk; ++i) {
        const uint8_t* q = tb[i]; uint64_t tick = 0; uint8_t rs = 0;
        while (q < te[i]) {
            tick += vlq(&q);
            uint8_t s = *q; if (s < 0x80) s = rs; else ++q;
            if (s == 0xff) {
                uint8_t ty = *q++; uint32_t len = vlq(&q);
                if (ty == 0x51 && len == 3) { if (ntp == cap) tp = realloc(tp, (cap *= 2) * sizeof *tp); tp[ntp++] = (tempo_pt){tick, (q[0] << 16) | (q[1] << 8) | q[2]}; }
                q += len; if (ty == 0x2f) break;
            } else if (s == 0xf0 || s == 0xf7) { q += vlq(&q); }
            else { rs = s; q += ((s & 0xe0) == 0xc0) ? 1 : 2; }
        }
    }
    qsort(tp, ntp, sizeof *tp, cmp_tempo);
    // seconds -> tick
    uint64_t t0 = 0, t1 = 0; int got0 = 0, got1 = 0; double sec = 0;
    for (size_t k = 0; k < ntp && !(got0 && got1); ++k) {
        const uint64_t nextTick = k + 1 < ntp ? tp[k + 1].tick : ~0ull;
        const double rate = (double)tp[k].us / 1e6 / div; // seconds per tick
        const double endSec = nextTick == ~0ull ? 1e300 : sec + (nextTick - tp[k].tick) * rate;
        if (!got0 && s0 < endSec) { t0 = tp[k].tick + (uint64_t)((s0 - sec) / rate); got0 = 1; }
        if (!got1 && s1 < endSec) { t1 = tp[k].tick + (uint64_t)((s1 - sec) / rate); got1 = 1; }
        sec = endSec;
    }
    fprintf(stderr, "ticks %llu..%llu\n", (unsigned long long)t0, (unsigned long long)t1);
    FILE* out = fopen(argv[2], "wb");
    uint8_t hdr[14] = {'M','T','h','d',0,0,0,6,0,1,(uint8_t)(ntrk >> 8),(uint8_t)ntrk,(uint8_t)(div >> 8),(uint8_t)div};
    fwrite(hdr, 1, 14, out);
    buf b = {0};
    for (int i = 0; i < ntrk; ++i) {
        b.n = 0;
        // State before the cut: last tempo, controllers, program, pitch per channel.
        static int32_t cc[16][128], prog[16], bend[16];
        for (int c = 0; c < 16; ++c) { prog[c] = bend[c] = -1; for (int k = 0; k < 128; ++k) cc[c][k] = -1; }
        int32_t lastTempo = -1;
        const uint8_t* q = tb[i]; uint64_t tick = 0, last = t0; uint8_t rs = 0; int started = 0;
#define EMIT_STATE() do { \
                started = 1; \
                if (lastTempo >= 0) { putvlq(&b, 0); uint8_t m[6] = {0xff, 0x51, 3, (uint8_t)(lastTempo >> 16), (uint8_t)(lastTempo >> 8), (uint8_t)lastTempo}; put(&b, m, 6); } \
                for (int c = 0; c < 16; ++c) { \
                    for (int k = 0; k < 128; ++k) if (cc[c][k] >= 0) { putvlq(&b, 0); putc8(&b, 0xb0 | c); putc8(&b, k); putc8(&b, cc[c][k]); } \
                    if (prog[c] >= 0) { putvlq(&b, 0); putc8(&b, 0xc0 | c); putc8(&b, prog[c]); } \
                    if (bend[c] >= 0) { putvlq(&b, 0); putc8(&b, 0xe0 | c); putc8(&b, bend[c] & 0x7f); putc8(&b, bend[c] >> 7); } \
                } } while (0)
        while (q < te[i]) {
            tick += vlq(&q);
            if (tick >= t1) break;
            const uint8_t* ev = q;
            uint8_t s = *q; int running = s < 0x80; if (running) s = rs; else ++q;
            if (tick >= t0 && !started) EMIT_STATE();
            if (s == 0xff) {
                uint8_t ty = *q++; uint32_t len = vlq(&q);
                if (ty == 0x2f) break;
                if (ty == 0x51 && len == 3) {
                    if (tick < t0) lastTempo = (q[0] << 16) | (q[1] << 8) | q[2];
                    else { putvlq(&b, (uint32_t)(tick - last)); last = tick; put(&b, ev + (running ? 0 : 0), (size_t)(q + len - ev)); }
                }
                q += len; continue;
            }
            if (s == 0xf0 || s == 0xf7) { uint32_t len = vlq(&q); q += len; continue; }
            rs = s;
            const int two = (s & 0xe0) != 0xc0;
            const uint8_t d1 = q[0], d2 = two ? q[1] : 0; q += two ? 2 : 1;
            const uint8_t ty = s & 0xf0, c = s & 0x0f;
            if (tick < t0) {
                if (ty == 0xb0) cc[c][d1] = d2; else if (ty == 0xc0) prog[c] = d1; else if (ty == 0xe0) bend[c] = d1 | (d2 << 7);
                continue;
            }
            putvlq(&b, (uint32_t)(tick - last)); last = tick;
            putc8(&b, s); putc8(&b, d1); if (two) putc8(&b, d2);
        }
        // A track with nothing inside the cut still carries its state (the tempo track, often).
        if (!started) EMIT_STATE();
        putvlq(&b, 0); { uint8_t m[3] = {0xff, 0x2f, 0}; put(&b, m, 3); }
        uint8_t th[8] = {'M','T','r','k',(uint8_t)(b.n >> 24),(uint8_t)(b.n >> 16),(uint8_t)(b.n >> 8),(uint8_t)b.n};
        fwrite(th, 1, 8, out); fwrite(b.d, 1, b.n, out);
    }
    fclose(out);
    return 0;
}
