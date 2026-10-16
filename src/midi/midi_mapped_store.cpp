#include "midi_mapped_store.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <deque>
#include <memory>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wasmidi {
namespace {

// SharpMIDI maps the source file and parses directly from the mapping. Browser
// File/Blob cannot be memory-mapped into WASM, so this is the equivalent: a
// small shared LRU of fixed source windows. File size never determines heap
// size. Browser Blob/FileReaderSync crossings are far more expensive than
// native mmap page faults, so use eight 8 MiB windows. This keeps a bounded
// 64 MiB source cache while halving JS<->WASM crossings compared with the
// previous 4 MiB pages.
constexpr std::size_t SourcePageBytes = 8u << 20;
constexpr std::size_t SourcePageCount = 8;
// Source checkpoints bound arbitrary-seek replay by both source distance and
// event count. Even two billion channel events produce only ~30k lightweight
// checkpoints; active-note snapshots have a separate hard memory budget below.
constexpr uint64_t TrackCheckpointEventStride = 1u << 16; // 65,536 events
constexpr uint64_t TrackCheckpointByteStride = 4u << 20;  // or 4 MiB source
constexpr std::size_t VisualCheckpointCount = 128;
constexpr std::size_t VisualStateCount = 16u * 128u;
// Bound the total seek-snapshot payload. Exact sparse snapshots make arbitrary
// seeks fast, but a pathological billion-note sustained chord must not be
// duplicated at every source checkpoint. When this budget is exhausted we
// simply fall back to an earlier exact snapshot and scan forward.
constexpr uint64_t MaxSeekSnapshotNotes = 1u << 20;

class MappedReader {
public:
    void reset(uint64_t size, MidiReadAt readAt, void* user)
    {
        size_ = size;
        readAt_ = readAt;
        user_ = user;
        failed_ = false;
        stamp_ = 1;
        lastPage_ = nullptr;
        for (auto& page : pages_) {
            page.start = 0;
            page.length = 0;
            page.stamp = 0;
            page.valid = false;
            page.bytes.clear();
        }
    }

    uint64_t size() const { return size_; }
    bool failed() const { return failed_; }

    bool read(uint64_t offset, void* destination, std::size_t bytes)
    {
        if (bytes == 0)
            return true;
        if (!destination || !readAt_ || offset > size_ ||
            uint64_t(bytes) > size_ - offset) {
            failed_ = true;
            return false;
        }

        auto* out = static_cast<uint8_t*>(destination);
        while (bytes != 0) {
            const uint64_t pageStart = offset & ~uint64_t(SourcePageBytes - 1u);
            Page* page = find(pageStart);
            if (!page)
                page = load(pageStart);
            if (!page)
                return false;

            const std::size_t within = static_cast<std::size_t>(offset - pageStart);
            if (within >= page->length) {
                failed_ = true;
                return false;
            }
            const std::size_t chunk = std::min(bytes, page->length - within);
            std::memcpy(out, page->bytes.data() + within, chunk);
            out += chunk;
            offset += chunk;
            bytes -= chunk;
        }
        return true;
    }

    bool byte(uint64_t offset, uint8_t& value)
    {
        if (!readAt_ || offset >= size_) {
            failed_ = true;
            return false;
        }

        const uint64_t pageStart = offset & ~uint64_t(SourcePageBytes - 1u);
        Page* page = lastPage_;
        if (!page || !page->valid || page->start != pageStart) {
            page = find(pageStart);
            if (!page)
                page = load(pageStart);
        }
        if (!page)
            return false;

        const std::size_t within = static_cast<std::size_t>(offset - pageStart);
        if (within >= page->length) {
            failed_ = true;
            return false;
        }

        value = page->bytes[within];
        page->stamp = ++stamp_;
        lastPage_ = page;
        return true;
    }

private:
    struct Page {
        uint64_t start = 0;
        std::size_t length = 0;
        uint64_t stamp = 0;
        bool valid = false;
        std::vector<uint8_t> bytes;
    };

    Page* find(uint64_t start)
    {
        if (lastPage_ && lastPage_->valid && lastPage_->start == start) {
            lastPage_->stamp = ++stamp_;
            return lastPage_;
        }
        for (auto& page : pages_) {
            if (page.valid && page.start == start) {
                page.stamp = ++stamp_;
                lastPage_ = &page;
                return &page;
            }
        }
        return nullptr;
    }

    Page* load(uint64_t start)
    {
        Page* victim = &pages_.front();
        for (auto& page : pages_) {
            if (!page.valid) {
                victim = &page;
                break;
            }
            if (page.stamp < victim->stamp)
                victim = &page;
        }

        const uint64_t remaining = size_ - start;
        const std::size_t length = static_cast<std::size_t>(
            std::min<uint64_t>(SourcePageBytes, remaining));
        if (victim->bytes.size() != SourcePageBytes)
            victim->bytes.resize(SourcePageBytes);
        if (length != 0 && !readAt_(user_, start, victim->bytes.data(), length)) {
            failed_ = true;
            victim->valid = false;
            return nullptr;
        }
        victim->start = start;
        victim->length = length;
        victim->stamp = ++stamp_;
        victim->valid = true;
        lastPage_ = victim;
        return victim;
    }

    uint64_t size_ = 0;
    MidiReadAt readAt_ = nullptr;
    void* user_ = nullptr;
    bool failed_ = false;
    uint64_t stamp_ = 1;
    Page* lastPage_ = nullptr;
    std::array<Page, SourcePageCount> pages_{};
};

class Cursor {
public:
    Cursor(MappedReader& reader, uint64_t begin, uint64_t end)
        : reader_(&reader), position_(begin), end_(end)
    {
    }

    uint64_t position() const { return position_; }
    uint64_t remaining() const { return position_ <= end_ ? end_ - position_ : 0; }

    bool readByte(uint8_t& value)
    {
        if (!reader_ || position_ >= end_ || !reader_->byte(position_, value))
            return false;
        ++position_;
        return true;
    }

    bool read(void* destination, std::size_t bytes)
    {
        if (!reader_ || uint64_t(bytes) > remaining() ||
            !reader_->read(position_, destination, bytes))
            return false;
        position_ += bytes;
        return true;
    }

    bool skip(uint64_t bytes)
    {
        if (bytes > remaining())
            return false;
        position_ += bytes;
        return true;
    }

private:
    MappedReader* reader_ = nullptr;
    uint64_t position_ = 0;
    uint64_t end_ = 0;
};

uint16_t read16(Cursor& c, bool& ok)
{
    uint8_t b[2]{};
    if (!ok || !c.read(b, 2)) {
        ok = false;
        return 0;
    }
    return uint16_t((uint16_t(b[0]) << 8) | uint16_t(b[1]));
}

uint32_t read32(Cursor& c, bool& ok)
{
    uint8_t b[4]{};
    if (!ok || !c.read(b, 4)) {
        ok = false;
        return 0;
    }
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) |
           (uint32_t(b[2]) << 8) | uint32_t(b[3]);
}

uint32_t readVar(Cursor& c, bool& ok)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t byte = 0;
        if (!ok || !c.readByte(byte)) {
            ok = false;
            return 0;
        }
        value = (value << 7) | uint32_t(byte & 0x7f);
        if ((byte & 0x80) == 0)
            return value;
    }
    ok = false;
    return 0;
}

// Running-status aware status read with the first data byte returned separately.
bool readStatusAndFirstData(
    Cursor& c,
    uint8_t& running,
    uint8_t& status,
    bool& hasFirstData,
    uint8_t& firstData)
{
    uint8_t next = 0;
    if (!c.readByte(next))
        return false;
    if (next < 0x80) {
        if (!running)
            return false;
        status = running;
        hasFirstData = true;
        firstData = next;
        return true;
    }

    status = next;
    hasFirstData = false;
    firstData = 0;
    if (status < 0xf0)
        running = status;
    else if (status < 0xf8 || status == 0xff)
        running = 0;
    return true;
}

int systemBytes(uint8_t status)
{
    switch (status) {
    case 0xf1: return 1;
    case 0xf2: return 2;
    case 0xf3: return 1;
    default: return 0;
    }
}

struct TrackActiveSnapshot {
    uint16_t key = 0; // channel << 7 | pitch
    uint32_t startTick = 0;
    uint8_t velocity = 0;
    uint64_t openOrder = 0;
};

struct TrackCheckpoint {
    uint64_t sourceOffset = 0;
    uint64_t channelEventIndex = 0;
    uint32_t tickBefore = 0;
    uint32_t firstTick = 0;
    uint8_t runningBefore = 0;
    bool activeSnapshotValid = false;
    // Exact active NoteOn state immediately before sourceOffset. This makes an
    // arbitrary visual seek O(one sparse source interval) instead of rescanning
    // from tick 0. Memory scales with checkpoint polyphony, not total notes.
    std::vector<TrackActiveSnapshot> activeNotes;
};

struct TrackIndex {
    uint64_t offset = 0;
    uint32_t length = 0;
    uint64_t channelEvents = 0;
    uint64_t renderEvents = 0;
    uint64_t synthStateEvents = 0;
    uint32_t maxTick = 0;
    uint16_t firstNoteSeenMask = 0;
    std::array<uint32_t, 16> firstNoteTick{};
    std::array<uint64_t, 16> firstNoteOrder{};
    std::array<uint64_t, 16> firstNoteCloseOrder{};
    std::vector<TrackCheckpoint> checkpoints;

    // SharpMIDI-style resident channel-event stream.  Loading performs one
    // cheap counting/index pass and one exact parse pass, then playback and
    // rendering never touch Blob/FileReader again.  This is the critical
    // distinction between SharpMIDI and the old on-demand WASMIDI path: dense
    // sections cannot suddenly become parser work at playback time.
    std::vector<MidiMappedStore::EventWord> hotEvents;
    // Renderer-only resident stream.  It contains only NoteOn/NoteOff events,
    // preserving per-track ordering while skipping every CC/program/pitch
    // event before realtime rendering begins.  This is built during the same
    // mandatory parse pass, not as an additional load-time analysis pass.
    std::vector<MidiMappedStore::EventWord> hotRenderEvents;
    std::vector<MidiMappedStore::EventWord> hotSynthStateEvents;
    // Source-event ordinal for each sparse selector event.  Tick alone is not
    // sufficient around same-tick GM/GS/XG resets: Program/Bank events before
    // a reset must not be resurrected by seek restoration, while events after
    // that reset at the same tick must be retained.
    std::vector<uint64_t> hotSynthStateOrders;
};

struct SysExRef {
    uint32_t tick = 0;
    uint32_t track = 0;
    uint64_t sourceOrder = 0;
    uint8_t status = 0xf0;
    std::vector<uint8_t> data;
};

struct ActiveVisualNote {
    uint32_t startTick = 0;
    uint8_t velocity = 0;
    uint8_t color = 0;
    uint64_t openOrder = 0;
};

using ActiveVisualQueue = std::deque<ActiveVisualNote>;

struct VisualState {
    // MPWGL2 pairs NoteOn/NoteOff per (track, channel, pitch) with FIFO
    // semantics. Keeping only one merged note per channel/pitch (Pass 13.0/13.1)
    // changed both overlap geometry and draw stacking, especially in per-track
    // mode. Store only currently-active queues: memory scales with polyphony,
    // not total MIDI note count.
    std::unordered_map<uint32_t, ActiveVisualQueue> pending;
};

struct VisualCheckpoint {
    uint32_t tick = 0;
    VisualState state;
};

struct VisualPageItem {
    VisualNote note{};
    uint32_t track = 0;
    uint64_t closeOrder = std::numeric_limits<uint64_t>::max();
    uint64_t openOrder = 0;
    bool minimumDuration = false;
};

void buildTempoIndex(MidiDocument& output)
{
    std::stable_sort(
        output.tempoMap.begin(), output.tempoMap.end(),
        [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });

    std::vector<TempoChange> deduped;
    deduped.reserve(output.tempoMap.size());
    for (const auto& tempo : output.tempoMap) {
        if (!deduped.empty() && deduped.back().tick == tempo.tick)
            deduped.back() = tempo;
        else
            deduped.push_back(tempo);
    }
    if (deduped.empty() || deduped.front().tick != 0)
        deduped.insert(deduped.begin(), {0, 500000});
    output.tempoMap.swap(deduped);
    output.tempoSeconds.resize(output.tempoMap.size());

    double seconds = 0.0;
    uint32_t previousTick = 0;
    uint32_t previousUs = 500000;
    const double ppq = std::max<uint16_t>(1, output.ticksPerBeat);
    for (std::size_t i = 0; i < output.tempoMap.size(); ++i) {
        const auto& tempo = output.tempoMap[i];
        seconds += double(tempo.tick - previousTick) / ppq *
                   (double(previousUs) / 1000000.0);
        output.tempoSeconds[i] = seconds;
        previousTick = tempo.tick;
        if (tempo.microsecondsPerBeat)
            previousUs = tempo.microsecondsPerBeat;
    }
}

} // namespace

struct MidiMappedStore::Impl {
    MappedReader reader;
    uint64_t sourceSize = 0;
    uint64_t seekSnapshotNotesStored = 0;
    std::vector<TrackIndex> tracks;
    std::vector<SysExRef> sysex;
    std::array<uint8_t, 16> globalColors{};
    std::vector<std::array<uint8_t, 16>> trackColors;
    std::vector<VisualCheckpoint> checkpoints;
    uint32_t visualCheckpointSpan = 1;

    // Sequential visual-page warming uses one rolling active-note state rather
    // than copying the full pending-note map into a checkpoint for every one of
    // the 64 future screens. Arbitrary seeks still use sparse checkpoints.
    VisualState rollingVisualState{};
    uint32_t rollingVisualTick = 0;
    bool rollingVisualValid = false;

    struct Decoder {
        Impl* owner = nullptr;
        uint32_t trackIndex = 0;
        std::unique_ptr<Cursor> cursor;
        uint32_t tick = 0;
        uint8_t running = 0;
        bool ok = true;
        bool currentValid = false;
        uint64_t channelEventIndex = 0;
        uint64_t currentOrder = 0;
        EventWord current{};

