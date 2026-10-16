#include "bpfa_midi_store.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

#include <zstd.h>

namespace wasmidi {

namespace {

constexpr uint64_t CheckpointEventInterval = 256;
constexpr uint64_t SnapshotEventInterval = 131072;
constexpr std::size_t SnapshotMaxNotes = 65536;
constexpr uint64_t SnapshotBudgetNotes = 16ull * 1024ull * 1024ull;
#ifndef WASMIDI_BPFA_NOTE_STORE
#define WASMIDI_BPFA_NOTE_STORE 0
#endif
constexpr uint32_t NotesPerBlock = 1024;
constexpr std::size_t ReadBlock = 8u * 1024u * 1024u;
constexpr uint32_t DurationMask = 0x00000fffu;
constexpr uint32_t LongDuration = DurationMask;
constexpr uint32_t VelocityShift = 12;
constexpr uint32_t NoteShift = 19;
constexpr uint32_t ChannelShift = 27;
constexpr uint32_t PendingDuration = 0x80000000u;
constexpr std::size_t SourceBlockSize = 64u * 1024u;
constexpr std::size_t SourceSlabSize = 16u * 1024u * 1024u;
constexpr uint32_t SourceBlockCompressed = 0x80000000u;
constexpr uint32_t SourceBlockStoredSize = 0x7fffffffu;
constexpr std::size_t SourceCacheSlots = 128;

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

struct Decoder {
    const uint8_t* bytes;
    uint64_t position;
    uint64_t end;
    uint32_t tick = 0;
    uint8_t runningStatus = 0;
    bool finished = false;
    bool wantText = false; // index scan only: report lyric/marker/text metas
    const BpfaMidiStore* store = nullptr;
    const uint8_t* block = nullptr;
    uint64_t blockBegin = 0;
    uint64_t blockEnd = 0;
    uint16_t blockSlot = 0xffffu;
    uint8_t* own = nullptr;

    bool byte(uint8_t& v) {
        // One unsigned compare covers both window edges (HANDOFF sec. 49).
        if (block && position - blockBegin < blockEnd - blockBegin) {
            v = block[position - blockBegin];
            ++position;
            return true;
        }
        return refill(v);
    }
    bool refill(uint8_t& v) {
        if (position >= end) return false;
        if (bytes) {
            block = bytes;
            blockBegin = 0;
            blockEnd = end;
            blockSlot = 0xffffu;
        } else if (!store->sourceBlock(position, blockSlot, block, blockBegin, blockEnd, own)) {
            return false;
        }
        if (blockEnd > end) blockEnd = end;
        v = block[position - blockBegin];
        ++position;
        return true;
    }
    bool var(uint32_t& v) {
        v = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t b = 0;
            if (!byte(b)) return false;
            v = (v << 7) | (b & 0x7f);
            if (!(b & 0x80)) return true;
        }
        return false;
    }
};

enum class Kind { Channel, Tempo, SysEx, Text, End };

// BPFA ReadNextEvent, extended to report tempo payloads of length >= 3 and every
// SysEx (WASMIDI keeps them all; the synth decides what it supports).
// Contiguous source for the raw-mode track scan: no block refills (HANDOFF sec. 55).
struct RawDecoder {
    const uint8_t* bytes;
    uint64_t position;
    uint64_t end;
    uint32_t tick = 0;
    uint8_t runningStatus = 0;
    bool finished = false;
    bool wantText = false;
    const uint8_t* block = nullptr;

    bool byte(uint8_t& v) {
        if (position >= end) return false;
        v = bytes[position++];
        return true;
    }
    bool var(uint32_t& v) {
        v = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t b = 0;
            if (!byte(b)) return false;
            v = (v << 7) | (b & 0x7f);
            if (!(b & 0x80)) return true;
        }
        return false;
    }
};

template <class D>
inline __attribute__((always_inline)) Kind readEventImpl(D& d, uint8_t& status, uint8_t& d1, uint8_t& d2,
                                                         uint32_t& tempo, uint64_t& dataOffset, uint32_t& dataLength)
{
    while (!d.finished && d.position < d.end) {
        uint32_t delta = 0;
        if (!d.var(delta) || delta > std::numeric_limits<uint32_t>::max() - d.tick) break;
        d.tick += delta;
        uint8_t s = 0;
        if (!d.byte(s)) break;
        if (s < 0x80) {
            if (d.runningStatus < 0x80 || d.runningStatus >= 0xf0) break;
            --d.position;
            s = d.runningStatus;
        } else if (s < 0xf0) {
            d.runningStatus = s;
        }
        if (s <= 0xef) {
            const uint8_t type = s & 0xf0;
            d1 = 0;
            d2 = 0;
            if (!d.byte(d1) || (type != 0xc0 && type != 0xd0 && !d.byte(d2)) || d2 >= 0x80) break;
            status = s;
            return Kind::Channel;
        }
        if (s == 0xff) {
            uint8_t type = 0;
            uint32_t length = 0;
            if (!d.byte(type) || !d.var(length) || length > d.end - d.position) break;
            if (type == 0x2f) {
                d.position += length;
                break;
            }
            if (type == 0x51 && length >= 3) {
                uint8_t t0 = 0, t1 = 0, t2 = 0;
                if (!d.byte(t0) || !d.byte(t1) || !d.byte(t2)) break;
                tempo = (uint32_t(t0) << 16) | (uint32_t(t1) << 8) | t2;
                d.position += length - 3u;
                return Kind::Tempo;
            }
            if (d.wantText && length && (type == 0x01 || type == 0x05 || type == 0x06)) {
                status = type;
                dataOffset = d.position;
                dataLength = length;
                d.position += length;
                return Kind::Text;
            }
            d.position += length;
            continue;
        }
        if (s == 0xf0 || s == 0xf7) {
            uint32_t length = 0;
            if (!d.var(length) || length > d.end - d.position) break;
            status = s;
            dataOffset = d.position;
            dataLength = length;
            d.position += length;
            return Kind::SysEx;
        }
        uint64_t skip = 0;
        switch (s) {
        case 0xf1: case 0xf3: skip = 1; break;
        case 0xf2: skip = 2; break;
        case 0xf6: case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd: case 0xfe: break;
        default: d.finished = true; return Kind::End;
        }
        if (s < 0xf8) d.runningStatus = 0;
        if (skip > d.end - d.position) break;
        d.position += skip;
    }
    d.finished = true;
    return Kind::End;
}

inline __attribute__((always_inline)) Kind readEvent(Decoder& d, uint8_t& status, uint8_t& d1, uint8_t& d2,
                                                     uint32_t& tempo, uint64_t& dataOffset, uint32_t& dataLength)
{
    return readEventImpl(d, status, d1, d2, tempo, dataOffset, dataLength);
}

inline __attribute__((always_inline)) Kind readEvent(RawDecoder& d, uint8_t& status, uint8_t& d1, uint8_t& d2,
                                                     uint32_t& tempo, uint64_t& dataOffset, uint32_t& dataLength)
{
    return readEventImpl(d, status, d1, d2, tempo, dataOffset, dataLength);
}

// V8 has no on-stack replacement for wasm: a loop entered once stays in baseline
// (Liftoff) code. Long loops run as many short calls through an opaque pointer so
// the optimized tier takes over (HANDOFF sec. 58, same fix as worker_cycle, sec. 19).
using ChunkFn = bool (*)(void*);
template <class F>
bool callChunk(void* f) { return (*static_cast<F*>(f))(); }
template <class F>
void runChunked(F& f)
{
    ChunkFn volatile fn = &callChunk<F>;
    while (!fn(&f)) {}
}
constexpr uint32_t ScanChunkEvents = 1u << 18;

void buildTempoIndexLocal(MidiDocument& output)
{
    std::stable_sort(output.tempoMap.begin(), output.tempoMap.end(),
        [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });
    std::vector<TempoChange> deduped;
    deduped.reserve(output.tempoMap.size());
    for (const auto& tempo : output.tempoMap) {
        if (!deduped.empty() && deduped.back().tick == tempo.tick) deduped.back() = tempo;
        else deduped.push_back(tempo);
    }
    if (deduped.empty() || deduped.front().tick != 0) deduped.insert(deduped.begin(), {0, 500000});
    output.tempoMap.swap(deduped);
    output.tempoSeconds.resize(output.tempoMap.size());
    double seconds = 0.0;
    uint32_t previousTick = 0, previousUs = 500000;
    const double ppq = std::max<uint16_t>(1, output.ticksPerBeat);
    for (std::size_t i = 0; i < output.tempoMap.size(); ++i) {
        const auto& tempo = output.tempoMap[i];
        seconds += double(tempo.tick - previousTick) / ppq * (double(previousUs) / 1000000.0);
        output.tempoSeconds[i] = seconds;
        previousTick = tempo.tick;
        if (tempo.microsecondsPerBeat) previousUs = tempo.microsecondsPerBeat;
    }
}

bool isResetSysEx(const std::vector<uint8_t>& bytes)
{
    std::size_t a = 0, n = bytes.size();
    if (n && bytes[0] == 0xf0) { ++a; --n; }
    if (n && bytes[a + n - 1] == 0xf7) --n;
    auto d = [&](std::size_t i) { return bytes[a + i]; };
    if (n >= 4 && d(0) == 0x7e && d(2) == 0x09 && (d(3) == 0x01 || d(3) == 0x03)) return true;
    if (n >= 8 && d(0) == 0x41 && d(2) == 0x42 && d(3) == 0x12 && d(4) == 0x40 && d(5) == 0x00 &&
        d(6) == 0x7f && d(7) == 0x00) return true;
    return n >= 7 && d(0) == 0x43 && d(2) == 0x4c && d(3) == 0x00 && d(4) == 0x00 && d(5) == 0x7e &&
        d(6) == 0x00;
}

} // namespace


void BpfaMidiStore::clear()
{
    valid_ = false;
    error_.clear();
    metadata_ = MidiDocument{};
    std::vector<uint8_t>().swap(fileBytes_);
    sourceSlabs_.clear();
    std::vector<SourceChunk>().swap(sourceChunks_);
    storedSourceBytes_ = 0;
    sourceSize_ = 0;
    sourceCache_.clear();
    sourceCacheAge_ = 0;
    ingest_.reset();
    std::vector<TextEvent>().swap(textEvents_);
    std::vector<uint32_t>().swap(density_);
    densityBucketTicks_ = 1;
    chunkBase_ = 0;
    chunkKeys_.clear();
    std::vector<uint8_t>().swap(partialBlob_);
    tracks_.clear();
    stateEvents_.clear();
    sysex_.clear();
    std::vector<PackedNote>().swap(notes_);
    longNotes_.clear();
    noteBlocks_.clear();
    eventCursor_ = MergeCursor{};
    renderCursor_ = MergeCursor{};
    eventSysExCursor_ = 0;
    firstNotes_.clear();
    trackColors_.clear();
    renderHead_ = 1;
    visualCheckpoints_.clear();
    rollingVisualState_ = VisualState{};
    rollingVisualValid_ = false;
    rollingVisualTick_ = 0;
    liveState_ = VisualState{};
    liveCursor_ = MergeCursor{};
    liveCursorValid_ = false;
    liveTick_ = -1.0;
    liveDeferredOffs_.clear();
    liveNps_.clear();
    liveCc_.clear();
    liveNpsCount_ = liveCcCount_ = 0;
}

std::size_t BpfaMidiStore::memoryBytes() const
{
    std::size_t bytes = fileBytes_.capacity() + sourceSlabs_.size() * SourceSlabSize +
        sourceChunks_.capacity() * sizeof(SourceChunk) +
        sourceCache_.size() * (onDemand_ ? onDemandPageBytes_ : sourceBlockSize_) +
        notes_.capacity() * sizeof(PackedNote) +
        longNotes_.capacity() * sizeof(LongNote) + noteBlocks_.capacity() * sizeof(NoteBlock) +
        stateEvents_.capacity() * sizeof(SeekStateEvent);
    for (const auto& t : tracks_) {
        bytes += t.checkpoints.capacity() * sizeof(TrackCheckpoint);
        for (const auto& v : t.snapshots) bytes += sizeof(VisualSnapshot) + v.notes.capacity() * sizeof(SnapshotNote);
    }
    for (const auto& s : sysex_) bytes += s.data.capacity() + sizeof(SysExRef);
    return bytes;
}

