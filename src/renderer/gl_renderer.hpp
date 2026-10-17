#pragma once

#include "note_raster_compositor.hpp"
#include <GLES3/gl3.h>

#include "../midi/midi_parser.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wasmidi {

class GLRenderer {
public:
    GLRenderer();
    ~GLRenderer();

    bool initialize();
    void resize(int width, int height);

    void setDocument(const MidiDocument* document);
    void setTransportRevision(uint64_t revision);
    void setCurrentTime(float seconds);
    void setNoteSpeed(float secondsPerWindow);
    void setPostBuffer(float seconds);
    void setPerTrackColors(bool enabled);
    void setChannelColor(uint8_t channel,
                         uint8_t r,
                         uint8_t g,
                         uint8_t b);
    void setNeuralVisual(float hue, float activity);

    bool renderRoll();

    // Called only by the browser visual-cache Worker bridge.
    void receiveVisualPage(uint32_t generation, uint32_t spanTicks,
                           uint32_t pageIndex, const uint32_t* words,
                           uint32_t noteCount, uint32_t sourceCount,
                           double difficulty);

    // Browser transport for the faithful SharpMIDI-raylib ring renderer.
    void receiveSharpRenderReset(uint32_t generation);
    void receiveSharpRenderDelta(
        uint32_t generation,
        uint32_t appendBase,
        const VisualNote* appends,
        uint32_t appendCount,
        const uint32_t* closeWords,
        uint32_t closeCount,
        uint32_t safeThrough,
        bool complete);

private:
    struct NeuralNode {
        float x = 0.0f;
        float y = 0.0f;
        float radius = 1.0f;
        float pulse = 0.0f;
        float pulseSpeed = 0.01f;
        float steer = 0.0f;
        float steerSpeed = 0.004f;
    };

    struct NeuralLineVertex {
        float x = 0.0f;
        float y = 0.0f;
        float alpha = 0.0f;
    };

    struct NeuralPointVertex {
        float x = 0.0f;
        float y = 0.0f;
        float size = 1.0f;
        float alpha = 1.0f;
    };

    bool createPrograms();
    void destroy();

    void initializeNeuralNodes();
    void updateNeuralNodes();
    void renderBackground();

    void allocateRing(std::size_t capacity);
    void ensureRingCapacity(std::size_t required);
    void uploadSourceRange(std::size_t begin, std::size_t end);
    void rebuildVisualCache(std::size_t begin, std::size_t end);
    void syncVisualCache(uint32_t viewStart, uint32_t viewEnd);
    void rebuildCarryCache(uint32_t viewStart, std::size_t desiredBegin);
    void advanceCarryCache(uint32_t viewStart, std::size_t oldBegin, std::size_t desiredBegin);
    void uploadCarryCache();
    void setInstanceBase(std::size_t physicalIndex);

    // SharpMIDI-raylib GLNoteRenderer port. Screen-page generation is not used
    // by this path; notes are opened/closed directly in a persistent ring.
    void resetSharpRenderer(bool requestRemote);
    void calculateSharpView(uint32_t& currentTick, uint32_t& viewStart,
                            uint32_t& viewEnd, uint32_t& windowTicks);
    void sweepLocalSharpRange(uint32_t fromTick, uint32_t toTick);
    void processLocalSharpEvent(uint32_t tick, const CompactEvent& event,
                                std::vector<uint32_t>& closeIds);
    void appendSharpNote(uint32_t startTick, uint8_t key, uint8_t velocity,
                         uint8_t noteIndex);
    void closeSharpNote(uint32_t noteId, uint32_t endTick,
                        std::vector<uint32_t>* closeIds = nullptr);
    void advanceSharpTail(uint32_t viewStart);
    void uploadSharpRange(uint32_t beginId, uint32_t endId);
    void uploadSharpCloseIds(std::vector<uint32_t>& closeIds);
    void requestRemoteSharpSweep(uint32_t startTick, uint32_t endTick,
                                 bool reset);
    void flushRemoteSharpBatches(uint32_t requiredThrough,
                                 uint32_t desiredThrough);
    void regenerateSharpPalette();
    void drawSharpRing(uint32_t currentTick, uint32_t viewStart,
                       uint32_t viewEnd, uint32_t windowTicks,
                       bool notesReady = true);
    uint32_t drawSharpRingRange(uint32_t viewEnd, bool notesReady);

