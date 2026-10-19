// Visual stream check (HANDOFF sec. 71): the renderer sweep and the live snapshot read from
// the folded visual stream must give the same output as the merge-heap paths. Also times
// both per song second, and checks that helper-decoded spans equal local ones.
//   ./visual_check FILE.mid START DURATION [LOOKAHEAD_SECONDS=2] [BUDGET=131072]
#include "bpfa_midi_store.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <vector>
using namespace wasmidi;
using Clock = std::chrono::steady_clock;
using LiveSnapshot = BpfaMidiStore::LiveSnapshot;
using RenderClose = BpfaMidiStore::RenderClose;
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

struct Run {
    std::vector<VisualNote> appends;
    std::vector<std::tuple<uint32_t, uint32_t>> closes;
    std::vector<LiveSnapshot> live;
    std::vector<std::size_t> frameLive;  // index in live of each frame's snapshot
    double sweepSeconds = 0, liveSeconds = 0, resetSeconds = 0;
    double helperDecodeSeconds = 0;
};

static bool sameLive(const LiveSnapshot& a, const LiveSnapshot& b) {
    return a.activeVoices == b.activeVoices && a.nps == b.nps && a.ccPerSecond == b.ccPerSecond &&
        a.keys.counts == b.keys.counts && a.keys.globalColors == b.keys.globalColors &&
        a.keys.trackColors == b.keys.trackColors;
}

// Plays [start, start + duration): every frame (1/60 s) the sweep is extended to
// now + lookahead and the live snapshot is taken at now (the app's pattern).
static Run play(BpfaMidiStore& store, bool stream, double start, double duration, double lookahead, std::size_t budget,
                BpfaMidiStore* helper) {
    Run run;
    store.setVisualStreamEnabled(stream);
    store.setVisualAutoDecode(helper == nullptr);
    const MidiDocument& md = store.metadata();
    const uint32_t startTick = uint32_t(md.secondsToTick(start));
    store.resetRenderCursor(startTick, false);
    std::vector<VisualNote> app; std::vector<RenderClose> cl; std::vector<uint8_t> request, result;
    auto live = [&](double tick, double nps, double cc, LiveSnapshot& snap, bool reset) {
        return store.buildLiveSnapshot(tick, nps, cc, snap, reset);
    };
    bool first = true;
    for (double t = start; t < start + duration; t += 1.0 / 60.0) {
        const uint32_t target = uint32_t(std::min<double>(md.maxTick, md.secondsToTick(t + lookahead)));
        auto t0 = Clock::now();
        for (;;) {
            if (helper && stream && !store.visualStreamDone() && store.visualStreamHead() <= target) {
                const uint32_t a = store.visualStreamHead(), b = store.visualSpanEnd(a, budget * 4);
                const auto h0 = Clock::now();
                const bool built = store.buildVisualRequest(a, b, request);
                const bool decoded = built && helper->decodeVisualRequest(request.data(), request.size(), result);
                run.helperDecodeSeconds += sec(h0);
                if (!decoded || !store.appendVisualResult(result.data(), result.size())) {
                    fprintf(stderr, "helper span failed: %s %s\n", store.error(), helper->error());
                    exit(1);
                }
            }
            uint32_t base = 0, next = 0; bool complete = false, hasNext = false;
            if (!store.buildRenderSweep(target, budget, app, cl, base, complete, next, hasNext)) { fprintf(stderr, "sweep failed\n"); exit(1); }
            if (base != run.appends.size() + 1) { fprintf(stderr, "append base %u vs %zu\n", base, run.appends.size() + 1); exit(1); }
            run.appends.insert(run.appends.end(), app.begin(), app.end());
            for (const RenderClose& c : cl) run.closes.emplace_back(c.noteId, c.endTick);
            if (complete) break;
        }
        run.sweepSeconds += sec(t0);
        const double tick = md.secondsToTick(t);
        LiveSnapshot snap;
        t0 = Clock::now();
        if (!live(tick, md.secondsToTick(std::max(0.0, t - 0.25)),
                  md.secondsToTick(std::max(0.0, t - 1.0)), snap, first)) { fprintf(stderr, "live failed: %s\n", store.error()); exit(1); }
        (first ? run.resetSeconds : run.liveSeconds) += sec(t0);
        first = false;
        run.frameLive.push_back(run.live.size());
        run.live.push_back(snap);
        // An exact tick now and then (deferred note-offs).
        if (run.live.size() % 7 == 0) {
            live(std::ceil(tick), md.secondsToTick(std::max(0.0, t - 0.25)),
                 md.secondsToTick(std::max(0.0, t - 1.0)), snap, false);
            run.live.push_back(snap);
        }
    }
    std::sort(run.closes.begin(), run.closes.end());
    return run;
}