bool BpfaMidiStore::index(uint64_t size, MidiReadAt readAt, void* readUser, MidiDocument& metadata,
                          MidiParseProgress progress, void* progressUser)
{
    clear();
    auto report = [&](int p, const char* stage) { if (progress) progress(progressUser, p, stage); };
    try {
        if (!readAt || size < 14) { error_ = "MIDI data is too short"; return false; }
        if (size > std::numeric_limits<std::size_t>::max()) { error_ = "MIDI file is too large"; return false; }

        // BPFA source blocks, streamed: the raw file is never resident (HANDOFF sec. 49).
        readAt_ = readAt;
        readUser_ = readUser;
        sourceSize_ = size;
        const std::size_t requestedPageBytes = onDemandRequestPageBytes_;
        if (onDemand_) {
            onDemandPageBytes_ = requestedPageBytes ? requestedPageBytes : 64u * 1024u;
            onDemandPages_ = requestedPageBytes && onDemandRequestPages_ ? onDemandRequestPages_ : 64u;
            sourceCache_.assign(onDemandPages_, CacheSlot{});
        } else if (size <= rawSourceLimit_ && !partialActive_) {
            fileBytes_.resize(static_cast<std::size_t>(size));
            for (uint64_t done = 0; done < size;) {
                const std::size_t amount = static_cast<std::size_t>(std::min<uint64_t>(ReadBlock, size - done));
                if (!readAt(readUser, done, fileBytes_.data() + done, amount)) {
                    error_ = "Could not read MIDI source";
                    return false;
                }
                done += amount;
                if (progress)
                    progress(progressUser, int(1 + 39.0 * double(done) / double(size)), "BPFA: reading source");
            }
        } else {
            beginIngest(size, readAt, readUser, progress, progressUser);
        }
        MidiDocument out;
        out.remoteIndexed = true;
        out.minPitch = 127;
        out.maxPitch = 0;
        uint64_t position = 0;
        uint16_t trackBegin = 0, trackEnd = 0;
        if (partialActive_) {
            out.format = partial_.format;
            out.trackCount = partial_.trackCount;
            out.ticksPerBeat = partial_.ticksPerBeat;
            position = partial_.firstChunkPosition;
            trackBegin = partial_.firstTrack;
            trackEnd = partial_.endTrack;
        } else {
            uint8_t header[14];
            if (!copySource(0, header, sizeof(header)) || std::memcmp(header, "MThd", 4) != 0) {
                error_ = "Missing MThd header";
                return false;
            }
            const uint32_t headerSize = be32(header + 4);
            if (headerSize < 6 || 8ull + headerSize > size) { error_ = "Invalid MIDI header"; return false; }
            out.format = be16(header + 8);
            out.trackCount = be16(header + 10);
            out.ticksPerBeat = be16(header + 12);
            position = 8ull + headerSize;
            trackEnd = out.trackCount;
        }
        if (out.format > 1) { error_ = "Only MIDI Format 0 and 1 are supported"; return false; }
        densityBucketTicks_ = std::max<uint32_t>(1u, uint32_t(out.ticksPerBeat) / 8u);
        if (out.ticksPerBeat == 0 || (out.ticksPerBeat & 0x8000)) { error_ = "SMPTE timing is not supported"; return false; }

        tracks_.resize(out.trackCount);
        firstNotes_.assign(out.trackCount, FirstNote{});

        if (onDemand_ && requestedPageBytes == 0) {
            const std::size_t budget = 64u * 1024u * 1024u;
            const std::size_t perTrack = budget / std::max<std::size_t>(1, 2u * out.trackCount);
            std::size_t page = 1u << 20;
            while (page > 64u * 1024u && page > perTrack) page >>= 1;
            onDemandPageBytes_ = page;
            onDemandPages_ = std::max<std::size_t>(2u * out.trackCount, budget / page);
            sourceCache_.assign(onDemandPages_, CacheSlot{});
        }
        out.activeChannelMasks.assign(out.trackCount, 0);
        out.tempoMap.push_back({0, 500000});
        std::vector<uint64_t> notesPerTrack(out.trackCount, 0);
        uint64_t totalNotes = 0, totalControls = 0;

        // BPFA ScanTrack: checkpoints, tempo, sparse state events, SysEx, counts.
        // Open notes per channel+key as FIFO lists over a recycled node pool (the
        // pairing structure of Kimrashi's packer; HANDOFF sec. 52).
        struct OpenNode { SnapshotNote note; uint32_t next; };
        constexpr uint32_t NoNode = 0xffffffffu;
        std::vector<OpenNode> openPool;
        uint32_t openFree = NoNode;
        std::array<uint32_t, 16u * 128u> openHead{}, openTail{};
        uint64_t snapshotNotesUsed = 0;
        const uint64_t snapshotBudget = partialActive_ ? partial_.snapshotBudget : SnapshotBudgetNotes;
        for (uint16_t t = trackBegin; t < trackEnd; ++t) {
            {
                // Chunk headers are read in file order so the single pass never jumps ahead.
                uint8_t chunk[8];
                if (size - position < 8 || !copySource(position, chunk, 8) || std::memcmp(chunk, "MTrk", 4) != 0) {
                    error_ = "Invalid or truncated MTrk chunk";
                    return false;
                }
                const uint32_t length = be32(chunk + 4);
                if (uint64_t(length) > size - position - 8) { error_ = "Truncated MIDI track"; return false; }
                tracks_[t].byteBegin = position + 8;
                tracks_[t].byteEnd = position + 8 + length;
                position += 8ull + length;
            }
            TrackStorage& storage = tracks_[t];
            storage.checkpoints.push_back({storage.byteBegin, 0, 0, 0});
            uint64_t events = 0, order = 0;
            FirstNote& first = firstNotes_[t];
            first.closeOrder.fill(std::numeric_limits<uint64_t>::max());
            std::array<uint32_t, 16u * 128u> activeCounts{};
            std::array<uint8_t, 16> firstPitch{};
            uint16_t firstClosedMask = 0;
            uint64_t closureOrder = 0;
            openPool.clear();
            openFree = NoNode;
            openHead.fill(NoNode);
            openTail.fill(NoNode);
            std::size_t openCount = 0;
            uint8_t status = 0, d1 = 0, d2 = 0;
            uint32_t tempo = 0, dataLength = 0;
            uint64_t dataOffset = 0;
            uint32_t finalTick = 0;
            auto scanEvents = [&](auto& d) -> bool {
            for (uint32_t chunkEvents = 0; chunkEvents < ScanChunkEvents; ++chunkEvents) {
                const Kind kind = readEvent(d, status, d1, d2, tempo, dataOffset, dataLength);
                if (kind == Kind::End) { finalTick = d.tick; return true; }
                if (kind == Kind::Tempo) {
                    out.tempoMap.push_back({d.tick, tempo});
                    ++order;
                    continue;
                }
                if (kind == Kind::Text) {
                    // Lyrics, markers and text for WASMIDI Audio (HANDOFF sec. 67). Not
                    // counted in `order`: the other decoders never see these events.
                    if (textEvents_.size() < MaxTextEvents) {
                        TextEvent te;
                        te.tick = d.tick;
                        te.track = t;
                        te.type = status;
                        te.text.resize(std::min<uint32_t>(dataLength, MaxTextBytes));
                        if (!te.text.empty() && !copySource(dataOffset, reinterpret_cast<uint8_t*>(&te.text[0]), te.text.size())) {
                            finalTick = d.tick;
                            return true;
                        }
                        d.block = nullptr;
                        textEvents_.push_back(std::move(te));
                    }
                    continue;
                }
                if (kind == Kind::SysEx) {
                    SysExRef sx;
                    sx.tick = d.tick;
                    sx.track = t;
                    sx.order = order++;
                    sx.data.resize(std::size_t(dataLength) + 1u);
                    sx.data[0] = status;
                    if (dataLength && !copySource(dataOffset, sx.data.data() + 1, dataLength)) { finalTick = d.tick; return true; }
                    d.block = nullptr;
                    sysex_.push_back(std::move(sx));
                    continue;
                }
                const uint8_t type = status & 0xf0, channel = status & 0x0f;
                const uint32_t message = status | (uint32_t(d1) << 8) | (uint32_t(d2) << 16);
                if (type == 0xc0 || type == 0xe0 || (type == 0xb0 && (d1 == 0 || d1 == 32)))
                    stateEvents_.push_back({d.tick, t, uint32_t(order), message});
                {
                    const bool noteOn = type == 0x90 && d2 != 0;
                    const bool noteOff = type == 0x80 || (type == 0x90 && d2 == 0);
                    const uint16_t vkey = uint16_t((uint16_t(channel) << 7) | (d1 & 0x7f));
                    const uint16_t bit = uint16_t(1u << channel);
                    if (noteOn) {
                        uint32_t node = openFree;
                        if (node != NoNode) {
                            openFree = openPool[node].next;
                        } else {
                            node = uint32_t(openPool.size());
                            openPool.push_back({});
                        }
                        openPool[node] = {{d.tick, uint32_t(std::min<uint64_t>(events, 0xffffffffu)), vkey, d2, 0}, NoNode};
                        if (openTail[vkey] == NoNode) openHead[vkey] = node;
                        else openPool[openTail[vkey]].next = node;
                        openTail[vkey] = node;
                        ++openCount;
                    } else if (noteOff && openHead[vkey] != NoNode) {
                        const uint32_t node = openHead[vkey];
                        openHead[vkey] = openPool[node].next;
                        if (openHead[vkey] == NoNode) openTail[vkey] = NoNode;
                        openPool[node].next = openFree;
                        openFree = node;
                        --openCount;
                    }
                    if (noteOn) {
                        if (activeCounts[vkey] != std::numeric_limits<uint32_t>::max()) ++activeCounts[vkey];
                        if ((first.seenMask & bit) == 0) {
                            first.seenMask |= bit;
                            first.tick[channel] = d.tick;
                            first.order[channel] = events;
                            firstPitch[channel] = d1 & 0x7f;
                        }
                    } else if (noteOff && activeCounts[vkey] != 0) {
                        if ((first.seenMask & bit) && !(firstClosedMask & bit) && firstPitch[channel] == (d1 & 0x7f)) {
                            first.closeOrder[channel] = closureOrder;
                            firstClosedMask |= bit;
                        }
                        --activeCounts[vkey];
                        ++closureOrder;
                    }
                }
                ++order;
                ++events;
                if ((events % CheckpointEventInterval) == 0)
                    storage.checkpoints.push_back({d.position, events, d.tick, d.runningStatus});
                if ((events % SnapshotEventInterval) == 0 && openCount <= SnapshotMaxNotes &&
                    snapshotNotesUsed + openCount <= snapshotBudget) {
                    snapshotNotesUsed += openCount;
                    VisualSnapshot snapshot;
                    snapshot.resume = {d.position, events, d.tick, d.runningStatus};
                    snapshot.notes.reserve(openCount);
                    for (uint32_t key = 0; key < openHead.size(); ++key)
                        for (uint32_t node = openHead[key]; node != NoNode; node = openPool[node].next)
                            snapshot.notes.push_back(openPool[node].note);
                    storage.snapshots.push_back(std::move(snapshot));
                }
                out.activeChannelMasks[t] |= (1u << channel);
                if (type == 0x90 && d2 != 0) {
                    ++notesPerTrack[t];
                    ++totalNotes;
                    {
                        const uint32_t bucket = d.tick / densityBucketTicks_;
                        if (bucket >= density_.size()) density_.resize(std::size_t(bucket) + 1024u, 0u);
                        ++density_[bucket];
                    }
                    out.hasPitch = true;
                    out.minPitch = std::min(out.minPitch, d1);
                    out.maxPitch = std::max(out.maxPitch, d1);
                } else if (type == 0xb0 || type == 0xc0 || type == 0xd0 || type == 0xe0) {
                    ++totalControls;
                }
            }
            finalTick = d.tick;
            return false;
            };
            if (!fileBytes_.empty()) {
                RawDecoder d{fileBytes_.data(), storage.byteBegin, storage.byteEnd};
                d.wantText = true;
                auto chunk = [&]() { return scanEvents(d); };
                runChunked(chunk);
            } else {
                Decoder d{nullptr, storage.byteBegin, storage.byteEnd};
                d.store = this;
                d.wantText = true;
                auto chunk = [&]() { return scanEvents(d); };
                runChunked(chunk);
            }
            for (uint16_t key = 0; key < activeCounts.size(); ++key) {
                const uint32_t count = activeCounts[key];
                if (!count) continue;
                const uint8_t ch = uint8_t((key >> 7) & 0x0f), pitch = uint8_t(key & 0x7f);
                const uint16_t bit = uint16_t(1u << ch);
                if ((first.seenMask & bit) && !(firstClosedMask & bit) && firstPitch[ch] == pitch) {
                    first.closeOrder[ch] = closureOrder;
                    firstClosedMask |= bit;
                }
                closureOrder += count;
            }
            storage.maxTick = finalTick;
            out.maxTick = std::max(out.maxTick, finalTick);
            if (!ingest_)
                report(40 + int(30.0 * double(t + 1) / double(std::max<int>(1, out.trackCount))), "BPFA: track scan");
        }
        if (!finishIngest()) {
            if (error_.empty()) error_ = "Could not read MIDI source";
            return false;
        }
        if (partialActive_) {
            exportPartial(out, notesPerTrack, totalControls);
            return true;
        }

#if WASMIDI_BPFA_NOTE_STORE
        // BPFA note store: 8-byte records, patched in place at the note-off
        // (per channel/key stack, newest first), long notes in a side table.
        notes_.resize(static_cast<std::size_t>(totalNotes));
        uint64_t write = 0;
        for (uint16_t t = 0; t < out.trackCount; ++t) {
            const TrackStorage& storage = tracks_[t];
            const uint64_t trackBegin = write;
            std::vector<std::vector<uint32_t>> active(16 * 256);
            Decoder d{nullptr, storage.byteBegin, storage.byteEnd};
            d.store = this;
            uint8_t status = 0, d1 = 0, d2 = 0;
            uint32_t tempo = 0, dataLength = 0;
            uint64_t dataOffset = 0;
            auto finish = [&](uint32_t index, uint32_t endTick) {
                PackedNote& note = notes_[index];
                const uint32_t duration = endTick - note.startTick;
                note.durationAndKey &= ~PendingDuration;
                if (duration < LongDuration) {
                    note.durationAndKey = (note.durationAndKey & ~DurationMask) | duration;
                } else {
                    note.durationAndKey = (note.durationAndKey & ~DurationMask) | LongDuration;
                    longNotes_.push_back({index, endTick});
                }
            };
            for (;;) {
                const Kind kind = readEvent(d, status, d1, d2, tempo, dataOffset, dataLength);
                if (kind == Kind::End) break;
                if (kind != Kind::Channel) continue;
                const uint8_t type = status & 0xf0, channel = status & 0x0f;
                const bool on = type == 0x90 && d2 != 0;
                const bool off = type == 0x80 || (type == 0x90 && d2 == 0);
                if (!on && !off) continue;
                auto& stack = active[channel * 256 + d1];
                if (on) {
                    if (write >= notes_.size()) break;
                    notes_[write] = {d.tick, PendingDuration | (uint32_t(d2) << VelocityShift) |
                                                 (uint32_t(d1) << NoteShift) | (uint32_t(channel) << ChannelShift)};
                    stack.push_back(uint32_t(write++));
                } else if (!stack.empty()) {
                    const uint32_t index = stack.back();
                    stack.pop_back();
                    finish(index, d.tick);
                }
            }
            for (auto& stack : active)
                for (uint32_t index : stack) finish(index, storage.maxTick);
            (void)trackBegin;
            report(70 + int(25.0 * double(t + 1) / double(std::max<int>(1, out.trackCount))), "BPFA: note store");
        }
        std::sort(longNotes_.begin(), longNotes_.end(),
                  [](const LongNote& a, const LongNote& b) { return a.noteIndex < b.noteIndex; });
        trackNoteBegin_.assign(notesPerTrack.size() + 1, 0);
        for (std::size_t t = 0; t < notesPerTrack.size(); ++t)
            trackNoteBegin_[t + 1] = trackNoteBegin_[t] + notesPerTrack[t];
        buildNoteBlocks();
#endif

        finalizeIndex(out, totalNotes, totalControls, metadata);
        report(100, "BPFA MIDI ready");
        return true;
    } catch (const std::bad_alloc&) {
        clear();
        error_ = "BPFA MIDI store exhausted browser memory";
        return false;
    } catch (...) {
        clear();
        error_ = "BPFA MIDI load failed";
        return false;
    }
}