        bool readNextChannel(EventWord& out, uint64_t& order)
        {
            if (!cursor)
                return false;

            while (cursor->remaining() && ok) {
                const uint32_t delta = readVar(*cursor, ok);
                if (!ok || delta > std::numeric_limits<uint32_t>::max() - tick) {
                    ok = false;
                    return false;
                }
                tick += delta;

                uint8_t status = 0;
                uint8_t firstData = 0;
                bool hasFirstData = false;
                if (!readStatusAndFirstData(
                        *cursor, running, status, hasFirstData, firstData)) {
                    ok = false;
                    return false;
                }

                if (status == 0xff) {
                    uint8_t meta = 0;
                    if (hasFirstData) { ok = false; return false; }
                    if (!cursor->readByte(meta)) { ok = false; return false; }
                    const uint32_t len = readVar(*cursor, ok);
                    if (!ok || !cursor->skip(len)) { ok = false; return false; }
                    if (meta == 0x2f)
                        return false;
                    continue;
                }

                if (status == 0xf0 || status == 0xf7) {
                    if (hasFirstData) { ok = false; return false; }
                    const uint32_t len = readVar(*cursor, ok);
                    if (!ok || !cursor->skip(len)) { ok = false; return false; }
                    continue;
                }

                if (status >= 0xf0) {
                    if (hasFirstData) { ok = false; return false; }
                    if (!cursor->skip(static_cast<uint64_t>(systemBytes(status)))) {
                        ok = false;
                        return false;
                    }
                    continue;
                }

                const uint8_t command = status & 0xf0;
                const uint8_t channel = status & 0x0f;
                const int dataBytes = (command == 0xc0 || command == 0xd0) ? 1 : 2;
                uint8_t data1 = 0, data2 = 0;
                if (hasFirstData) {
                    data1 = firstData;
                } else if (!cursor->readByte(data1)) {
                    ok = false;
                    return false;
                }
                if (dataBytes == 2 && !cursor->readByte(data2)) {
                    ok = false;
                    return false;
                }

                if (command == 0x90 && data2 == 0) {
                    status = static_cast<uint8_t>(0x80 | channel);
                    data2 = 64;
                }

                const uint8_t color = static_cast<uint8_t>(
                    (owner->globalColors[channel] & 0x0f) |
                    ((owner->trackColors[trackIndex][channel] & 0x0f) << 4));
                out.tick = tick;
                out.packed = uint32_t(status) |
                             (uint32_t(data1) << 8) |
                             (uint32_t(data2) << 16) |
                             (uint32_t(color) << 24);
                order = channelEventIndex++;
                return true;
            }
            return false;
        }

        bool resetFromCheckpoint(
            Impl* o,
            uint32_t track,
            const TrackCheckpoint& checkpoint)
        {
            owner = o;
            trackIndex = track;
            ok = true;
            currentValid = false;

            const TrackIndex& ti = owner->tracks[track];
            cursor = std::make_unique<Cursor>(
                owner->reader,
                checkpoint.sourceOffset,
                ti.offset + uint64_t(ti.length));
            tick = checkpoint.tickBefore;
            running = checkpoint.runningBefore;
            channelEventIndex = checkpoint.channelEventIndex;
            return true;
        }

        bool reset(Impl* o, uint32_t track, uint32_t startTick)
        {
            owner = o;
            trackIndex = track;
            ok = true;
            currentValid = false;

            const TrackIndex& ti = owner->tracks[track];
            if (ti.checkpoints.empty())
                return true;

            // Choose the latest source checkpoint whose first channel event is
            // strictly before STARTTICK. Strictness is essential at a billion-
            // note crashpoint: seeking to tick T must not skip earlier events at
            // the same T merely because several sparse checkpoints share it.
            auto it = std::lower_bound(
                ti.checkpoints.begin(), ti.checkpoints.end(), startTick,
                [](const TrackCheckpoint& cp, uint32_t value) {
                    return cp.firstTick < value;
                });
            if (it == ti.checkpoints.begin()) {
                it = ti.checkpoints.begin();
            } else {
                --it;
            }

            resetFromCheckpoint(owner, track, *it);

            EventWord ev;
            uint64_t order = 0;
            while (readNextChannel(ev, order)) {
                if (ev.tick >= startTick) {
                    current = ev;
                    currentOrder = order;
                    currentValid = true;
                    return true;
                }
            }
            return ok;
        }

        bool advance()
        {
            EventWord ev;
            uint64_t order = 0;
            if (readNextChannel(ev, order)) {
                current = ev;
                currentOrder = order;
                currentValid = true;
                return true;
            }
            // End-of-track / end-of-cursor is a normal condition.  Only
            // propagate false when the decoder actually marked itself bad.
            // Iterator::next() uses this return value to distinguish EOF from
            // malformed MIDI data.
            currentValid = false;
            return ok;
        }
    };

    struct HeapNode {
        uint32_t tick = 0;
        uint32_t track = 0;
    };
    struct HeapGreater {
        bool operator()(const HeapNode& a, const HeapNode& b) const
        {
            if (a.tick != b.tick)
                return a.tick > b.tick;
            return a.track > b.track;
        }
    };

    struct Iterator {
        Impl* owner = nullptr;
        uint32_t endTick = std::numeric_limits<uint32_t>::max();
        std::vector<Decoder> decoders;
        std::priority_queue<HeapNode, std::vector<HeapNode>, HeapGreater> heap;
        bool ok = true;

        void reset(Impl* o, uint32_t start, uint32_t end)
        {
            owner = o;
            endTick = end;
            ok = true;
            heap = {};
            decoders.clear();
            decoders.resize(owner->tracks.size());

            for (uint32_t t = 0; t < owner->tracks.size(); ++t) {
                if (!decoders[t].reset(owner, t, start)) {
                    ok = false;
                    return;
                }
                if (decoders[t].currentValid && decoders[t].current.tick <= endTick)
                    heap.push({decoders[t].current.tick, t});
            }
        }

        bool peek(
            EventWord& out,
            uint32_t* trackOut = nullptr,
            uint64_t* orderOut = nullptr) const
        {
            if (heap.empty())
                return false;
            const auto n = heap.top();
            const Decoder& decoder = decoders[n.track];
            out = decoder.current;
            if (trackOut)
                *trackOut = n.track;
            if (orderOut)
                *orderOut = decoder.currentOrder;
            return true;
        }

        bool next(
            EventWord& out,
            uint32_t* trackOut = nullptr,
            uint64_t* orderOut = nullptr)
        {
            if (heap.empty())
                return false;
            const auto n = heap.top();
            heap.pop();
            Decoder& decoder = decoders[n.track];
            out = decoder.current;
            if (trackOut)
                *trackOut = n.track;
            if (orderOut)
                *orderOut = decoder.currentOrder;
            if (!decoder.advance()) {
                ok = false;
                return true; // deliver the current valid event, fail afterwards
            }
            if (decoder.currentValid && decoder.current.tick <= endTick)
                heap.push({decoder.current.tick, n.track});
            return true;
        }

        bool failed() const
        {
            if (!ok)
                return true;
            for (const auto& decoder : decoders) {
                if (!decoder.ok)
                    return true;
            }
            return false;
        }
    };

    // Runtime iterator over the resident event arrays built during loading.
    // Ordering matches the previous mapped-source iterator exactly: tick, then
    // track index, then source order within the track.  No file I/O or VLQ
    // decoding is permitted on this path.
    struct HotTrackCursor {
        std::size_t index = 0;
    };

    struct HotIterator {
        Impl* owner = nullptr;
        uint32_t endTick = std::numeric_limits<uint32_t>::max();
        std::vector<HotTrackCursor> cursors;
        std::priority_queue<HeapNode, std::vector<HeapNode>, HeapGreater> heap;
        bool ok = true;
        bool renderOnly = false;

        const std::vector<MidiMappedStore::EventWord>& eventsFor(uint32_t track) const
        {
            return renderOnly
                ? owner->tracks[track].hotRenderEvents
                : owner->tracks[track].hotEvents;
        }

        void reset(Impl* o, uint32_t start, uint32_t end, bool render = false)
        {
            owner = o;
            endTick = end;
            ok = true;
            renderOnly = render;
            heap = {};
            cursors.assign(owner->tracks.size(), {});

            for (uint32_t t = 0; t < owner->tracks.size(); ++t) {
                const auto& events = eventsFor(t);
                auto it = std::lower_bound(
                    events.begin(), events.end(), start,
                    [](const EventWord& event, uint32_t tick) {
                        return event.tick < tick;
                    });
                cursors[t].index = static_cast<std::size_t>(it - events.begin());
                if (it != events.end() && it->tick <= endTick)
                    heap.push({it->tick, t});
            }
        }

        bool peek(EventWord& out, uint32_t* trackOut = nullptr,
                  uint64_t* orderOut = nullptr) const
        {
            if (!owner || heap.empty())
                return false;
            const HeapNode n = heap.top();
            const auto& events = eventsFor(n.track);
            const std::size_t index = cursors[n.track].index;
            if (index >= events.size())
                return false;
            out = events[index];
            if (trackOut) *trackOut = n.track;
            if (orderOut) *orderOut = static_cast<uint64_t>(index);
            return true;
        }

        bool next(EventWord& out, uint32_t* trackOut = nullptr,
                  uint64_t* orderOut = nullptr)
        {
            if (!owner || heap.empty())
                return false;
            const HeapNode n = heap.top();
            heap.pop();
            auto& cursor = cursors[n.track];
            const auto& events = eventsFor(n.track);
            if (cursor.index >= events.size()) {
                ok = false;
                return false;
            }
            const std::size_t delivered = cursor.index;
            out = events[delivered];
            if (trackOut) *trackOut = n.track;
            if (orderOut) *orderOut = static_cast<uint64_t>(delivered);
            ++cursor.index;
            if (cursor.index < events.size() &&
                events[cursor.index].tick <= endTick) {
                heap.push({events[cursor.index].tick, n.track});
            }
            return true;
        }

        bool failed() const { return !ok; }
    };

    HotIterator eventCursor;
    bool eventCursorValid = false;
    std::size_t eventSysExCursor = 0;

    struct LiveDensityPoint {
        uint32_t tick = 0;
        uint32_t count = 0;
    };
    struct DeferredLiveOff {
        EventWord event{};
        uint32_t track = 0;
        uint64_t order = 0;
    };

    // One rolling state serves the piano keyboard and all live graph values.
    // The old implementation inserted a full VisualCheckpoint for every UI
    // frame; on a dense MIDI that copied the active-note queues repeatedly and
    // eventually made the keyboard appear stuck.  This state advances only by
    // the events that actually crossed since the previous frame.
    VisualState liveVisualState{};
    HotIterator liveCursor;
    bool liveCursorValid = false;
    double liveTick = -1.0;
    std::vector<DeferredLiveOff> liveDeferredOffs;
    std::deque<LiveDensityPoint> liveNpsDensity;
    std::deque<LiveDensityPoint> liveCcDensity;
    uint64_t liveNpsCount = 0;
    uint64_t liveCcCount = 0;

    // Independent SharpMIDI visual sweep cursor/state. The renderer in the Qt
    // wasm32 module receives only ring deltas; all expensive source walking
    // stays beside the Memory64 mapped File. This mirrors SharpMIDI's
    // GLNoteRenderer active-count/active-color/active-id tables.
    HotIterator renderCursor;
    bool renderCursorValid = false;
    bool renderPerTrackColors = false;
    uint32_t renderHead = 1;
    std::array<uint16_t, 128u * 16u> renderActiveCounts{};
    std::array<uint8_t, 128u * 16u> renderActiveColors{};
    std::array<uint32_t, 128u * 16u> renderActiveIds{};

