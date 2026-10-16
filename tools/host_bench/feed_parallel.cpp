// Parallel synth feed on the host (HANDOFF sec. 70): decode helpers are separate store
// instances built from exportFeedImage (as the browser helper Workers are), each in its own
// thread, decoding chunk requests; the coordinator stitches the results in order. Checks
// that the output is identical to the sequential feed with the same chunk size, and
// measures the time per song second.
//   ./feed_parallel FILE.mid HELPERS CHUNK_BLOCKS VOICES START DURATION [CHUNKS_PER_REQUEST]
#include "bpfa_midi_store.hpp"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace wasmidi;
using Clock = std::chrono::steady_clock;
using EW = BpfaMidiStore::EventWord;
static int g_fd = -1;
static bool readAt(void*, uint64_t off, uint8_t* dst, std::size_t n) {
    std::size_t done = 0;
    while (done < n) { ssize_t r = pread(g_fd, dst + done, n - done, off + done); if (r <= 0) return false; done += r; }
    return true;
}
static BpfaMidiStore* g_main = nullptr;
static bool fetchSlab(void*, uint64_t offset, uint8_t* dst, std::size_t n) {
    const std::size_t slab = BpfaMidiStore::sourceSlabBytes();
    if (offset / slab >= g_main->sourceSlabCount()) return false;
    std::memcpy(dst, g_main->sourceSlab(offset / slab) + offset % slab, n);
    return true;
}
static double sec(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }

int main(int argc, char** argv) {
    if (argc < 7) { fprintf(stderr, "usage: feed_parallel FILE HELPERS CHUNK_BLOCKS VOICES START DURATION\n"); return 2; }
    const int helpers = atoi(argv[2]);
    const uint32_t chunkBlocks = uint32_t(atoi(argv[3]));
    const uint32_t voices = uint32_t(atoi(argv[4]));
    const double start = atof(argv[5]), duration = atof(argv[6]);
    const uint32_t span = argc > 7 ? uint32_t(atoi(argv[7])) : 1u;
    g_fd = open(argv[1], O_RDONLY);
    struct stat st; if (g_fd < 0 || fstat(g_fd, &st)) { perror("open"); return 1; }
    static BpfaMidiStore store; MidiDocument meta; store.setRawSourceLimit(0);
    g_main = &store;
    auto t0 = Clock::now();
    if (!store.index(uint64_t(st.st_size), readAt, nullptr, meta, nullptr, nullptr)) { fprintf(stderr, "index failed: %s\n", store.error()); return 1; }
    const MidiDocument& md = store.metadata();
    printf("index %.1f s, %u tracks, %.1f s song\n", sec(t0), md.trackCount, md.durationSeconds);
    BpfaMidiStore::SynthFeedConfig cfg;
    cfg.maxVoices = voices ? voices : 1024u;
    cfg.admissionPercent = voices ? 25u : 0u;
    cfg.windowBlocks = chunkBlocks;
    store.setSynthFeedConfig(cfg);
    const uint32_t startTick = uint32_t(md.secondsToTick(start));
    const double end = start + duration;

    // Sequential reference.
    std::vector<EW> ref, batch; std::vector<double> times, refTimes; std::vector<BpfaMidiStore::SysExBatchEvent> sx; std::vector<uint8_t> sxb;
    double safe = 0; bool done = false;
    store.resetSynthFeed(startTick);
    t0 = Clock::now();
    do {
        store.buildSynthFeed(end, 1u << 20, batch, times, sx, sxb, safe, done);
        ref.insert(ref.end(), batch.begin(), batch.end());
        refTimes.insert(refTimes.end(), times.begin(), times.end());
    } while (!done && safe < end);
    const double seqSeconds = sec(t0);
    const double refSafe = safe;
    printf("sequential: %zu events in %.3f s = %.2fx realtime\n", ref.size(), seqSeconds, seqSeconds / duration);

    // Helpers.
    std::vector<uint8_t> image;
    if (!store.exportFeedImage(image)) { fprintf(stderr, "export failed\n"); return 1; }
    std::vector<std::unique_ptr<BpfaMidiStore>> workers;
    for (int h = 0; h < helpers; ++h) {
        workers.emplace_back(new BpfaMidiStore);
        if (!workers.back()->importFeedImage(image.data(), image.size(), fetchSlab, nullptr)) { fprintf(stderr, "import failed\n"); return 1; }
    }
    printf("feed image %zu bytes\n", image.size());
    store.resetSynthFeed(startTick);
    const uint64_t firstBlock = store.synthFeedBlock();
    const uint64_t lastBlock = uint64_t(std::ceil(end / store.synthFeedBlockSeconds()));
    const uint64_t spanBlocks = uint64_t(chunkBlocks) * span;
    const uint64_t chunks = (lastBlock - firstBlock + spanBlocks - 1) / spanBlocks;
    std::vector<std::vector<uint8_t>> requests(chunks), results(chunks);
    std::vector<int> ready(chunks, 0);
    std::mutex mu; std::condition_variable cv;
    uint64_t nextChunk = 0;
    t0 = Clock::now();
    for (uint64_t c = 0; c < chunks; ++c) store.buildFeedRequest(firstBlock + c * spanBlocks, chunkBlocks, span, requests[c]);
    const double planSeconds = sec(t0);
    std::vector<std::thread> threads;
    for (int h = 0; h < helpers; ++h) {
        threads.emplace_back([&, h] {
            for (;;) {
                uint64_t c;
                { std::lock_guard<std::mutex> lock(mu); if (nextChunk >= chunks) return; c = nextChunk++; }
                std::vector<uint8_t> out;
                if (!workers[h]->decodeFeedRequest(requests[c].data(), requests[c].size(), out)) fprintf(stderr, "decode failed: %s\n", workers[h]->error());
                { std::lock_guard<std::mutex> lock(mu); results[c].swap(out); ready[c] = 1; }
                cv.notify_all();
            }
        });
    }
    std::vector<EW> got; std::vector<double> gotTimes;
    done = false;
    for (uint64_t c = 0; c < chunks && !done; ++c) {
        { std::unique_lock<std::mutex> lock(mu); cv.wait(lock, [&] { return ready[c] != 0; }); }
        if (!store.stitchFeedResult(results[c].data(), results[c].size(), batch, times, sx, sxb, safe, done)) { fprintf(stderr, "stitch failed: %s\n", store.error()); return 1; }
        got.insert(got.end(), batch.begin(), batch.end());
        gotTimes.insert(gotTimes.end(), times.begin(), times.end());
        std::vector<uint8_t>().swap(results[c]);
    }
    const double parSeconds = sec(t0);
    for (auto& t : threads) t.join();
    // The sequential run may stop one window later (it emits whole windows up to `end`).
    std::size_t n = std::min(ref.size(), got.size()), diff = 0;
    while (diff < n && ref[diff].tick == got[diff].tick && ref[diff].packed == got[diff].packed && refTimes[diff] == gotTimes[diff]) ++diff;
    const bool same = diff == n && (ref.size() == got.size() || safe != refSafe);
    printf("parallel %d helpers, chunk %u blocks x %u: %zu events in %.3f s (plan %.3f s) = %.2fx realtime -> %s",
        helpers, chunkBlocks, span, got.size(), parSeconds, planSeconds, parSeconds / duration, same ? "IDENTICAL" : "DIFFER");
    if (!same) printf(" (first difference at %zu of %zu/%zu)", diff, ref.size(), got.size());
    printf("\n");
    return same ? 0 : 1;
}