void BpfaMidiStore::buildNoteBlocks()
{
    noteBlocks_.clear();
    auto endTick = [&](uint32_t index) {
        const PackedNote& n = notes_[index];
        const uint32_t duration = n.durationAndKey & DurationMask;
        if (duration != LongDuration) return n.startTick + duration;
        const auto it = std::lower_bound(longNotes_.begin(), longNotes_.end(), index,
            [](const LongNote& e, uint32_t i) { return e.noteIndex < i; });
        return it != longNotes_.end() && it->noteIndex == index ? it->endTick : n.startTick;
    };
    for (std::size_t t = 0; t + 1 < trackNoteBegin_.size(); ++t) {
        for (uint64_t begin = trackNoteBegin_[t]; begin < trackNoteBegin_[t + 1]; begin += NotesPerBlock) {
            const uint32_t count = uint32_t(std::min<uint64_t>(NotesPerBlock, trackNoteBegin_[t + 1] - begin));
            uint32_t maximumEnd = 0;
            for (uint32_t i = 0; i < count; ++i) maximumEnd = std::max(maximumEnd, endTick(uint32_t(begin + i)));
            noteBlocks_.push_back({notes_[begin].startTick, maximumEnd, uint32_t(begin), uint16_t(count), 0});
        }
    }
}

void BpfaMidiStore::buildColorTables()
{
    globalColors_.fill(0);
    std::array<int8_t, 16> assigned{};
    assigned.fill(-1);
    int next = 0;
    for (uint32_t mask : metadata_.activeChannelMasks)
        for (int ch = 0; ch < 16; ++ch)
            if ((mask & (1u << ch)) && assigned[ch] < 0) assigned[ch] = int8_t(next++ & 15);
    for (int ch = 0; ch < 16; ++ch) globalColors_[ch] = assigned[ch] >= 0 ? uint8_t(assigned[ch]) : uint8_t(ch);
    trackColors_.assign(metadata_.trackCount, {});
    for (auto& colors : trackColors_)
        for (int ch = 0; ch < 16; ++ch) colors[ch] = uint8_t(ch);
    struct Appearance { uint32_t tick; uint32_t track; uint64_t order; uint64_t closeOrder; uint8_t channel; };
    std::vector<Appearance> list;
    for (uint32_t t = 0; t < firstNotes_.size(); ++t)
        for (uint8_t ch = 0; ch < 16; ++ch)
            if (firstNotes_[t].seenMask & uint16_t(1u << ch))
                list.push_back({firstNotes_[t].tick[ch], t, firstNotes_[t].order[ch], firstNotes_[t].closeOrder[ch], ch});
    std::stable_sort(list.begin(), list.end(), [](const Appearance& a, const Appearance& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.track != b.track) return a.track < b.track;
        if (a.closeOrder != b.closeOrder) return a.closeOrder < b.closeOrder;
        return a.order < b.order;
    });
    next = 0;
    for (const Appearance& a : list) trackColors_[a.track][a.channel] = uint8_t(next++ & 15);
}

bool BpfaMidiStore::decodeNext(Cursor& c, bool notesOnly) const
{
    Decoder d{fileBytes_.empty() ? nullptr : fileBytes_.data(), c.position, c.end, c.tick, c.runningStatus,
              c.finished};
    d.store = this;
    if (compressedSource()) {
        if (!c.own) c.own.reset(new uint8_t[sourceBlockSize_]);
        d.own = c.own.get();
    }
    if (c.block && sourceSlotValid(c.blockSlot, c.blockBegin)) {
        d.block = c.block;
        d.blockBegin = c.blockBegin;
        d.blockEnd = c.blockEnd;
        d.blockSlot = c.blockSlot;
    }
    struct Save {
        Cursor& c;
        Decoder& d;
        ~Save() { c.block = d.block; c.blockBegin = d.blockBegin; c.blockEnd = d.blockEnd; c.blockSlot = d.blockSlot; }
    } save{c, d};
    uint8_t status = 0, d1 = 0, d2 = 0;
    uint32_t tempo = 0, dataLength = 0;
    uint64_t dataOffset = 0;
    for (;;) {
        const Kind kind = readEvent(d, status, d1, d2, tempo, dataOffset, dataLength);
        if (kind == Kind::End) {
            c.position = d.position; c.tick = d.tick; c.runningStatus = d.runningStatus; c.finished = true;
            return false;
        }
        if (kind != Kind::Channel) continue;
        const uint64_t index = c.nextIndex++;
        const uint8_t type = status & 0xf0;
        if (notesOnly && type != 0x80 && type != 0x90) continue;
        c.eventIndex = index;
        uint8_t st = status, v2 = d2;
        if (type == 0x90 && d2 == 0) { st = uint8_t(0x80 | (status & 0x0f)); v2 = 64; }
        c.position = d.position; c.tick = d.tick; c.runningStatus = d.runningStatus; c.finished = false;
        c.eventTick = d.tick;
        c.eventMessage = st | (uint32_t(d1) << 8) | (uint32_t(v2) << 16);
        return true;
    }
}

void BpfaMidiStore::mergeReset(MergeCursor& m, uint32_t startTick, bool notesOnly)
{
    m.notesOnly = notesOnly;
    m.heap.clear();
    m.cursors.clear();
    m.cursors.resize(tracks_.size());
    auto less = [&m](uint16_t a, uint16_t b) {
        const uint32_t ta = m.cursors[a].eventTick, tb = m.cursors[b].eventTick;
        return ta < tb || (ta == tb && a < b);
    };
    for (uint16_t t = 0; t < tracks_.size(); ++t) {
        const TrackStorage& storage = tracks_[t];
        const auto& cps = storage.checkpoints;
        auto it = std::lower_bound(cps.begin(), cps.end(), startTick,
            [](const TrackCheckpoint& cp, uint32_t tick) { return cp.tick < tick; });
        const TrackCheckpoint& cp = it == cps.begin() ? cps.front() : *(it - 1);
        Cursor& c = m.cursors[t];
        c.position = cp.byteOffset;
        c.end = storage.byteEnd;
        c.tick = cp.tick;
        c.runningStatus = cp.runningStatus;
        c.finished = false;
        c.nextIndex = cp.eventIndex;
        bool ok = decodeNext(c, notesOnly);
        while (ok && c.eventTick < startTick) ok = decodeNext(c, notesOnly);
        if (!ok) continue;
        m.heap.push_back(t);
        std::size_t i = m.heap.size() - 1;
        while (i > 0) {
            const std::size_t p = (i - 1) / 2;
            if (!less(m.heap[i], m.heap[p])) break;
            std::swap(m.heap[i], m.heap[p]);
            i = p;
        }
    }
    m.valid = true;
}

bool BpfaMidiStore::mergePeek(const MergeCursor& m, uint32_t& tick, uint32_t& message, uint16_t& track) const
{
    if (m.heap.empty()) return false;
    track = m.heap.front();
    tick = m.cursors[track].eventTick;
    message = m.cursors[track].eventMessage;
    return true;
}

void BpfaMidiStore::mergePop(MergeCursor& m)
{
    auto less = [&m](uint16_t a, uint16_t b) {
        const uint32_t ta = m.cursors[a].eventTick, tb = m.cursors[b].eventTick;
        return ta < tb || (ta == tb && a < b);
    };
    const uint16_t top = m.heap.front();
    if (!decodeNext(m.cursors[top], m.notesOnly)) {
        m.heap.front() = m.heap.back();
        m.heap.pop_back();
        if (m.heap.empty()) return;
    }
    std::size_t i = 0;
    for (;;) {
        const std::size_t l = 2 * i + 1, r = l + 1;
        std::size_t best = i;
        if (l < m.heap.size() && less(m.heap[l], m.heap[best])) best = l;
        if (r < m.heap.size() && less(m.heap[r], m.heap[best])) best = r;
        if (best == i) break;
        std::swap(m.heap[i], m.heap[best]);
        i = best;
    }
}

void BpfaMidiStore::resetEventCursor(uint32_t startTick)
{
    eventSysExCursor_ = 0;
    eventCursor_.valid = false;
    if (!valid_) return;
    startTick = std::min(startTick, metadata_.maxTick);
    mergeReset(eventCursor_, startTick, false);
    eventSysExCursor_ = static_cast<std::size_t>(std::lower_bound(sysex_.begin(), sysex_.end(), startTick,
        [](const SysExRef& e, uint32_t tick) { return e.tick < tick; }) - sysex_.begin());
}