    bool scanTrack(
        uint32_t trackIndex,
        MidiDocument& metadata,
        uint64_t& totalEvents,
        uint64_t& totalNotes,
        uint64_t& totalControls)
    {
        TrackIndex& track = tracks[trackIndex];
        track.checkpoints.clear();
        track.channelEvents = 0;
        track.renderEvents = 0;
        track.synthStateEvents = 0;
        track.maxTick = 0;
        track.firstNoteSeenMask = 0;
        track.firstNoteTick.fill(0);
        track.firstNoteOrder.fill(0);
        track.firstNoteCloseOrder.fill(std::numeric_limits<uint64_t>::max());
        track.checkpoints.reserve(
            static_cast<std::size_t>(track.length / TrackCheckpointByteStride) + 2u);
        // Initial checkpoint includes leading meta/SysEx and is always safe.
        track.checkpoints.push_back({track.offset, 0, 0, 0, 0, true, {}});

        Cursor cursor(reader, track.offset, track.offset + uint64_t(track.length));
        uint32_t tick = 0;
        uint8_t running = 0;
        bool ok = true;
        uint64_t channelEventIndex = 0;
        uint64_t noteClosureOrder = 0;

        // Keep the mandatory load pass close to SharpMIDI's cheap event scan.
        // We only need per-key overlap counts here to know whether a NoteOff
        // closes a real pending NoteOn. Full active-note snapshots used to copy
        // potentially millions of note records at sparse checkpoints and made
        // loading dense files much slower than MPWGL2/SharpMIDI. Random visual
        // seeks can rebuild from the initial exact checkpoint and then create
        // normal lazy VisualCheckpoints as pages are requested.
        std::array<uint32_t, 16u * 128u> activeCounts{};
        std::array<uint8_t, 16> firstNotePitch{};
        uint16_t firstNoteClosedMask = 0;

        uint64_t sourceOrder = 0;
        while (cursor.remaining() && ok) {
            const uint64_t eventStart = cursor.position();
            const uint32_t tickBefore = tick;
            const uint8_t runningBefore = running;
            const uint32_t delta = readVar(cursor, ok);
            if (!ok || delta > std::numeric_limits<uint32_t>::max() - tick)
                return false;
            tick += delta;

            uint8_t status = 0;
            uint8_t firstData = 0;
            bool hasFirstData = false;
            if (!readStatusAndFirstData(
                    cursor, running, status, hasFirstData, firstData))
                return false;

            if (status == 0xff) {
                if (hasFirstData)
                    return false;
                uint8_t meta = 0;
                if (!cursor.readByte(meta))
                    return false;
                const uint32_t len = readVar(cursor, ok);
                if (!ok || uint64_t(len) > cursor.remaining())
                    return false;
                if (meta == 0x51 && len >= 3) {
                    uint8_t b[3]{};
                    if (!cursor.read(b, 3))
                        return false;
                    metadata.tempoMap.push_back({
                        tick,
                        (uint32_t(b[0]) << 16) |
                        (uint32_t(b[1]) << 8) |
                        uint32_t(b[2])
                    });
                    if (!cursor.skip(uint64_t(len) - 3u))
                        return false;
                } else if (!cursor.skip(len)) {
                    return false;
                }
                if (meta == 0x2f)
                    break;
                ++sourceOrder;
                continue;
            }

            if (status == 0xf0 || status == 0xf7) {
                if (hasFirstData)
                    return false;
                const uint32_t len = readVar(cursor, ok);
                if (!ok || uint64_t(len) > cursor.remaining())
                    return false;
                SysExRef sx;
                sx.tick = tick;
                sx.track = trackIndex;
                sx.sourceOrder = sourceOrder;
                sx.status = status;
                sx.data.resize(static_cast<std::size_t>(len) + 1u);
                sx.data[0] = status;
                if (len != 0 && !cursor.read(sx.data.data() + 1u, len))
                    return false;
                sysex.push_back(std::move(sx));
                ++sourceOrder;
                continue;
            }

            if (status >= 0xf0) {
                if (hasFirstData)
                    return false;
                if (!cursor.skip(static_cast<uint64_t>(systemBytes(status))))
                    return false;
                ++sourceOrder;
                continue;
            }

            const uint8_t command = status & 0xf0;
            const uint8_t channel = status & 0x0f;
            const int dataBytes = (command == 0xc0 || command == 0xd0) ? 1 : 2;
            uint8_t d1 = 0, d2 = 0;
            if (hasFirstData)
                d1 = firstData;
            else if (!cursor.readByte(d1))
                return false;
            if (dataBytes == 2 && !cursor.readByte(d2))
                return false;

            // Record a sparse source checkpoint BEFORE this channel event. It
            // costs ~32 bytes per 65k channel events instead of retaining a
            // TickGroup/CompactEvent per event/tick.
            const TrackCheckpoint& lastCp = track.checkpoints.back();
            if (channelEventIndex != 0 &&
                ((channelEventIndex % TrackCheckpointEventStride) == 0 ||
                 eventStart - lastCp.sourceOffset >= TrackCheckpointByteStride)) {
                TrackCheckpoint checkpoint{
                    eventStart,
                    channelEventIndex,
                    tickBefore,
                    tick,
                    runningBefore,
                    false,
                    {}
                };
                // Source/timing checkpoint only. Deliberately do not snapshot
                // active-note queues during initial load; those are warmed lazily
                // by the renderer where they are actually needed.
                checkpoint.activeSnapshotValid = false;
                track.checkpoints.push_back(std::move(checkpoint));
            } else if (channelEventIndex == 0) {
                // Refine the initial checkpoint to the actual first channel
                // event so repeated random access does not rescan huge leading
                // metadata. Its firstTick remains the event's absolute tick.
                track.checkpoints.front().firstTick = tick;
            }

            const bool noteOn = command == 0x90 && d2 != 0;
            const bool noteOff = command == 0x80 || (command == 0x90 && d2 == 0);
            const bool control = command == 0xb0 || command == 0xc0 ||
                                 command == 0xd0 || command == 0xe0;
            // Historical SF2 selector restoration only needs Bank Select
            // MSB/LSB, Program Change and Pitch Bend.  Pass 13.8 duplicated
            // every CC into a second resident vector; CC-dense Black MIDIs
            // could therefore retain hundreds of MiB of redundant state and
            // compete with renderer/synth caches.  Keep only the selector
            // messages that are actually required for exact bank/program
            // reconstruction.
            const bool synthState = command == 0xc0 || command == 0xe0 ||
                                    (command == 0xb0 && (d1 == 0 || d1 == 32));

            const uint16_t visualKey =
                uint16_t((uint16_t(channel) << 7) | uint16_t(d1 & 0x7f));
            if (noteOn) {
                if (activeCounts[visualKey] != std::numeric_limits<uint32_t>::max())
                    ++activeCounts[visualKey];
            } else if (noteOff && activeCounts[visualKey] != 0) {
                const uint16_t channelBit = uint16_t(1u << channel);
                if ((track.firstNoteSeenMask & channelBit) != 0 &&
                    (firstNoteClosedMask & channelBit) == 0 &&
                    firstNotePitch[channel] == (d1 & 0x7f)) {
                    // FIFO pairing means the first matching NoteOff always
                    // closes the first NoteOn for this channel/pitch, even if
                    // later NoteOns overlap it. This preserves the only closure
                    // rank needed by the palette-order compatibility path.
                    track.firstNoteCloseOrder[channel] = noteClosureOrder;
                    firstNoteClosedMask |= channelBit;
                }
                --activeCounts[visualKey];
                ++noteClosureOrder;
            }

            ++channelEventIndex;
            ++sourceOrder;
            ++track.channelEvents;
            if (noteOn || noteOff)
                ++track.renderEvents;
            ++totalEvents;
            metadata.activeChannelMasks[trackIndex] |= (1u << channel);
            if (noteOn) {
                const uint16_t channelBit = uint16_t(1u << channel);
                if ((track.firstNoteSeenMask & channelBit) == 0) {
                    track.firstNoteSeenMask |= channelBit;
                    track.firstNoteTick[channel] = tick;
                    // channelEventIndex is the source order before this event
                    // is committed below. MPWGL2 assigns per-track colors from
                    // first appearance in its stable start-sorted note stream;
                    // this preserves the same track/time/source ordering without
                    // retaining every note in memory.
                    track.firstNoteOrder[channel] = channelEventIndex;
                    firstNotePitch[channel] = d1 & 0x7f;
                }

                ++totalNotes;
                metadata.hasPitch = true;
                metadata.minPitch = std::min(metadata.minPitch, d1);
                metadata.maxPitch = std::max(metadata.maxPitch, d1);
            }
            if (control)
                ++totalControls;
            if (synthState)
                ++track.synthStateEvents;
            (void)noteOff;
        }

        // MPWGL2 flushes still-pending NoteOns at EndOfTrack in numeric
        // pending-key order. A fixed 2048-counter table gives the same ordering
        // without allocating/dequeuing one object per active note during load.
        for (uint16_t key = 0; key < activeCounts.size(); ++key) {
            uint32_t count = activeCounts[key];
            if (count == 0)
                continue;
            const uint8_t orphanChannel = uint8_t((key >> 7) & 0x0f);
            const uint8_t orphanPitch = uint8_t(key & 0x7f);
            const uint16_t channelBit = uint16_t(1u << orphanChannel);
            if ((track.firstNoteSeenMask & channelBit) != 0 &&
                (firstNoteClosedMask & channelBit) == 0 &&
                firstNotePitch[orphanChannel] == orphanPitch) {
                track.firstNoteCloseOrder[orphanChannel] = noteClosureOrder;
                firstNoteClosedMask |= channelBit;
            }
            noteClosureOrder += count;
        }

        track.maxTick = tick;
        return ok;
    }