    // BPFA-style culled tile cache (HANDOFF sec. 38).
    struct CullTile {
        uint32_t index = 0;
        GLuint vbo = 0;
        uint32_t count = 0;
        uint32_t coverage = 0;
    };
    void updateCullTiles(uint32_t viewStart, uint32_t viewEnd, uint32_t windowTicks, bool notesReady);
    void buildCullTile(uint32_t index, uint32_t windowTicks);
    CullTile* findCullTileMutable(uint32_t index);
    void releaseCullTiles();
    const CullTile* findCullTile(uint32_t index) const;
    std::vector<CullTile> cullTiles_;
    uint64_t cullSignature_ = 0;
    uint64_t sharpRingEpoch_ = 0;
    GLuint cullVao_ = 0;
    std::vector<CompositorNote> cullInput_;
    CompositorScratch cullScratch_;
    CompositorResult cullResult_;

    struct VisualPage {
        uint32_t spanTicks = 0;
        uint32_t pageIndex = 0;
        uint32_t sourceCount = 0;
        double difficulty = 0.0;
        std::vector<VisualNote> notes;
    };

    void resetVisualPageCache(bool reinstallDocument);
    void primeVisualPageCache(uint32_t viewStart, uint32_t viewEnd);
    bool collectCachedPageNotes(uint32_t searchStart, uint32_t viewEnd,
                                std::vector<VisualNote>& output) const;
    bool buildDenseDrawList(uint32_t viewStart, uint32_t viewEnd,
                            std::size_t desiredBegin, std::size_t desiredEnd);
    void uploadDenseDrawList();
    void drawDenseNotes();

    void calculateView(uint32_t& currentTick,
                       uint32_t& viewStart,
                       uint32_t& viewEnd) const;

    int width_ = 1;
    int height_ = 1;

    float currentTime_ = 0.0f;
    float noteSpeed_ = 1.0f;
    float postBuffer_ = 0.0f;
    bool perTrackColors_ = false;

    float neuralHue_ = 230.0f;
    float neuralTargetHue_ = 230.0f;
    float neuralActivity_ = 0.0f;

    const MidiDocument* document_ = nullptr;

    bool initialized_ = false;
    bool paletteDirty_ = true;
    bool forceCacheReset_ = true;

    GLuint noteProgram_ = 0;
    GLuint noteVao_ = 0;
    GLuint noteVbo_ = 0;
    GLuint carryVao_ = 0;
    GLuint carryVbo_ = 0;
    GLuint denseVao_ = 0;
    GLuint denseVbo_ = 0;
    GLuint sharpPaletteTexture_ = 0;

    GLint viewStartUniform_ = -1;
    GLint viewEndUniform_ = -1;
    GLint currentTickUniform_ = -1;
    GLint viewportWidthUniform_ = -1;
    GLint perTrackUniform_ = -1;
    GLint paletteUniform_ = -1;
    GLint metricsUniform_ = -1;
    GLint glowUniform_ = -1;
    GLint transparencyUniform_ = -1;

    GLuint backgroundProgram_ = 0;
    GLuint backgroundVao_ = 0;
    GLint backgroundHueUniform_ = -1;
    GLint backgroundActivityUniform_ = -1;
    GLint backgroundAspectUniform_ = -1;

    GLuint neuralProgram_ = 0;
    GLuint neuralLineVao_ = 0;
    GLuint neuralLineVbo_ = 0;
    GLuint neuralPointVao_ = 0;
    GLuint neuralPointVbo_ = 0;
    GLint neuralHueUniform_ = -1;
    GLint neuralPointModeUniform_ = -1;

    std::array<std::array<uint8_t, 4>, 16>
        channelColors_{};

    // CPU mirror of only the cached start-ordered visual range.
    std::vector<VisualNote> ring_;
    // Notes whose starts have left the rolling start-time cache but whose ends
    // still intersect the visible history. Kept in source order and drawn first.
    std::vector<VisualNote> carryNotes_;
    // Resolution-dependent dense draw list. It is rebuilt only for MIDI views
    // large enough to benefit, and contains only notes that can contribute at
    // least one currently visible pixel after source-order occlusion.
    std::vector<VisualNote> denseNotes_;
    std::vector<VisualNote> denseSourceScratch_;
    // One bit per snapped horizontal pixel per pitch. Bitset coverage makes
    // full-occlusion queries O(width/64) instead of scanning every pixel for
    // every rejected note at Black-MIDI crashpoints.
    std::vector<uint64_t> denseCoverage_;
    std::unordered_map<uint32_t, VisualPage> visualPages_;
    // Pages requested from the mapped Worker but not delivered yet. This
    // prevents per-frame request storms while still allowing a seek to cancel
    // the old window and immediately request the new one.
    std::unordered_set<uint32_t> visualPendingPages_;
    uint32_t visualCacheGeneration_ = 1;
    uint32_t visualPageSpanTicks_ = 0;
    uint32_t visualWantedFirstPage_ = 0;
    uint32_t visualWantedPageCount_ = 0;
    uint32_t visualCurrentPage_ = 0;
    int visualPrimeWidth_ = 0;
    // Remote mapped pages are immutable. Cache the assembled GPU draw list and
    // rebuild it only when one of the pages actually used by the viewport
    // changes, rather than concatenating/re-uploading page geometry every frame.
    uint64_t visualPageRevision_ = 1;
    uint64_t remoteDrawRevision_ = 0;
    uint32_t remoteDrawSpan_ = 0;
    uint32_t remoteDrawFirstPage_ = std::numeric_limits<uint32_t>::max();
    uint32_t remoteDrawLastPage_ = std::numeric_limits<uint32_t>::max();
    uint32_t remoteWantedDrawFirstPage_ = std::numeric_limits<uint32_t>::max();
    uint32_t remoteWantedDrawLastPage_ = std::numeric_limits<uint32_t>::max();
    std::size_t ringCapacity_ = 0;
    std::size_t ringMask_ = 0;

