// Parser synth feed + WasmiSynth on the host (HANDOFF sec. 70): indexes a MIDI, then for each
// requested start point streams the admitted events into the real engine block by block and
// reports feed and render cost per second of song. Optional WAV per point.
//   ./pipeline_bench SF2 MID WORKERS DUR START [START...]
// Env: VOICES (1024), FEED=0 (send every event; the synth admits), ADMIT, STEALCAP, DET=1,
//      WAV=prefix (writes prefix_<start>.wav), GPUSIDE=1 (voices go to the WebGPU request list and
//      are not rendered: the CPU side of the GPU mode)
#include "bpfa_midi_store.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
int ssw_init_ex(int, int, int, int, int, int, int, int, int, int, int, int, int, int);
int ssw_load_sf2(const char*);
int ssw_warmup(void);
int ssw_queue_events(const uint32_t*, const double*, int);
int ssw_render_queued_into(uintptr_t, int);
void ssw_set_song_time(double);
void ssw_reset(void);
void ssw_set_admission_percent(int);
void ssw_set_steal_cap_percent(int);
int ssw_admission_dropped(void);
void ssw_set_deterministic(int);
int ssw_active_voices(void), ssw_steals(void), ssw_dropped_notes(void), ssw_worker_count(void);
int ssw_set_debug_metrics(int);
int ssw_path_fast_voices(void), ssw_path_scalar_voices(void), ssw_path_simd_voice_voices(void);
int ssw_miss_interp(void), ssw_miss_loop(void), ssw_miss_filter(void), ssw_miss_other(void), ssw_notes_started(void);
int ssw_alloc_us(void), ssw_worker_busy_us(void), ssw_workers_participating(void);
}
using namespace wasmidi;
using Clock = std::chrono::steady_clock;
static int g_fd = -1;
static bool readAt(void*, uint64_t off, uint8_t* dst, std::size_t n) {
    std::size_t done = 0;
    while (done < n) { ssize_t r = pread(g_fd, dst + done, n - done, off + done); if (r <= 0) return false; done += r; }
    return true;
}
static double ms(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }

static void writeWav(const char* path, const std::vector<int16_t>& pcm, int sr) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    uint32_t data = uint32_t(pcm.size() * 2), riff = data + 36, fs = 16, rate = sr, br = sr * 4;
    uint16_t fmt = 1, ch = 2, ba = 4, bps = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fs, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm.data(), 2, pcm.size(), f); fclose(f);
}