    bool parseTrackEvents(uint32_t trackIndex)
    {
        TrackIndex& track = tracks[trackIndex];
        track.hotEvents.clear();
        track.hotRenderEvents.clear();
        track.hotSynthStateEvents.clear();
        track.hotSynthStateOrders.clear();
        if (track.channelEvents >
            static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()))
            return false;
        track.hotEvents.resize(static_cast<std::size_t>(track.channelEvents));
        if (track.renderEvents >
            static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()))
            return false;
        track.hotRenderEvents.reserve(static_cast<std::size_t>(track.renderEvents));
        if (track.synthStateEvents >
            static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()))
            return false;
        track.hotSynthStateEvents.reserve(
            static_cast<std::size_t>(track.synthStateEvents));
        track.hotSynthStateOrders.reserve(
            static_cast<std::size_t>(track.synthStateEvents));

        Cursor cursor(reader, track.offset, track.offset + uint64_t(track.length));
        uint32_t tick = 0;
        uint8_t running = 0;
        bool ok = true;
        std::size_t write = 0;
        uint64_t sourceOrder = 0;

        while (cursor.remaining() && ok) {
            const uint32_t delta = readVar(cursor, ok);
            if (!ok || delta > std::numeric_limits<uint32_t>::max() - tick)
                return false;
            tick += delta;

            uint8_t status = 0;
            uint8_t firstData = 0;
            bool hasFirstData = false;
            if (!readStatusAndFirstData(
                    cursor, running, status, hasFirstData, firstData))
                return false;

            if (status == 0xff) {
                if (hasFirstData) return false;
                uint8_t meta = 0;
                if (!cursor.readByte(meta)) return false;
                const uint32_t len = readVar(cursor, ok);
                if (!ok || uint64_t(len) > cursor.remaining() || !cursor.skip(len))
                    return false;
                if (meta == 0x2f) break;
                ++sourceOrder;
                continue;
            }
            if (status == 0xf0 || status == 0xf7) {
                if (hasFirstData) return false;
                const uint32_t len = readVar(cursor, ok);
                if (!ok || uint64_t(len) > cursor.remaining() || !cursor.skip(len))
                    return false;
                ++sourceOrder;
                continue;
            }
            if (status >= 0xf0) {
                if (hasFirstData) return false;
                if (!cursor.skip(static_cast<uint64_t>(systemBytes(status))))
                    return false;
                ++sourceOrder;
                continue;
            }

            const uint8_t command = status & 0xf0;
            const uint8_t channel = status & 0x0f;
            const int dataBytes = (command == 0xc0 || command == 0xd0) ? 1 : 2;
            uint8_t d1 = 0, d2 = 0;
            if (hasFirstData) d1 = firstData;
            else if (!cursor.readByte(d1)) return false;
            if (dataBytes == 2 && !cursor.readByte(d2)) return false;

            if (command == 0x90 && d2 == 0) {
                status = static_cast<uint8_t>(0x80 | channel);
                d2 = 64;
            }

            if (write >= track.hotEvents.size())
                return false;
            const uint8_t color = static_cast<uint8_t>(
                (globalColors[channel] & 0x0f) |
                ((trackColors[trackIndex][channel] & 0x0f) << 4));
            track.hotEvents[write++] = {
                tick,
                uint32_t(status) |
                    (uint32_t(d1) << 8) |
                    (uint32_t(d2) << 16) |
                    (uint32_t(color) << 24)
            };

            if (command == 0x90 || command == 0x80) {
                track.hotRenderEvents.push_back(track.hotEvents[write - 1]);
            }

            if (command == 0xc0 || command == 0xe0 ||
                (command == 0xb0 && (d1 == 0 || d1 == 32))) {
                track.hotSynthStateEvents.push_back({
                    tick,
                    uint32_t(status) |
                        (uint32_t(d1) << 8) |
                        (uint32_t(d2) << 16)
                });
                track.hotSynthStateOrders.push_back(sourceOrder);
            }
            ++sourceOrder;
        }

        if (!ok || write != track.hotEvents.size() ||
            track.hotRenderEvents.size() !=
                static_cast<std::size_t>(track.renderEvents) ||
            track.hotSynthStateEvents.size() !=
                static_cast<std::size_t>(track.synthStateEvents) ||
            track.hotSynthStateOrders.size() != track.hotSynthStateEvents.size())
            return false;
        return true;
    }

    void buildDerivedStatsHot(MidiDocument& metadata)
    {
        metadata.derivedNpsTimeline.clear();
        metadata.derivedCcTimeline.clear();
        metadata.derivedPeakNps = 0;
        metadata.derivedPeakNpsTime = 0.0f;
        metadata.derivedPeakPolyphony = 0;

        if (metadata.durationSeconds > 0.0f) {
            constexpr double Step = 0.5;
            constexpr std::size_t MaxTimelineBuckets = 1u << 20;
            const double wanted =
                std::ceil(double(metadata.durationSeconds) / Step) + 1.0;
            const std::size_t buckets = static_cast<std::size_t>(
                std::clamp<double>(wanted, 1.0, double(MaxTimelineBuckets)));
            metadata.derivedNpsTimeline.assign(buckets, 0);
            metadata.derivedCcTimeline.assign(buckets, 0);
        }

        HotIterator it;
        it.reset(this, 0, metadata.maxTick);
        EventWord event;
        uint32_t trackIndex = 0;
        uint64_t order = 0;
        uint64_t active = 0;
        uint64_t peak = 0;

        // Track/channel/key is the real NoteOff pairing domain.  Allocate one
        // compact count table per track (4 KiB/track) rather than one object per
        // active note.  This makes the mountain/timeline controls live again
        // without rebuilding VisualNote arrays in Qt.
        std::vector<std::array<uint16_t, 16u * 128u>> counts(tracks.size());

        while (it.next(event, &trackIndex, &order)) {
            const uint8_t status = uint8_t(event.packed & 0xffu);
            const uint8_t command = status & 0xf0u;
            const uint8_t channel = status & 0x0fu;
            const uint8_t pitch = uint8_t((event.packed >> 8) & 0x7fu);
            const uint8_t velocity = uint8_t((event.packed >> 16) & 0x7fu);
            const std::size_t key = std::size_t(channel) * 128u + pitch;

            const bool control = command == 0xb0u || command == 0xc0u ||
                command == 0xd0u || command == 0xe0u;
            if (control && !metadata.derivedCcTimeline.empty()) {
                constexpr double Step = 0.5;
                std::size_t bucket = static_cast<std::size_t>(
                    std::max(0.0, std::floor(metadata.tickToSeconds(event.tick) / Step)));
                bucket = std::min(bucket, metadata.derivedCcTimeline.size() - 1);
                uint32_t& value = metadata.derivedCcTimeline[bucket];
                if (value != std::numeric_limits<uint32_t>::max()) ++value;
            }

            if (command == 0x90u && velocity != 0) {
                uint16_t& count = counts[trackIndex][key];
                if (count != std::numeric_limits<uint16_t>::max()) ++count;
                ++active;
                peak = std::max(peak, active);

                if (!metadata.derivedNpsTimeline.empty()) {
                    constexpr double Step = 0.5;
                    std::size_t bucket = static_cast<std::size_t>(
                        std::max(0.0, std::floor(metadata.tickToSeconds(event.tick) / Step)));
                    bucket = std::min(bucket, metadata.derivedNpsTimeline.size() - 1);
                    uint32_t& value = metadata.derivedNpsTimeline[bucket];
                    if (value != std::numeric_limits<uint32_t>::max()) ++value;
                }
            } else if (command == 0x80u) {
                uint16_t& count = counts[trackIndex][key];
                if (count != 0) {
                    --count;
                    if (active != 0) --active;
                }
            }
        }

        metadata.derivedPeakPolyphony = static_cast<uint32_t>(
            std::min<uint64_t>(peak, std::numeric_limits<uint32_t>::max()));

        // The displayed mountain uses half-second buckets.  For peak NPS use a
        // one-second window made from adjacent buckets; unlike the old code this
        // is bounded and never allocates one timestamp per NoteOn.
        for (std::size_t i = 0; i < metadata.derivedNpsTimeline.size(); ++i) {
            const uint64_t oneSecond = uint64_t(metadata.derivedNpsTimeline[i]) +
                (i ? uint64_t(metadata.derivedNpsTimeline[i - 1]) : 0u);
            const uint32_t value = static_cast<uint32_t>(
                std::min<uint64_t>(oneSecond, std::numeric_limits<uint32_t>::max()));
            if (value > metadata.derivedPeakNps) {
                metadata.derivedPeakNps = value;
                metadata.derivedPeakNpsTime = static_cast<float>(double(i) * 0.5);
            }
        }
        metadata.derivedStatsReady = true;
    }

    void buildColorTables(const MidiDocument& metadata)
    {
        globalColors.fill(0);
        std::array<int8_t, 16> assigned{};
        assigned.fill(-1);
        int next = 0;
        for (uint32_t mask : metadata.activeChannelMasks) {
            for (int ch = 0; ch < 16; ++ch) {
                if ((mask & (1u << ch)) && assigned[ch] < 0)
                    assigned[ch] = static_cast<int8_t>(next++ & 15);
            }
        }
        for (int ch = 0; ch < 16; ++ch)
            globalColors[ch] = assigned[ch] >= 0 ? uint8_t(assigned[ch]) : uint8_t(ch);

        trackColors.resize(metadata.trackCount);
        for (auto& colors : trackColors) {
            for (int ch = 0; ch < 16; ++ch)
                colors[ch] = uint8_t(ch);
        }

        // MPWGL2's per-track palette is assigned by first appearance in the
        // globally start-sorted note stream, not merely track/channel number.
        // The mapped index records each track/channel's first NoteOn during the
        // one mandatory source scan, so reproduce that ordering without a
        // second all-note pass or permanent note array.
        struct FirstAppearance {
            uint32_t tick = 0;
            uint32_t track = 0;
            uint64_t order = 0;
            uint64_t closeOrder = std::numeric_limits<uint64_t>::max();
            uint8_t channel = 0;
        };

        std::vector<FirstAppearance> appearances;
        appearances.reserve(trackColors.size() * 4u);
        for (uint32_t t = 0; t < tracks.size(); ++t) {
            const TrackIndex& track = tracks[t];
            for (uint8_t ch = 0; ch < 16; ++ch) {
                if ((track.firstNoteSeenMask & uint16_t(1u << ch)) == 0)
                    continue;
                appearances.push_back({
                    track.firstNoteTick[ch],
                    t,
                    track.firstNoteOrder[ch],
                    track.firstNoteCloseOrder[ch],
                    ch
                });
            }
        }

        std::stable_sort(
            appearances.begin(),
            appearances.end(),
            [](const FirstAppearance& a, const FirstAppearance& b) {
                if (a.tick != b.tick)
                    return a.tick < b.tick;
                if (a.track != b.track)
                    return a.track < b.track;
                if (a.closeOrder != b.closeOrder)
                    return a.closeOrder < b.closeOrder;
                return a.order < b.order;
            });

        next = 0;
        for (const FirstAppearance& appearance : appearances)
            trackColors[appearance.track][appearance.channel] =
                uint8_t(next++ & 15);
    }

    static uint32_t visualPendingKey(
        uint32_t track,
        uint8_t channel,
        uint8_t pitch)
    {
        return (track << 11) |
               (uint32_t(channel & 0x0f) << 7) |
               uint32_t(pitch & 0x7f);
    }

    static VisualNote makeVisualNote(
        uint32_t startTick,
        uint32_t endTick,
        uint8_t pitch,
        uint8_t velocity,
        uint8_t color,
        uint32_t track)
    {
        return {
            startTick,
            endTick,
            uint32_t(velocity & 0x7f) |
            (uint32_t(pitch & 0x7f) << 8) |
            (uint32_t(color & 0x0f) << 16) |
            (uint32_t((color >> 4) & 0x0f) << 20) |
            // The mapped renderer does not need the legacy channel nibble in
            // bits 24..27 (keyboard state comes from KeySnapshot). Keep a low
            // track byte in the otherwise-unused high byte so repeated carry
            // notes from adjacent tiles can be disambiguated more strongly.
            // Draw order itself is preserved by the page builder and never
            // depends on this truncated value.
            (uint32_t(track & 0xffu) << 24)
        };
    }

    void closeExpiredOrphans(
        VisualState& state,
        uint32_t beforeTick)
    {
        for (auto it = state.pending.begin(); it != state.pending.end();) {
            const uint32_t track = it->first >> 11;
            if (track < tracks.size() &&
                tracks[track].maxTick < beforeTick) {
                it = state.pending.erase(it);
            } else {
                ++it;
            }
        }
    }

    void closePageOrphans(
        VisualState& state,
        uint32_t pageStart,
        uint32_t pageEnd,
        std::vector<VisualPageItem>& output,
        std::unordered_map<uint32_t, std::deque<std::size_t>>& outputIndices)
    {
        for (auto it = state.pending.begin(); it != state.pending.end();) {
            const uint32_t key = it->first;
            const uint32_t track = key >> 11;
            if (track >= tracks.size()) {
                ++it;
                continue;
            }

            const uint32_t endTick = tracks[track].maxTick;
            if (endTick < pageStart || endTick > pageEnd) {
                ++it;
                continue;
            }

            auto oi = outputIndices.find(key);
            while (!it->second.empty()) {
                if (oi != outputIndices.end() && !oi->second.empty()) {
                    const std::size_t index = oi->second.front();
                    oi->second.pop_front();
                    if (index < output.size()) {
                        VisualPageItem& item = output[index];
                        item.minimumDuration = endTick <= item.note.startTick;
                        item.note.endTick =
                            endTick > item.note.startTick
                                ? endTick
                                : (item.note.startTick ==
                                           std::numeric_limits<uint32_t>::max()
                                       ? item.note.startTick
                                       : item.note.startTick + 1u);
                        // MPWGL2 flushes orphan queues after the track parse.
                        // Numeric pending keys are visited in ascending order.
                        item.closeOrder =
                            std::numeric_limits<uint64_t>::max() -
                            uint64_t(0xffffffffu - key);
                    }
                }
                it->second.pop_front();
            }

            if (oi != outputIndices.end())
                outputIndices.erase(oi);
            it = state.pending.erase(it);
        }
    }

    static void applyVisualEvent(
        VisualState& state,
        const EventWord& event,
        uint32_t track,
        uint64_t sourceOrder,
        std::vector<VisualPageItem>* output,
        std::unordered_map<uint32_t, std::deque<std::size_t>>* outputIndices,
        bool emitNewNotes = true)
    {
        const uint8_t status = uint8_t(event.packed & 0xffu);
        const uint8_t command = status & 0xf0;
        if (command != 0x90 && command != 0x80)
            return;

        const uint8_t channel = status & 0x0f;
        const uint8_t pitch = uint8_t((event.packed >> 8) & 0x7fu);
        const uint8_t velocity = uint8_t((event.packed >> 16) & 0x7fu);
        const uint8_t color = uint8_t((event.packed >> 24) & 0xffu);
        const uint32_t key = visualPendingKey(track, channel, pitch);
        const bool noteOn = command == 0x90 && velocity != 0;

        if (noteOn) {
            state.pending[key].push_back({
                event.tick,
                velocity,
                color,
                sourceOrder
            });

            if (emitNewNotes && output && outputIndices) {
                const std::size_t index = output->size();
                output->push_back({
                    makeVisualNote(
                        event.tick,
                        0,
                        pitch,
                        velocity,
                        color,
                        track),
                    track,
                    std::numeric_limits<uint64_t>::max(),
                    sourceOrder
                });
                (*outputIndices)[key].push_back(index);
            }
            return;
        }

        auto active = state.pending.find(key);
        if (active == state.pending.end() || active->second.empty())
            return;

        active->second.pop_front();

        if (output && outputIndices) {
            auto oi = outputIndices->find(key);
            if (oi != outputIndices->end() && !oi->second.empty()) {
                const std::size_t index = oi->second.front();
                oi->second.pop_front();
                if (index < output->size()) {
                    VisualPageItem& item = (*output)[index];
                    item.minimumDuration = event.tick <= item.note.startTick;
                    item.note.endTick =
                        event.tick > item.note.startTick
                            ? event.tick
                            : (item.note.startTick ==
                                       std::numeric_limits<uint32_t>::max()
                                   ? item.note.startTick
                                   : item.note.startTick + 1u);
                    item.closeOrder = sourceOrder;
                }
                if (oi->second.empty())
                    outputIndices->erase(oi);
            }
        }

        if (active->second.empty())
            state.pending.erase(active);
    }

    void closeTrackOrphansForResolvedOrder(
        uint32_t trackIndex,
        VisualState& state,
        std::vector<VisualPageItem>& output,
        std::unordered_map<uint32_t, std::deque<std::size_t>>& outputIndices)
    {
        if (trackIndex >= tracks.size())
            return;

        std::vector<uint32_t> keys;
        keys.reserve(32);
        for (const auto& entry : state.pending) {
            if ((entry.first >> 11) == trackIndex)
                keys.push_back(entry.first);
        }
        std::sort(keys.begin(), keys.end());

        const uint32_t endTick = tracks[trackIndex].maxTick;
        for (uint32_t key : keys) {
            auto active = state.pending.find(key);
            if (active == state.pending.end())
                continue;
            auto oi = outputIndices.find(key);
            uint64_t queueOrder = 0;
            while (!active->second.empty()) {
                if (oi != outputIndices.end() && !oi->second.empty()) {
                    const std::size_t index = oi->second.front();
                    oi->second.pop_front();
                    if (index < output.size()) {
                        VisualPageItem& item = output[index];
                        item.minimumDuration = endTick <= item.note.startTick;
                        item.note.endTick =
                            endTick > item.note.startTick
                                ? endTick
                                : (item.note.startTick ==
                                           std::numeric_limits<uint32_t>::max()
                                       ? item.note.startTick
                                       : item.note.startTick + 1u);
                        // JS object integer keys enumerate ascending; notes in
                        // one key retain FIFO queue order.
                        item.closeOrder =
                            (uint64_t(1) << 63) |
                            (uint64_t(key) << 20) |
                            std::min<uint64_t>(queueOrder, (1u << 20) - 1u);
                    }
                }
                active->second.pop_front();
                ++queueOrder;
            }
            if (oi != outputIndices.end())
                outputIndices.erase(oi);
            state.pending.erase(active);
        }
    }

    void resolveAmbiguousOpenOrder(
        uint32_t pageEnd,
        VisualState& state,
        std::vector<VisualPageItem>& output,
        std::unordered_map<uint32_t, std::deque<std::size_t>>& outputIndices)
    {
        // MPWGL2 does not sort equal-start notes by NoteOn order. It appends a
        // note only when its NoteOff is encountered, then performs a stable sort
        // by start time. Therefore equal-start notes on the same track/row are
        // visually ordered by closure order. A page ending before those
        // NoteOffs used to fall back to NoteOn order, which made overlapping
        // colors visibly different from MPWGL2. Resolve only genuinely
        // ambiguous groups (2+ still-open notes with the same track/start/pitch)
        // by scanning that track forward. Normal pages pay no second-pass cost.
        const auto groupKeyFor = [](const VisualPageItem& item) -> uint64_t {
            const uint64_t pitch = (item.note.packedData >> 8) & 0x7fu;
            return (uint64_t(item.track) << 39) |
                   (uint64_t(item.note.startTick) << 7) |
                   pitch;
        };

        std::unordered_map<uint64_t, uint32_t> openCounts;
        openCounts.reserve(output.size());
        for (const VisualPageItem& item : output) {
            if (item.note.endTick == 0)
                ++openCounts[groupKeyFor(item)];
        }

        std::vector<uint8_t> needsResolution(output.size(), 0);
        std::unordered_map<uint64_t, uint32_t> unresolved;
        std::unordered_map<uint32_t, uint32_t> ambiguousGroupsByTrack;
        unresolved.reserve(openCounts.size());
        ambiguousGroupsByTrack.reserve(8);

        for (std::size_t i = 0; i < output.size(); ++i) {
            if (output[i].note.endTick != 0)
                continue;
            const uint64_t group = groupKeyFor(output[i]);
            const auto countIt = openCounts.find(group);
            if (countIt == openCounts.end() || countIt->second < 2)
                continue;
            needsResolution[i] = 1;
            if (unresolved.emplace(group, countIt->second).second)
                ++ambiguousGroupsByTrack[output[i].track];
        }

        if (ambiguousGroupsByTrack.empty() ||
            pageEnd == std::numeric_limits<uint32_t>::max()) {
            return;
        }

        const uint32_t futureStart = pageEnd + 1u;

        for (auto trackEntry : ambiguousGroupsByTrack) {
            const uint32_t trackIndex = trackEntry.first;
            uint32_t groupsRemaining = trackEntry.second;
            if (trackIndex >= tracks.size() || groupsRemaining == 0)
                continue;

            Decoder decoder;
            if (!decoder.reset(this, trackIndex, futureStart) || !decoder.ok)
                continue;

            auto process = [&](const EventWord& event, uint64_t sourceOrder) {
                const uint8_t status = uint8_t(event.packed & 0xffu);
                const uint8_t command = status & 0xf0;
                if (command == 0x80 ||
                    (command == 0x90 && ((event.packed >> 16) & 0x7fu) == 0)) {
                    const uint8_t channel = status & 0x0f;
                    const uint8_t pitch = uint8_t((event.packed >> 8) & 0x7fu);
                    const uint32_t pendingKey =
                        visualPendingKey(trackIndex, channel, pitch);
                    auto oi = outputIndices.find(pendingKey);
                    if (oi != outputIndices.end() && !oi->second.empty()) {
                        const std::size_t index = oi->second.front();
                        if (index < needsResolution.size() && needsResolution[index]) {
                            const uint64_t group = groupKeyFor(output[index]);
                            auto unresolvedIt = unresolved.find(group);
                            if (unresolvedIt != unresolved.end() &&
                                unresolvedIt->second > 1) {
                                --unresolvedIt->second;
                                needsResolution[index] = 0;
                                if (unresolvedIt->second == 1 && groupsRemaining > 0)
                                    --groupsRemaining;
                            }
                        }
                    }
                }

                // Keep future NoteOns in the FIFO state so subsequent NoteOffs
                // still pair exactly, but never append notes that start outside
                // this requested page to the page's output array.
                applyVisualEvent(
                    state,
                    event,
                    trackIndex,
                    sourceOrder,
                    &output,
                    &outputIndices,
                    false);
            };

            if (decoder.currentValid) {
                process(decoder.current, decoder.currentOrder);
                decoder.advance();
            }
            while (groupsRemaining > 0 && decoder.currentValid && decoder.ok) {
                process(decoder.current, decoder.currentOrder);
                decoder.advance();
            }

            // If we genuinely reached EndOfTrack before the ambiguous group
            // could be reduced to one open note, its remaining NoteOns are
            // orphans. Resolve them using MPWGL2's numeric pending-key order.
            if (groupsRemaining > 0 && !decoder.currentValid && decoder.ok) {
                closeTrackOrphansForResolvedOrder(
                    trackIndex,
                    state,
                    output,
                    outputIndices);
            }
        }

        // Do not manufacture an EndOfTrack close for the one unresolved note
        // left in a group after its relative order is already known. The page
        // intentionally keeps that note open (endTick=0); a later tile will
        // reveal its real NoteOff. EOT orphans are closed by closePageOrphans()
        // when the requested tile actually reaches that track's end.
    }

    bool rebuildVisualStateAt(
        uint32_t targetTick,
        VisualState& state)
    {
        state.pending.clear();

        for (uint32_t trackIndex = 0;
             trackIndex < tracks.size();
             ++trackIndex) {
            const TrackIndex& track = tracks[trackIndex];
            if (track.checkpoints.empty())
                continue;

            // Find the nearest source checkpoint at/before the target. If a
            // pathological high-polyphony checkpoint exceeded the bounded
            // snapshot budget, walk backward to the nearest checkpoint whose
            // active-note state was retained. Correctness is never sacrificed;
            // only the amount of source that must be replayed increases.
            auto cp = std::lower_bound(
                track.checkpoints.begin(),
                track.checkpoints.end(),
                targetTick,
                [](const TrackCheckpoint& item, uint32_t value) {
                    return item.firstTick < value;
                });

            if (cp == track.checkpoints.end() || cp->firstTick > targetTick) {
                if (cp != track.checkpoints.begin())
                    --cp;
            }

            while (cp != track.checkpoints.begin() &&
                   !cp->activeSnapshotValid) {
                --cp;
            }
            if (!cp->activeSnapshotValid)
                cp = track.checkpoints.begin();

            for (const TrackActiveSnapshot& active : cp->activeNotes) {
                const uint8_t channel = uint8_t((active.key >> 7) & 0x0f);
                const uint8_t pitch = uint8_t(active.key & 0x7f);
                const uint8_t color = static_cast<uint8_t>(
                    (globalColors[channel] & 0x0f) |
                    ((trackColors[trackIndex][channel] & 0x0f) << 4));
                state.pending[
                    visualPendingKey(trackIndex, channel, pitch)]
                    .push_back({
                        active.startTick,
                        active.velocity,
                        color,
                        active.openOrder
                    });
            }

            // The second loading pass already materialized every channel event.
            // Rebuild arbitrary seek state from the checkpoint's resident
            // channel-event index; never return to Blob/FileReader during
            // playback or seeking.
            std::size_t index = static_cast<std::size_t>(std::min<uint64_t>(
                cp->channelEventIndex, track.hotEvents.size()));
            while (index < track.hotEvents.size()) {
                const EventWord& event = track.hotEvents[index];
                if (event.tick >= targetTick)
                    break;
                applyVisualEvent(
                    state,
                    event,
                    trackIndex,
                    static_cast<uint64_t>(index),
                    nullptr,
                    nullptr);
                ++index;
            }
        }

        closeExpiredOrphans(state, targetTick);
        return true;
    }

    const VisualCheckpoint& checkpointFor(uint32_t tick) const
    {
        auto it = std::upper_bound(
            checkpoints.begin(), checkpoints.end(), tick,
            [](uint32_t value, const VisualCheckpoint& cp) {
                return value < cp.tick;
            });
        if (it == checkpoints.begin())
            return checkpoints.front();
        return *(it - 1);
    }

    const VisualCheckpoint& ensureCheckpoint(uint32_t targetTick)
    {
        if (checkpoints.empty())
            checkpoints.push_back({0, VisualState{}});

        targetTick = std::min(targetTick, metadataMaxTick());

        auto exact = std::lower_bound(
            checkpoints.begin(), checkpoints.end(), targetTick,
            [](const VisualCheckpoint& cp, uint32_t value) {
                return cp.tick < value;
            });
        if (exact != checkpoints.end() && exact->tick == targetTick)
            return *exact;

        auto baseIt = exact;
        if (baseIt == checkpoints.begin()) {
            baseIt = checkpoints.begin();
        } else {
            --baseIt;
        }

        const uint32_t baseTick = baseIt->tick;
        VisualState state = baseIt->state;

        // A far seek should not replay the global visual state from tick zero.
        // Reconstruct it independently from each track's bounded source
        // checkpoint, whose active-note snapshot was captured during the one
        // mandatory indexing pass. Sequential page warming still uses the
        // existing previous visual checkpoint because that is cheaper.
        const uint64_t gap = uint64_t(targetTick) - uint64_t(baseTick);
        const uint64_t directThreshold =
            std::max<uint64_t>(
                1u,
                uint64_t(visualCheckpointSpan) / 2u);
        if (gap > directThreshold) {
            if (!rebuildVisualStateAt(targetTick, state))
                return checkpointFor(baseTick);

            auto pos = std::lower_bound(
                checkpoints.begin(), checkpoints.end(), targetTick,
                [](const VisualCheckpoint& cp, uint32_t value) {
                    return cp.tick < value;
                });
            if (pos == checkpoints.end() || pos->tick != targetTick)
                pos = checkpoints.insert(pos, {targetTick, state});
            return *pos;
        }

        HotIterator it;
        it.reset(this, baseTick, targetTick);
        if (it.failed())
            return checkpointFor(baseTick);

        uint32_t nextGrid = baseTick;
        if (visualCheckpointSpan > 0) {
            const uint64_t grid =
                (uint64_t(baseTick) / visualCheckpointSpan + 1u) *
                uint64_t(visualCheckpointSpan);
            nextGrid = static_cast<uint32_t>(
                std::min<uint64_t>(grid, metadataMaxTick()));
        }

        EventWord event;
        uint32_t eventTrack = 0;
        uint64_t eventOrder = 0;
        while (it.peek(event, &eventTrack, &eventOrder) &&
               event.tick < targetTick) {
            while (visualCheckpointSpan > 0 &&
                   nextGrid > baseTick &&
                   nextGrid < targetTick &&
                   nextGrid <= event.tick) {
                auto pos = std::lower_bound(
                    checkpoints.begin(), checkpoints.end(), nextGrid,
                    [](const VisualCheckpoint& cp, uint32_t value) {
                        return cp.tick < value;
                    });
                closeExpiredOrphans(state, nextGrid);
                if (pos == checkpoints.end() || pos->tick != nextGrid)
                    checkpoints.insert(pos, {nextGrid, state});

                if (metadataMaxTick() - nextGrid < visualCheckpointSpan) {
                    nextGrid = targetTick;
                    break;
                }
                nextGrid += visualCheckpointSpan;
            }

            it.next(event, &eventTrack, &eventOrder);
            applyVisualEvent(
                state,
                event,
                eventTrack,
                eventOrder,
                nullptr,
                nullptr);
        }

        if (it.failed())
            return checkpointFor(baseTick);

        closeExpiredOrphans(state, targetTick);

        auto pos = std::lower_bound(
            checkpoints.begin(), checkpoints.end(), targetTick,
            [](const VisualCheckpoint& cp, uint32_t value) {
                return cp.tick < value;
            });
        if (pos == checkpoints.end() || pos->tick != targetTick)
            pos = checkpoints.insert(pos, {targetTick, state});
        return *pos;
    }

    uint32_t metadataMaxTick() const
    {
        return cachedMaxTick;
    }

    uint32_t cachedMaxTick = 0;

    bool buildCheckpoints(
        MidiDocument& metadata,
        MidiParseProgress progress,
        void* user,
        uint64_t totalEvents)
    {
        checkpoints.clear();
        checkpoints.reserve(VisualCheckpointCount + 2);
        VisualState visualState;
        std::array<uint64_t, VisualStateCount> rawCounts{};
        uint64_t rawActive = 0;
        uint64_t peakActive = 0;

        const uint32_t span = std::max<uint32_t>(
            1u,
            static_cast<uint32_t>(
                (uint64_t(metadata.maxTick) + VisualCheckpointCount - 1u) /
                VisualCheckpointCount));
        uint32_t nextCheckpoint = 0;

        if (metadata.durationSeconds > 0.0f) {
            constexpr double Step = 0.5;
            const double wanted = std::ceil(double(metadata.durationSeconds) / Step) + 1.0;
            // Keep pathological multi-year timing from turning a tiny MIDI into
            // an enormous stats allocation. This is UI metadata only.
            constexpr std::size_t MaxTimelineBuckets = 1u << 20;
            const std::size_t buckets = static_cast<std::size_t>(
                std::clamp<double>(wanted, 1.0, double(MaxTimelineBuckets)));
            metadata.derivedNpsTimeline.assign(buckets, 0);
        }

        HotIterator it;
        it.reset(this, 0, metadata.maxTick);
        if (it.failed())
            return false;

        EventWord event;
        uint32_t eventTrack = 0;
        uint64_t eventOrder = 0;
        uint64_t processed = 0;
        auto saveBefore = [&](uint32_t eventTick) {
            while (nextCheckpoint <= metadata.maxTick &&
                   nextCheckpoint <= eventTick &&
                   checkpoints.size() <= VisualCheckpointCount + 1) {
                checkpoints.push_back({nextCheckpoint, visualState});
                if (metadata.maxTick - nextCheckpoint < span) {
                    if (nextCheckpoint == metadata.maxTick)
                        break;
                    nextCheckpoint = metadata.maxTick;
                } else {
                    nextCheckpoint += span;
                }
            }
        };

        while (it.next(event, &eventTrack, &eventOrder)) {
            // A checkpoint at T means state strictly before events at T.
            if (event.tick >= nextCheckpoint)
                saveBefore(event.tick);

            const uint8_t status = uint8_t(event.packed & 0xffu);
            const uint8_t command = status & 0xf0;
            const uint8_t channel = status & 0x0f;
            const uint8_t pitch = uint8_t((event.packed >> 8) & 0x7fu);
            const uint8_t velocity = uint8_t((event.packed >> 16) & 0x7fu);
            const std::size_t rawIndex = std::size_t(channel) * 128u + pitch;

            if (command == 0x90 && velocity != 0) {
                ++rawCounts[rawIndex];
                ++rawActive;
                peakActive = std::max(peakActive, rawActive);

                if (!metadata.derivedNpsTimeline.empty()) {
                    const double sec = metadata.tickToSeconds(event.tick);
                    constexpr double Step = 0.5;
                    std::size_t bucket = static_cast<std::size_t>(
                        std::max(0.0, std::floor(sec / Step)));
                    bucket = std::min(bucket, metadata.derivedNpsTimeline.size() - 1);
                    uint32_t& value = metadata.derivedNpsTimeline[bucket];
                    if (value != std::numeric_limits<uint32_t>::max())
                        ++value;
                }
            } else if (command == 0x80) {
                if (rawCounts[rawIndex] != 0) {
                    --rawCounts[rawIndex];
                    --rawActive;
                }
            }

            applyVisualEvent(
                visualState,
                event,
                eventTrack,
                eventOrder,
                nullptr,
                nullptr);
            ++processed;
            if (progress && (processed & ((1u << 20) - 1u)) == 0) {
                const int p = 70 + int(std::min<uint64_t>(23, processed * 23 /
                    std::max<uint64_t>(1, totalEvents)));
                progress(user, p, "Building SharpMIDI-style mapped checkpoints");
            }
        }
        if (it.failed())
            return false;

        if (checkpoints.empty() || checkpoints.front().tick != 0)
            checkpoints.insert(checkpoints.begin(), {0, VisualState{}});
        if (checkpoints.back().tick != metadata.maxTick)
            checkpoints.push_back({metadata.maxTick, visualState});

        metadata.derivedPeakPolyphony = static_cast<uint32_t>(
            std::min<uint64_t>(peakActive, std::numeric_limits<uint32_t>::max()));
        metadata.derivedPeakNps = 0;
        metadata.derivedPeakNpsTime = 0.0f;
        for (std::size_t b = 0; b < metadata.derivedNpsTimeline.size(); ++b) {
            const uint64_t perSecond = uint64_t(metadata.derivedNpsTimeline[b]) * 2u;
            const uint32_t value = static_cast<uint32_t>(
                std::min<uint64_t>(perSecond, std::numeric_limits<uint32_t>::max()));
            if (value > metadata.derivedPeakNps) {
                metadata.derivedPeakNps = value;
                metadata.derivedPeakNpsTime = static_cast<float>(double(b) * 0.5);
            }
        }
        return true;
    }
};

