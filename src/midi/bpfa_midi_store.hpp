#pragma once

// BPFA PreprocessedMidi loader behind the MidiMappedStore contract (HANDOFF sec. 43).

#include "midi_mapped_store.hpp"

#include <array>
#include <cstddef>
#include <deque>
#include <memory>
#include <unordered_map>
#include <cstdint>
#include <string>
#include <vector>

namespace wasmidi {

class BpfaMidiStore {
public:
    using EventWord = MidiMappedStore::EventWord;
    using SysExBatchEvent = MidiMappedStore::SysExBatchEvent;
    using RenderClose = MidiMappedStore::RenderClose;
    using KeySnapshot = MidiMappedStore::KeySnapshot;
    using LiveSnapshot = MidiMappedStore::LiveSnapshot;

    // BPFA PreprocessedMidi::PackedNote: start tick + 12-bit duration
    // (0x0fff -> LongNote side table), velocity, note, channel.
    struct PackedNote {
        uint32_t startTick;
        uint32_t durationAndKey;
    };
    struct LongNote {
        uint32_t noteIndex;
        uint32_t endTick;
    };
    struct NoteBlock {
        uint32_t firstStartTick;
        uint32_t maximumEndTick;
        uint32_t noteOffset;
        uint16_t noteCount;
        uint16_t reserved;
    };

    BpfaMidiStore();
    BpfaMidiStore(const BpfaMidiStore&) = delete;
    BpfaMidiStore& operator=(const BpfaMidiStore&) = delete;

    void clear();
    // Discarded experiment, off by default (HANDOFF sec. 50-51): read the MIDI from readAt on demand instead of
    // keeping compressed source blocks. readAt must stay valid after index().
    // pageBytes 0 = automatic: a ~64 MiB cache with at least two pages per track.
    // Files up to this size stay uncompressed in RAM; bigger ones use the zstd source.
    // The worker sets it from the heap/RAM budget (HANDOFF sec. 53-54). 0 = always zstd.
    void setRawSourceLimit(uint64_t bytes) { rawSourceLimit_ = bytes; }
    void setOnDemandSource(bool enabled, std::size_t pageBytes = 0, std::size_t pages = 0)
    { onDemand_ = enabled; onDemandRequestPageBytes_ = pageBytes; onDemandRequestPages_ = pages; }
    bool index(uint64_t size, MidiReadAt readAt, void* readUser, MidiDocument& metadata,
               MidiParseProgress progress = nullptr, void* progressUser = nullptr);

    bool valid() const { return valid_; }
    const char* error() const { return error_.c_str(); }
    const MidiDocument& metadata() const { return metadata_; }

    void resetEventCursor(uint32_t startTick);
    bool buildEventBatch(uint32_t endTick, std::size_t maxEvents, std::vector<EventWord>& output,
                         std::vector<SysExBatchEvent>& sysExEvents, std::vector<uint8_t>& sysExBytes,
                         bool& complete, uint32_t& nextTick, bool& hasNextTick);
    bool buildHistoricalSysEx(uint32_t startTick, std::vector<SysExBatchEvent>& sysExEvents,
                              std::vector<uint8_t>& sysExBytes);
    bool buildHistoricalSelectorState(uint32_t startTick, std::vector<EventWord>& output);

    void resetRenderCursor(uint32_t startTick, bool perTrackColors);
    bool buildRenderSweep(uint32_t endTick, std::size_t maxSourceEvents, std::vector<VisualNote>& appends,
                          std::vector<RenderClose>& closes, uint32_t& appendBase, bool& complete,
                          uint32_t& nextTick, bool& hasNextTick);

    bool buildKeySnapshot(uint32_t tick, KeySnapshot& output);
    bool buildLiveSnapshot(double tick, double npsStartTick, double ccStartTick, LiveSnapshot& output,
                           bool forceReset = false);
    bool buildVisualPage(uint32_t pageStart, uint32_t pageEnd, std::vector<VisualNote>& output);

    // BPFA compressed source: 64 KiB zstd blocks, deduplicated, LRU-decoded.
    // own: a cursor's private block buffer (sourceBlockSize() bytes); null uses the shared
    // LRU cache. With thousands of tracks the shared cache thrashes (HANDOFF sec. 56).
    bool sourceBlock(uint64_t position, uint16_t& slotHint, const uint8_t*& data, uint64_t& begin,
                     uint64_t& end, uint8_t* own = nullptr) const;
    std::size_t sourceBlockSize() const { return sourceBlockSize_; }
    bool compressedSource() const { return fileBytes_.empty() && !onDemand_ && !sourceChunks_.empty(); }
    bool sourceSlotValid(uint16_t slot, uint64_t begin) const;
    uint64_t storedSourceBytes() const { return storedSourceBytes_; }