extern "C" { int ssw_wgpu_set_enabled(int); void ssw_wgpu_begin_block(void); void ssw_wgpu_set_frame_base(int);
int ssw_wgpu_request_count(void); int ssw_wgpu_collect_samples(uintptr_t, int, uintptr_t); }
static long g_reqs = 0;
int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: pipeline_bench SF2 MID WORKERS DUR START [START...]\n"); return 2; }
    const int workers = atoi(argv[3]);
    const double dur = atof(argv[4]);
    const int SR = 48000, BLOCK = 512;
    const int voices = getenv("VOICES") ? atoi(getenv("VOICES")) : 1024;
    const bool feed = !getenv("FEED") || atoi(getenv("FEED")) != 0;
    g_fd = open(argv[2], O_RDONLY);
    struct stat st; if (g_fd < 0 || fstat(g_fd, &st)) { perror("open"); return 1; }
    BpfaMidiStore store; MidiDocument meta; store.setRawSourceLimit(0);
    auto t0 = Clock::now();
    if (!store.index(uint64_t(st.st_size), readAt, nullptr, meta, nullptr, nullptr)) { fprintf(stderr, "index: %s\n", store.error()); return 1; }
    const MidiDocument& md = store.metadata();
    printf("index %.0f s, %llu notes, %.1f s\n", ms(t0) / 1000.0, (unsigned long long)md.noteCount, md.durationSeconds);

    if (!ssw_init_ex(SR, 2, 32, BLOCK, 16, 0, voices, 0, workers, 0, 1, 1, 0, 1)) { puts("init fail"); return 1; }
    if (ssw_load_sf2(argv[1]) <= 0) { puts("sf2 fail"); return 1; }
    ssw_warmup();
    if (getenv("ADMIT")) ssw_set_admission_percent(atoi(getenv("ADMIT")));
    if (getenv("STEALCAP")) ssw_set_steal_cap_percent(atoi(getenv("STEALCAP")));
    if (getenv("DET")) ssw_set_deterministic(1);
    if (getenv("PATHS")) ssw_set_debug_metrics(1);
    const bool gpuside = getenv("GPUSIDE") != nullptr;
    if (gpuside) { static uint32_t table[4 * 65536]; uint32_t total = 0;
        ssw_wgpu_collect_samples((uintptr_t)table, 65536, (uintptr_t)&total); printf("gpu side enabled=%d\n", ssw_wgpu_set_enabled(1)); }

    BpfaMidiStore::SynthFeedConfig cfg;
    cfg.maxVoices = uint32_t(voices);
    cfg.admissionPercent = feed ? 25u : 0u;
    cfg.sampleRate = SR;
    cfg.blockFrames = BLOCK;
    store.setSynthFeedConfig(cfg);

    std::vector<BpfaMidiStore::EventWord> batch; std::vector<double> times;
    std::vector<BpfaMidiStore::SysExBatchEvent> sx; std::vector<uint8_t> sxb;
    std::vector<uint32_t> msgs; std::vector<float> buf(BLOCK * 2);
    for (int a = 5; a < argc; ++a) {
        const double start = atof(argv[a]);
        ssw_reset();
        ssw_set_song_time(start);
        store.resetSynthFeed(uint32_t(md.secondsToTick(start)));
        double safe = start; bool done = false;
        double feedMs = 0, renderMs = 0, maxBlock = 0, secFeed = 0, secRender = 0, secMax = 0;
        long secEvents = 0, events = 0; int secBlocks = 0; double secAlloc = 0, secBusy = 0, secPart = 0;
        int steals0 = ssw_steals(), drops0 = ssw_dropped_notes(), adm0 = ssw_admission_dropped();
        std::vector<int16_t> pcm;
        double mark = start;
        for (double bt = start; bt < start + dur - 1e-9; bt += double(BLOCK) / SR) {
            const double bend = bt + double(BLOCK) / SR;
            while (!done && safe < bend + 0.05) {
                auto f0 = Clock::now();
                store.buildSynthFeed(bend + 0.05, 1u << 18, batch, times, sx, sxb, safe, done);
                msgs.resize(batch.size());
                for (std::size_t i = 0; i < batch.size(); ++i) msgs[i] = batch[i].packed;
                if (!msgs.empty()) ssw_queue_events(msgs.data(), times.data(), int(msgs.size()));
                const double f = ms(f0); feedMs += f; secFeed += f;
                events += long(msgs.size()); secEvents += long(msgs.size());
            }
            auto r0 = Clock::now();
            if (gpuside) { ssw_wgpu_begin_block(); ssw_wgpu_set_frame_base(0); }
            ssw_render_queued_into(uintptr_t(buf.data()), BLOCK);
            if (gpuside) g_reqs += ssw_wgpu_request_count();
            const double r = ms(r0); renderMs += r; secRender += r; maxBlock = std::max(maxBlock, r); secMax = std::max(secMax, r);
            ++secBlocks;
            secAlloc += ssw_alloc_us(); secBusy += ssw_worker_busy_us(); secPart += ssw_workers_participating();
            if (getenv("WAV")) for (float v : buf) pcm.push_back(int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
            if (bend >= mark + 1.0 - 1e-9) {
                const double span = bend - mark;
                printf("  t=%7.1f s: feed %6.1f ms (%7ld events) | render avg %5.2f max %5.2f ms/block | act %5d steals/s %6.0f drops/s %6.0f\n",
                    bend, secFeed / span, secEvents, secRender / secBlocks, secMax, ssw_active_voices(),
                    (ssw_steals() - steals0) / span, (ssw_dropped_notes() - drops0) / span);
                if (getenv("PATHS"))
                    printf("      alloc %.2f ms/block, worker busy %.2f ms/block, workers participating %.2f | started %d\n",
                        secAlloc / secBlocks / 1000.0, secBusy / secBlocks / 1000.0, secPart / secBlocks, ssw_notes_started());
                secAlloc = secBusy = secPart = 0;
                fflush(stdout);
                steals0 = ssw_steals(); drops0 = ssw_dropped_notes();
                secFeed = secRender = secMax = 0; secEvents = 0; secBlocks = 0; mark = bend;
            }
        }
        const auto& fs = store.synthFeedStats();
        printf("START %.1f: %.1f s, feed %.2fx realtime (%ld events, %llu decoded, %llu note-ons, parser dropped %llu), synth admission dropped %d,"
               " render %.2fx (max block %.2f ms of %.2f)\n",
            start, dur, feedMs / (dur * 1000.0), events, (unsigned long long)fs.decoded, (unsigned long long)fs.noteOns,
            (unsigned long long)fs.droppedNoteOns, ssw_admission_dropped() - adm0, renderMs / (dur * 1000.0), maxBlock,
            1000.0 * BLOCK / SR);
        if (getenv("WAV")) {
            const std::string path = std::string(getenv("WAV")) + "_" + std::to_string(int(start)) + ".wav";
            writeWav(path.c_str(), pcm, SR);
        }
    }
    if (getenv("GPUSIDE")) printf("gpu requests %ld\n", g_reqs);
    return 0;
}