MidiMappedStore::MidiMappedStore()
    : impl_(new Impl)
{
}

MidiMappedStore::~MidiMappedStore()
{
    delete impl_;
}

void MidiMappedStore::clear()
{
    valid_ = false;
    error_.clear();
    metadata_ = MidiDocument{};
    if (!impl_)
        impl_ = new Impl;
    impl_->sourceSize = 0;
    impl_->seekSnapshotNotesStored = 0;
    impl_->tracks.clear();
    impl_->sysex.clear();
    impl_->trackColors.clear();
    impl_->checkpoints.clear();
    impl_->visualCheckpointSpan = 1;
    impl_->rollingVisualState = VisualState{};
    impl_->rollingVisualTick = 0;
    impl_->rollingVisualValid = false;
    impl_->cachedMaxTick = 0;
    impl_->eventCursor = {};
    impl_->eventCursorValid = false;
    impl_->liveVisualState = VisualState{};
    impl_->liveCursor = {};
    impl_->liveCursorValid = false;
    impl_->liveTick = -1.0;
    impl_->liveDeferredOffs.clear();
    impl_->liveNpsDensity.clear();
    impl_->liveCcDensity.clear();
    impl_->liveNpsCount = 0;
    impl_->liveCcCount = 0;
    impl_->renderCursor = {};
    impl_->renderCursorValid = false;
    impl_->renderPerTrackColors = false;
    impl_->renderHead = 1;
    impl_->renderActiveCounts.fill(0);
    impl_->renderActiveColors.fill(0);
    impl_->renderActiveIds.fill(0);
}