// A roll catch-up (HANDOFF sec. 81): a third of the way in, the sweep restarts `jump`
// seconds ahead of the playhead, as the renderer does when it falls behind. The keyboard
// holds its last keys meanwhile; once the stream covers its density window it must match
// the merge reference again. Returns the live snapshots and the longest request.
static Run playJump(BpfaMidiStore& store, double start, double duration, double lookahead, std::size_t budget,
                    double jump, double& longest) {
    Run run;
    store.setVisualStreamEnabled(true);
    store.setVisualAutoDecode(true);
    const MidiDocument& md = store.metadata();
    store.resetRenderCursor(uint32_t(md.secondsToTick(start)), false);
    std::vector<VisualNote> app; std::vector<RenderClose> cl;
    bool first = true, jumped = false;
    double sweepFrom = start;
    longest = 0.0;
    for (double t = start; t < start + duration; t += 1.0 / 60.0) {
        if (!jumped && t >= start + duration / 3.0) {
            jumped = true;
            sweepFrom = t + jump;
            store.resetRenderCursor(uint32_t(md.secondsToTick(sweepFrom)), false);
        }
        const uint32_t target = uint32_t(std::min<double>(md.maxTick, md.secondsToTick(std::max(t, sweepFrom) + lookahead)));
        for (;;) {
            uint32_t base = 0, next = 0; bool complete = false, hasNext = false;
            if (!store.buildRenderSweep(target, budget, app, cl, base, complete, next, hasNext)) { fprintf(stderr, "sweep failed\n"); exit(1); }
            if (complete) break;
        }
        const double tick = md.secondsToTick(t);
        LiveSnapshot snap;
        const auto t0 = Clock::now();
        if (!store.buildLiveSnapshot(tick, md.secondsToTick(std::max(0.0, t - 0.25)),
                                     md.secondsToTick(std::max(0.0, t - 1.0)), snap, first)) { fprintf(stderr, "live failed\n"); exit(1); }
        longest = std::max(longest, sec(t0));
        first = false;
        run.live.push_back(snap);
    }
    return run;
}

