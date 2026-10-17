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

    // Synth feed (HANDOFF sec. 70): the event stream already thinned by the synth's block
    // admission, decoded track by track per window instead of through the merge heap.
    struct SynthFeedConfig {
        uint32_t maxVoices = 1024;
        uint32_t admissionPercent = 25;   // 0 = keep every note-on
        uint32_t sampleRate = 48000;
        uint32_t blockFrames = 512;
        uint32_t velocityFloor = 0;
        uint32_t windowBlocks = 4;
    };
    struct SynthFeedStats {
        uint64_t decoded = 0;
        uint64_t noteOns = 0;
        uint64_t droppedNoteOns = 0;
        uint64_t droppedNoteOffs = 0;
    };
    void setSynthFeedConfig(const SynthFeedConfig& config) { feedConfig_ = config; }
    void resetSynthFeed(uint32_t startTick);
    // Emits every admitted event of whole admission windows until the window that
    // reaches endSeconds or until maxEvents is passed. safeSeconds: everything before
    // it has been emitted. complete: the song has no events left.
    bool buildSynthFeed(double endSeconds, std::size_t maxEvents, std::vector<EventWord>& output,
                        std::vector<double>& times, std::vector<SysExBatchEvent>& sysExEvents,
                        std::vector<uint8_t>& sysExBytes, double& safeSeconds, bool& complete);
    const SynthFeedStats& synthFeedStats() const { return feedStats_; }

    // Parallel synth feed (HANDOFF sec. 70). The feed is cut into chunks of whole admission
    // blocks; a chunk is decoded with local note-off pairing (feedDecodeChunk) and then
    // stitched in order against the carried pairing queues (feedStitch). buildSynthFeed does
    // both on this store; decode helpers do the first half on their own instance with only
    // the source and track ranges (exportFeedImage / importFeedImage).
    struct FeedChunk {
        uint64_t firstBlock = 0;
        uint32_t blocks = 0;
        uint32_t endTick = 0;
        bool anyLeft = false;
        SynthFeedStats stats;
        std::vector<EventWord> events;          // (tick, track) order, stacked notes (high byte)
        std::vector<uint32_t> deferred;         // (event index, track) of each deferred note-off
        std::vector<uint32_t> pending;          // per key: track << 11 | key, runs, (count << 1 | kept)...
    };
    using FeedSourceFetch = bool (*)(void* user, uint64_t physicalOffset, uint8_t* destination, std::size_t length);
    bool exportFeedImage(std::vector<uint8_t>& blob) const;
    bool importFeedImage(const uint8_t* blob, std::size_t size, FeedSourceFetch fetch, void* fetchUser);
    std::size_t sourceSlabCount() const { return sourceSlabs_.size(); }
    const uint8_t* sourceSlab(std::size_t index) const { return sourceSlabs_[index].get(); }
    static std::size_t sourceSlabBytes();
    // Coordinator: the request of `chunks` consecutive chunks of `blocks` blocks from firstBlock.
    bool buildFeedRequest(uint64_t firstBlock, uint32_t blocks, uint32_t chunks, std::vector<uint8_t>& request) const;
    // Helper: decodes one request into serialized FeedChunks (cursors carried across them).
    bool decodeFeedRequest(const uint8_t* request, std::size_t size, std::vector<uint8_t>& result);
    // Coordinator: stitches the chunks of a result (results must arrive in block order).
    bool stitchFeedResult(const uint8_t* result, std::size_t size, std::vector<EventWord>& output,
                          std::vector<double>& times, std::vector<SysExBatchEvent>& sysExEvents,
                          std::vector<uint8_t>& sysExBytes, double& safeSeconds, bool& complete);
    uint64_t synthFeedBlock() const { return feedBlock_; }
    double synthFeedBlockSeconds() const;

    // Visual stream (HANDOFF sec. 71): the renderer sweep and the live snapshot read one
    // stream of folded note/controller words in (tick, track) order, decoded here span by
    // span or by the decode helpers. Disabled, both walk the merge heap as before.
    void setVisualStreamEnabled(bool enabled) { visualEnabled_ = enabled; }
    bool visualStreamEnabled() const { return visualEnabled_; }
    // false: buildRenderSweep only consumes what appendVisualResult delivered.
    void setVisualAutoDecode(bool enabled) { visualAutoDecode_ = enabled; }
    uint32_t visualStreamHead() const { return visHead_; }
    bool visualStreamDone() const { return visDone_; }
    // False while the stream holds VisualPlanWords past the live snapshot: no new span.
    bool visualRoomAhead() const;
    // End tick of a span from startTick that holds about sourceEvents events (from the density).
    uint32_t visualSpanEnd(uint32_t startTick, uint64_t sourceEvents) const;
    bool decodeVisualLocal(uint32_t endTick);
    bool buildVisualRequest(uint32_t startTick, uint32_t endTick, std::vector<uint8_t>& request) const;
    bool decodeVisualRequest(const uint8_t* request, std::size_t size, std::vector<uint8_t>& result);
    // Results must arrive in order: each one starts where the stream ends.
    bool appendVisualResult(const uint8_t* result, std::size_t size);

    void resetRenderCursor(uint32_t startTick, bool perTrackColors);
    bool buildRenderSweep(uint32_t endTick, std::size_t maxSourceEvents, std::vector<VisualNote>& appends,
                          std::vector<RenderClose>& closes, uint32_t& appendBase, bool& complete,
                          uint32_t& nextTick, bool& hasNextTick);

    bool buildKeySnapshot(uint32_t tick, KeySnapshot& output);
    bool buildLiveSnapshot(double tick, double npsStartTick, double ccStartTick, LiveSnapshot& output,
                           bool forceReset = false);
    bool buildVisualPage(uint32_t pageStart, uint32_t pageEnd, std::vector<VisualNote>& output);

    // Keyboard Worker (HANDOFF sec. 83): with the decode helpers the live snapshot runs on a
    // second instance that holds a copy of the stream, the colors and the track ends; on a
    // reset it takes the keys rebuilt by this store (exportLiveRebuild).
    enum class LiveStep { Done, Rebuild, RebuildDensity, Failed };
    bool exportLiveImage(std::vector<uint8_t>& blob) const;
    bool importLiveImage(const uint8_t* blob, std::size_t size);
    bool exportVisualStream(std::vector<uint8_t>& blob) const;
    bool importVisualStream(const uint8_t* blob, std::size_t size);
    void restartVisualStream(uint32_t startTick);
    // density: also the density window decoded from the checkpoints (RebuildDensity).
    bool exportLiveRebuild(double tick, double npsStartTick, double ccStartTick, bool density,
                           std::vector<uint8_t>& blob);
    LiveStep liveWorkerStep(double tick, double npsStartTick, double ccStartTick, bool forceReset,
                            LiveSnapshot& output);
    bool liveWorkerResume(const uint8_t* rebuild, std::size_t size, double tick, double npsStartTick,
                          double ccStartTick, LiveSnapshot& output);
    // Coordinator: the keyboard Worker reads the stream; its position (-1: off the stream)
    // bounds how far ahead spans are planned.
    void setLiveExternal(bool external);
    void setLiveExternalPosition(int64_t position) { liveExternalPos_ = position; }
    int64_t liveStreamPosition() const { return liveOnStream_ ? int64_t(livePos_) : -1; }

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
    // Per (channel, key) with open notes: how many, and when the newest one started.
    struct LiveSnapshotKey {
        uint16_t key;
        uint16_t reserved;
        uint32_t total;
        uint32_t tailStart;
    };
    struct LiveSnapshotPoint {
        TrackCheckpoint resume;
        std::vector<LiveSnapshotKey> keys;
    };
    struct TrackStorage {
        uint64_t byteBegin = 0;
        uint64_t byteEnd = 0;
        uint32_t maxTick = 0;
        uint64_t channelEnd = 0;  // just past the last channel event (rev. 82)
        std::vector<TrackCheckpoint> checkpoints;
        std::vector<VisualSnapshot> snapshots;
        std::vector<LiveSnapshotPoint> liveSnapshots;
    };
    // Cursors stop after the last channel event: a tempo-only tail is never re-read.
    static uint64_t cursorEnd(const TrackStorage& t) { return t.channelEnd ? t.channelEnd : t.byteEnd; }
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
public:
    struct TextEvent { uint32_t tick = 0; uint16_t track = 0; uint8_t type = 0; std::string text; };
    const std::vector<TextEvent>& textEvents() const { return textEvents_; }
    // Note-ons per `densityBucketTicks()` ticks, for the NPS graph of WASMIDI Audio.
    const std::vector<uint32_t>& noteDensity() const { return density_; }
    uint32_t densityBucketTicks() const { return densityBucketTicks_; }