bool MidiMappedStore::index(
    uint64_t size,
    MidiReadAt readAt,
    void* readUser,
    MidiDocument& metadata,
    MidiParseProgress progress,
    void* progressUser)
{
    clear();
    auto report = [&](int p, const char* stage) {
        if (progress)
            progress(progressUser, p, stage);
    };

    try {
        if (!readAt || size < 14) {
            error_ = "MIDI data is too short";
            return false;
        }

        impl_->sourceSize = size;
        impl_->reader.reset(size, readAt, readUser);
        MidiDocument out;
        out.remoteIndexed = true;
        out.minPitch = 127;
        out.maxPitch = 0;
        report(1, "Mapping MIDI header without copying the file");

        Cursor cursor(impl_->reader, 0, size);
        uint8_t id[4]{};
        bool ok = true;
        if (!cursor.read(id, 4) || std::memcmp(id, "MThd", 4) != 0) {
            error_ = "Missing MThd header";
            return false;
        }
        const uint32_t headerLength = read32(cursor, ok);
        if (!ok || headerLength < 6 || uint64_t(headerLength) > cursor.remaining()) {
            error_ = "Invalid MIDI header";
            return false;
        }
        const uint64_t headerEnd = cursor.position() + headerLength;
        out.format = read16(cursor, ok);
        out.trackCount = read16(cursor, ok);
        out.ticksPerBeat = read16(cursor, ok);
        if (!ok || out.format > 1) {
            error_ = "Only MIDI Format 0 and 1 are supported";
            return false;
        }
        if (out.ticksPerBeat == 0 || (out.ticksPerBeat & 0x8000)) {
            error_ = "SMPTE timing is not supported";
            return false;
        }
        if (cursor.position() > headerEnd || !cursor.skip(headerEnd - cursor.position())) {
            error_ = "Invalid MIDI header";
            return false;
        }

        impl_->tracks.resize(out.trackCount);
        for (uint16_t t = 0; t < out.trackCount; ++t) {
            if (!cursor.read(id, 4) || std::memcmp(id, "MTrk", 4) != 0) {
                error_ = "Invalid or truncated MTrk chunk";
                return false;
            }
            const uint32_t len = read32(cursor, ok);
            if (!ok || uint64_t(len) > cursor.remaining()) {
                error_ = "Truncated MIDI track";
                return false;
            }
            impl_->tracks[t].offset = cursor.position();
            impl_->tracks[t].length = len;
            if (!cursor.skip(len)) {
                error_ = "Truncated MIDI track";
                return false;
            }
        }

        out.activeChannelMasks.assign(out.trackCount, 0);
        out.tempoMap.reserve(64);
        out.tempoMap.push_back({0, 500000});

        uint64_t totalEvents = 0;
        uint64_t totalNotes = 0;
        uint64_t totalControls = 0;
        report(5, "SharpMIDI pass 1: indexing tracks");
        for (uint32_t t = 0; t < impl_->tracks.size(); ++t) {
            if (!impl_->scanTrack(t, out, totalEvents, totalNotes, totalControls)) {
                error_ = impl_->reader.failed()
                    ? "Could not read mapped MIDI source"
                    : "Malformed MIDI event data";
                return false;
            }
            out.maxTick = std::max(out.maxTick, impl_->tracks[t].maxTick);
            const int p = 5 + int(55.0 * double(t + 1) /
                double(std::max<std::size_t>(1, impl_->tracks.size())));
            report(p, "SharpMIDI pass 1: indexing tracks");
        }

        out.noteCount = totalNotes;
        out.controlEventCount = totalControls;
        buildTempoIndex(out);
        out.durationSeconds = static_cast<float>(out.tickToSeconds(out.maxTick));
        impl_->buildColorTables(out);

        // SharpMIDI pass 2: materialize the compact channel-event stream once.
        // Runtime audio/render sweeps use only this Memory64-resident data.
        report(62, "SharpMIDI pass 2: parsing compact events");
        for (uint32_t t = 0; t < impl_->tracks.size(); ++t) {
            if (!impl_->parseTrackEvents(t)) {
                error_ = impl_->reader.failed()
                    ? "Could not read mapped MIDI source during compact parse"
                    : "Compact MIDI event parse mismatch";
                return false;
            }
            const int p = 62 + int(25.0 * double(t + 1) /
                double(std::max<std::size_t>(1, impl_->tracks.size())));
            report(p, "SharpMIDI pass 2: parsing compact events");
        }

        impl_->cachedMaxTick = out.maxTick;
        impl_->visualCheckpointSpan = std::max<uint32_t>(
            1u,
            static_cast<uint32_t>(
                (uint64_t(out.maxTick) + VisualCheckpointCount - 1u) /
                VisualCheckpointCount));
        impl_->checkpoints.clear();
        impl_->checkpoints.push_back({0, VisualState{}});

        // Graph statistics are intentionally NOT precomputed here. NPS,
        // polyphony and controller density are derived from the same rolling
        // resident-event cursor that drives the keyboard while playback runs.
        // This removes the old third all-song event pass from MIDI loading.
        out.derivedNpsTimeline.clear();
        out.derivedCcTimeline.clear();
        out.derivedPeakNps = 0;
        out.derivedPeakNpsTime = 0.0f;
        out.derivedPeakPolyphony = 0;
        out.derivedStatsReady = false;
        report(94, "Resident SharpMIDI event stream ready");

        std::stable_sort(
            impl_->sysex.begin(), impl_->sysex.end(),
            [](const SysExRef& a, const SysExRef& b) {
                if (a.tick != b.tick)
                    return a.tick < b.tick;
                if (a.track != b.track)
                    return a.track < b.track;
                return a.sourceOrder < b.sourceOrder;
            });

        // Qt receives metadata only. The compact source-event arrays remain
        // resident exclusively in the Memory64 worker; the wasm32 Qt heap still
        // does not scale with source event count.
        out.events.clear();
        out.tickGroups.clear();
        out.visualNotes.clear();
        out.visualBlockMaxEnd.clear();
        out.visualKeyStarts.clear();
        out.visualKeyEnds.clear();
        out.visualKeyOwners.clear();
        out.sysEx.clear();

        metadata_ = out;
        metadata = out;
        valid_ = true;
        report(100, "Mapped MIDI ready");
        return true;
    } catch (const std::bad_alloc&) {
        error_ = "Mapped MIDI compact event store exhausted browser memory";
        return false;
    } catch (...) {
        error_ = "Mapped MIDI index failed";
        return false;
    }
}

bool MidiMappedStore::buildVisualPage(
    uint32_t pageStart,
    uint32_t pageEnd,
    std::vector<VisualNote>& output)
{
    output.clear();
    if (!valid_ || impl_->checkpoints.empty())
        return false;
    if (pageEnd < pageStart)
        std::swap(pageStart, pageEnd);
    pageStart = std::min(pageStart, metadata_.maxTick);
    pageEnd = std::min(pageEnd, metadata_.maxTick);

    // Sequential 64-screen warming consumes the exact state left by the
    // preceding page. Only a discontinuity/seek falls back to a sparse
    // checkpoint. This mirrors SharpMIDI's forward sweep and avoids duplicating
    // a huge held-note map once per future screen.
    VisualState state;
    if (impl_->rollingVisualValid && impl_->rollingVisualTick == pageStart) {
        state = std::move(impl_->rollingVisualState);
        impl_->rollingVisualState = VisualState{};
        impl_->rollingVisualValid = false;
    } else {
        const auto& cp = impl_->ensureCheckpoint(pageStart);
        state = cp.state;
    }

    std::vector<VisualPageItem> items;
    std::unordered_map<uint32_t, std::deque<std::size_t>> outputIndices;

    std::size_t activeCount = 0;
    for (const auto& entry : state.pending)
        activeCount += entry.second.size();
    items.reserve(activeCount + 1024);
    outputIndices.reserve(state.pending.size() + 64);

    // Carry notes are emitted once for this page. state.pending is an
    // unordered_map, so first put only the currently-active carry set into
    // start/source order. New NoteOns are then appended by Iterator, which is
    // already globally ordered by tick, track and source order. This avoids a
    // full O(N log N) sort of every dense screen.
    struct CarrySeed {
        uint32_t key = 0;
        VisualPageItem item{};
    };
    std::vector<CarrySeed> carries;
    carries.reserve(activeCount);

    for (const auto& entry : state.pending) {
        const uint32_t key = entry.first;
        const uint32_t track = key >> 11;
        const uint8_t pitch = uint8_t(key & 0x7f);
        for (const ActiveVisualNote& active : entry.second) {
            carries.push_back({
                key,
                {
                    Impl::makeVisualNote(
                        active.startTick,
                        0,
                        pitch,
                        active.velocity,
                        active.color,
                        track),
                    track,
                    std::numeric_limits<uint64_t>::max(),
                    active.openOrder
                }
            });
        }
    }

    std::stable_sort(
        carries.begin(), carries.end(),
        [](const CarrySeed& a, const CarrySeed& b) {
            if (a.item.note.startTick != b.item.note.startTick)
                return a.item.note.startTick < b.item.note.startTick;
            if (a.item.track != b.item.track)
                return a.item.track < b.item.track;
            return a.item.openOrder < b.item.openOrder;
        });

    for (CarrySeed& seed : carries) {
        const std::size_t index = items.size();
        outputIndices[seed.key].push_back(index);
        items.push_back(std::move(seed.item));
    }

    Impl::HotIterator it;
    it.reset(impl_, pageStart, pageEnd);
    if (it.failed())
        return false;

    EventWord ev{};
    uint32_t eventTrack = 0;
    uint64_t eventOrder = 0;
    while (it.peek(ev, &eventTrack, &eventOrder)) {
        if (ev.tick > pageEnd)
            break;

        it.next(ev, &eventTrack, &eventOrder);
        Impl::applyVisualEvent(
            state,
            ev,
            eventTrack,
            eventOrder,
            &items,
            &outputIndices);
    }
    if (it.failed())
        return false;

    // MPWGL2 closes orphan NoteOns at that track's EndOfTrack. First close
    // those whose EOT is inside this tile. Equal-start ordering is resolved
    // locally below without a future-source rescan.
    impl_->closePageOrphans(
        state,
        pageStart,
        pageEnd,
        items,
        outputIndices);

    // Preserve one exact forward state for the next half-open tile. Do not add
    // it to the sparse checkpoint vector: on a sustained Black MIDI that would
    // duplicate the same enormous pending-note queues 64 times.
    if (pageEnd < metadata_.maxTick) {
        impl_->rollingVisualTick = pageEnd + 1u;
        impl_->rollingVisualState = std::move(state);
        impl_->rollingVisualValid = true;
    } else {
        impl_->rollingVisualState = VisualState{};
        impl_->rollingVisualTick = metadata_.maxTick;
        impl_->rollingVisualValid = false;
    }

    // MPWGL2 gives a zero-length note a 15 ms visible minimum. The mapped
    // pairing code marks those cases while closing them, then converts the same
    // duration through the MIDI tempo map here. A one-tick substitute was
    // visibly shorter at normal PPQ/tempo values.
    for (VisualPageItem& item : items) {
        if (!item.minimumDuration)
            continue;
        const double minEndSeconds =
            metadata_.tickToSeconds(item.note.startTick) + 0.015;
        const double minEndTick =
            std::ceil(metadata_.secondsToTick(minEndSeconds));
        item.note.endTick = static_cast<uint32_t>(
            std::clamp<double>(
                minEndTick,
                double(item.note.startTick) + 1.0,
                double(std::numeric_limits<uint32_t>::max())));
    }

    // Iterator already emits new NoteOns in global start/track/source order,
    // and the carry prefix was ordered above. Only equal-start notes from the
    // same track need MPWGL2's closure-order tie break. Sort those tiny local
    // runs instead of sorting the whole page -- critical for million-note
    // crashpoint screens. Notes whose NoteOff lies beyond this tile keep
    // closeOrder=max and retain their NoteOn order until a later page.
    for (std::size_t first = 0; first < items.size();) {
        std::size_t last = first + 1;
        while (last < items.size() &&
               items[last].note.startTick == items[first].note.startTick &&
               items[last].track == items[first].track) {
            ++last;
        }
        if (last - first > 1) {
            std::stable_sort(
                items.begin() + static_cast<std::ptrdiff_t>(first),
                items.begin() + static_cast<std::ptrdiff_t>(last),
                [](const VisualPageItem& a, const VisualPageItem& b) {
                    if (a.closeOrder != b.closeOrder)
                        return a.closeOrder < b.closeOrder;
                    return a.openOrder < b.openOrder;
                });
        }
        first = last;
    }

    output.reserve(items.size());
    for (const VisualPageItem& item : items)
        output.push_back(item.note);

    // Notes whose real NoteOff is beyond this tile intentionally retain
    // endTick=0. The remote shader extends the sentinel to the current viewEnd.
    return true;
}

bool MidiMappedStore::buildKeySnapshot(
    uint32_t tick,
    KeySnapshot& output)
{
    LiveSnapshot live;
    if (!buildLiveSnapshot(double(tick), double(tick), double(tick), live))
        return false;
    output = live.keys;
    return true;
}