// The keyboard Worker (HANDOFF sec. 83): helper spans are appended to the coordinator and to a
// keyboard store made from exportLiveImage; the live snapshots run there and the coordinator
// only rebuilds the keys on a reset. With jump > 0 the sweep restarts as in playJump. With a plan
// (HANDOFF sec. 88) the helper spans run ahead of the roll up to that many words.
static Run playWorker(BpfaMidiStore& store, BpfaMidiStore& helper, double start, double duration, double lookahead,
                      std::size_t budget, double jump, double& longest, std::size_t& rebuilds, std::size_t plan = 0) {
    Run run;
    store.setVisualPlan(plan ? plan : std::size_t(32) << 20);
    store.setVisualStreamEnabled(true);
    store.setVisualAutoDecode(false);
    std::vector<uint8_t> image, request, result, rebuild;
    static BpfaMidiStore keys;
    if (!store.exportLiveImage(image) || !keys.importLiveImage(image.data(), image.size())) {
        fprintf(stderr, "keyboard image failed: %s %s\n", store.error(), keys.error());
        exit(1);
    }
    keys.setVisualPlan(plan ? plan : std::size_t(32) << 20);
    store.setLiveExternal(true);
    const MidiDocument& md = store.metadata();
    store.resetRenderCursor(uint32_t(md.secondsToTick(start)), false);
    keys.restartVisualStream(uint32_t(md.secondsToTick(start)));
    std::vector<VisualNote> app; std::vector<RenderClose> cl;
    bool first = true, jumped = jump <= 0.0;
    double sweepFrom = start;
    longest = 0.0;
    rebuilds = 0;
    auto live = [&](double tick, double nps, double cc, LiveSnapshot& snap, bool reset) {
        const auto t0 = Clock::now();
        BpfaMidiStore::LiveStep step = keys.liveWorkerStep(tick, nps, cc, reset, snap);
        if (step == BpfaMidiStore::LiveStep::Rebuild || step == BpfaMidiStore::LiveStep::RebuildDensity) {
            ++rebuilds;
            if (!store.exportLiveRebuild(tick, nps, cc, step == BpfaMidiStore::LiveStep::RebuildDensity, rebuild) ||
                !keys.liveWorkerResume(rebuild.data(), rebuild.size(), tick, nps, cc, snap)) {
                fprintf(stderr, "keyboard rebuild failed: %s %s\n", store.error(), keys.error());
                exit(1);
            }
        } else if (step != BpfaMidiStore::LiveStep::Done) {
            fprintf(stderr, "keyboard step failed: %s\n", keys.error());
            exit(1);
        }
        store.setLiveExternalPosition(keys.liveStreamPosition());
        return sec(t0);
    };
    for (double t = start; t < start + duration; t += 1.0 / 60.0) {
        if (!jumped && t >= start + duration / 3.0) {
            jumped = true;
            sweepFrom = t + jump;
            store.resetRenderCursor(uint32_t(md.secondsToTick(sweepFrom)), false);
            keys.restartVisualStream(uint32_t(md.secondsToTick(sweepFrom)));
        }
        const uint32_t target = uint32_t(std::min<double>(md.maxTick, md.secondsToTick(std::max(t, sweepFrom) + lookahead)));
        for (;;) {
            for (int spans = 0; spans < (plan ? 4 : 1); ++spans) {
                if (store.visualStreamDone() || (!plan && store.visualStreamHead() > target) || !store.visualRoomAhead()) break;
                const uint32_t a = store.visualStreamHead(), b = store.visualSpanEnd(a, budget * 4);
                if (!store.buildVisualRequest(a, b, request) ||
                    !helper.decodeVisualRequest(request.data(), request.size(), result) ||
                    !store.appendVisualResult(result.data(), result.size()) ||
                    !keys.appendVisualResult(result.data(), result.size())) {
                    fprintf(stderr, "helper span failed: %s %s %s\n", store.error(), helper.error(), keys.error());
                    exit(1);
                }
            }
            uint32_t base = 0, next = 0; bool complete = false, hasNext = false;
            if (!store.buildRenderSweep(target, budget, app, cl, base, complete, next, hasNext)) { fprintf(stderr, "sweep failed\n"); exit(1); }
            if (jump <= 0.0) {
                if (base != run.appends.size() + 1) { fprintf(stderr, "append base %u vs %zu\n", base, run.appends.size() + 1); exit(1); }
                run.appends.insert(run.appends.end(), app.begin(), app.end());
                for (const RenderClose& c : cl) run.closes.emplace_back(c.noteId, c.endTick);
            }
            if (complete || (!store.visualStreamDone() && store.visualStreamHead() <= next && !store.visualRoomAhead())) break;
        }
        const double tick = md.secondsToTick(t);
        LiveSnapshot snap;
        const double took = live(tick, md.secondsToTick(std::max(0.0, t - 0.25)), md.secondsToTick(std::max(0.0, t - 1.0)), snap, first);
        longest = std::max(longest, took);
        (first ? run.resetSeconds : run.liveSeconds) += took;
        first = false;
        run.frameLive.push_back(run.live.size());
        run.live.push_back(snap);
        if (jump <= 0.0 && run.live.size() % 7 == 0) {
            live(std::ceil(tick), md.secondsToTick(std::max(0.0, t - 0.25)), md.secondsToTick(std::max(0.0, t - 1.0)), snap, false);
            run.live.push_back(snap);
        }
    }
    store.setLiveExternal(false);
    store.setVisualPlan(std::size_t(32) << 20);
    std::sort(run.closes.begin(), run.closes.end());
    return run;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: visual_check FILE START DURATION [LOOKAHEAD] [BUDGET]\n"); return 2; }
    const double start = atof(argv[2]), duration = atof(argv[3]);
    const double lookahead = argc > 4 ? atof(argv[4]) : 2.0;
    const std::size_t budget = argc > 5 ? std::size_t(atol(argv[5])) : 131072u;
    g_fd = open(argv[1], O_RDONLY);
    struct stat st; if (g_fd < 0 || fstat(g_fd, &st)) { perror("open"); return 1; }
    static BpfaMidiStore store; MidiDocument meta; store.setRawSourceLimit(0);
    g_main = &store;
    if (!store.index(uint64_t(st.st_size), readAt, nullptr, meta, nullptr, nullptr)) { fprintf(stderr, "index failed: %s\n", store.error()); return 1; }
    printf("%u tracks, %.1f s song\n", store.metadata().trackCount, store.metadata().durationSeconds);
    Run merge = play(store, false, start, duration, lookahead, budget, nullptr);
    Run local = play(store, true, start, duration, lookahead, budget, nullptr);
    std::vector<uint8_t> image;
    BpfaMidiStore helper;
    const bool haveHelper = store.exportFeedImage(image) && helper.importFeedImage(image.data(), image.size(), fetchSlab, nullptr);
    Run spans = haveHelper ? play(store, true, start, duration, lookahead, budget, &helper) : Run{};
    int failures = 0;
    auto compare = [&](const char* name, const Run& r) {
        bool appends = r.appends.size() == merge.appends.size() &&
            std::equal(r.appends.begin(), r.appends.end(), merge.appends.begin(), [](const VisualNote& a, const VisualNote& b) {
                return a.startTick == b.startTick && a.endTick == b.endTick && a.packedData == b.packedData; });
        bool closes = r.closes == merge.closes;
        std::size_t liveDiff = 0;
        while (liveDiff < std::min(r.live.size(), merge.live.size()) && sameLive(r.live[liveDiff], merge.live[liveDiff])) ++liveDiff;
        bool live = liveDiff == merge.live.size() && r.live.size() == merge.live.size();
        printf("%-7s appends %zu %s, closes %zu %s, live %zu %s | sweep %.3f s, live %.3f s per song second, live reset %.3f s\n", name,
               r.appends.size(), appends ? "IDENTICAL" : "DIFFER", r.closes.size(), closes ? "IDENTICAL" : "DIFFER",
               r.live.size(), live ? "IDENTICAL" : "DIFFER", r.sweepSeconds / duration, r.liveSeconds / duration, r.resetSeconds);
        if (!live && liveDiff < r.live.size() && liveDiff < merge.live.size())
            printf("        first live difference at %zu: voices %u/%u nps %u/%u cc %u/%u\n", liveDiff,
                   r.live[liveDiff].activeVoices, merge.live[liveDiff].activeVoices, r.live[liveDiff].nps,
                   merge.live[liveDiff].nps, r.live[liveDiff].ccPerSecond, merge.live[liveDiff].ccPerSecond);
        if (r.helperDecodeSeconds > 0)
            printf("        of the sweep, %.3f s per song second is the helpers' decode\n", r.helperDecodeSeconds / duration);
        failures += !(appends && closes && live);
    };
    printf("merge   appends %zu, closes %zu, live %zu | sweep %.3f s, live %.3f s per song second, live reset %.3f s\n", merge.appends.size(),
           merge.closes.size(), merge.live.size(), merge.sweepSeconds / duration, merge.liveSeconds / duration, merge.resetSeconds);
    compare("stream", local);
    if (haveHelper) compare("helper", spans);
    else printf("helper  skipped (no compressed source)\n");
    if (haveHelper) {
        double longest = 0.0;
        std::size_t rebuilds = 0;
        Run worker = playWorker(store, helper, start, duration, lookahead, budget, 0.0, longest, rebuilds);
        std::size_t liveDiff = 0;
        while (liveDiff < std::min(worker.live.size(), merge.live.size()) && sameLive(worker.live[liveDiff], merge.live[liveDiff])) ++liveDiff;
        const bool same = liveDiff == merge.live.size() && worker.live.size() == merge.live.size();
        printf("keys    live in the keyboard Worker %zu %s, %zu rebuilds | live %.3f s per song second, reset %.3f s\n",
               worker.live.size(), same ? "IDENTICAL" : "DIFFER", rebuilds, worker.liveSeconds / duration, worker.resetSeconds);
        if (!same && liveDiff < worker.live.size() && liveDiff < merge.live.size())
            printf("        first live difference at %zu: voices %u/%u nps %u/%u cc %u/%u\n", liveDiff,
                   worker.live[liveDiff].activeVoices, merge.live[liveDiff].activeVoices, worker.live[liveDiff].nps,
                   merge.live[liveDiff].nps, worker.live[liveDiff].ccPerSecond, merge.live[liveDiff].ccPerSecond);
        failures += !same;
        const double jump = 1.0;
        Run jumped = playWorker(store, helper, start, duration, lookahead, budget, jump, longest, rebuilds);
        const double resume = start + duration / 3.0 + jump + 1.0 + 1.0 / 30.0;
        std::size_t compared = 0, differ = 0, frame = 0;
        for (double t = start; frame < jumped.live.size() && frame < merge.frameLive.size(); t += 1.0 / 60.0, ++frame) {
            if (t < resume) continue;
            ++compared;
            differ += !sameLive(jumped.live[frame], merge.live[merge.frameLive[frame]]);
        }
        printf("keyjump live in the keyboard Worker after a %.1f s roll jump: %zu compared, %s, %zu rebuilds | longest request %.3f s\n",
               jump, compared, differ ? "DIFFER" : "IDENTICAL", rebuilds, longest);
        failures += differ != 0 || compared == 0;
        Run ahead = playWorker(store, helper, start, duration, lookahead, budget, 0.0, longest, rebuilds, std::size_t(256) << 20);
        compare("ahead", ahead);
    }
    {
        const double jump = 1.0;
        double longest = 0.0;
        Run jumped = playJump(store, start, duration, lookahead, budget, jump, longest);
        // Frames from the jump until the stream covers the keyboard's density window may hold.
        const double resume = start + duration / 3.0 + jump + 1.0 + 1.0 / 30.0;
        std::size_t compared = 0, differ = 0, frame = 0;
        for (double t = start; frame < jumped.live.size() && frame < merge.frameLive.size(); t += 1.0 / 60.0, ++frame) {
            if (t < resume) continue;
            ++compared;
            differ += !sameLive(jumped.live[frame], merge.live[merge.frameLive[frame]]);
        }
        printf("jump    live after a %.1f s roll jump: %zu compared, %s | longest live request %.3f s\n", jump, compared,
               differ ? "DIFFER" : "IDENTICAL", longest);
        failures += differ != 0 || compared == 0;
    }
    return failures ? 1 : 0;
}