bool BpfaMidiStore::buildEventBatch(uint32_t endTick, std::size_t maxEvents, std::vector<EventWord>& output,
                                    std::vector<SysExBatchEvent>& sysExEvents, std::vector<uint8_t>& sysExBytes,
                                    bool& complete, uint32_t& nextTick, bool& hasNextTick)
{
    output.clear();
    sysExEvents.clear();
    sysExBytes.clear();
    complete = false;
    nextTick = 0;
    hasNextTick = false;
    if (!valid_ || !eventCursor_.valid) return false;
    maxEvents = std::max<std::size_t>(1, maxEvents);
    endTick = std::min(endTick, metadata_.maxTick);
    const std::size_t sourceBudget =
        maxEvents > std::numeric_limits<std::size_t>::max() / 8 ? maxEvents : maxEvents * 8;
    std::size_t consumed = 0;
    bool doneForRequest = false;
    while (output.size() < maxEvents && consumed < sourceBudget) {
        uint32_t tick = 0, midi = 0;
        uint16_t track = 0;
        if (!mergePeek(eventCursor_, tick, midi, track)) { doneForRequest = true; break; }
        if (tick > endTick) { doneForRequest = true; break; }
        mergePop(eventCursor_);
        ++consumed;
        if ((midi & 0xf0u) == 0x90u && ((midi >> 16) & 0x7fu) != 0 && !output.empty()) {
            EventWord& previous = output.back();
            if (previous.tick == tick && (previous.packed & 0x00ffffffu) == midi) {
                const uint32_t encoded = (previous.packed >> 24) & 0xffu;
                if (encoded < 0xffu) {
                    previous.packed = midi | ((encoded + 1u) << 24);
                    continue;
                }
            }
        }
        output.push_back(EventWord{tick, midi});
    }
    {
        uint32_t tick = 0, midi = 0;
        uint16_t track = 0;
        if (!mergePeek(eventCursor_, tick, midi, track)) {
            complete = true;
            hasNextTick = false;
        } else {
            nextTick = tick;
            hasNextTick = true;
            if (nextTick > endTick) complete = true;
        }
    }
    if (doneForRequest) complete = true;

    const uint64_t safeExclusive =
        complete ? uint64_t(endTick) + 1u : (hasNextTick ? uint64_t(nextTick) : uint64_t(endTick) + 1u);
    while (eventSysExCursor_ < sysex_.size() && uint64_t(sysex_[eventSysExCursor_].tick) < safeExclusive) {
        const SysExRef& sx = sysex_[eventSysExCursor_++];
        if (sx.data.empty()) continue;
        SysExBatchEvent e;
        e.tick = sx.tick;
        e.offset = static_cast<uint32_t>(sysExBytes.size());
        e.length = static_cast<uint32_t>(sx.data.size());
        sysExBytes.insert(sysExBytes.end(), sx.data.begin(), sx.data.end());
        sysExEvents.push_back(e);
    }
    return true;
}

bool BpfaMidiStore::buildHistoricalSysEx(uint32_t startTick, std::vector<SysExBatchEvent>& sysExEvents,
                                         std::vector<uint8_t>& sysExBytes)
{
    sysExEvents.clear();
    sysExBytes.clear();
    if (!valid_) return false;
    for (const SysExRef& sx : sysex_) {
        if (sx.tick >= startTick) break;
        if (sx.data.empty()) continue;
        SysExBatchEvent e;
        e.tick = sx.tick;
        e.offset = static_cast<uint32_t>(sysExBytes.size());
        e.length = static_cast<uint32_t>(sx.data.size());
        sysExBytes.insert(sysExBytes.end(), sx.data.begin(), sx.data.end());
        sysExEvents.push_back(e);
    }
    return true;
}

bool BpfaMidiStore::buildHistoricalSelectorState(uint32_t startTick, std::vector<EventWord>& output)
{
    output.clear();
    if (!valid_) return false;
    bool haveReset = false;
    uint32_t resetTick = 0;
    uint16_t resetTrack = 0;
    uint64_t resetOrder = 0;
    for (const SysExRef& sx : sysex_) {
        if (sx.tick >= startTick) break;
        if (isResetSysEx(sx.data)) { haveReset = true; resetTick = sx.tick; resetTrack = sx.track; resetOrder = sx.order; }
    }
    struct State { uint8_t pm = 0, pl = 0, am = 0, al = 0, prog = 0; uint16_t bend = 8192; bool spm = false, spl = false, sp = false, sb = false; };
    std::array<State, 16> st{};
    for (const SeekStateEvent& e : stateEvents_) {
        if (e.tick >= startTick) break;
        if (haveReset) {
            const bool after = e.tick != resetTick ? e.tick > resetTick
                : (e.track != resetTrack ? e.track > resetTrack : uint64_t(e.order) > resetOrder);
            if (!after) continue;
        }
        const uint32_t m = e.message;
        const uint8_t cmd = m & 0xf0;
        State& cs = st[m & 0x0f];
        const uint8_t v1 = (m >> 8) & 0x7f, v2 = (m >> 16) & 0x7f;
        if (cmd == 0xb0) {
            if (v1 == 0) { cs.pm = v2; cs.spm = true; }
            else if (v1 == 32) { cs.pl = v2; cs.spl = true; }
        } else if (cmd == 0xc0) {
            cs.prog = v1; cs.am = cs.pm; cs.al = cs.pl; cs.sp = true;
        } else if (cmd == 0xe0) {
            cs.bend = uint16_t(v1 | (v2 << 7)); cs.sb = true;
        }
    }
    auto emit = [&](uint8_t s, uint8_t a, uint8_t b) {
        output.push_back(EventWord{startTick, uint32_t(s) | (uint32_t(a) << 8) | (uint32_t(b) << 16)});
    };
    for (uint8_t c = 0; c < 16; ++c) {
        const State& cs = st[c];
        if (cs.sp) {
            if (cs.am != 0) emit(0xb0 | c, 0, cs.am);
            if (cs.al != 0) emit(0xb0 | c, 32, cs.al);
            emit(0xc0 | c, cs.prog, 0);
            if (cs.spm && cs.pm != cs.am) emit(0xb0 | c, 0, cs.pm);
            if (cs.spl && cs.pl != cs.al) emit(0xb0 | c, 32, cs.pl);
        } else {
            if (cs.spm) emit(0xb0 | c, 0, cs.pm);
            if (cs.spl) emit(0xb0 | c, 32, cs.pl);
        }
        if (cs.sb && cs.bend != 8192) emit(0xe0 | c, cs.bend & 0x7f, (cs.bend >> 7) & 0x7f);
    }
    return true;
}

void BpfaMidiStore::resetRenderCursor(uint32_t startTick, bool perTrackColors)
{
    renderHead_ = 1;
    renderPerTrackColors_ = perTrackColors;
    renderActiveCounts_.fill(0);
    renderActiveColors_.fill(0);
    renderActiveIds_.fill(0);
    renderCursor_.valid = false;
    if (!valid_) return;
    mergeReset(renderCursor_, std::min(startTick, metadata_.maxTick), true);
}

bool BpfaMidiStore::buildRenderSweep(uint32_t endTick, std::size_t maxSourceEvents, std::vector<VisualNote>& appends,
                                     std::vector<RenderClose>& closes, uint32_t& appendBase, bool& complete,
                                     uint32_t& nextTick, bool& hasNextTick)
{
    appends.clear();
    closes.clear();
    appendBase = renderHead_;
    complete = false;
    nextTick = 0;
    hasNextTick = false;
    if (!valid_ || !renderCursor_.valid) return false;
    endTick = std::min(endTick, metadata_.maxTick);
    maxSourceEvents = std::max<std::size_t>(1, maxSourceEvents);
    std::size_t consumed = 0;
    uint32_t tick = 0, message = 0;
    uint16_t track = 0;
    while (consumed < maxSourceEvents) {
        if (!mergePeek(renderCursor_, tick, message, track)) { complete = true; return true; }
        nextTick = tick;
        hasNextTick = true;
        if (tick > endTick) { complete = true; return true; }
        mergePop(renderCursor_);
        ++consumed;
        const uint8_t status = uint8_t(message & 0xffu), command = uint8_t(status & 0xf0u);
        if (command != 0x90u && command != 0x80u) continue;
        const uint8_t channel = uint8_t(status & 0x0fu);
        const uint8_t key = uint8_t((message >> 8) & 0x7fu);
        const uint8_t velocity = uint8_t((message >> 16) & 0x7fu);
        const bool noteOn = command == 0x90u && velocity != 0;
        const uint8_t colorByte = uint8_t((globalColors_[channel] & 0x0f) | ((trackColors_[track][channel] & 0x0f) << 4));
        const uint8_t ownerIndex = renderPerTrackColors_ ? uint8_t(((track & 0x0fu) << 4) | channel) : channel;
        const uint8_t colorSlot = renderPerTrackColors_ ? uint8_t((colorByte >> 4) & 0x0fu) : uint8_t(colorByte & 0x0fu);
        const std::size_t headerIndex = (std::size_t(channel) << 7) | std::size_t(key);
        uint16_t count = renderActiveCounts_[headerIndex];
        const uint8_t activeOwner = renderActiveColors_[headerIndex];
        if (noteOn) {
            if (count != 0 && activeOwner != ownerIndex) {
                const uint32_t oldId = renderActiveIds_[headerIndex];
                if (oldId != 0) closes.push_back({oldId, tick});
                count = 0;
            }
            if (count == 0) {
                const uint32_t id = renderHead_++;
                renderActiveIds_[headerIndex] = id;
                renderActiveColors_[headerIndex] = ownerIndex;
                appends.push_back({tick, 0u, uint32_t(velocity) | (uint32_t(key) << 8) | (uint32_t(colorSlot) << 16)});
            }
            count = uint16_t(count + 1u);
        } else if (count > 0 && activeOwner == ownerIndex) {
            count = uint16_t(count - 1u);
            if (count == 0) {
                const uint32_t id = renderActiveIds_[headerIndex];
                if (id != 0) closes.push_back({id, tick});
            }
        }
        renderActiveCounts_[headerIndex] = count;
    }
    if (!mergePeek(renderCursor_, tick, message, track)) {
        complete = true;
        hasNextTick = false;
    } else {
        nextTick = tick;
        hasNextTick = true;
        if (tick > endTick) complete = true;
    }
    return true;
}

uint8_t BpfaMidiStore::colorByte(uint16_t track, uint8_t channel) const
{
    return uint8_t((globalColors_[channel] & 0x0f) | ((trackColors_[track][channel] & 0x0f) << 4));
}

void BpfaMidiStore::applyVisualEvent(VisualState& state, uint32_t tick, uint32_t message, uint16_t track,
                                     uint64_t order, std::vector<PageItem>* output, OutputIndices* outputIndices) const
{
    const uint8_t status = uint8_t(message & 0xffu), command = status & 0xf0;
    if (command != 0x90 && command != 0x80) return;
    const uint8_t channel = status & 0x0f;
    const uint8_t pitch = uint8_t((message >> 8) & 0x7fu);
    const uint8_t velocity = uint8_t((message >> 16) & 0x7fu);
    const uint8_t color = colorByte(track, channel);
    const uint32_t key = (uint32_t(track) << 11) | (uint32_t(channel) << 7) | uint32_t(pitch);
    if (command == 0x90 && velocity != 0) {
        state.pending[key].push_back({tick, velocity, color, order});
        if (output && outputIndices) {
            const std::size_t index = output->size();
            PageItem item;
            item.note = {tick, 0u, uint32_t(velocity & 0x7f) | (uint32_t(pitch & 0x7f) << 8) |
                                       (uint32_t(color & 0x0f) << 16) | (uint32_t((color >> 4) & 0x0f) << 20) |
                                       (uint32_t(track & 0xffu) << 24)};
            item.track = track;
            item.openOrder = order;
            output->push_back(item);
            (*outputIndices)[key].push_back(index);
        }
        return;
    }
    auto active = state.pending.find(key);
    if (active == state.pending.end() || active->second.empty()) return;
    active->second.pop_front();
    if (output && outputIndices) {
        auto oi = outputIndices->find(key);
        if (oi != outputIndices->end() && !oi->second.empty()) {
            const std::size_t index = oi->second.front();
            oi->second.pop_front();
            if (index < output->size()) {
                PageItem& item = (*output)[index];
                item.minimumDuration = tick <= item.note.startTick;
                item.note.endTick = tick > item.note.startTick ? tick
                    : (item.note.startTick == std::numeric_limits<uint32_t>::max() ? item.note.startTick
                                                                                   : item.note.startTick + 1u);
                item.closeOrder = order;
            }
            if (oi->second.empty()) outputIndices->erase(oi);
        }
    }
    if (active->second.empty()) state.pending.erase(active);
}

