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

enum class Kind { Channel, Tempo, SysEx, End };

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

BpfaMidiStore::~BpfaMidiStore()
{
    if (decompressContext_) ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(decompressContext_));
}

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
        } else if (size <= rawSourceLimit_) {
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
        } else if (!ingestSource(size, readAt, readUser, progress, progressUser)) {
            if (error_.empty()) error_ = "Could not read MIDI source";
            return false;
        }
        uint8_t header[14];
        if (!copySource(0, header, sizeof(header)) || std::memcmp(header, "MThd", 4) != 0) {
            error_ = "Missing MThd header";
            return false;
        }
        const uint32_t headerSize = be32(header + 4);
        if (headerSize < 6 || 8ull + headerSize > size) { error_ = "Invalid MIDI header"; return false; }
        MidiDocument out;
        out.remoteIndexed = true;
        out.minPitch = 127;
        out.maxPitch = 0;
        out.format = be16(header + 8);
        out.trackCount = be16(header + 10);
        out.ticksPerBeat = be16(header + 12);
        if (out.format > 1) { error_ = "Only MIDI Format 0 and 1 are supported"; return false; }
        if (out.ticksPerBeat == 0 || (out.ticksPerBeat & 0x8000)) { error_ = "SMPTE timing is not supported"; return false; }

        uint64_t position = 8ull + headerSize;
        tracks_.resize(out.trackCount);
        firstNotes_.assign(out.trackCount, FirstNote{});
        for (uint16_t t = 0; t < out.trackCount; ++t) {
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
        for (uint16_t t = 0; t < out.trackCount; ++t) {
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
                    snapshotNotesUsed + openCount <= SnapshotBudgetNotes) {
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
                auto chunk = [&]() { return scanEvents(d); };
                runChunked(chunk);
            } else {
                Decoder d{nullptr, storage.byteBegin, storage.byteEnd};
                d.store = this;
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
            report(40 + int(30.0 * double(t + 1) / double(std::max<int>(1, out.trackCount))), "BPFA: track scan");
        }
        out.noteCount = totalNotes;
        out.controlEventCount = totalControls;
        buildTempoIndexLocal(out);
        out.durationSeconds = static_cast<float>(out.tickToSeconds(out.maxTick));

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


        metadata_ = out;
        buildColorTables();
        visualCheckpointSpan_ = std::max<uint32_t>(1u, uint32_t((uint64_t(out.maxTick) + 127u) / 128u));
        metadata = out;
        valid_ = true;
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

bool BpfaMidiStore::ingestSource(uint64_t size, MidiReadAt readAt, void* readUser, MidiParseProgress progress,
                                 void* progressUser)
{
    struct UniqueBlock { uint32_t chunk; uint32_t next; };
    constexpr uint32_t NoCollision = 0xffffffffu;
    sourceSize_ = size;
    // Block size from the track count, read from the header: every track cursor owns
    // one decoded block, so thousands of tracks need small blocks (HANDOFF sec. 56).
    sourceBlockSize_ = 64u * 1024u;
    std::size_t SourceBlockSize = sourceBlockSize_;
    sourceChunks_.clear();
    std::vector<UniqueBlock> unique;
    std::unordered_map<uint64_t, uint32_t> heads;
    std::size_t slabUsed = SourceSlabSize;
    auto append = [&](const uint8_t* data, std::size_t length) -> uint64_t {
        if (slabUsed + length > SourceSlabSize) {
            sourceSlabs_.emplace_back(new uint8_t[SourceSlabSize]);
            slabUsed = 0;
        }
        const uint64_t offset = uint64_t(sourceSlabs_.size() - 1) * SourceSlabSize + slabUsed;
        std::memcpy(sourceSlabs_.back().get() + slabUsed, data, length);
        slabUsed += length;
        storedSourceBytes_ += length;
        return offset;
    };
    auto stored = [&](const SourceChunk& chunk) {
        return sourceSlabs_[chunk.physicalOffset / SourceSlabSize].get() + chunk.physicalOffset % SourceSlabSize;
    };
    std::vector<uint8_t> window(ReadBlock);
    std::vector<uint8_t> compressed(ZSTD_compressBound(SourceBlockSize));
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    if (!decompressContext_) decompressContext_ = ZSTD_createDCtx();
    ZSTD_DCtx* dctx = static_cast<ZSTD_DCtx*>(decompressContext_);
    struct FreeCctx { ZSTD_CCtx* c; ~FreeCctx() { ZSTD_freeCCtx(c); } } freeCctx{cctx};
    std::vector<uint8_t> candidate(SourceBlockSize);
    uint64_t windowBegin = 0;
    bool ingestFailed = false;
    auto ingestWindow = [&]() -> bool {
        if (windowBegin >= size) return true;
        const std::size_t windowSize = static_cast<std::size_t>(std::min<uint64_t>(ReadBlock, size - windowBegin));
        if (!readAt(readUser, windowBegin, window.data(), windowSize)) {
            error_ = "Could not read MIDI source";
            ingestFailed = true;
            return true;
        }
        if (windowBegin == 0) {
            uint16_t tracks = 1;
            if (windowSize >= 12 && std::memcmp(window.data(), "MThd", 4) == 0)
                tracks = std::max<uint16_t>(1, be16(window.data() + 10));
            const std::size_t perCursor = (256u * 1024u * 1024u) / (3u * std::size_t(tracks));
            while (sourceBlockSize_ > 8u * 1024u && sourceBlockSize_ > perCursor) sourceBlockSize_ >>= 1;
            SourceBlockSize = sourceBlockSize_;
            sourceChunks_.reserve(static_cast<std::size_t>((size + SourceBlockSize - 1) / SourceBlockSize));
        }
        for (std::size_t begin = 0; begin < windowSize; begin += SourceBlockSize) {
            const std::size_t length = std::min(SourceBlockSize, windowSize - begin);
            const uint8_t* block = window.data() + begin;
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
            SourceChunk chunk{0, 0, uint32_t(length)};
            bool duplicate = false;
            auto head = heads.find(key);
            if (head != heads.end()) {
                for (uint32_t u = head->second; u != NoCollision && !duplicate; u = unique[u].next) {
                    const SourceChunk& other = sourceChunks_[unique[u].chunk];
                    if (other.size != length) continue;
                    const uint8_t* otherBytes = stored(other);
                    if (other.stored & SourceBlockCompressed) {
                        const std::size_t decoded = ZSTD_decompressDCtx(dctx, candidate.data(), length, otherBytes,
                                                                        other.stored & SourceBlockStoredSize);
                        if (ZSTD_isError(decoded) || decoded != length) continue;
                        otherBytes = candidate.data();
                    }
                    if (std::memcmp(otherBytes, block, length) == 0) {
                        chunk.physicalOffset = other.physicalOffset;
                        chunk.stored = other.stored;
                        duplicate = true;
                    }
                }
            }
            if (!duplicate) {
                const std::size_t result = ZSTD_compressCCtx(cctx, compressed.data(), compressed.size(), block, length, 1);
                const bool ok = !ZSTD_isError(result) && result < length;
                chunk.physicalOffset = append(ok ? compressed.data() : block, ok ? result : length);
                chunk.stored = uint32_t(ok ? result : length) | (ok ? SourceBlockCompressed : 0u);
                const uint32_t index = uint32_t(unique.size());
                unique.push_back({uint32_t(sourceChunks_.size()), head == heads.end() ? NoCollision : head->second});
                if (head == heads.end()) heads.emplace(key, index);
                else head->second = index;
            }
            sourceChunks_.push_back(chunk);
        }
        if (progress)
            progress(progressUser, int(1 + 39.0 * double(windowBegin + windowSize) / double(size)),
                     "BPFA: compressing source");
        windowBegin += ReadBlock;
        return false;
    };
    runChunked(ingestWindow);
    if (ingestFailed) return false;
    sourceCache_.assign(SourceCacheSlots, CacheSlot{});
    return true;
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
    if (slot == 0xffffu || slot == 0xfffeu) return true;
    return slot < sourceCache_.size() &&
        sourceCache_[slot].chunk == begin / (onDemand_ ? onDemandPageBytes_ : sourceBlockSize_);
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
    const uint64_t chunkIndex = position / sourceBlockSize_;
    if (chunkIndex >= sourceChunks_.size()) return false;
    const SourceChunk& chunk = sourceChunks_[chunkIndex];
    const uint8_t* stored = sourceSlabs_[chunk.physicalOffset / SourceSlabSize].get() +
        chunk.physicalOffset % SourceSlabSize;
    begin = chunkIndex * sourceBlockSize_;
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

} // namespace wasmidi