    // Parallel load (HANDOFF sec. 60). A helper indexes a contiguous range of tracks
    // and exports a blob; the main store merges the blobs in track order.
    struct PartialPlan {
        uint16_t format = 1;
        uint16_t trackCount = 0;
        uint16_t ticksPerBeat = 0;
        uint16_t firstTrack = 0;
        uint16_t endTrack = 0;
        uint64_t firstChunkPosition = 0;
        uint64_t rangeBegin = 0;
        uint64_t rangeEnd = 0;
        uint32_t blockSize = 64u * 1024u;
        bool compress = true;
        uint64_t snapshotBudget = 0;
    };
    bool indexPartial(uint64_t size, MidiReadAt readAt, void* readUser, const PartialPlan& plan,
                      MidiParseProgress progress = nullptr, void* progressUser = nullptr);
    const std::vector<uint8_t>& partialBlob() const { return partialBlob_; }
    void releasePartialBlob() { std::vector<uint8_t>().swap(partialBlob_); }

    bool beginMerge(uint64_t size, uint16_t format, uint16_t trackCount, uint16_t ticksPerBeat,
                    uint32_t blockSize, bool compressed);
    bool mergeReadRaw(MidiReadAt readAt, void* readUser, MidiParseProgress progress = nullptr,
                      void* progressUser = nullptr);
    bool mergePartial(const uint8_t* blob, std::size_t size);
    bool finishMerge(MidiDocument& metadata);
    static uint32_t blockSizeForTracks(uint32_t trackCount);

    std::size_t noteRecordCount() const { return notes_.size(); }
    std::size_t longNoteCount() const { return longNotes_.size(); }
    std::size_t memoryBytes() const;

private:
#pragma pack(push, 1)
    struct TrackCheckpoint {
        uint64_t byteOffset;
        uint64_t eventIndex;
        uint32_t tick;
        uint8_t runningStatus;
    };
    struct SeekStateEvent {
        uint32_t tick;
        uint16_t track;
        uint32_t order;
        uint32_t message;
    };
#pragma pack(pop)
    // Active-note snapshot of one track, for fast far seeks (HANDOFF sec. 48).
    struct SnapshotNote {
        uint32_t startTick;
        uint32_t openOrder;
        uint16_t key;
        uint8_t velocity;
        uint8_t reserved;
    };
    struct VisualSnapshot {
        TrackCheckpoint resume;
        std::vector<SnapshotNote> notes;
    };
    struct TrackStorage {
        uint64_t byteBegin = 0;
        uint64_t byteEnd = 0;
        uint32_t maxTick = 0;
        std::vector<TrackCheckpoint> checkpoints;
        std::vector<VisualSnapshot> snapshots;
    };
    struct SysExRef {
        uint32_t tick = 0;
        uint16_t track = 0;
        uint64_t order = 0;
        std::vector<uint8_t> data;
    };
    struct Cursor {
        uint64_t position = 0;
        uint64_t end = 0;
        uint32_t tick = 0;
        uint8_t runningStatus = 0;
        bool finished = false;
        uint32_t eventTick = 0;
        uint32_t eventMessage = 0;
        uint64_t nextIndex = 0;
        uint64_t eventIndex = 0;
        const uint8_t* block = nullptr;
        uint64_t blockBegin = 0;
        uint64_t blockEnd = 0;
        uint16_t blockSlot = 0xffffu;
        std::unique_ptr<uint8_t[]> own;
    };

    // Merged (tick, track) cursor over the original bytes; notesOnly keeps
    // NoteOn/NoteOff, the renderer's stream.
    struct MergeCursor {
        std::vector<Cursor> cursors;
        std::vector<uint16_t> heap;
        bool notesOnly = false;
        bool valid = false;
    };
    bool decodeNext(Cursor& cursor, bool notesOnly) const;
    void mergeReset(MergeCursor& m, uint32_t startTick, bool notesOnly);
    bool mergePeek(const MergeCursor& m, uint32_t& tick, uint32_t& message, uint16_t& track) const;
    uint64_t mergeOrder(const MergeCursor& m) const { return m.cursors[m.heap.front()].eventIndex; }
    void mergePop(MergeCursor& m);
    void buildNoteBlocks();
    void buildColorTables();
    // Single pass (HANDOFF sec. 59): the scan pulls bytes and each 8 MiB window is
    // read, deduplicated and compressed the first time the scan reaches it.
    struct IngestState;
    void beginIngest(uint64_t size, MidiReadAt readAt, void* readUser, MidiParseProgress progress,
                     void* progressUser);
    bool ingestNextWindow();
    bool finishIngest();
    std::unique_ptr<IngestState> ingest_;
    PartialPlan partial_;
    bool partialActive_ = false;
    uint64_t chunkBase_ = 0;
    std::vector<uint64_t> chunkKeys_;
    std::vector<uint8_t> partialBlob_;
    MidiDocument mergeOut_;
    uint64_t mergeTotalNotes_ = 0;
    uint64_t mergeTotalControls_ = 0;
    bool merging_ = false;
    void exportPartial(const MidiDocument& out, const std::vector<uint64_t>& notesPerTrack, uint64_t totalControls);
    void finalizeIndex(MidiDocument& out, uint64_t totalNotes, uint64_t totalControls, MidiDocument& metadata);
    bool copySource(uint64_t position, uint8_t* destination, std::size_t length) const;
    struct SourceChunk {
        uint64_t physicalOffset;
        uint32_t stored;
        uint32_t size;
    };
    struct CacheSlot {
        uint64_t chunk = ~uint64_t(0);
        uint64_t age = 0;
        std::vector<uint8_t> bytes;
    };
    std::vector<std::unique_ptr<uint8_t[]>> sourceSlabs_;
    std::vector<SourceChunk> sourceChunks_;
    uint64_t storedSourceBytes_ = 0;
    bool onDemand_ = false;
    uint64_t rawSourceLimit_ = 512ull * 1024ull * 1024ull;
    std::size_t onDemandRequestPageBytes_ = 0;
    std::size_t onDemandRequestPages_ = 0;
    std::size_t onDemandPageBytes_ = 64u * 1024u;
    std::size_t onDemandPages_ = 64;
    MidiReadAt readAt_ = nullptr;
    void* readUser_ = nullptr;
    mutable uint64_t onDemandReads_ = 0;
  public:
    uint64_t onDemandReads() const { return onDemandReads_; }
  private:
    uint64_t sourceSize_ = 0;
    mutable std::vector<CacheSlot> sourceCache_;
    std::size_t sourceBlockSize_ = 64u * 1024u;
    mutable void* decompressContext_ = nullptr;
  public:
    ~BpfaMidiStore();
  private:
    mutable uint64_t sourceCacheAge_ = 0;

