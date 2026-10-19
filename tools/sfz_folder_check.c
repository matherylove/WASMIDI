/*
 * SFZ folder check (HANDOFF sec. 91).
 *
 * Writes a small sample library to a temporary folder the way libraries made
 * on Windows look (backslashes, other letter case, spaces, #define, #include,
 * samples set in <group>, several headers on one line, 8/24-bit, float and
 * extensible WAV, a missing sample, *sine), loads it with the real parser and
 * engine, and checks the regions and that a note sounds.
 *
 * From the repo root:
 *   S=third_party/snappysynthv2
 *   cc -O1 -Wall -I$S -DSNAPPYSYNTH_WASM=1 -pthread -o /tmp/sfzc tools/sfz_folder_check.c \
 *      $S/snappy_wasm_core.c $S/Voice/voice.c $S/Parser/sf2_parser.c $S/Parser/sfz_parser.c \
 *      $S/Parser/wav_loader.c $S/wasm_stubs.c -lm && /tmp/sfzc
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "Parser/sfz_parser.h"

int ssw_init_ex(int, int, int, int, int, int, int, int, int, int, int, int, int, int);
int ssw_load_sf2(const char*);
int ssw_region_count(void);
int ssw_render_queued_into(uintptr_t, int);
int ssw_queue_events(const uint32_t*, const double*, int);
void ssw_clear_soundfonts(void);

static int failures = 0;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

/* format 1 = PCM, 3 = float, 0xFFFE = extensible PCM. */
static void write_wav(const char* path, int format, int bits, int channels, int rate, int frames) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    int bytes = bits / 8;
    uint32_t data = (uint32_t)(frames * channels * bytes);
    int fmt_size = format == 0xFFFE ? 40 : 16;
    fwrite("RIFF", 1, 4, f);
    put32(f, 4 + 8 + (uint32_t)fmt_size + 8 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, (uint32_t)fmt_size);
    put16(f, (uint16_t)format);
    put16(f, (uint16_t)channels);
    put32(f, (uint32_t)rate);
    put32(f, (uint32_t)(rate * channels * bytes));
    put16(f, (uint16_t)(channels * bytes));
    put16(f, (uint16_t)bits);
    if (format == 0xFFFE) {
        put16(f, 22);
        put16(f, (uint16_t)bits);
        put32(f, 3);
        put16(f, 1);
        static const unsigned char tail[14] = { 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
        fwrite(tail, 1, 14, f);
    }
    fwrite("data", 1, 4, f);
    put32(f, data);
    for (int i = 0; i < frames * channels; ++i) {
        double v = 0.5 * sin(2.0 * 3.14159265358979 * 440.0 * (double)(i / channels) / rate);
        if (format == 3) {
            float x = (float)v;
            fwrite(&x, 4, 1, f);
        } else if (bits == 8) {
            unsigned char x = (unsigned char)(128 + (int)lrint(v * 127.0));
            fwrite(&x, 1, 1, f);
        } else {
            int32_t x = (int32_t)lrint(v * 2147483647.0);
            fwrite((const unsigned char*)&x + (4 - bytes), 1, (size_t)bytes, f);
        }
    }
    fclose(f);
}

static void write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