void BpfaMidiStore::closePageOrphans(VisualState& state, uint32_t pageStart, uint32_t pageEnd,
                                     std::vector<PageItem>& output, OutputIndices& outputIndices) const
{
    for (auto it = state.pending.begin(); it != state.pending.end();) {
        const uint32_t key = it->first, track = key >> 11;
        if (track >= tracks_.size()) { ++it; continue; }
        const uint32_t endTick = tracks_[track].maxTick;
        if (endTick < pageStart || endTick > pageEnd) { ++it; continue; }
        auto oi = outputIndices.find(key);
        while (!it->second.empty()) {
            if (oi != outputIndices.end() && !oi->second.empty()) {
                const std::size_t index = oi->second.front();
                oi->second.pop_front();
                if (index < output.size()) {
                    PageItem& item = output[index];
                    item.minimumDuration = endTick <= item.note.startTick;
                    item.note.endTick = endTick > item.note.startTick ? endTick
                        : (item.note.startTick == std::numeric_limits<uint32_t>::max() ? item.note.startTick
                                                                                       : item.note.startTick + 1u);
                    item.closeOrder = std::numeric_limits<uint64_t>::max() - uint64_t(0xffffffffu - key);
                }
            }
            it->second.pop_front();
        }
        if (oi != outputIndices.end()) outputIndices.erase(oi);
        it = state.pending.erase(it);
    }
}

const BpfaMidiStore::VisualState& BpfaMidiStore::ensureCheckpoint(uint32_t targetTick)
{
    auto& cps = visualCheckpoints_;
    if (cps.empty()) cps.push_back({0, VisualState{}});
    targetTick = std::min(targetTick, metadata_.maxTick);
    auto lowerTick = [](const VisualCheckpointEntry& cp, uint32_t value) { return cp.tick < value; };
    auto exact = std::lower_bound(cps.begin(), cps.end(), targetTick, lowerTick);
    if (exact != cps.end() && exact->tick == targetTick) return exact->state;
    auto baseIt = exact == cps.begin() ? cps.begin() : exact - 1;
    const uint32_t baseTick = baseIt->tick;
    VisualState state = baseIt->state;
    const uint64_t gap = uint64_t(targetTick) - uint64_t(baseTick);
    const uint64_t directThreshold = std::max<uint64_t>(1u, uint64_t(visualCheckpointSpan_) / 2u);
    if (gap > directThreshold) {
        rebuildVisualStateAt(targetTick, state);
        auto pos = std::lower_bound(cps.begin(), cps.end(), targetTick, lowerTick);
        if (pos == cps.end() || pos->tick != targetTick) pos = cps.insert(pos, {targetTick, state});
        return pos->state;
    }
    MergeCursor it;
    mergeReset(it, baseTick, false);
    uint32_t nextGrid = baseTick;
    if (visualCheckpointSpan_ > 0) {
        const uint64_t grid = (uint64_t(baseTick) / visualCheckpointSpan_ + 1u) * uint64_t(visualCheckpointSpan_);
        nextGrid = static_cast<uint32_t>(std::min<uint64_t>(grid, metadata_.maxTick));
    }
    uint32_t tick = 0, message = 0;
    uint16_t track = 0;
    while (mergePeek(it, tick, message, track) && tick < targetTick) {
        while (visualCheckpointSpan_ > 0 && nextGrid > baseTick && nextGrid < targetTick && nextGrid <= tick) {
            auto pos = std::lower_bound(cps.begin(), cps.end(), nextGrid, lowerTick);
            closeExpiredOrphans(state, nextGrid);
            if (pos == cps.end() || pos->tick != nextGrid) cps.insert(pos, {nextGrid, state});
            if (metadata_.maxTick - nextGrid < visualCheckpointSpan_) {
                nextGrid = targetTick;
                break;
            }
            nextGrid += visualCheckpointSpan_;
        }
        const uint64_t order = mergeOrder(it);
        mergePop(it);
        applyVisualEvent(state, tick, message, track, order);
    }
    closeExpiredOrphans(state, targetTick);
    auto pos = std::lower_bound(cps.begin(), cps.end(), targetTick, lowerTick);
    if (pos == cps.end() || pos->tick != targetTick) pos = cps.insert(pos, {targetTick, state});
    return pos->state;
}

void BpfaMidiStore::closeExpiredOrphans(VisualState& state, uint32_t beforeTick) const
{
    for (auto it = state.pending.begin(); it != state.pending.end();) {
        const uint32_t track = it->first >> 11;
        if (track < tracks_.size() && tracks_[track].maxTick < beforeTick) it = state.pending.erase(it);
        else ++it;
    }
}

void BpfaMidiStore::rebuildVisualStateAt(uint32_t targetTick, VisualState& state) const
{
    state.pending.clear();
    for (uint16_t t = 0; t < tracks_.size(); ++t) {
        Cursor c;
        c.position = tracks_[t].byteBegin;
        c.end = tracks_[t].byteEnd;
        const auto& snaps = tracks_[t].snapshots;
        auto snap = std::lower_bound(snaps.begin(), snaps.end(), targetTick,
            [](const VisualSnapshot& v, uint32_t tick) { return v.resume.tick < tick; });
        if (snap != snaps.begin()) {
            const VisualSnapshot& use = *(snap - 1);
            for (const SnapshotNote& note : use.notes) {
                const uint8_t channel = uint8_t(note.key >> 7);
                const uint32_t key = (uint32_t(t) << 11) | note.key;
                state.pending[key].push_back({note.startTick, note.velocity, colorByte(t, channel), note.openOrder});
            }
            c.position = use.resume.byteOffset;
            c.tick = use.resume.tick;
            c.runningStatus = use.resume.runningStatus;
            c.nextIndex = use.resume.eventIndex;
        }
        while (decodeNext(c, false)) {
            if (c.eventTick >= targetTick) break;
            applyVisualEvent(state, c.eventTick, c.eventMessage, t, c.eventIndex);
        }
    }
    closeExpiredOrphans(state, targetTick);
}

bool BpfaMidiStore::buildKeySnapshot(uint32_t tick, KeySnapshot& output)
{
    LiveSnapshot live;
    if (!buildLiveSnapshot(double(tick), double(tick), double(tick), live)) return false;
    output = live.keys;
    return true;
}

bool BpfaMidiStore::buildLiveSnapshot(double tick, double npsStartTick, double ccStartTick, LiveSnapshot& output,
                                      bool forceReset)
{
    output = LiveSnapshot{};
    if (!valid_ || tracks_.empty() || !std::isfinite(tick) || !std::isfinite(npsStartTick) ||
        !std::isfinite(ccStartTick))
        return false;
    const double maxTick = double(metadata_.maxTick);
    tick = std::clamp(tick, 0.0, maxTick);
    npsStartTick = std::clamp(npsStartTick, 0.0, tick);
    ccStartTick = std::clamp(ccStartTick, 0.0, tick);
    const uint32_t wholeTick = static_cast<uint32_t>(std::floor(tick));
    const bool exactTick = std::abs(tick - double(wholeTick)) < 1.0e-9;

    auto isNoteOff = [](uint32_t m) { return (m & 0xf0u) == 0x80u; };
    auto isNoteOn = [](uint32_t m) { return (m & 0xf0u) == 0x90u && ((m >> 16) & 0x7fu) != 0; };
    auto isControl = [](uint32_t m) {
        const uint32_t c = m & 0xf0u;
        return c == 0xb0u || c == 0xc0u || c == 0xd0u || c == 0xe0u;
    };
    auto addPoint = [](std::deque<DensityPoint>& points, uint64_t& total, uint32_t eventTick) {
        if (!points.empty() && points.back().tick == eventTick) {
            if (points.back().count != std::numeric_limits<uint32_t>::max()) ++points.back().count;
        } else {
            points.push_back({eventTick, 1u});
        }
        if (total != std::numeric_limits<uint64_t>::max()) ++total;
    };
    auto addDensity = [&](uint32_t eventTick, uint32_t m) {
        if (isNoteOn(m)) addPoint(liveNps_, liveNpsCount_, eventTick);
        if (isControl(m)) addPoint(liveCc_, liveCcCount_, eventTick);
    };
    auto expire = [](std::deque<DensityPoint>& points, uint64_t& total, double lowerTick) {
        while (!points.empty() && double(points.front().tick) < lowerTick) {
            total = total >= points.front().count ? total - points.front().count : 0u;
            points.pop_front();
        }
    };

    const double resetDistance = double(std::max<uint32_t>(4096u, visualCheckpointSpan_ * 2u));
    const bool reset = forceReset || !liveCursorValid_ || liveTick_ < 0.0 || tick + 1.0e-9 < liveTick_ ||
        tick - liveTick_ > resetDistance;
    uint32_t evTick = 0, evMessage = 0;
    uint16_t evTrack = 0;
    if (reset) {
        liveState_ = VisualState{};
        liveDeferredOffs_.clear();
        liveNps_.clear();
        liveCc_.clear();
        liveNpsCount_ = liveCcCount_ = 0;
        rebuildVisualStateAt(wholeTick, liveState_);
        mergeReset(liveCursor_, wholeTick, false);
        liveCursorValid_ = true;
        const double densityStart = std::min(npsStartTick, ccStartTick);
        MergeCursor density;
        mergeReset(density, static_cast<uint32_t>(std::floor(densityStart)), false);
        while (mergePeek(density, evTick, evMessage, evTrack)) {
            if (evTick > wholeTick) break;
            mergePop(density);
            if (double(evTick) + 1.0e-9 >= densityStart) addDensity(evTick, evMessage);
        }
        while (mergePeek(liveCursor_, evTick, evMessage, evTrack)) {
            if (evTick > wholeTick) break;
            mergePop(liveCursor_);
            if (exactTick && evTick == wholeTick && isNoteOff(evMessage))
                liveDeferredOffs_.push_back({evTick, evMessage, evTrack});
            else
                applyVisualEvent(liveState_, evTick, evMessage, evTrack, 0);
        }
        liveTick_ = tick;
    } else if (tick > liveTick_ + 1.0e-9) {
        for (const DeferredOff& off : liveDeferredOffs_)
            applyVisualEvent(liveState_, off.tick, off.message, off.track, 0);
        liveDeferredOffs_.clear();
        while (mergePeek(liveCursor_, evTick, evMessage, evTrack)) {
            if (evTick > wholeTick) break;
            mergePop(liveCursor_);
            addDensity(evTick, evMessage);
            if (exactTick && evTick == wholeTick && isNoteOff(evMessage))
                liveDeferredOffs_.push_back({evTick, evMessage, evTrack});
            else
                applyVisualEvent(liveState_, evTick, evMessage, evTrack, 0);
        }
        liveTick_ = tick;
    }

    closeExpiredOrphans(liveState_, wholeTick);
    expire(liveNps_, liveNpsCount_, npsStartTick);
    expire(liveCc_, liveCcCount_, ccStartTick);

    std::array<uint64_t, 128> ownerRank{};
    uint64_t activeVoices = 0;
    for (const auto& entry : liveState_.pending) {
        if (entry.second.empty()) continue;
        const uint32_t key = entry.first, track = key >> 11;
        const uint8_t pitch = uint8_t(key & 0x7fu);
        const uint64_t count = entry.second.size();
        activeVoices = std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), activeVoices + count);
        uint32_t& pitchCount = output.keys.counts[pitch];
        pitchCount = static_cast<uint32_t>(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(),
                                                              uint64_t(pitchCount) + count));
        const ActiveVisualNote& owner = entry.second.back();
        const uint64_t rank = (uint64_t(owner.startTick) << 32) | (uint64_t(track & 0xffffu) << 16) |
            ((count - 1u) & 0xffffu);
        if (pitchCount == count || rank >= ownerRank[pitch]) {
            ownerRank[pitch] = rank;
            output.keys.globalColors[pitch] = owner.color & 0x0fu;
            output.keys.trackColors[pitch] = (owner.color >> 4) & 0x0fu;
        }
    }
    output.activeVoices = static_cast<uint32_t>(activeVoices);
    output.nps = static_cast<uint32_t>(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), liveNpsCount_ * 4u));
    output.ccPerSecond = static_cast<uint32_t>(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), liveCcCount_));
    return true;
}