    bool valid_ = false;
    std::string error_;
    MidiDocument metadata_;
    std::vector<uint8_t> fileBytes_;
    std::vector<TrackStorage> tracks_;
    std::vector<SeekStateEvent> stateEvents_;
    std::vector<SysExRef> sysex_;
    std::vector<PackedNote> notes_;
    std::vector<LongNote> longNotes_;
    std::vector<NoteBlock> noteBlocks_;
    std::vector<uint64_t> trackNoteBegin_;

    struct FirstNote {
        uint16_t seenMask = 0;
        std::array<uint32_t, 16> tick{};
        std::array<uint64_t, 16> order{};
        std::array<uint64_t, 16> closeOrder{};
    };
    std::vector<FirstNote> firstNotes_;
    std::array<uint8_t, 16> globalColors_{};
    std::vector<std::array<uint8_t, 16>> trackColors_;

    struct ActiveVisualNote { uint32_t startTick; uint8_t velocity; uint8_t color; uint64_t openOrder; };
    struct VisualState { std::unordered_map<uint32_t, std::deque<ActiveVisualNote>> pending; };
    struct DensityPoint { uint32_t tick; uint32_t count; };
    struct DeferredOff { uint32_t tick; uint32_t message; uint16_t track; };
    uint8_t colorByte(uint16_t track, uint8_t channel) const;
    struct PageItem {
        VisualNote note{};
        uint32_t track = 0;
        uint64_t closeOrder = ~uint64_t(0);
        uint64_t openOrder = 0;
        bool minimumDuration = false;
    };
    using OutputIndices = std::unordered_map<uint32_t, std::deque<std::size_t>>;
    struct VisualCheckpoint;
    void applyVisualEvent(VisualState& state, uint32_t tick, uint32_t message, uint16_t track, uint64_t order,
                          std::vector<PageItem>* output = nullptr, OutputIndices* outputIndices = nullptr) const;
    void closePageOrphans(VisualState& state, uint32_t pageStart, uint32_t pageEnd, std::vector<PageItem>& output,
                          OutputIndices& outputIndices) const;
    const VisualState& ensureCheckpoint(uint32_t targetTick);
    void closeExpiredOrphans(VisualState& state, uint32_t beforeTick) const;
    void rebuildVisualStateAt(uint32_t targetTick, VisualState& state) const;
    struct VisualCheckpointEntry { uint32_t tick; VisualState state; };
    std::vector<VisualCheckpointEntry> visualCheckpoints_;
    VisualState rollingVisualState_;
    uint32_t rollingVisualTick_ = 0;
    bool rollingVisualValid_ = false;
    VisualState liveState_;
    MergeCursor liveCursor_;
    bool liveCursorValid_ = false;
    double liveTick_ = -1.0;
    std::vector<DeferredOff> liveDeferredOffs_;
    std::deque<DensityPoint> liveNps_;
    std::deque<DensityPoint> liveCc_;
    uint64_t liveNpsCount_ = 0;
    uint64_t liveCcCount_ = 0;
    uint32_t visualCheckpointSpan_ = 1;

    MergeCursor eventCursor_;
    std::size_t eventSysExCursor_ = 0;

    MergeCursor renderCursor_;
    bool renderPerTrackColors_ = false;
    uint32_t renderHead_ = 1;
    std::array<uint16_t, 128u * 16u> renderActiveCounts_{};
    std::array<uint8_t, 128u * 16u> renderActiveColors_{};
    std::array<uint32_t, 128u * 16u> renderActiveIds_{};
};

} // namespace wasmidi