private:
    static constexpr std::size_t MaxTextEvents = 200000;
    static constexpr std::size_t MaxTextBytes = 256;
    std::vector<TextEvent> textEvents_;
    std::vector<uint32_t> density_;
    uint32_t densityBucketTicks_ = 1;
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

    struct FeedRun { uint32_t count = 0; uint32_t next = ~0u; uint8_t kept = 0; };
    struct FeedQueue { uint32_t head = ~0u; uint32_t tail = ~0u; };
    // Note-off pairing queues per (track, channel, key): runs of kept/dropped note-ons
    // in a shared pool (a linked list per key; nearly always one run long).
    struct FeedPairs {
        std::vector<std::array<std::unique_ptr<FeedQueue[]>, 16>> heads;
        std::vector<FeedRun> runs;
        uint32_t freeRun = ~0u;
        std::vector<uint32_t> touched;          // track << 11 | key that became non-empty
        void reset(std::size_t tracks);
        FeedQueue& queue(uint32_t track, uint32_t key);
        void push(FeedQueue& queue, bool kept, uint32_t count, uint32_t trackKey);
        // Pops up to count note-ons; kept: how many were kept; returns the count left unpaired.
        uint32_t pop(FeedQueue& queue, uint32_t count, uint32_t& kept);
    };
    // Decoded feed event; the empty constructor keeps vector growth from zero-filling the
    // room reserved ahead of the decode loop.
    struct FeedWord { uint32_t tick; uint32_t packed; FeedWord() {} FeedWord(uint32_t t, uint32_t m) : tick(t), packed(m) {} };
    void decodeTrackUntil(Cursor& cursor, uint32_t endTick, std::vector<FeedWord>& out);
    void foldEvent(std::vector<FeedWord>& out, std::size_t& used, uint32_t tick, uint32_t message);
    std::array<uint32_t, 4096> unitTag_{}, unitAt_{};
    uint32_t unitGen_ = 0, unitTick_ = ~0u;
    void feedEdges(uint64_t firstBlock, uint32_t blocks, uint32_t* edge) const;
    void feedDecodeChunk(std::vector<Cursor>& cursors, const uint32_t* edge, uint32_t blocks, uint32_t admit,
                         uint8_t velocityFloor, FeedChunk& out);
    void feedStitch(const FeedChunk& chunk, std::vector<EventWord>& output, std::vector<double>& times,
                    std::vector<SysExBatchEvent>& sysExEvents, std::vector<uint8_t>& sysExBytes,
                    double& safeSeconds, bool& complete);
    uint32_t feedAdmit() const;
    SynthFeedConfig feedConfig_;
    SynthFeedStats feedStats_;
    MergeCursor feedCursor_;
    std::size_t feedSysExCursor_ = 0;
    uint64_t feedBlock_ = 0;       // next admission block (absolute, from song time 0)
    uint32_t feedStartTick_ = 0;   // seek tick of the current feed
    uint32_t feedTick_ = 0;        // events before this tick were consumed
    uint32_t feedCursorTick_ = 0;  // where feedCursor_ stands (sequential path)
    bool feedValid_ = false;
    std::vector<FeedWord> feedRaw_;
    std::vector<EventWord> feedKept_;
    std::vector<uint32_t> feedKeptTrack_;     // bit 31: deferred note-off
    std::vector<uint32_t> feedTrackEnd_, feedOrder_;
    FeedPairs feedCarried_, feedLocal_;
    FeedChunk feedChunk_;
    FeedSourceFetch externalFetch_ = nullptr;
    void* externalFetchUser_ = nullptr;
    mutable std::vector<uint8_t> externalScratch_;

    bool buildRenderSweepStream(uint32_t endTick, std::size_t maxWords, std::vector<VisualNote>& appends,
                                std::vector<RenderClose>& closes, bool& complete, uint32_t& nextTick,
                                bool& hasNextTick);
    void renderStep(uint32_t tick, uint32_t message, uint16_t track, uint32_t count,
                    std::vector<VisualNote>& appends, std::vector<RenderClose>& closes);
    void visualReset(uint32_t startTick);
    void visualDecodeSpan(std::vector<Cursor>& cursors, uint32_t endTick, std::vector<EventWord>& events,
                          std::vector<uint16_t>& tracks, bool& anyLeft);
    void visualAppend(const EventWord* events, const uint16_t* tracks, std::size_t count, uint32_t endTick,
                      bool anyLeft);
    void visualTrim();
    bool visualEnabled_ = true;
    bool visualAutoDecode_ = true;
    bool visValid_ = false;
    bool visDone_ = false;
    std::vector<EventWord> visEvents_;
    std::vector<uint16_t> visTracks_;
    uint64_t visBase_ = 0;          // absolute index of visEvents_[0]
    uint32_t visStart_ = 0;         // the stream holds every event in [visStart_, visHead_)
    uint32_t visHead_ = 0;
    MergeCursor visCursor_;
    uint32_t visCursorTick_ = ~0u;
    std::vector<FeedWord> visRaw_;
    std::vector<uint16_t> visRawTrack_;
    std::vector<uint32_t> visOrder_;
    std::vector<uint32_t> visTickCount_;
    std::vector<EventWord> visSpanEvents_;
    std::vector<uint16_t> visSpanTracks_;
    uint64_t renderPos_ = 0;        // absolute index of the next word for the sweep

    // Live snapshot on the stream: per (track, channel, key) the number of open notes and
    // the newest one still open (offs close the oldest first, so it only changes on a note-on
    // or when the key empties). Each (track, channel) gets 128 slots on its first note
    // (HANDOFF sec. 82).
    struct LiveKey { uint32_t total = 0; uint32_t tailStart = 0; uint32_t activeAt = ~0u; uint32_t tailColor = 0; };
    struct LiveDeferred { uint32_t tick; uint32_t message; uint16_t track; uint32_t count; };
    bool buildLiveSnapshotMerge(double tick, double npsStartTick, double ccStartTick, LiveSnapshot& output,
                                bool forceReset);
    bool buildLiveSnapshotStream(double tick, double npsStartTick, double ccStartTick, LiveSnapshot& output,
                                 bool forceReset);
    void applyLiveEvent(uint32_t tick, uint32_t message, uint16_t track, uint32_t count);
    void liveRebuildAt(uint32_t targetTick);
    std::vector<FeedWord> liveWords_;
    uint32_t liveSlot(uint16_t track, uint8_t channel, uint8_t pitch, bool create);
    void liveClearSlot(uint32_t slot);
    uint32_t liveSlotKey(uint32_t slot) const { return livePairKey_[slot >> 7] | (slot & 127u); }
    void liveReset();
    std::vector<uint32_t> livePairOf_;     // track * 16 + channel -> pair, ~0u before its first note
    std::vector<uint32_t> livePairKey_;    // pair -> (track << 11) | (channel << 7)
    std::vector<LiveKey> liveSlots_;       // pair * 128 + pitch
    std::vector<uint32_t> liveActive_;     // slots with notes, in no particular order
    std::vector<LiveDeferred> liveDeferred_;
    uint32_t liveNextTick_ = 0;     // every event before it was applied (or deferred)
    bool liveOnStream_ = false;
    bool liveCursorSynced_ = false;
    bool liveSinceVisualReset_ = false;  // a live snapshot ran since the stream restarted
    LiveSnapshot liveHeld_{};             // the last keys, shown while the stream is ahead
    uint64_t livePos_ = 0;
    bool liveOnly_ = false;               // keyboard Worker instance
    bool liveExternal_ = false;           // coordinator with a keyboard Worker
    int64_t liveExternalPos_ = -1;
    struct LiveRequest { double tick = 0, npsStartTick = 0, ccStartTick = 0; uint32_t wholeTick = 0; bool exactTick = false; };
    enum class LiveAction { Hold, Reset, Advance, Stay };
    bool liveRequest(double tick, double npsStartTick, double ccStartTick, LiveRequest& r) const;
    LiveAction liveDecide(const LiveRequest& r, bool forceReset) const;
    void liveAddDensity(uint32_t tick, uint32_t message, uint32_t count);
    void liveConsume(const LiveRequest& r, uint32_t tick, uint32_t message, uint16_t track, uint32_t count,
                     bool density);
    void liveBeginReset();
    bool liveDensityOnStream(const LiveRequest& r) const;
    void liveDensityFromStream(const LiveRequest& r);
    void liveDensityDecode(const LiveRequest& r);
    void liveEndReset(const LiveRequest& r);
    void liveAdvance(const LiveRequest& r);
    void liveStreamRun(uint32_t endTick);
    void liveFinish(const LiveRequest& r, LiveSnapshot& output);

    MergeCursor renderCursor_;
    bool renderPerTrackColors_ = false;
    uint32_t renderHead_ = 1;
    std::array<uint16_t, 128u * 16u> renderActiveCounts_{};
    std::array<uint8_t, 128u * 16u> renderActiveColors_{};
    std::array<uint32_t, 128u * 16u> renderActiveIds_{};
};

} // namespace wasmidi