bool BpfaMidiStore::buildVisualPage(uint32_t pageStart, uint32_t pageEnd, std::vector<VisualNote>& output)
{
    output.clear();
    if (!valid_) return false;
    if (pageEnd < pageStart) std::swap(pageStart, pageEnd);
    pageStart = std::min(pageStart, metadata_.maxTick);
    pageEnd = std::min(pageEnd, metadata_.maxTick);
    VisualState state;
    if (rollingVisualValid_ && rollingVisualTick_ == pageStart) {
        state = std::move(rollingVisualState_);
        rollingVisualState_ = VisualState{};
        rollingVisualValid_ = false;
    } else {
        state = ensureCheckpoint(pageStart);
    }
    std::vector<PageItem> items;
    OutputIndices outputIndices;
    std::size_t activeCount = 0;
    for (const auto& entry : state.pending) activeCount += entry.second.size();
    items.reserve(activeCount + 1024);
    outputIndices.reserve(state.pending.size() + 64);
    struct CarrySeed { uint32_t key; PageItem item; };
    std::vector<CarrySeed> carries;
    carries.reserve(activeCount);
    for (const auto& entry : state.pending) {
        const uint32_t key = entry.first, track = key >> 11;
        const uint8_t pitch = uint8_t(key & 0x7f);
        for (const ActiveVisualNote& active : entry.second) {
            PageItem item;
            item.note = {active.startTick, 0u, uint32_t(active.velocity & 0x7f) | (uint32_t(pitch & 0x7f) << 8) |
                                                   (uint32_t(active.color & 0x0f) << 16) |
                                                   (uint32_t((active.color >> 4) & 0x0f) << 20) |
                                                   (uint32_t(track & 0xffu) << 24)};
            item.track = track;
            item.openOrder = active.openOrder;
            carries.push_back({key, item});
        }
    }
    std::stable_sort(carries.begin(), carries.end(), [](const CarrySeed& a, const CarrySeed& b) {
        if (a.item.note.startTick != b.item.note.startTick) return a.item.note.startTick < b.item.note.startTick;
        if (a.item.track != b.item.track) return a.item.track < b.item.track;
        return a.item.openOrder < b.item.openOrder;
    });
    for (CarrySeed& seed : carries) {
        outputIndices[seed.key].push_back(items.size());
        items.push_back(seed.item);
    }
    MergeCursor it;
    mergeReset(it, pageStart, false);
    uint32_t tick = 0, message = 0;
    uint16_t track = 0;
    while (mergePeek(it, tick, message, track)) {
        if (tick > pageEnd) break;
        const uint64_t order = mergeOrder(it);
        mergePop(it);
        applyVisualEvent(state, tick, message, track, order, &items, &outputIndices);
    }
    closePageOrphans(state, pageStart, pageEnd, items, outputIndices);
    if (pageEnd < metadata_.maxTick) {
        rollingVisualTick_ = pageEnd + 1u;
        rollingVisualState_ = std::move(state);
        rollingVisualValid_ = true;
    } else {
        rollingVisualState_ = VisualState{};
        rollingVisualTick_ = metadata_.maxTick;
        rollingVisualValid_ = false;
    }
    for (PageItem& item : items) {
        if (!item.minimumDuration) continue;
        const double minEndSeconds = metadata_.tickToSeconds(item.note.startTick) + 0.015;
        const double minEndTick = std::ceil(metadata_.secondsToTick(minEndSeconds));
        item.note.endTick = static_cast<uint32_t>(std::clamp<double>(minEndTick, double(item.note.startTick) + 1.0,
                                                                     double(std::numeric_limits<uint32_t>::max())));
    }
    for (std::size_t first = 0; first < items.size();) {
        std::size_t last = first + 1;
        while (last < items.size() && items[last].note.startTick == items[first].note.startTick &&
               items[last].track == items[first].track) ++last;
        if (last - first > 1)
            std::stable_sort(items.begin() + std::ptrdiff_t(first), items.begin() + std::ptrdiff_t(last),
                [](const PageItem& a, const PageItem& b) {
                    if (a.closeOrder != b.closeOrder) return a.closeOrder < b.closeOrder;
                    return a.openOrder < b.openOrder;
                });
        first = last;
    }
    output.reserve(items.size());
    for (const PageItem& item : items) output.push_back(item.note);
    return true;
}

struct BpfaMidiStore::IngestState {
    struct UniqueBlock { uint32_t chunk; uint32_t next; };
    MidiReadAt readAt = nullptr;
    void* readUser = nullptr;
    MidiParseProgress progress = nullptr;
    void* progressUser = nullptr;
    std::vector<uint8_t> window;
    std::vector<uint8_t> prevWindow;
    uint64_t rawBegin = 0;
    uint64_t frontier = 0;
    uint64_t prevBegin = 0;
    uint64_t prevEnd = 0;
    uint64_t start = 0;
    uint64_t end = 0;
    bool store = true;
    std::vector<UniqueBlock> unique;
    std::unordered_map<uint64_t, uint32_t> heads;
    std::size_t slabUsed = SourceSlabSize;
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> candidate;
    ZSTD_CCtx* cctx = nullptr;
    ~IngestState() { if (cctx) ZSTD_freeCCtx(cctx); }
};

BpfaMidiStore::BpfaMidiStore() = default;

BpfaMidiStore::~BpfaMidiStore()
{
    if (decompressContext_) ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(decompressContext_));
}

void BpfaMidiStore::beginIngest(uint64_t size, MidiReadAt readAt, void* readUser, MidiParseProgress progress,
                                void* progressUser)
{
    sourceSize_ = size;
    sourceBlockSize_ = 64u * 1024u;
    sourceChunks_.clear();
    ingest_.reset(new IngestState);
    IngestState& in = *ingest_;
    in.readAt = readAt;
    in.readUser = readUser;
    in.progress = progress;
    in.progressUser = progressUser;
    in.window.resize(ReadBlock);
    in.compressed.resize(ZSTD_compressBound(sourceBlockSize_));
    in.candidate.resize(sourceBlockSize_);
    in.cctx = ZSTD_createCCtx();
    if (!decompressContext_) decompressContext_ = ZSTD_createDCtx();
    sourceCache_.assign(SourceCacheSlots, CacheSlot{});
    in.end = size;
    if (partialActive_) {
        sourceBlockSize_ = partial_.blockSize;
        const uint64_t b = sourceBlockSize_;
        in.start = (partial_.rangeBegin / b) * b;
        in.end = std::min<uint64_t>(size, ((partial_.rangeEnd + b - 1) / b) * b);
        in.frontier = in.rawBegin = in.prevBegin = in.prevEnd = in.start;
        in.store = partial_.compress;
        chunkBase_ = in.start / b;
        in.compressed.resize(ZSTD_compressBound(sourceBlockSize_));
        in.candidate.resize(sourceBlockSize_);
    }
}

bool BpfaMidiStore::ingestNextWindow()
{
    constexpr uint32_t NoCollision = 0xffffffffu;
    IngestState& in = *ingest_;
    const uint64_t size = sourceSize_;
    const uint64_t windowBegin = in.frontier;
    if (windowBegin >= in.end) return false;
    const std::size_t windowSize = static_cast<std::size_t>(std::min<uint64_t>(ReadBlock, in.end - windowBegin));
    if (!in.store) {
        std::swap(in.window, in.prevWindow);
        in.prevBegin = in.rawBegin;
        in.prevEnd = in.frontier;
        in.window.resize(ReadBlock);
    }
    if (!in.readAt(in.readUser, windowBegin, in.window.data(), windowSize)) {
        error_ = "Could not read MIDI source";
        return false;
    }
    if (!in.store) {
        in.rawBegin = windowBegin;
        in.frontier = windowBegin + windowSize;
        if (in.progress)
            in.progress(in.progressUser, int(1 + 94.0 * double(in.frontier - in.start) / double(std::max<uint64_t>(1, in.end - in.start))),
                        "BPFA: reading and scanning");
        return true;
    }
    if (windowBegin == 0 && !partialActive_) {
        // Block size from the track count: every track cursor owns one decoded block,
        // so thousands of tracks need small blocks (HANDOFF sec. 56).
        uint16_t tracks = 1;
        if (windowSize >= 12 && std::memcmp(in.window.data(), "MThd", 4) == 0)
            tracks = std::max<uint16_t>(1, be16(in.window.data() + 10));
        sourceBlockSize_ = blockSizeForTracks(tracks);
        sourceChunks_.reserve(static_cast<std::size_t>((size + sourceBlockSize_ - 1) / sourceBlockSize_));
    }
    const std::size_t blockSize = sourceBlockSize_;
    auto append = [&](const uint8_t* data, std::size_t length) -> uint64_t {
        if (in.slabUsed + length > SourceSlabSize) {
            sourceSlabs_.emplace_back(new uint8_t[SourceSlabSize]);
            in.slabUsed = 0;
        }
        const uint64_t offset = uint64_t(sourceSlabs_.size() - 1) * SourceSlabSize + in.slabUsed;
        std::memcpy(sourceSlabs_.back().get() + in.slabUsed, data, length);
        in.slabUsed += length;
        storedSourceBytes_ += length;
        return offset;
    };
    ZSTD_DCtx* dctx = static_cast<ZSTD_DCtx*>(decompressContext_);
    for (std::size_t begin = 0; begin < windowSize; begin += blockSize) {
        const std::size_t length = std::min(blockSize, windowSize - begin);
        const uint8_t* block = in.window.data() + begin;
        uint64_t hash = 1469598103934665603ull;
        std::size_t i = 0;
        for (; i + 8 <= length; i += 8) {
            uint64_t word;
            std::memcpy(&word, block + i, 8);
            hash = (hash ^ word) * 1099511628211ull;
            hash ^= hash >> 29;
        }
        for (; i < length; ++i) hash = (hash ^ block[i]) * 1099511628211ull;
        const uint64_t key = hash ^ (uint64_t(length) * 0x9e3779b97f4a7c15ull);
        if (partialActive_) chunkKeys_.push_back(key);
        SourceChunk chunk{0, 0, uint32_t(length)};
        bool duplicate = false;
        auto head = in.heads.find(key);
        if (head != in.heads.end()) {
            for (uint32_t u = head->second; u != NoCollision && !duplicate; u = in.unique[u].next) {
                const SourceChunk& other = sourceChunks_[in.unique[u].chunk];
                if (other.size != length) continue;
                const uint8_t* otherBytes = sourceSlabs_[other.physicalOffset / SourceSlabSize].get() +
                    other.physicalOffset % SourceSlabSize;
                if (other.stored & SourceBlockCompressed) {
                    const std::size_t decoded = ZSTD_decompressDCtx(dctx, in.candidate.data(), length, otherBytes,
                                                                    other.stored & SourceBlockStoredSize);
                    if (ZSTD_isError(decoded) || decoded != length) continue;
                    otherBytes = in.candidate.data();
                }
                if (std::memcmp(otherBytes, block, length) == 0) {
                    chunk.physicalOffset = other.physicalOffset;
                    chunk.stored = other.stored;
                    duplicate = true;
                }
            }
        }
        if (!duplicate) {
            const std::size_t result = ZSTD_compressCCtx(in.cctx, in.compressed.data(), in.compressed.size(),
                                                         block, length, 1);
            const bool ok = !ZSTD_isError(result) && result < length;
            chunk.physicalOffset = append(ok ? in.compressed.data() : block, ok ? result : length);
            chunk.stored = uint32_t(ok ? result : length) | (ok ? SourceBlockCompressed : 0u);
            const uint32_t index = uint32_t(in.unique.size());
            in.unique.push_back({uint32_t(sourceChunks_.size()), head == in.heads.end() ? NoCollision : head->second});
            if (head == in.heads.end()) in.heads.emplace(key, index);
            else head->second = index;
        }
        sourceChunks_.push_back(chunk);
    }
    in.rawBegin = windowBegin;
    in.frontier = windowBegin + windowSize;
    if (in.progress)
        in.progress(in.progressUser,
                    int(1 + 94.0 * double(in.frontier - in.start) / double(std::max<uint64_t>(1, in.end - in.start))),
                    "BPFA: reading, compressing and scanning");
    return true;
}

bool BpfaMidiStore::finishIngest()
{
    if (!ingest_) return true;
    auto rest = [&]() -> bool {
        if (ingest_->frontier >= ingest_->end) return true;
        if (!ingestNextWindow()) { ingest_->frontier = ~uint64_t(0); return true; }
        return false;
    };
    runChunked(rest);
    const bool ok = ingest_->frontier == ingest_->end;
    ingest_.reset();
    sourceCache_.assign(SourceCacheSlots, CacheSlot{});
    return ok;
}

bool BpfaMidiStore::copySource(uint64_t position, uint8_t* destination, std::size_t length) const
{
    uint16_t slot = 0xffffu;
    while (length) {
        const uint8_t* data = nullptr;
        uint64_t begin = 0, end = 0;
        if (!sourceBlock(position, slot, data, begin, end) || position >= end) return false;
        const std::size_t amount = static_cast<std::size_t>(std::min<uint64_t>(length, end - position));
        std::memcpy(destination, data + (position - begin), amount);
        destination += amount;
        position += amount;
        length -= amount;
    }
    return true;
}

