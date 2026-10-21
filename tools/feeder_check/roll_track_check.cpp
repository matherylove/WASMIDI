// Roll summaries in colors per track (HANDOFF sec. 113): on a dense generated MIDI, the sweep that
// applies the summaries (computed with each span, or again for a new fold through the helper
// request) must send the same notes as the sweep that reads every word.
//   roll_track_check            prints one IDENTICAL/DIFFER line per case
#include "bpfa_midi_store.hpp"
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace wasmidi;

static std::vector<uint8_t> g_midi;
static bool readAt(void*, uint64_t off, uint8_t* dst, std::size_t n)
{
    if (off + n > g_midi.size()) return false;
    std::memcpy(dst, g_midi.data() + off, n);
    return true;
}

static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s)); }
static void putVlq(std::vector<uint8_t>& v, uint32_t x)
{
    uint8_t b[5]; int n = 0;
    do { b[n++] = uint8_t(x & 127); x >>= 7; } while (x);
    while (n--) v.push_back(uint8_t(b[n] | (n ? 128 : 0)));
}

// 48 tracks on 3 channels and 12 keys: owners take keys from each other, copies stack, offs come
// from tracks that hold nothing.
static void makeMidi()
{
    std::mt19937 rng(11);
    const int tracks = 48, events = 125000;
    g_midi = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, uint8_t(tracks + 1), 0x03, 0xc0};
    std::vector<uint8_t> t0 = {0, 0xff, 0x51, 3, 0x07, 0xa1, 0x20, 0, 0xff, 0x2f, 0};
    g_midi.insert(g_midi.end(), {'M', 'T', 'r', 'k'});
    put32(g_midi, uint32_t(t0.size()));
    g_midi.insert(g_midi.end(), t0.begin(), t0.end());
    for (int t = 0; t < tracks; ++t) {
        std::vector<uint8_t> body;
        for (int i = 0; i < events; ++i) {
            putVlq(body, rng() % 4);
            const uint8_t ch = uint8_t(rng() % 3), key = uint8_t(60 + rng() % 12), r = uint8_t(rng() % 10);
            if (r < 5) body.insert(body.end(), {uint8_t(0x90 | ch), key, uint8_t(1 + rng() % 127)});
            else if (r < 9) body.insert(body.end(), {uint8_t(0x80 | ch), key, 64});
            else body.insert(body.end(), {uint8_t(0x90 | ch), key, 0});
        }
        body.insert(body.end(), {0, 0xff, 0x2f, 0});
        g_midi.insert(g_midi.end(), {'M', 'T', 'r', 'k'});
        put32(g_midi, uint32_t(body.size()));
        g_midi.insert(g_midi.end(), body.begin(), body.end());
    }
}

struct Notes { std::vector<VisualNote> appends; std::vector<MidiMappedStore::RenderClose> closes; };

static bool sweep(BpfaMidiStore& store, bool perTrack, Notes& out)
{
    out = Notes{};
    store.resetRenderCursor(0, perTrack);
    const uint32_t maxTick = store.metadata().maxTick;
    std::vector<VisualNote> appends;
    std::vector<MidiMappedStore::RenderClose> closes;
    for (int frame = 1; frame <= 40; ++frame) {
        const uint32_t endTick = uint32_t(uint64_t(maxTick) * frame / 40);
        for (bool complete = false; !complete;) {
            uint32_t base = 0, next = 0;
            bool hasNext = false;
            if (!store.buildRenderSweep(endTick, 1500000, appends, closes, base, complete, next, hasNext)) return false;
            out.appends.insert(out.appends.end(), appends.begin(), appends.end());
            out.closes.insert(out.closes.end(), closes.begin(), closes.end());
        }
    }
    return true;
}

static bool open(BpfaMidiStore& store)
{
    MidiDocument meta;
    if (!store.index(g_midi.size(), readAt, nullptr, meta)) return false;
    store.setVisualStreamEnabled(true);
    store.setVisualAutoDecode(true);
    store.setVisualKeepAll(true);
    store.setVisualPlan(std::size_t(64) << 20);
    return true;
}

static bool same(const Notes& a, const Notes& b)
{
    if (a.appends.size() != b.appends.size() || a.closes.size() != b.closes.size()) return false;
    for (std::size_t i = 0; i < a.appends.size(); ++i)
        if (a.appends[i].startTick != b.appends[i].startTick || a.appends[i].packedData != b.appends[i].packedData) return false;
    for (std::size_t i = 0; i < a.closes.size(); ++i)
        if (a.closes[i].noteId != b.closes[i].noteId || a.closes[i].endTick != b.closes[i].endTick) return false;
    return true;
}

int main()
{
    makeMidi();
    bool ok = true;
    for (const uint32_t window : {20000u, 2000u}) {
        const uint32_t pixels = 64;
        Notes raw, direct, again;
        BpfaMidiStore a, b, c, helper;
        if (!open(a) || !open(b) || !open(c) || !open(helper)) { std::puts("roll track check: index failed"); return 1; }
        a.setRollSummaryMode(0);
        a.setRenderFold(window, pixels);
        b.setRollSummaryMode(2);
        b.setRenderFold(window, pixels);
        // c: spans with channel summaries first, then the summaries for this fold from the helper.
        Notes channel;
        c.setRollSummaryMode(1);
        bool run = sweep(a, true, raw) && sweep(b, true, direct) && sweep(c, false, channel);
        c.setRollSummaryMode(2);
        c.setRenderFold(window, pixels);
        c.resetRenderCursor(0, true);
        std::vector<uint8_t> request, result;
        int pieces = 0;
        while (run && c.buildRollRequest(request)) {
            run = helper.decodeRollRequest(request.data(), request.size(), result) && c.attachRollResult(result.data(), result.size());
            ++pieces;
        }
        run = run && pieces > 1 && sweep(c, true, again);
        const bool good = run && !raw.appends.empty() && same(raw, direct) && same(raw, again);
        std::printf("roll track summary fold %u:%u notes %zu, %d pieces summarized again %s\n", window, pixels,
                    raw.appends.size(), pieces, good ? "IDENTICAL" : "DIFFER");
        ok = ok && good;
    }
    return ok ? 0 : 1;
}