bool MidiMappedStore::buildLiveSnapshot(
    double tick,
    double npsStartTick,
    double ccStartTick,
    LiveSnapshot& output,
    bool forceReset)
{
    output = LiveSnapshot{};
    if (!valid_ || !impl_ || impl_->tracks.empty() ||
        !std::isfinite(tick) || !std::isfinite(npsStartTick) ||
        !std::isfinite(ccStartTick)) {
        return false;
    }

    const double maxTick = double(metadata_.maxTick);
    tick = std::clamp(tick, 0.0, maxTick);
    npsStartTick = std::clamp(npsStartTick, 0.0, tick);
    ccStartTick = std::clamp(ccStartTick, 0.0, tick);
    const uint32_t wholeTick = static_cast<uint32_t>(std::floor(tick));
    const bool exactTick = std::abs(tick - double(wholeTick)) < 1.0e-9;

    auto isNoteOff = [](const EventWord& event) {
        return (uint8_t(event.packed & 0xf0u) == 0x80u);
    };
    auto isNoteOn = [](const EventWord& event) {
        return uint8_t(event.packed & 0xf0u) == 0x90u &&
            uint8_t((event.packed >> 16) & 0x7fu) != 0;
    };
    auto isControl = [](const EventWord& event) {
        const uint8_t command = uint8_t(event.packed & 0xf0u);
        return command == 0xb0u || command == 0xc0u ||
            command == 0xd0u || command == 0xe0u;
    };
    auto addPoint = [](std::deque<Impl::LiveDensityPoint>& points,
                       uint64_t& total,
                       uint32_t eventTick) {
        if (!points.empty() && points.back().tick == eventTick) {
            if (points.back().count != std::numeric_limits<uint32_t>::max())
                ++points.back().count;
        } else {
            points.push_back({eventTick, 1u});
        }
        if (total != std::numeric_limits<uint64_t>::max())
            ++total;
    };
    auto addDensityEvent = [&](const EventWord& event) {
        if (isNoteOn(event))
            addPoint(impl_->liveNpsDensity, impl_->liveNpsCount, event.tick);
        if (isControl(event))
            addPoint(impl_->liveCcDensity, impl_->liveCcCount, event.tick);
    };
    auto expire = [](std::deque<Impl::LiveDensityPoint>& points,
                     uint64_t& total,
                     double lowerTick) {
        while (!points.empty() && double(points.front().tick) < lowerTick) {
            total = total >= points.front().count
                ? total - points.front().count
                : 0u;
            points.pop_front();
        }
    };

    // Treat a large forward discontinuity as a seek. Rebuilding from the sparse
    // per-track source checkpoints is much cheaper than replaying a huge event
    // range just because the user dragged the playhead.
    const double resetDistance = double(std::max<uint32_t>(
        4096u, impl_->visualCheckpointSpan * 2u));
    const bool reset = forceReset || !impl_->liveCursorValid || impl_->liveTick < 0.0 ||
        tick + 1.0e-9 < impl_->liveTick ||
        tick - impl_->liveTick > resetDistance;

    if (reset) {
        impl_->liveVisualState = VisualState{};
        impl_->liveDeferredOffs.clear();
        impl_->liveNpsDensity.clear();
        impl_->liveCcDensity.clear();
        impl_->liveNpsCount = 0;
        impl_->liveCcCount = 0;

        // State immediately before WHOLETICK. Events at WHOLETICK are consumed
        // below so NoteOff-at-T can remain visible at exactly T, matching the
        // horizontal renderer's inclusive lifetime rule.
        if (!impl_->rebuildVisualStateAt(wholeTick, impl_->liveVisualState))
            return false;
        impl_->liveCursor.reset(impl_, wholeTick, metadata_.maxTick);
        if (impl_->liveCursor.failed())
            return false;
        impl_->liveCursorValid = true;

        // Reconstruct only the two short live-density windows. This is bounded
        // playback work and replaces the old whole-song graph-statistics pass.
        const double densityStart = std::min(npsStartTick, ccStartTick);
        Impl::HotIterator density;
        density.reset(
            impl_,
            static_cast<uint32_t>(std::floor(densityStart)),
            wholeTick);
        EventWord densityEvent;
        uint32_t densityTrack = 0;
        uint64_t densityOrder = 0;
        while (density.peek(densityEvent, &densityTrack, &densityOrder)) {
            if (densityEvent.tick > wholeTick)
                break;
            density.next(densityEvent, &densityTrack, &densityOrder);
            if (double(densityEvent.tick) + 1.0e-9 >= densityStart)
                addDensityEvent(densityEvent);
        }
        if (density.failed())
            return false;

        EventWord event;
        uint32_t track = 0;
        uint64_t order = 0;
        while (impl_->liveCursor.peek(event, &track, &order)) {
            if (event.tick > wholeTick)
                break;
            impl_->liveCursor.next(event, &track, &order);
            if (exactTick && event.tick == wholeTick && isNoteOff(event)) {
                impl_->liveDeferredOffs.push_back({event, track, order});
            } else {
                Impl::applyVisualEvent(
                    impl_->liveVisualState, event, track, order, nullptr, nullptr);
            }
        }
        if (impl_->liveCursor.failed())
            return false;
        impl_->liveTick = tick;
    } else if (tick > impl_->liveTick + 1.0e-9) {
        // A NoteOff at an exact previous tick becomes effective as soon as the
        // playhead moves past that tick. It was already consumed from the hot
        // iterator, so apply the tiny deferred list before advancing further.
        for (const Impl::DeferredLiveOff& deferred : impl_->liveDeferredOffs) {
            Impl::applyVisualEvent(
                impl_->liveVisualState,
                deferred.event,
                deferred.track,
                deferred.order,
                nullptr,
                nullptr);
        }
        impl_->liveDeferredOffs.clear();

        EventWord event;
        uint32_t track = 0;
        uint64_t order = 0;
        while (impl_->liveCursor.peek(event, &track, &order)) {
            if (event.tick > wholeTick)
                break;
            impl_->liveCursor.next(event, &track, &order);
            addDensityEvent(event);
            if (exactTick && event.tick == wholeTick && isNoteOff(event)) {
                impl_->liveDeferredOffs.push_back({event, track, order});
            } else {
                Impl::applyVisualEvent(
                    impl_->liveVisualState, event, track, order, nullptr, nullptr);
            }
        }
        if (impl_->liveCursor.failed())
            return false;
        impl_->liveTick = tick;
    }

    impl_->closeExpiredOrphans(impl_->liveVisualState, wholeTick);
    expire(impl_->liveNpsDensity, impl_->liveNpsCount, npsStartTick);
    expire(impl_->liveCcDensity, impl_->liveCcCount, ccStartTick);

    std::array<uint64_t, 128> ownerRank{};
    std::array<uint32_t, 128> ownerKey{};
    uint64_t activeVoices = 0;
    for (const auto& entry : impl_->liveVisualState.pending) {
        if (entry.second.empty())
            continue;
        const uint32_t key = entry.first;
        const uint32_t track = key >> 11;
        const uint8_t pitch = uint8_t(key & 0x7fu);
        const uint64_t count = entry.second.size();
        activeVoices = std::min<uint64_t>(
            std::numeric_limits<uint32_t>::max(), activeVoices + count);

        uint32_t& pitchCount = output.keys.counts[pitch];
        pitchCount = static_cast<uint32_t>(std::min<uint64_t>(
            std::numeric_limits<uint32_t>::max(), uint64_t(pitchCount) + count));

        // Within one track/channel/pitch queue the color is constant. The last
        // active note has the greatest start tick, which dominates the exact
        // rank used by the previous snapshot builder.
        const ActiveVisualNote& owner = entry.second.back();
        const uint64_t rank =
            (uint64_t(owner.startTick) << 32) |
            (uint64_t(track & 0xffffu) << 16) |
            ((count - 1u) & 0xffffu);
        // Equal ranks (two channels of one track) go to the higher key, not to hash order.
        if (pitchCount == count || rank > ownerRank[pitch] ||
            (rank == ownerRank[pitch] && key > ownerKey[pitch])) {
            ownerRank[pitch] = rank;
            ownerKey[pitch] = key;
            output.keys.globalColors[pitch] = owner.color & 0x0fu;
            output.keys.trackColors[pitch] = (owner.color >> 4) & 0x0fu;
        }
    }

    output.activeVoices = static_cast<uint32_t>(activeVoices);
    output.nps = static_cast<uint32_t>(std::min<uint64_t>(
        std::numeric_limits<uint32_t>::max(), impl_->liveNpsCount * 4u));
    output.ccPerSecond = static_cast<uint32_t>(std::min<uint64_t>(
        std::numeric_limits<uint32_t>::max(), impl_->liveCcCount));
    return true;
}

void MidiMappedStore::resetEventCursor(uint32_t startTick)
{
    if (!valid_) {
        impl_->eventCursorValid = false;
        impl_->eventSysExCursor = 0;
        return;
    }
    startTick = std::min(startTick, metadata_.maxTick);
    impl_->eventCursor.reset(impl_, startTick, metadata_.maxTick);
    impl_->eventCursorValid = !impl_->eventCursor.failed();
    impl_->eventSysExCursor = static_cast<std::size_t>(
        std::lower_bound(
            impl_->sysex.begin(), impl_->sysex.end(), startTick,
            [](const SysExRef& event, uint32_t tick) {
                return event.tick < tick;
            }) - impl_->sysex.begin());
}

bool MidiMappedStore::buildEventBatch(
    uint32_t endTick,
    std::size_t maxEvents,
    std::vector<EventWord>& output,
    std::vector<SysExBatchEvent>& sysExEvents,
    std::vector<uint8_t>& sysExBytes,
    bool& complete,
    uint32_t& nextTick,
    bool& hasNextTick)
{
    output.clear();
    sysExEvents.clear();
    sysExBytes.clear();
    complete = false;
    nextTick = 0;
    hasNextTick = false;
    if (!valid_ || !impl_->eventCursorValid)
        return false;
    maxEvents = std::max<std::size_t>(1, maxEvents);
    endTick = std::min(endTick, metadata_.maxTick);

    // A Black MIDI can place millions of identical NoteOns on one tick. The
    // supplied SnappySynthV2 core already has an exact overlap-count encoding
    // in bits 24..31, so collapse only consecutive identical NoteOns while the
    // mapped cursor is hot. This prevents the browser from allocating and
    // transferring millions of redundant 8-byte EventWords just to reconstruct
    // the exact same voice stack in JavaScript.
    //
    // The source is now a resident compact array, not Blob/VLQ parsing. Walk a
    // larger raw window before yielding so exact same-tick stacks collapse
    // efficiently, but do not monopolize the Memory64 Worker. Audio and the
    // SharpMIDI renderer share this resident-event worker; ~2.1M raw events per
    // call is enough to amortize the cursor while still allowing visual sweeps
    // to run between dense synth batches. PCM itself never waits on this cursor.
    const std::size_t sourceBudget =
        maxEvents > std::numeric_limits<std::size_t>::max() / 8
            ? maxEvents
            : maxEvents * 8;
    std::size_t sourceConsumed = 0;

    auto isStackableNoteOn = [](uint32_t packed) {
        const uint32_t midi = packed & 0x00ffffffu;
        const uint8_t status = static_cast<uint8_t>(midi & 0xffu);
        const uint8_t velocity = static_cast<uint8_t>((midi >> 16) & 0x7fu);
        return (status & 0xf0u) == 0x90u && velocity != 0;
    };

    EventWord ev;
    bool channelDoneForRequest = false;
    while (output.size() < maxEvents && sourceConsumed < sourceBudget) {
        if (!impl_->eventCursor.peek(ev)) {
            if (impl_->eventCursor.failed())
                return false;
            channelDoneForRequest = true;
            break;
        }
        nextTick = ev.tick;
        hasNextTick = true;
        if (ev.tick > endTick) {
            // Do not consume the first event outside the requested horizon.
            channelDoneForRequest = true;
            break;
        }
        if (!impl_->eventCursor.next(ev)) {
            if (impl_->eventCursor.failed())
                return false;
            channelDoneForRequest = true;
            break;
        }
        ++sourceConsumed;

        // EventWord's source high byte is renderer colour metadata. Synth
        // batches do not use colour, so repurpose that byte solely in this
        // output vector as SnappySynth's (stack_count - 1) field.
        const uint32_t midi = ev.packed & 0x00ffffffu;
        if (isStackableNoteOn(midi) && !output.empty()) {
            EventWord& previous = output.back();
            if (previous.tick == ev.tick &&
                (previous.packed & 0x00ffffffu) == midi) {
                const uint32_t encoded = (previous.packed >> 24) & 0xffu;
                if (encoded < 0xffu) {
                    previous.packed = midi | ((encoded + 1u) << 24);
                    continue;
                }
            }
        }

        output.push_back(EventWord{ev.tick, midi});
    }

    // The output or raw-source budget filled. Report the first event that has
    // not been consumed. Everything strictly before that tick is now safe for
    // SnappySynth to render. If we yielded in the middle of one enormous
    // same-tick crashpoint, nextTick deliberately remains that tick: the core
    // must receive the whole tick before it renders across it.
    if (!impl_->eventCursor.peek(ev)) {
        complete = !impl_->eventCursor.failed();
        hasNextTick = false;
    } else {
        nextTick = ev.tick;
        hasNextTick = true;
        if (ev.tick > endTick)
            complete = true;
    }
    if (channelDoneForRequest)
        complete = true;
    if (impl_->eventCursor.failed())
        return false;

    // Never publish a SysEx from a tick whose channel-event group is only
    // partially transferred.  For partial batches NEXTTICK is an exclusive
    // safety boundary; once the channel cursor completes the request, all
    // SysEx through ENDTICK are safe.  Payload bytes were retained during the
    // mandatory loading pass, so this path performs no runtime file I/O.
    const uint64_t safeExclusive = complete
        ? uint64_t(endTick) + 1u
        : (hasNextTick ? uint64_t(nextTick) : uint64_t(endTick) + 1u);
    while (impl_->eventSysExCursor < impl_->sysex.size()) {
        const SysExRef& sx = impl_->sysex[impl_->eventSysExCursor];
        if (uint64_t(sx.tick) >= safeExclusive)
            break;
        if (sx.data.empty()) {
            ++impl_->eventSysExCursor;
            continue;
        }
        if (sysExBytes.size() > std::numeric_limits<uint32_t>::max() - sx.data.size())
            return false;
        const uint32_t offset = static_cast<uint32_t>(sysExBytes.size());
        sysExBytes.insert(sysExBytes.end(), sx.data.begin(), sx.data.end());
        sysExEvents.push_back({
            sx.tick,
            offset,
            static_cast<uint32_t>(sx.data.size())
        });
        ++impl_->eventSysExCursor;
    }
    return true;
}