    // Absolute source indices into MidiDocument::visualNotes. Physical VBO
    // location is sourceIndex & ringMask_. This preserves source/draw order.
    std::size_t sourceBegin_ = 0;
    std::size_t sourceEnd_ = 0;

    // Absolute IDs match SharpMIDI's _head/_tail model. Physical VBO position
    // is id & ringMask_. uint16 overlap counts intentionally match ushort.
    uint32_t sharpHead_ = 1;
    uint32_t sharpTail_ = 1;
    int64_t sharpLastSweepEnd_ = -1;
    uint32_t sharpLastWindowTicks_ = 0;
    uint32_t sharpStableWindowTicks_ = 0;
    float sharpStableNoteSpeed_ = -1.0f;
    uint32_t sharpLookaheadTicks_ = 4000;
    bool sharpForceReset_ = true;
    uint64_t transportRevision_ = 0;
    bool transportRevisionValid_ = false;
    bool sharpModePerTrack_ = false;
    std::array<uint8_t, 256u * 4u> sharpPaletteData_{};
    bool sharpPaletteUploadPending_ = true;
    std::array<uint16_t, 128u * 16u> sharpActiveCount_{};
    std::array<uint8_t, 128u * 16u> sharpActiveColor_{};
    std::array<uint32_t, 128u * 16u> sharpActiveId_{};

    struct SharpRemoteBatch {
        uint32_t generation = 0;
        uint32_t appendBase = 0;
        std::vector<VisualNote> appends;
        std::vector<uint32_t> closeWords;
        uint32_t safeThrough = 0;
        bool complete = false;
    };
    std::vector<SharpRemoteBatch> sharpRemoteBatches_;

    uint32_t sharpRemoteGeneration_ = 1;
    uint32_t sharpRemoteRequestedEnd_ = 0;
    uint32_t sharpRemoteSafeThrough_ = 0;
    uint32_t sharpRemoteUrgentThrough_ = 0;
    uint32_t sharpRemoteStartTick_ = 0;
    bool sharpRemoteWaitingReset_ = false;
    std::chrono::steady_clock::time_point sharpCatchUpAt_{};
    std::chrono::steady_clock::time_point sharpHeldSince_{};
    std::chrono::steady_clock::time_point sharpReadySince_{};
    uint32_t sharpHeldTick_ = 0;
    uint32_t sharpCatchUpLead_ = 0;
    uint32_t sharpCatchUpFloor_ = 0;
    uint32_t sharpCatchUpFrom_ = 0;
    bool rollFrameValid_ = false;
    int rollFrameWidth_ = 0;
    int rollFrameHeight_ = 0;

    // Remote resets/seeks stream progressively into the same persistent ring.
    // Partial same-tick batches remain hidden until that tick is safe, but the
    // renderer never pins the whole UI to a previously presented frame.
    bool sharpRemoteRebuilding_ = false;
    uint32_t sharpRemoteBuildTarget_ = 0;
    uint32_t sharpRemoteBuildWindowTicks_ = 0;
    bool sharpRemoteBuildPerTrack_ = false;

    uint32_t sharpRemotePendingBase_ = 0;
    // Appends are pre-staged into the CPU/GPU ring as soon as they arrive, but
    // sharpHead_ is advanced only across complete ticks. This preserves the
    // no-flicker atomic-tick rule without a giant last-second upload.
    uint32_t sharpRemotePreparedHead_ = 1;
    std::vector<VisualNote> sharpRemotePendingAppends_;
    std::vector<uint32_t> sharpRemotePendingCloseWords_;
    std::size_t sharpRemotePendingCloseOffset_ = 0;


    std::array<NeuralNode, 95> neuralNodes_{};
    std::vector<NeuralLineVertex> neuralLines_;
    std::vector<NeuralPointVertex> neuralPoints_;

    std::chrono::steady_clock::time_point neuralClock_ =
        std::chrono::steady_clock::now();
};

} // namespace wasmidi