bool BpfaMidiStore::sourceSlotValid(uint16_t slot, uint64_t begin) const
{
    if (slot == 0xfffdu) return false;
    if (slot == 0xffffu || slot == 0xfffeu) return true;
    return slot < sourceCache_.size() &&
        sourceCache_[slot].chunk == begin / (onDemand_ ? onDemandPageBytes_ : sourceBlockSize_) - (onDemand_ ? 0 : chunkBase_);
}

bool BpfaMidiStore::sourceBlock(uint64_t position, uint16_t& slotHint, const uint8_t*& data, uint64_t& begin,
                                uint64_t& end, uint8_t* own) const
{
    if (!fileBytes_.empty()) {
        if (position >= fileBytes_.size()) return false;
        data = fileBytes_.data();
        begin = 0;
        end = fileBytes_.size();
        slotHint = 0xffffu;
        return true;
    }
    if (onDemand_) {
        if (position >= sourceSize_) return false;
        const uint64_t page = position / onDemandPageBytes_;
        std::size_t selected = sourceCache_.size(), oldest = 0;
        if (slotHint < sourceCache_.size() && sourceCache_[slotHint].chunk == page) selected = slotHint;
        for (std::size_t i = 0; selected == sourceCache_.size() && i < sourceCache_.size(); ++i) {
            if (sourceCache_[i].chunk == page) selected = i;
            else if (sourceCache_[i].age < sourceCache_[oldest].age) oldest = i;
        }
        begin = page * onDemandPageBytes_;
        end = std::min<uint64_t>(sourceSize_, begin + onDemandPageBytes_);
        if (selected == sourceCache_.size()) {
            selected = oldest;
            CacheSlot& slot = sourceCache_[selected];
            slot.bytes.resize(onDemandPageBytes_);
            ++onDemandReads_;
            if (!readAt_(readUser_, begin, slot.bytes.data(), static_cast<std::size_t>(end - begin))) {
                slot.chunk = ~uint64_t(0);
                return false;
            }
            slot.chunk = page;
        }
        CacheSlot& slot = sourceCache_[selected];
        slot.age = ++sourceCacheAge_;
        slotHint = uint16_t(selected);
        data = slot.bytes.data();
        return true;
    }
    if (ingest_) {
        BpfaMidiStore* self = const_cast<BpfaMidiStore*>(this);
        while (position >= ingest_->frontier && ingest_->frontier < ingest_->end)
            if (!self->ingestNextWindow()) return false;
        if (position >= ingest_->rawBegin && position < ingest_->frontier) {
            data = ingest_->window.data();
            begin = ingest_->rawBegin;
            end = ingest_->frontier;
            slotHint = 0xfffdu;
            return true;
        }
        if (!ingest_->store) {
            if (position < ingest_->prevBegin || position >= ingest_->prevEnd) return false;
            data = ingest_->prevWindow.data();
            begin = ingest_->prevBegin;
            end = ingest_->prevEnd;
            slotHint = 0xfffdu;
            return true;
        }
    }
    const uint64_t globalChunk = position / sourceBlockSize_;
    if (globalChunk < chunkBase_) return false;
    const uint64_t chunkIndex = globalChunk - chunkBase_;
    if (chunkIndex >= sourceChunks_.size()) return false;
    const SourceChunk& chunk = sourceChunks_[chunkIndex];
    const uint8_t* stored = sourceSlabs_[chunk.physicalOffset / SourceSlabSize].get() +
        chunk.physicalOffset % SourceSlabSize;
    begin = globalChunk * sourceBlockSize_;
    end = begin + chunk.size;
    if (!(chunk.stored & SourceBlockCompressed)) {
        data = stored;
        slotHint = 0xffffu;
        return true;
    }
    if (!decompressContext_) decompressContext_ = ZSTD_createDCtx();
    ZSTD_DCtx* dctx = static_cast<ZSTD_DCtx*>(decompressContext_);
    if (own) {
        const std::size_t decoded = ZSTD_decompressDCtx(dctx, own, chunk.size, stored, chunk.stored & SourceBlockStoredSize);
        if (ZSTD_isError(decoded) || decoded != chunk.size) return false;
        data = own;
        slotHint = 0xfffeu;
        return true;
    }
    std::size_t selected = sourceCache_.size(), oldest = 0;
    if (slotHint < sourceCache_.size() && sourceCache_[slotHint].chunk == chunkIndex) selected = slotHint;
    for (std::size_t i = 0; selected == sourceCache_.size() && i < sourceCache_.size(); ++i) {
        if (sourceCache_[i].chunk == chunkIndex) selected = i;
        else if (sourceCache_[i].age < sourceCache_[oldest].age) oldest = i;
    }
    if (selected == sourceCache_.size()) {
        selected = oldest;
        CacheSlot& slot = sourceCache_[selected];
        slot.bytes.resize(sourceBlockSize_);
        const std::size_t decoded = ZSTD_decompressDCtx(dctx, slot.bytes.data(), chunk.size, stored,
                                                        chunk.stored & SourceBlockStoredSize);
        if (ZSTD_isError(decoded) || decoded != chunk.size) {
            slot.chunk = ~uint64_t(0);
            return false;
        }
        slot.chunk = chunkIndex;
    }
    CacheSlot& slot = sourceCache_[selected];
    slot.age = ++sourceCacheAge_;
    slotHint = uint16_t(selected);
    data = slot.bytes.data();
    return true;
}

uint32_t BpfaMidiStore::blockSizeForTracks(uint32_t trackCount)
{
    const std::size_t perCursor = (256u * 1024u * 1024u) / (3u * std::max<std::size_t>(1, trackCount));
    uint32_t block = 64u * 1024u;
    while (block > 8u * 1024u && block > perCursor) block >>= 1;
    return block;
}

void BpfaMidiStore::finalizeIndex(MidiDocument& out, uint64_t totalNotes, uint64_t totalControls,
                                  MidiDocument& metadata)
{
    out.noteCount = totalNotes;
    out.controlEventCount = totalControls;
    buildTempoIndexLocal(out);
    out.durationSeconds = static_cast<float>(out.tickToSeconds(out.maxTick));
    std::stable_sort(sysex_.begin(), sysex_.end(), [](const SysExRef& a, const SysExRef& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.track != b.track) return a.track < b.track;
        return a.order < b.order;
    });
    std::stable_sort(stateEvents_.begin(), stateEvents_.end(),
                     [](const SeekStateEvent& a, const SeekStateEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.track != b.track) return a.track < b.track;
        return a.order < b.order;
    });
    std::stable_sort(textEvents_.begin(), textEvents_.end(), [](const TextEvent& a, const TextEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.track < b.track;
    });
    density_.resize(std::size_t(out.maxTick / densityBucketTicks_) + 1u, 0u);
    metadata_ = out;
    buildColorTables();
    visualCheckpointSpan_ = std::max<uint32_t>(1u, uint32_t((uint64_t(out.maxTick) + 127u) / 128u));
    metadata = out;
    valid_ = true;
}