bool MidiMappedStore::buildHistoricalSysEx(
    uint32_t startTick,
    std::vector<SysExBatchEvent>& sysExEvents,
    std::vector<uint8_t>& sysExBytes)
{
    sysExEvents.clear();
    sysExBytes.clear();
    if (!valid_)
        return false;

    const auto end = std::lower_bound(
        impl_->sysex.begin(), impl_->sysex.end(), startTick,
        [](const SysExRef& event, uint32_t tick) {
            return event.tick < tick;
        });
    for (auto it = impl_->sysex.begin(); it != end; ++it) {
        if (it->data.empty())
            continue;
        if (sysExBytes.size() > std::numeric_limits<uint32_t>::max() - it->data.size())
            return false;
        const uint32_t offset = static_cast<uint32_t>(sysExBytes.size());
        sysExBytes.insert(sysExBytes.end(), it->data.begin(), it->data.end());
        sysExEvents.push_back({
            it->tick,
            offset,
            static_cast<uint32_t>(it->data.size())
        });
    }
    return true;
}

bool MidiMappedStore::buildHistoricalSelectorState(
    uint32_t startTick,
    std::vector<EventWord>& output)
{
    output.clear();
    if (!valid_)
        return false;

    struct ChannelSelector {
        uint8_t pendingMsb = 0;
        uint8_t pendingLsb = 0;
        uint8_t appliedMsb = 0;
        uint8_t appliedLsb = 0;
        uint8_t program = 0;
        uint16_t bend = 8192;
        bool seenPendingMsb = false;
        bool seenPendingLsb = false;
        bool seenProgram = false;
        bool seenBend = false;
    };
    std::array<ChannelSelector, 16> state{};

    // ssw_reset() starts from GM defaults. Historical SysEx is replayed before
    // this selector sequence, so a later GM/GS/XG reset must invalidate every
    // Bank Select/Program/Pitch message that happened before that reset. The
    // old fold ignored resets and could resurrect an earlier preset after the
    // reset, which is especially obvious with full-GM SF2s such as Arachno.
    auto normalizedPayload = [](const std::vector<uint8_t>& bytes,
                                const uint8_t*& data,
                                std::size_t& length) {
        data = bytes.data();
        length = bytes.size();
        if (length && data[0] == 0xf0u) { ++data; --length; }
        if (length && data[length - 1] == 0xf7u) --length;
    };
    auto isResetSysEx = [&](const std::vector<uint8_t>& bytes) {
        const uint8_t* data = nullptr;
        std::size_t length = 0;
        normalizedPayload(bytes, data, length);
        if (!data) return false;
        // GM / GM2 System On.
        if (length >= 4u && data[0] == 0x7eu && data[2] == 0x09u &&
            (data[3] == 0x01u || data[3] == 0x03u))
            return true;
        // Roland GS reset: 41 .. 42 12 40 00 7F 00 ...
        if (length >= 8u && data[0] == 0x41u && data[2] == 0x42u &&
            data[3] == 0x12u && data[4] == 0x40u && data[5] == 0x00u &&
            data[6] == 0x7fu && data[7] == 0x00u)
            return true;
        // Yamaha XG System On: 43 .. 4C 00 00 7E 00
        return length >= 7u && data[0] == 0x43u && data[2] == 0x4cu &&
            data[3] == 0x00u && data[4] == 0x00u && data[5] == 0x7eu &&
            data[6] == 0x00u;
    };

    uint32_t selectorHistoryStart = 0;
    uint32_t selectorHistoryTrack = 0;
    uint64_t selectorHistoryOrder = 0;
    bool haveReset = false;
    for (const SysExRef& sx : impl_->sysex) {
        if (sx.tick >= startTick)
            break;
        if (isResetSysEx(sx.data)) {
            // SysExRef is globally sorted by (tick, track, sourceOrder), the
            // same deterministic tie-break used by the mapped event merge.
            // Keep the complete boundary so selector events earlier than a
            // same-tick reset are discarded without losing events that follow
            // it later in that tick.
            selectorHistoryStart = sx.tick;
            selectorHistoryTrack = sx.track;
            selectorHistoryOrder = sx.sourceOrder;
            haveReset = true;
        }
    }

    struct CursorNode {
        uint32_t tick = 0;
        uint32_t track = 0;
        std::size_t index = 0;
    };
    struct CursorGreater {
        bool operator()(const CursorNode& a, const CursorNode& b) const {
            if (a.tick != b.tick) return a.tick > b.tick;
            if (a.track != b.track) return a.track > b.track;
            return a.index > b.index;
        }
    };

    std::priority_queue<CursorNode, std::vector<CursorNode>, CursorGreater> heap;
    auto selectorIsAfterReset = [&](uint32_t trackIndex, std::size_t eventIndex,
                                    const EventWord& ev) {
        if (!haveReset)
            return true;
        if (ev.tick != selectorHistoryStart)
            return ev.tick > selectorHistoryStart;
        if (trackIndex != selectorHistoryTrack)
            return trackIndex > selectorHistoryTrack;
        const auto& orders = impl_->tracks[trackIndex].hotSynthStateOrders;
        return eventIndex < orders.size() &&
            orders[eventIndex] > selectorHistoryOrder;
    };

    for (uint32_t t = 0; t < impl_->tracks.size(); ++t) {
        const auto& events = impl_->tracks[t].hotSynthStateEvents;
        if (events.empty())
            continue;
        auto begin = haveReset
            ? std::lower_bound(
                  events.begin(), events.end(), selectorHistoryStart,
                  [](const EventWord& event, uint32_t tick) {
                      return event.tick < tick;
                  })
            : events.begin();
        if (begin != events.end() && begin->tick < startTick) {
            heap.push({begin->tick, t,
                       static_cast<std::size_t>(begin - events.begin())});
        }
    }

    while (!heap.empty()) {
        const CursorNode node = heap.top();
        heap.pop();
        const auto& events = impl_->tracks[node.track].hotSynthStateEvents;
        if (node.index >= events.size())
            continue;
        const EventWord& ev = events[node.index];
        if (ev.tick >= startTick)
            continue;

        const bool afterReset = selectorIsAfterReset(node.track, node.index, ev);
        const uint8_t status = static_cast<uint8_t>(ev.packed & 0xffu);
        const uint8_t command = status & 0xf0u;
        const uint8_t ch = status & 0x0fu;
        const uint8_t d1 = static_cast<uint8_t>((ev.packed >> 8) & 0x7fu);
        const uint8_t d2 = static_cast<uint8_t>((ev.packed >> 16) & 0x7fu);
        if (afterReset) {
            ChannelSelector& cs = state[ch];
            if (command == 0xb0u) {
                if (d1 == 0) {
                    cs.pendingMsb = d2;
                    cs.seenPendingMsb = true;
                } else if (d1 == 32) {
                    cs.pendingLsb = d2;
                    cs.seenPendingLsb = true;
                }
            } else if (command == 0xc0u) {
                cs.program = d1;
                cs.appliedMsb = cs.pendingMsb;
                cs.appliedLsb = cs.pendingLsb;
                cs.seenProgram = true;
            } else if (command == 0xe0u) {
                cs.bend = static_cast<uint16_t>(uint16_t(d1) | (uint16_t(d2) << 7));
                cs.seenBend = true;
            }
        }

        const std::size_t next = node.index + 1u;
        if (next < events.size() && events[next].tick < startTick)
            heap.push({events[next].tick, node.track, next});
    }

    output.reserve(16u * 6u);
    auto emit3 = [&](uint8_t status, uint8_t d1, uint8_t d2) {
        output.push_back({
            startTick,
            uint32_t(status) | (uint32_t(d1) << 8) | (uint32_t(d2) << 16)
        });
    };
    for (uint8_t ch = 0; ch < 16; ++ch) {
        const ChannelSelector& cs = state[ch];
        // Recreate only state that actually existed after the latest reset.
        // The synth reset already provides bank 0 / program 0 / centered bend,
        // so emitting 48 synthetic defaults on every Play/seek is unnecessary
        // and can disturb same-tick reset/program ordering.
        if (cs.seenProgram) {
            if (cs.appliedMsb != 0)
                emit3(static_cast<uint8_t>(0xb0u | ch), 0, cs.appliedMsb);
            if (cs.appliedLsb != 0)
                emit3(static_cast<uint8_t>(0xb0u | ch), 32, cs.appliedLsb);
            emit3(static_cast<uint8_t>(0xc0u | ch), cs.program, 0);

            // Bank Select received after the committed Program Change remains
            // pending for the next Program Change, including an explicit zero
            // that clears a non-zero applied bank.
            if (cs.seenPendingMsb && cs.pendingMsb != cs.appliedMsb)
                emit3(static_cast<uint8_t>(0xb0u | ch), 0, cs.pendingMsb);
            if (cs.seenPendingLsb && cs.pendingLsb != cs.appliedLsb)
                emit3(static_cast<uint8_t>(0xb0u | ch), 32, cs.pendingLsb);
        } else {
            // No Program Change occurred after the reset: Bank Select values
            // are purely pending and must not be committed accidentally.
            if (cs.seenPendingMsb)
                emit3(static_cast<uint8_t>(0xb0u | ch), 0, cs.pendingMsb);
            if (cs.seenPendingLsb)
                emit3(static_cast<uint8_t>(0xb0u | ch), 32, cs.pendingLsb);
        }

        if (cs.seenBend && cs.bend != 8192u) {
            emit3(static_cast<uint8_t>(0xe0u | ch),
                  static_cast<uint8_t>(cs.bend & 0x7fu),
                  static_cast<uint8_t>((cs.bend >> 7) & 0x7fu));
        }
    }
    return true;
}

void MidiMappedStore::resetRenderCursor(
    uint32_t startTick,
    bool perTrackColors)
{
    impl_->renderHead = 1;
    impl_->renderPerTrackColors = perTrackColors;
    impl_->renderActiveCounts.fill(0);
    impl_->renderActiveColors.fill(0);
    impl_->renderActiveIds.fill(0);

    if (!valid_) {
        impl_->renderCursorValid = false;
        return;
    }

    impl_->renderCursor.reset(
        impl_,
        std::min(startTick, metadata_.maxTick),
        metadata_.maxTick,
        true);
    impl_->renderCursorValid = !impl_->renderCursor.failed();
}

bool MidiMappedStore::buildRenderSweep(
    uint32_t endTick,
    std::size_t maxSourceEvents,
    std::vector<VisualNote>& appends,
    std::vector<RenderClose>& closes,
    uint32_t& appendBase,
    bool& complete,
    uint32_t& nextTick,
    bool& hasNextTick)
{
    appends.clear();
    closes.clear();
    appendBase = impl_->renderHead;
    complete = false;
    nextTick = 0;
    hasNextTick = false;

    if (!valid_ || !impl_->renderCursorValid)
        return false;

    endTick = std::min(endTick, metadata_.maxTick);
    maxSourceEvents = std::max<std::size_t>(1, maxSourceEvents);

    EventWord event;
    uint32_t trackIndex = 0;
    uint64_t sourceOrder = 0;
    std::size_t consumed = 0;

    while (consumed < maxSourceEvents) {
        if (!impl_->renderCursor.peek(event, &trackIndex, &sourceOrder)) {
            if (impl_->renderCursor.failed())
                return false;
            complete = true;
            return true;
        }

        nextTick = event.tick;
        hasNextTick = true;
        if (event.tick > endTick) {
            complete = true;
            return true;
        }

        if (!impl_->renderCursor.next(event, &trackIndex, &sourceOrder)) {
            if (impl_->renderCursor.failed())
                return false;
            complete = true;
            return true;
        }
        ++consumed;

        const uint8_t status = static_cast<uint8_t>(event.packed & 0xffu);
        const uint8_t command = static_cast<uint8_t>(status & 0xf0u);
        if (command != 0x90u && command != 0x80u)
            continue;

        const uint8_t channel = static_cast<uint8_t>(status & 0x0fu);
        const uint8_t key = static_cast<uint8_t>((event.packed >> 8) & 0x7fu);
        const uint8_t velocity = static_cast<uint8_t>((event.packed >> 16) & 0x7fu);
        const bool noteOn = command == 0x90u && velocity != 0;

        // Ownership and display color are intentionally separate. SharpMIDI's
        // overlap remover still needs the (track,channel) identity so a note
        // from another owner closes/replaces the previous run, while WASMIDI's
        // keyboard uses the parser-assigned 16-slot palette stored in the
        // event color byte. Keeping the same palette slot here makes the
        // horizontal renderer and piano keyboard visually identical.
        const uint8_t ownerIndex = impl_->renderPerTrackColors
            ? static_cast<uint8_t>(((trackIndex & 0x0fu) << 4) | channel)
            : channel;
        const uint8_t colorByte = static_cast<uint8_t>((event.packed >> 24) & 0xffu);
        const uint8_t colorSlot = impl_->renderPerTrackColors
            ? static_cast<uint8_t>((colorByte >> 4) & 0x0fu)
            : static_cast<uint8_t>(colorByte & 0x0fu);
        const std::size_t headerIndex =
            (std::size_t(channel) << 7) | std::size_t(key);

        uint16_t count = impl_->renderActiveCounts[headerIndex];
        const uint8_t activeOwner = impl_->renderActiveColors[headerIndex];

        if (noteOn) {
            if (count != 0 && activeOwner != ownerIndex) {
                const uint32_t oldId = impl_->renderActiveIds[headerIndex];
                if (oldId != 0)
                    closes.push_back({oldId, event.tick});
                count = 0;
            }

            if (count == 0) {
                const uint32_t id = impl_->renderHead++;
                impl_->renderActiveIds[headerIndex] = id;
                impl_->renderActiveColors[headerIndex] = ownerIndex;
                appends.push_back({
                    event.tick,
                    0u,
                    uint32_t(velocity) |
                        (uint32_t(key) << 8) |
                        (uint32_t(colorSlot) << 16)
                });
            }

            // Deliberately uint16_t: SharpMIDI uses ushort for overlap count.
            count = static_cast<uint16_t>(count + 1u);
        } else {
            if (count > 0 && activeOwner == ownerIndex) {
                count = static_cast<uint16_t>(count - 1u);
                if (count == 0) {
                    const uint32_t id = impl_->renderActiveIds[headerIndex];
                    if (id != 0)
                        closes.push_back({id, event.tick});
                }
            }
        }

        impl_->renderActiveCounts[headerIndex] = count;
    }

    if (!impl_->renderCursor.peek(event, &trackIndex, &sourceOrder)) {
        complete = !impl_->renderCursor.failed();
        hasNextTick = false;
    } else {
        nextTick = event.tick;
        hasNextTick = true;
        if (event.tick > endTick)
            complete = true;
    }

    return !impl_->renderCursor.failed();
}

} // namespace wasmidi