int main(void) {
    char root[] = "/tmp/sfz_folder_check_XXXXXX";
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    char p[512];
    snprintf(p, sizeof(p), "%s/Lib", root); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Lib/Samples", root); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Lib/Samples/Soft Layer", root); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Lib/Samples/Soft Layer/piano c4.wav", root); write_wav(p, 1, 24, 2, 44100, 4410);
    snprintf(p, sizeof(p), "%s/Lib/Samples/Soft Layer/Piano D4.wav", root); write_wav(p, 3, 32, 1, 48000, 4800);
    snprintf(p, sizeof(p), "%s/Lib/Samples/byte.wav", root); write_wav(p, 1, 8, 1, 22050, 2205);
    snprintf(p, sizeof(p), "%s/Lib/Samples/ext.wav", root); write_wav(p, 0xFFFE, 24, 2, 44100, 4410);
    snprintf(p, sizeof(p), "%s/Lib/common.sfzh", root);
    write_text(p, "<group> lovel=1 hivel=127\n<region> sample=$DIR\\byte.wav key=40\n");
    snprintf(p, sizeof(p), "%s/Lib/Piano.sfz", root);
    write_text(p,
        "// test library\n"
        "/* block\n comment <region> sample=nothing.wav */\n"
        "#define $DIR SAMPLES\n"
        "<control> default_path=\n"
        "<global> ampeg_release=0.2\n"
        "<group> sample=$DIR\\SOFT LAYER\\PIANO C4.WAV\n"
        "<region> lokey=55 hikey=61 pitch_keycenter=60 <region> key=72 sample=*sine\n"
        "<region> sample=Samples\\Soft Layer\\piano d4.wav lokey=62 hikey=64 pitch_keycenter=d4\n"
        "<region> sample=samples/ext.WAV key=50 loopmode=loop_continuous\n"
        "<region> sample=missing.wav key=30\n"
        "#include \"common.sfzh\"\n"
        "<effect> type=reverb\n");

    snprintf(p, sizeof(p), "%s/Lib/Piano.sfz", root);
    sfz_instrument* inst = sfz_parse(p);
    check(inst != NULL, "sfz_parse returns an instrument");
    if (inst) {
        check(inst->num_regions == 5, "5 regions with samples (missing one dropped)");
        int c4 = -1, d4 = -1, sine = -1, ext = -1, byte = -1;
        for (int i = 0; i < inst->num_regions; ++i) {
            const sfz_region* r = &inst->regions[i];
            printf("     region %d: %s keys %d-%d center %d rate %d ch %d n %d\n", i, r->sample, r->lokey, r->hikey,
                   r->pitch_keycenter, r->sample_data ? r->sample_data->sample_rate : 0,
                   r->sample_data ? r->sample_data->num_channels : 0, r->sample_data ? r->sample_data->num_samples : 0);
            if (r->lokey == 55) c4 = i;
            if (r->lokey == 62) d4 = i;
            if (r->lokey == 72) sine = i;
            if (r->lokey == 50) ext = i;
            if (r->lokey == 40) byte = i;
        }
        check(c4 >= 0 && inst->regions[c4].sample_data && inst->regions[c4].sample_data->num_channels == 2 &&
              inst->regions[c4].sample_data->num_samples == 4410, "group sample, backslashes, other case, 24-bit stereo");
        check(d4 >= 0 && inst->regions[d4].pitch_keycenter == 62 && inst->regions[d4].sample_data &&
              inst->regions[d4].sample_data->sample_rate == 48000, "unquoted sample path with spaces, float WAV");
        check(sine >= 0 && inst->regions[sine].sample_data && inst->regions[sine].sample_data->has_loop,
              "two headers on one line, *sine generator");
        check(ext >= 0 && inst->regions[ext].sample_data && inst->regions[ext].sample_data->num_samples == 4410 &&
              inst->regions[ext].loop_mode == 1, "extensible WAV, loopmode alias");
        check(byte >= 0 && inst->regions[byte].sample_data && inst->regions[byte].sample_data->sample_rate == 22050,
              "#include relative to the main file, #define, 8-bit WAV");
        if (c4 >= 0 && inst->regions[c4].sample_data) {
            int peak = 0;
            const wav_data* w = inst->regions[c4].sample_data;
            for (int i = 0; i < w->num_samples * w->num_channels; ++i) {
                int a = abs(w->data[i]);
                if (a > peak) peak = a;
            }
            check(peak > 15000 && peak < 17000, "24-bit samples converted to the right level");
        }
        sfz_free(inst);
    }

    if (!ssw_init_ex(44100, 2, 32, 512, 16, 0, 256, 0, 1, 0, 1, 1, 0, 1)) {
        puts("FAIL ssw_init_ex");
        return 1;
    }
    check(ssw_load_sf2(p) > 0 && ssw_region_count() == 5, "ssw_load_sf2 loads a .sfz path");
    const uint32_t msgs[2] = { 0x643C90u, 0x003C80u };
    const double times[2] = { 0.0, 0.08 };
    ssw_queue_events(msgs, times, 2);
    static float buf[512 * 2];
    float peak = 0.0f;
    for (int b = 0; b < 8; ++b) {
        ssw_render_queued_into((uintptr_t)buf, 512);
        for (int i = 0; i < 1024; ++i) {
            if (fabsf(buf[i]) > peak) peak = fabsf(buf[i]);
        }
    }
    check(peak > 0.01f, "a note on key 60 sounds");
    ssw_clear_soundfonts();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    puts("OK sfz folder");
    return 0;
}