namespace {
struct BlobWriter {
    std::vector<uint8_t>& out;
    template <class T> void put(const T& v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    void bytes(const void* p, std::size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
};
struct BlobReader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (std::size_t(end - p) < sizeof(T)) { ok = false; return v; }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    const uint8_t* take(std::size_t n) {
        if (std::size_t(end - p) < n) { ok = false; return nullptr; }
        const uint8_t* r = p;
        p += n;
        return r;
    }
};
constexpr uint32_t BlobMagic = 0x42504d57u;
constexpr uint32_t BlobVersion = 2;
}

void BpfaMidiStore::exportPartial(const MidiDocument& out, const std::vector<uint64_t>& notesPerTrack,
                                  uint64_t totalControls)
{
    std::vector<uint8_t>& blob = partialBlob_;
    blob.clear();
    BlobWriter w{blob};
    w.put(BlobMagic);
    w.put(BlobVersion);
    w.put(partial_.firstTrack);
    w.put(partial_.endTrack);
    w.put(uint32_t(sourceBlockSize_));
    const uint64_t b = sourceBlockSize_;
    const uint64_t ownedFirst = partial_.compress ? (partial_.rangeBegin + b - 1) / b : 0;
    const uint64_t ownedEnd = partial_.compress ? (partial_.rangeEnd + b - 1) / b : 0;
    w.put(ownedFirst);
    w.put(ownedEnd - ownedFirst);
    // Owned blocks: key, stored word, size, offset into the data area that follows.
    std::unordered_map<uint64_t, uint64_t> dataOffsetOf;
    uint64_t dataBytes = 0;
    std::vector<uint64_t> offsets;
    for (uint64_t g = ownedFirst; g < ownedEnd; ++g) {
        const SourceChunk& c = sourceChunks_[g - chunkBase_];
        auto it = dataOffsetOf.find(c.physicalOffset);
        if (it == dataOffsetOf.end()) {
            it = dataOffsetOf.emplace(c.physicalOffset, dataBytes).first;
            dataBytes += c.stored & SourceBlockStoredSize;
        }
        offsets.push_back(it->second);
        w.put(chunkKeys_[g - chunkBase_]);
        w.put(c.stored);
        w.put(c.size);
        w.put(it->second);
    }
    w.put(dataBytes);
    {
        std::unordered_map<uint64_t, bool> done;
        for (uint64_t g = ownedFirst; g < ownedEnd; ++g) {
            const SourceChunk& c = sourceChunks_[g - chunkBase_];
            if (!done.emplace(c.physicalOffset, true).second) continue;
            w.bytes(sourceSlabs_[c.physicalOffset / SourceSlabSize].get() + c.physicalOffset % SourceSlabSize,
                    c.stored & SourceBlockStoredSize);
        }
    }
    for (uint16_t t = partial_.firstTrack; t < partial_.endTrack; ++t) {
        const TrackStorage& tr = tracks_[t];
        w.put(tr.byteBegin);
        w.put(tr.byteEnd);
        w.put(tr.maxTick);
        w.put(out.activeChannelMasks[t]);
        w.put(notesPerTrack[t]);
        const FirstNote& f = firstNotes_[t];
        w.put(f.seenMask);
        w.bytes(f.tick.data(), sizeof(f.tick));
        w.bytes(f.order.data(), sizeof(f.order));
        w.bytes(f.closeOrder.data(), sizeof(f.closeOrder));
        w.put(uint64_t(tr.checkpoints.size()));
        w.bytes(tr.checkpoints.data(), tr.checkpoints.size() * sizeof(TrackCheckpoint));
        w.put(uint64_t(tr.snapshots.size()));
        for (const VisualSnapshot& v : tr.snapshots) {
            w.put(v.resume);
            w.put(uint64_t(v.notes.size()));
            w.bytes(v.notes.data(), v.notes.size() * sizeof(SnapshotNote));
        }
    }
    w.put(totalControls);
    w.put(uint8_t(out.hasPitch ? 1 : 0));
    w.put(out.minPitch);
    w.put(out.maxPitch);
    w.put(out.maxTick);
    w.put(uint64_t(out.tempoMap.size() - 1));
    for (std::size_t i = 1; i < out.tempoMap.size(); ++i) w.put(out.tempoMap[i]);
    w.put(uint64_t(stateEvents_.size()));
    w.bytes(stateEvents_.data(), stateEvents_.size() * sizeof(SeekStateEvent));
    w.put(uint64_t(sysex_.size()));
    for (const SysExRef& x : sysex_) {
        w.put(x.tick);
        w.put(x.track);
        w.put(x.order);
        w.put(uint64_t(x.data.size()));
        w.bytes(x.data.data(), x.data.size());
    }
    w.put(uint64_t(textEvents_.size()));
    for (const TextEvent& te : textEvents_) {
        w.put(te.tick);
        w.put(te.track);
        w.put(te.type);
        w.put(uint32_t(te.text.size()));
        w.bytes(te.text.data(), te.text.size());
    }
    w.put(densityBucketTicks_);
    w.put(uint64_t(density_.size()));
    w.bytes(density_.data(), density_.size() * sizeof(uint32_t));
}

bool BpfaMidiStore::indexPartial(uint64_t size, MidiReadAt readAt, void* readUser, const PartialPlan& plan,
                                 MidiParseProgress progress, void* progressUser)
{
    partial_ = plan;
    partialActive_ = true;
    MidiDocument unused;
    const bool ok = index(size, readAt, readUser, unused, progress, progressUser);
    partialActive_ = false;
    if (!ok) return false;
    // Only the blob is needed now.
    const std::vector<uint8_t> keep = std::move(partialBlob_);
    clear();
    partialBlob_ = std::move(keep);
    return true;
}

bool BpfaMidiStore::beginMerge(uint64_t size, uint16_t format, uint16_t trackCount, uint16_t ticksPerBeat,
                               uint32_t blockSize, bool compressed)
{
    clear();
    try {
        sourceSize_ = size;
        sourceBlockSize_ = blockSize;
        tracks_.resize(trackCount);
        firstNotes_.assign(trackCount, FirstNote{});
        mergeOut_ = MidiDocument{};
        mergeOut_.remoteIndexed = true;
        mergeOut_.minPitch = 127;
        mergeOut_.maxPitch = 0;
        mergeOut_.format = format;
        mergeOut_.trackCount = trackCount;
        mergeOut_.ticksPerBeat = ticksPerBeat;
        mergeOut_.activeChannelMasks.assign(trackCount, 0);
        mergeOut_.tempoMap.push_back({0, 500000});
        mergeTotalNotes_ = mergeTotalControls_ = 0;
        if (compressed) {
            // The merge reuses the ingest dedup state; nothing is read here.
            ingest_.reset(new IngestState);
            ingest_->compressed.resize(ZSTD_compressBound(blockSize));
            ingest_->candidate.resize(blockSize);
            ingest_->frontier = ingest_->end = size;
            if (!decompressContext_) decompressContext_ = ZSTD_createDCtx();
            sourceChunks_.reserve(static_cast<std::size_t>((size + blockSize - 1) / blockSize));
        }
        merging_ = true;
        return true;
    } catch (...) {
        clear();
        error_ = "BPFA merge could not start";
        return false;
    }
}

bool BpfaMidiStore::mergeReadRaw(MidiReadAt readAt, void* readUser, MidiParseProgress progress, void* progressUser)
{
    try {
        fileBytes_.resize(static_cast<std::size_t>(sourceSize_));
        for (uint64_t done = 0; done < sourceSize_;) {
            const std::size_t amount = static_cast<std::size_t>(std::min<uint64_t>(ReadBlock, sourceSize_ - done));
            if (!readAt(readUser, done, fileBytes_.data() + done, amount)) {
                error_ = "Could not read MIDI source";
                return false;
            }
            done += amount;
            if (progress)
                progress(progressUser, int(1 + 39.0 * double(done) / double(sourceSize_)), "BPFA: reading source");
        }
        return true;
    } catch (...) {
        error_ = "BPFA MIDI store exhausted browser memory";
        return false;
    }
}

bool BpfaMidiStore::mergePartial(const uint8_t* data, std::size_t size)
{
    try {
        BlobReader r{data, data + size};
        if (r.get<uint32_t>() != BlobMagic || r.get<uint32_t>() != BlobVersion) { error_ = "Bad partial index"; return false; }
        const uint16_t t0 = r.get<uint16_t>(), t1 = r.get<uint16_t>();
        const uint32_t blockSize = r.get<uint32_t>();
        const uint64_t ownedFirst = r.get<uint64_t>(), ownedCount = r.get<uint64_t>();
        if (!r.ok || t1 > tracks_.size() || t0 > t1) { error_ = "Bad partial index"; return false; }
        struct Owned { uint64_t key; uint32_t stored; uint32_t size; uint64_t offset; };
        std::vector<Owned> owned(static_cast<std::size_t>(ownedCount));
        for (Owned& o : owned) {
            o.key = r.get<uint64_t>();
            o.stored = r.get<uint32_t>();
            o.size = r.get<uint32_t>();
            o.offset = r.get<uint64_t>();
        }
        const uint64_t dataBytes = r.get<uint64_t>();
        const uint8_t* area = r.take(static_cast<std::size_t>(dataBytes));
        if (!r.ok) { error_ = "Bad partial index"; return false; }
        if (ownedCount) {
            if (!ingest_ || blockSize != sourceBlockSize_ || ownedFirst != sourceChunks_.size()) {
                error_ = "Partial index blocks out of order";
                return false;
            }
            // Same dedup as the single-threaded ingest: blocks in file order against
            // every earlier distinct block, compared byte for byte.
            constexpr uint32_t NoCollision = 0xffffffffu;
            IngestState& in = *ingest_;
            ZSTD_DCtx* dctx = static_cast<ZSTD_DCtx*>(decompressContext_);
            std::vector<uint8_t> mine(blockSize);
            for (const Owned& o : owned) {
                const uint8_t* bytes = area + o.offset;
                const uint32_t storedSize = o.stored & SourceBlockStoredSize;
                SourceChunk chunk{0, 0, o.size};
                bool duplicate = false;
                bool mineReady = false;
                auto head = in.heads.find(o.key);
                if (head != in.heads.end()) {
                    for (uint32_t u = head->second; u != NoCollision && !duplicate; u = in.unique[u].next) {
                        const SourceChunk& other = sourceChunks_[in.unique[u].chunk];
                        if (other.size != o.size) continue;
                        if (!mineReady) {
                            if (o.stored & SourceBlockCompressed) {
                                const std::size_t n = ZSTD_decompressDCtx(dctx, mine.data(), o.size, bytes, storedSize);
                                if (ZSTD_isError(n) || n != o.size) { error_ = "Bad partial block"; return false; }
                            } else {
                                std::memcpy(mine.data(), bytes, o.size);
                            }
                            mineReady = true;
                        }
                        const uint8_t* otherBytes = sourceSlabs_[other.physicalOffset / SourceSlabSize].get() +
                            other.physicalOffset % SourceSlabSize;
                        if (other.stored & SourceBlockCompressed) {
                            const std::size_t n = ZSTD_decompressDCtx(dctx, in.candidate.data(), o.size, otherBytes,
                                                                      other.stored & SourceBlockStoredSize);
                            if (ZSTD_isError(n) || n != o.size) continue;
                            otherBytes = in.candidate.data();
                        }
                        if (std::memcmp(otherBytes, mine.data(), o.size) == 0) {
                            chunk.physicalOffset = other.physicalOffset;
                            chunk.stored = other.stored;
                            duplicate = true;
                        }
                    }
                }
                if (!duplicate) {
                    if (in.slabUsed + storedSize > SourceSlabSize) {
                        sourceSlabs_.emplace_back(new uint8_t[SourceSlabSize]);
                        in.slabUsed = 0;
                    }
                    chunk.physicalOffset = uint64_t(sourceSlabs_.size() - 1) * SourceSlabSize + in.slabUsed;
                    std::memcpy(sourceSlabs_.back().get() + in.slabUsed, bytes, storedSize);
                    in.slabUsed += storedSize;
                    storedSourceBytes_ += storedSize;
                    chunk.stored = o.stored;
                    const uint32_t index = uint32_t(in.unique.size());
                    in.unique.push_back({uint32_t(sourceChunks_.size()), head == in.heads.end() ? NoCollision : head->second});
                    if (head == in.heads.end()) in.heads.emplace(o.key, index);
                    else head->second = index;
                }
                sourceChunks_.push_back(chunk);
            }
        }
        for (uint16_t t = t0; t < t1; ++t) {
            TrackStorage& tr = tracks_[t];
            tr.byteBegin = r.get<uint64_t>();
            tr.byteEnd = r.get<uint64_t>();
            tr.maxTick = r.get<uint32_t>();
            mergeOut_.activeChannelMasks[t] = r.get<uint32_t>();
            mergeTotalNotes_ += r.get<uint64_t>();
            FirstNote& f = firstNotes_[t];
            f.seenMask = r.get<uint16_t>();
            const uint8_t* p = r.take(sizeof(f.tick));
            if (p) std::memcpy(f.tick.data(), p, sizeof(f.tick));
            p = r.take(sizeof(f.order));
            if (p) std::memcpy(f.order.data(), p, sizeof(f.order));
            p = r.take(sizeof(f.closeOrder));
            if (p) std::memcpy(f.closeOrder.data(), p, sizeof(f.closeOrder));
            const uint64_t nc = r.get<uint64_t>();
            p = r.take(static_cast<std::size_t>(nc * sizeof(TrackCheckpoint)));
            if (!r.ok) break;
            tr.checkpoints.resize(static_cast<std::size_t>(nc));
            std::memcpy(tr.checkpoints.data(), p, static_cast<std::size_t>(nc * sizeof(TrackCheckpoint)));
            const uint64_t ns = r.get<uint64_t>();
            tr.snapshots.resize(static_cast<std::size_t>(ns));
            for (VisualSnapshot& v : tr.snapshots) {
                v.resume = r.get<TrackCheckpoint>();
                const uint64_t n = r.get<uint64_t>();
                p = r.take(static_cast<std::size_t>(n * sizeof(SnapshotNote)));
                if (!r.ok) break;
                v.notes.resize(static_cast<std::size_t>(n));
                std::memcpy(v.notes.data(), p, static_cast<std::size_t>(n * sizeof(SnapshotNote)));
            }
            mergeOut_.maxTick = std::max(mergeOut_.maxTick, tr.maxTick);
        }
        mergeTotalControls_ += r.get<uint64_t>();
        const bool hasPitch = r.get<uint8_t>() != 0;
        const uint8_t minPitch = r.get<uint8_t>(), maxPitch = r.get<uint8_t>();
        const uint32_t maxTick = r.get<uint32_t>();
        if (hasPitch) {
            mergeOut_.hasPitch = true;
            mergeOut_.minPitch = std::min(mergeOut_.minPitch, minPitch);
            mergeOut_.maxPitch = std::max(mergeOut_.maxPitch, maxPitch);
        }
        mergeOut_.maxTick = std::max(mergeOut_.maxTick, maxTick);
        const uint64_t nt = r.get<uint64_t>();
        for (uint64_t i = 0; i < nt && r.ok; ++i) mergeOut_.tempoMap.push_back(r.get<TempoChange>());
        const uint64_t ne = r.get<uint64_t>();
        const uint8_t* ep = r.take(static_cast<std::size_t>(ne * sizeof(SeekStateEvent)));
        if (r.ok && ne) {
            const std::size_t base = stateEvents_.size();
            stateEvents_.resize(base + static_cast<std::size_t>(ne));
            std::memcpy(stateEvents_.data() + base, ep, static_cast<std::size_t>(ne * sizeof(SeekStateEvent)));
        }
        const uint64_t nx = r.get<uint64_t>();
        for (uint64_t i = 0; i < nx && r.ok; ++i) {
            SysExRef x;
            x.tick = r.get<uint32_t>();
            x.track = r.get<uint16_t>();
            x.order = r.get<uint64_t>();
            const uint64_t n = r.get<uint64_t>();
            const uint8_t* xp = r.take(static_cast<std::size_t>(n));
            if (!r.ok) break;
            x.data.assign(xp, xp + n);
            sysex_.push_back(std::move(x));
        }
        const uint64_t ntext = r.get<uint64_t>();
        for (uint64_t i = 0; i < ntext && r.ok; ++i) {
            TextEvent te;
            te.tick = r.get<uint32_t>();
            te.track = r.get<uint16_t>();
            te.type = r.get<uint8_t>();
            const uint32_t n = r.get<uint32_t>();
            const uint8_t* tp = r.take(n);
            if (!r.ok) break;
            te.text.assign(reinterpret_cast<const char*>(tp), n);
            if (textEvents_.size() < MaxTextEvents) textEvents_.push_back(std::move(te));
        }
        densityBucketTicks_ = std::max<uint32_t>(1u, r.get<uint32_t>());
        const uint64_t nd = r.get<uint64_t>();
        const uint8_t* dp = r.take(static_cast<std::size_t>(nd * sizeof(uint32_t)));
        if (r.ok && nd) {
            if (density_.size() < nd) density_.resize(static_cast<std::size_t>(nd), 0u);
            for (uint64_t i = 0; i < nd; ++i) {
                uint32_t c;
                std::memcpy(&c, dp + i * 4, 4);
                density_[i] += c;
            }
        }
        if (!r.ok) { error_ = "Truncated partial index"; return false; }
        return true;
    } catch (...) {
        error_ = "BPFA MIDI store exhausted browser memory";
        return false;
    }
}

bool BpfaMidiStore::finishMerge(MidiDocument& metadata)
{
    try {
        if (!merging_) { error_ = "No merge in progress"; return false; }
        if (ingest_) {
            if (sourceChunks_.size() != (sourceSize_ + sourceBlockSize_ - 1) / sourceBlockSize_) {
                error_ = "Parallel load left source blocks missing";
                return false;
            }
            ingest_.reset();
            sourceCache_.assign(SourceCacheSlots, CacheSlot{});
        }
        merging_ = false;
        finalizeIndex(mergeOut_, mergeTotalNotes_, mergeTotalControls_, metadata);
        mergeOut_ = MidiDocument{};
        return true;
    } catch (...) {
        clear();
        error_ = "BPFA MIDI store exhausted browser memory";
        return false;
    }
}

} // namespace wasmidi
