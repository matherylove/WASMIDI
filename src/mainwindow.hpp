#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QColor>
#include <QElapsedTimer>
#include <QUrl>
#include <QVariantList>
#include <QVector>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

#include "midi/midi_parser.hpp"
#include "midi/scheduler.hpp"

class MainWindow : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(PlayerController)

    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool hasMidi READ hasMidi NOTIFY noteCountChanged)
    Q_PROPERTY(float currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(float duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileNameChanged)
    Q_PROPERTY(qint64 noteCount READ noteCount NOTIFY noteCountChanged)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY trackCountChanged)
    Q_PROPERTY(int activeVoices READ activeVoices NOTIFY activeVoicesChanged)
    Q_PROPERTY(int activeChannelCount READ activeChannelCount NOTIFY activeChannelCountChanged)
    Q_PROPERTY(int nps READ nps NOTIFY npsChanged)
    Q_PROPERTY(int ccPerSecond READ ccPerSecond NOTIFY ccPerSecondChanged)
    Q_PROPERTY(float bpm READ bpm NOTIFY bpmChanged)
    Q_PROPERTY(float noteSpeed READ noteSpeed WRITE setNoteSpeed NOTIFY noteSpeedChanged)
    Q_PROPERTY(float postBuffer READ postBuffer WRITE setPostBuffer NOTIFY postBufferChanged)
    Q_PROPERTY(bool postBufferAuto READ postBufferAuto NOTIFY postBufferAutoChanged)
    Q_PROPERTY(bool perTrackColors READ perTrackColors WRITE setPerTrackColors NOTIFY perTrackColorsChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(int midiFormat READ midiFormat NOTIFY midiFormatChanged)
    Q_PROPERTY(int ppq READ ppq NOTIFY ppqChanged)
    Q_PROPERTY(int tempoChangeCount READ tempoChangeCount NOTIFY tempoChangeCountChanged)
    Q_PROPERTY(qint64 controlEventCount READ controlEventCount NOTIFY controlEventCountChanged)
    Q_PROPERTY(int peakNps READ peakNps NOTIFY peakNpsChanged)
    Q_PROPERTY(float peakNpsTime READ peakNpsTime NOTIFY timelineChanged)
    Q_PROPERTY(int peakPolyphony READ peakPolyphony NOTIFY peakPolyphonyChanged)
    Q_PROPERTY(QString pitchRange READ pitchRange NOTIFY pitchRangeChanged)
    Q_PROPERTY(int skippedVelocity READ skippedVelocity NOTIFY skippedVelocityChanged)
    Q_PROPERTY(QVariantList npsTimeline READ npsTimeline NOTIFY timelineChanged)
    Q_PROPERTY(quint64 documentRevision READ documentRevision NOTIFY documentRevisionChanged)
    Q_PROPERTY(quint64 transportRevision READ transportRevision NOTIFY transportRevisionChanged)
    Q_PROPERTY(bool midiLoading READ midiLoading NOTIFY midiLoadingChanged)
    Q_PROPERTY(int midiLoadingProgress READ midiLoadingProgress NOTIFY midiLoadingChanged)
    Q_PROPERTY(QString midiLoadingStage READ midiLoadingStage NOTIFY midiLoadingChanged)

    Q_PROPERTY(bool synthReady READ synthReady NOTIFY synthStateChanged)
    Q_PROPERTY(bool soundfontLoaded READ soundfontLoaded NOTIFY synthStateChanged)
    Q_PROPERTY(QString soundfontName READ soundfontName NOTIFY synthStateChanged)
    Q_PROPERTY(QString synthStatus READ synthStatus NOTIFY synthStateChanged)
    Q_PROPERTY(int synthSampleRate READ synthSampleRate NOTIFY synthStateChanged)
    Q_PROPERTY(int synthActiveVoices READ synthActiveVoices NOTIFY synthStateChanged)
    Q_PROPERTY(int synthFreeVoices READ synthFreeVoices NOTIFY synthStateChanged)
    Q_PROPERTY(int synthSteals READ synthSteals NOTIFY synthStateChanged)
    Q_PROPERTY(int synthRebalanced READ synthRebalanced NOTIFY synthStateChanged)
    Q_PROPERTY(int synthDroppedNotes READ synthDroppedNotes NOTIFY synthStateChanged)
    Q_PROPERTY(double synthRenderLoad READ synthRenderLoad NOTIFY synthStateChanged)
    Q_PROPERTY(double synthRenderLatencyMs READ synthRenderLatencyMs NOTIFY synthStateChanged)
    Q_PROPERTY(double synthDispatchMs READ synthDispatchMs NOTIFY synthStateChanged)
    Q_PROPERTY(int synthSimdPercent READ synthSimdPercent NOTIFY synthStateChanged)
    Q_PROPERTY(int synthWorkersActive READ synthWorkersActive NOTIFY synthStateChanged)
    Q_PROPERTY(double synthAllocMs READ synthAllocMs NOTIFY synthStateChanged)
    Q_PROPERTY(int synthFreeRatio READ synthFreeRatio NOTIFY synthStateChanged)
    Q_PROPERTY(int synthCcCollapsed READ synthCcCollapsed NOTIFY synthStateChanged)
    Q_PROPERTY(int synthCcHotNumber READ synthCcHotNumber NOTIFY synthStateChanged)
    Q_PROPERTY(int synthCcHotPercent READ synthCcHotPercent NOTIFY synthStateChanged)
    Q_PROPERTY(int synthCcHotCount READ synthCcHotCount NOTIFY synthStateChanged)
    Q_PROPERTY(int synthPresampleSeen READ synthPresampleSeen NOTIFY synthStateChanged)
    Q_PROPERTY(int synthPresampleResampled READ synthPresampleResampled NOTIFY synthStateChanged)
    Q_PROPERTY(int synthPresampleSkipped READ synthPresampleSkipped NOTIFY synthStateChanged)
    Q_PROPERTY(int synthMissReason READ synthMissReason NOTIFY synthStateChanged)
    Q_PROPERTY(int synthMissPercent READ synthMissPercent NOTIFY synthStateChanged)
    Q_PROPERTY(double synthWorkerBusyMs READ synthWorkerBusyMs NOTIFY synthStateChanged)
    Q_PROPERTY(int synthPumpGapMs READ synthPumpGapMs NOTIFY synthStateChanged)
    Q_PROPERTY(int synthRingFill READ synthRingFill NOTIFY synthStateChanged)
    Q_PROPERTY(int synthLayers READ synthLayers NOTIFY synthStateChanged)
    Q_PROPERTY(int synthRegions READ synthRegions NOTIFY synthStateChanged)
    Q_PROPERTY(int synthUnderruns READ synthUnderruns NOTIFY synthStateChanged)
    Q_PROPERTY(int synthMaxVoices READ synthMaxVoices WRITE setSynthMaxVoices NOTIFY synthConfigChanged)
    Q_PROPERTY(int synthMinVoices READ synthMinVoices WRITE setSynthMinVoices NOTIFY synthConfigChanged)
    Q_PROPERTY(int synthBufferFrames READ synthBufferFrames WRITE setSynthBufferFrames NOTIFY synthConfigChanged)
    Q_PROPERTY(int synthNumBuffers READ synthNumBuffers WRITE setSynthNumBuffers NOTIFY synthConfigChanged)
    Q_PROPERTY(float synthPrebufferSeconds READ synthPrebufferSeconds WRITE setSynthPrebufferSeconds NOTIFY synthConfigChanged)
    Q_PROPERTY(int synthVelocityFloor READ synthVelocityFloor WRITE setSynthVelocityFloor NOTIFY synthConfigChanged)
    Q_PROPERTY(int synthRequestedSampleRate READ synthRequestedSampleRate WRITE setSynthRequestedSampleRate NOTIFY synthConfigChanged)
    Q_PROPERTY(int synthChannels READ synthChannels WRITE setSynthChannels NOTIFY synthConfigChanged)
    Q_PROPERTY(int synthBitsPerSample READ synthBitsPerSample WRITE setSynthBitsPerSample NOTIFY synthConfigChanged)
    Q_PROPERTY(bool synthRealtimePriority READ synthRealtimePriority WRITE setSynthRealtimePriority NOTIFY synthConfigChanged)
    Q_PROPERTY(int synthWorkers READ synthWorkers WRITE setSynthWorkers NOTIFY synthConfigChanged)
    Q_PROPERTY(int synthWorkerCount READ synthWorkerCount NOTIFY synthStateChanged)
    Q_PROPERTY(int synthNoteSharding READ synthNoteSharding WRITE setSynthNoteSharding NOTIFY synthConfigChanged)
    Q_PROPERTY(bool synthStealScoreCache READ synthStealScoreCache WRITE setSynthStealScoreCache NOTIFY synthConfigChanged)
    Q_PROPERTY(bool synthDebugMetrics READ synthDebugMetrics WRITE setSynthDebugMetrics NOTIFY synthConfigChanged)
    Q_PROPERTY(bool synthFastNoteOff READ synthFastNoteOff WRITE setSynthFastNoteOff NOTIFY synthConfigChanged)
    Q_PROPERTY(bool synthValidateState READ synthValidateState WRITE setSynthValidateState NOTIFY synthConfigChanged)
    Q_PROPERTY(bool synthSoftClip READ synthSoftClip WRITE setSynthSoftClip NOTIFY synthConfigChanged)
    Q_PROPERTY(bool synthOverlapGain READ synthOverlapGain WRITE setSynthOverlapGain NOTIFY synthConfigChanged)
    Q_PROPERTY(bool synthGpu READ synthGpu WRITE setSynthGpu NOTIFY synthConfigChanged)
    // Roll modes for the next MIDI (HANDOFF sec. 89): 1 look-ahead with a RAM limit, 2 whole song.
    Q_PROPERTY(int rollMode READ rollMode WRITE setRollMode NOTIFY rollConfigChanged)
    Q_PROPERTY(int rollLookAheadMB READ rollLookAheadMB WRITE setRollLookAheadMB NOTIFY rollConfigChanged)

    Q_PROPERTY(QVariantList channelColorList READ channelColorList NOTIFY channelColorsChanged)
    Q_PROPERTY(float dominantHue READ dominantHue NOTIFY neuralVisualChanged)
    Q_PROPERTY(float neuralActivity READ neuralActivity NOTIFY neuralVisualChanged)

public:
    explicit MainWindow(QObject* parent = nullptr);
    ~MainWindow() override;

    bool isPlaying() const { return isPlaying_; }
    bool hasMidi() const { return noteCount_ > 0; }
    float currentTime() const { return currentTime_; }
    float duration() const { return duration_; }
    QString fileName() const { return fileName_; }
    qint64 noteCount() const { return noteCount_; }
    int trackCount() const { return trackCount_; }
    int activeVoices() const { return activeVoices_; }
    int activeChannelCount() const { return activeChannelCount_; }
    int nps() const { return nps_; }
    int ccPerSecond() const { return ccPerSecond_; }
    float bpm() const { return bpm_; }
    float noteSpeed() const { return noteSpeed_; }
    float postBuffer() const { return postBuffer_; }
    bool postBufferAuto() const { return postBufferAuto_; }
    bool perTrackColors() const { return perTrackColors_; }
    int volume() const { return volume_; }
    int midiFormat() const { return midiFormat_; }
    int ppq() const { return ppq_; }
    int tempoChangeCount() const { return tempoChangeCount_; }
    qint64 controlEventCount() const { return controlEventCount_; }
    int peakNps() const { return peakNps_; }
    float peakNpsTime() const { return peakNpsTime_; }
    int peakPolyphony() const { return peakPolyphony_; }
    QString pitchRange() const { return pitchRange_; }
    int skippedVelocity() const { return skippedVelocity_; }
    QVariantList npsTimeline() const { return npsTimeline_; }
    quint64 documentRevision() const { return documentRevision_; }
    quint64 transportRevision() const { return transportRevision_; }
    bool midiLoading() const { return midiLoading_; }
    int midiLoadingProgress() const { return midiLoadingProgress_; }
    QString midiLoadingStage() const { return midiLoadingStage_; }

    bool synthReady() const { return synthReady_; }
    bool soundfontLoaded() const { return soundfontLoaded_; }
    QString soundfontName() const { return soundfontName_; }
    QString synthStatus() const { return synthStatus_; }
    int synthSampleRate() const { return synthSampleRate_; }
    int synthActiveVoices() const { return synthActiveVoices_; }
    int synthFreeVoices() const { return synthFreeVoices_; }
    int synthSteals() const { return synthSteals_; }
    int synthRebalanced() const { return synthRebalanced_; }
    int synthDroppedNotes() const { return synthDroppedNotes_; }
    double synthRenderLoad() const { return synthRenderLoad_; }
    double synthRenderLatencyMs() const { return synthRenderLatencyMs_; }
    double synthDispatchMs() const { return synthDispatchMs_; }
    int synthSimdPercent() const { return synthSimdPercent_; }
    int synthWorkersActive() const { return synthWorkersActive_; }
    double synthAllocMs() const { return synthAllocMs_; }
    int synthFreeRatio() const { return synthFreeRatio_; }
    int synthCcCollapsed() const { return synthCcCollapsed_; }
    int synthCcHotNumber() const { return synthCcHotNumber_; }
    int synthCcHotPercent() const { return synthCcHotPercent_; }
    int synthCcHotCount() const { return synthCcHotCount_; }
    bool synthDebugMetrics() const { return synthDebugMetrics_; }
    void setSynthDebugMetrics(bool value);
    int synthPresampleSeen() const { return synthPresampleSeen_; }
    int synthPresampleResampled() const { return synthPresampleResampled_; }
    int synthPresampleSkipped() const { return synthPresampleSkipped_; }
    int synthMissReason() const { return synthMissReason_; }
    int synthMissPercent() const { return synthMissPercent_; }
    double synthWorkerBusyMs() const { return synthWorkerBusyMs_; }
    int synthPumpGapMs() const { return synthPumpGapMs_; }
    int synthRingFill() const { return synthRingFill_; }
    int synthLayers() const { return synthLayers_; }
    int synthRegions() const { return synthRegions_; }
    int synthUnderruns() const { return synthUnderruns_; }
    int synthMaxVoices() const { return synthMaxVoices_; }
    int synthMinVoices() const { return synthMinVoices_; }
    int synthBufferFrames() const { return synthBufferFrames_; }
    int synthNumBuffers() const { return synthNumBuffers_; }
    float synthPrebufferSeconds() const { return synthPrebufferSeconds_; }
    int synthVelocityFloor() const { return synthVelocityFloor_; }
    int synthRequestedSampleRate() const { return synthRequestedSampleRate_; }
    int synthChannels() const { return synthChannels_; }
    int synthBitsPerSample() const { return synthBitsPerSample_; }
    bool synthRealtimePriority() const { return synthRealtimePriority_; }
    int synthWorkers() const { return synthWorkers_; }
    int synthWorkerCount() const { return synthWorkerCount_; }
    int synthNoteSharding() const { return synthNoteSharding_; }
    bool synthStealScoreCache() const { return synthStealScoreCache_; }
    bool synthFastNoteOff() const { return synthFastNoteOff_; }
    bool synthValidateState() const { return synthValidateState_; }
    bool synthSoftClip() const { return synthSoftClip_; }
    bool synthOverlapGain() const { return synthOverlapGain_; }
    bool synthGpu() const { return synthGpu_; }
    int rollMode() const { return rollMode_; }
    int rollLookAheadMB() const { return rollLookAheadMB_; }

    QVariantList channelColorList() const;
    float dominantHue() const { return dominantHue_; }
    float neuralActivity() const { return neuralActivity_; }

    const wasmidi::MidiDocument& document() const { return document_; }
    const QVector<QColor>& channelColors() const { return channelColors_; }

    std::array<uint8_t, 128> activePitchMask() const {
        return visualPitchMask_;
    }

    std::array<int8_t, 128> activePitchColorIndices() const {
        return visualPitchColor_;
    }

    Q_INVOKABLE bool loadMidiFile(const QByteArray& data);
    Q_INVOKABLE bool loadMidiFileNamed(const QByteArray& data, const QString& fileName);
    bool loadMidiRaw(const uint8_t* data, std::size_t size, const QString& fileName);
    bool loadMidiSerializedRaw(const uint8_t* data, std::size_t size, const QString& fileName);
    void setMidiLoadingProgress(int progress, const QString& stage);
    void failMidiLoading(const QString& message);
    void receiveKeyboardVisualPage(
        uint32_t generation,
        uint32_t spanTicks,
        uint32_t pageIndex,
        const uint32_t* words,
        uint32_t wordCount);
    void receiveRemoteKeyState(
        double tick,
        const uint32_t* words,
        uint32_t wordCount);
    Q_INVOKABLE bool loadMidiUrl(const QUrl& url);
    Q_INVOKABLE void openMidiPicker();
    Q_INVOKABLE void openSoundfontPicker();
    Q_INVOKABLE void clearSoundfonts();
    Q_INVOKABLE void clearFile();
    Q_INVOKABLE void notifyVisualizerFramePresented();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(float seconds);
    Q_INVOKABLE void setPostBufferAuto();
    Q_INVOKABLE void setChannelColor(int channel, const QColor& color);

    void setNoteSpeed(float speed);
    void setPostBuffer(float buffer);
    void setPerTrackColors(bool enable);
    void setVolume(int value);
    void setSynthMaxVoices(int value);
    void setSynthMinVoices(int value);
    void setSynthBufferFrames(int value);
    void setSynthNumBuffers(int value);
    void setSynthPrebufferSeconds(float value);
    void setSynthVelocityFloor(int value);
    void setSynthRequestedSampleRate(int value);
    void setSynthChannels(int value);
    void setSynthBitsPerSample(int value);
    void setSynthRealtimePriority(bool enabled);
    void setSynthWorkers(int value);
    void setSynthNoteSharding(int value);
    void setSynthStealScoreCache(bool enabled);
    void setSynthFastNoteOff(bool enabled);
    void setSynthValidateState(bool enabled);
    void setSynthSoftClip(bool enabled);
    void setSynthOverlapGain(bool enabled);
    void setSynthGpu(bool enabled);
    void setRollMode(int mode);
    void setRollLookAheadMB(int megabytes);

signals:
    void playingChanged();
    void currentTimeChanged();
    void durationChanged();
    void fileNameChanged();
    void noteCountChanged();
    void trackCountChanged();
    void activeVoicesChanged();
    void activeChannelCountChanged();
    void npsChanged();
    void ccPerSecondChanged();
    void bpmChanged();
    void noteSpeedChanged();
    void postBufferChanged();
    void postBufferAutoChanged();
    void perTrackColorsChanged();
    void volumeChanged();
    void midiFormatChanged();
    void ppqChanged();
    void tempoChangeCountChanged();
    void controlEventCountChanged();
    void peakNpsChanged();
    void peakPolyphonyChanged();
    void pitchRangeChanged();
    void skippedVelocityChanged();
    void timelineChanged();
    void documentRevisionChanged();
    void transportRevisionChanged();
    void midiLoadingChanged();
    void synthStateChanged();
    void synthConfigChanged();
    void rollConfigChanged();
    void channelColorsChanged();
    void activePitchesChanged();
    void neuralVisualChanged();
    void fileLoaded();
    void loadFailed(QString message);

private:
    void setPlaying(bool playing);
    void rebuildDerivedStats();
    void updateLiveStats();
    void updateCurrentTime();
    void updateNeuralVisuals();
    void dispatchScheduler();
    void publishDocumentMetadata();
    bool adoptParsedDocument(wasmidi::MidiDocument&& parsed, const QString& fileName);

    void invalidateLiveTrackers();
    void clearVisualState();
    void syncVisualState(double targetTick, bool forceRebuild = false);
    void applyRemoteLiveSnapshot(double targetTick);
    void rebuildVisualStateAt(double targetTick);
    bool restoreVisualStateFromPage(double targetTick);
    void advanceVisualStateTo(double targetTick);
    void clearKeyboardVisualPageCache();
    void addVisualNote(std::size_t sourceIndex);
    void addVisualCount(uint8_t pitch, uint8_t color, uint32_t count);
    void removeVisualCount(uint8_t pitch, uint8_t color, uint32_t count);
    void applyVisualKeyEvent(const wasmidi::VisualKeyEvent& event, bool add);

    void pollSynthState();
    void applySynthConfig();
    void resetSynthSchedule(float seconds);
    void scheduleSynthAhead();
    void updateSynthSynchronization();
    void updateEffectiveVelocityFloor(double renderLoadRatio);
    void publishSynthPrebufferConfig();
    uint32_t packSynthMessage(const wasmidi::CompactEvent& event) const;

    wasmidi::MidiParser parser_;
    wasmidi::MidiScheduler scheduler_;
    wasmidi::MidiDocument document_;

    bool isPlaying_ = false;
    float currentTime_ = 0.0f;
    float duration_ = 0.0f;
    QString fileName_;
    qint64 noteCount_ = 0;
    int trackCount_ = 0;
    int activeVoices_ = 0;
    int activeChannelCount_ = 0;
    int nps_ = 0;
    int ccPerSecond_ = 0;
    float bpm_ = 120.0f;
    float noteSpeed_ = 1.0f;
    float postBuffer_ = 0.0f;
    bool postBufferAuto_ = true;
    bool perTrackColors_ = false;
    int volume_ = 100;
    int midiFormat_ = 0;
    int ppq_ = 480;
    int tempoChangeCount_ = 0;
    qint64 controlEventCount_ = 0;
    int peakNps_ = 0;
    float peakNpsTime_ = 0.0f;
    int peakPolyphony_ = 0;
    QString pitchRange_ = QStringLiteral("—");
    int skippedVelocity_ = 0;
    QVariantList npsTimeline_;
    quint64 documentRevision_ = 0;
    quint64 transportRevision_ = 0;
    bool midiLoading_ = false;
    int midiLoadingProgress_ = 0;
    QString midiLoadingStage_;

    QVector<QColor> channelColors_;
    QElapsedTimer playbackClock_;
    float playbackAnchorSeconds_ = 0.0f;
    qint64 playbackLastElapsedMs_ = 0;
    // Wall clock leads; audio only re-anchors it (HANDOFF sec. 48).
    double presentationOffsetSeconds_ = 0.0;
    bool presentationOffsetLocked_ = false;
    quint64 visualFrameSerial_ = 0;
    quint64 playbackConsumedVisualFrameSerial_ = 0;

    std::vector<float> tempoTimes_;
    std::vector<float> tempoBpms_;

    // Sliding live-stat windows over sparse TickGroup, no per-note arrays.
    bool liveWindowsValid_ = false;
    float liveLastTime_ = 0.0f;
    std::size_t liveHi_ = 0;
    std::size_t liveNpsLo_ = 0;
    std::size_t liveCcLo_ = 0;
    uint64_t liveNpsCount_ = 0;
    uint64_t liveCcCount_ = 0;

    // Mapped-MIDI live state is prepared only a couple of display frames ahead
    // of the authoritative audio clock.  Pass 13.8 used a 250 ms queue and
    // vector insertion/erasure every frame; under load that made the keyboard
    // itself another source of main-thread/Worker pressure.  One monotonic
    // deque hides normal Worker latency without building a second long future
    // timeline.
    struct RemoteLiveSnapshot {
        double tick = 0.0;
        int activeVoices = 0;
        int nps = 0;
        int ccPerSecond = 0;
        std::array<uint32_t, 128> counts{};
        std::array<uint8_t, 128> globalColors{};
        std::array<uint8_t, 128> trackColors{};
    };

    std::deque<RemoteLiveSnapshot> remoteLiveSnapshots_;
    bool remoteLiveBaselinePending_ = false;

    // Exact keyboard state from the parser-worker-built compressed start/end
    // timeline. Dense identical notes advance as counted deltas instead of one
    // heap operation per note on the UI thread.
    bool visualStateValid_ = false;
    double visualTick_ = 0.0;
    std::size_t visualKeyStartCursor_ = 0;
    std::size_t visualKeyEndCursor_ = 0;
    std::size_t visualKeyOwnerCursor_ = 0;
    int visualActiveVoices_ = 0;

    std::array<uint32_t, 128> visualPitchCount_{};
    std::array<uint8_t, 128> visualPitchMask_{};
    std::array<int8_t, 128> visualPitchColor_{};
    std::array<std::array<uint32_t, 16>, 128> visualPitchColorCounts_{};
    std::array<uint32_t, 16> visualColorVoices_{};

    // The horizontal visual-cache Worker also prepares exact keyboard state at
    // the same rolling 64 screen boundaries. A seek can restore one of these
    // compact snapshots and replay only the tiny residual event interval. This
    // is the keyboard equivalent of the roll's pre-render page cache and keeps
    // dense crashpoints off Qt's UI thread while still retaining a raw-index
    // recovery path if the requested page has not finished yet.
    struct KeyboardVisualPage {
        uint32_t generation = 0;
        uint32_t spanTicks = 0;
        uint32_t pageIndex = 0;
        uint32_t startCursor = 0;
        uint32_t endCursor = 0;
        uint32_t ownerCursor = 0;
        std::array<std::array<uint32_t, 16>, 128> globalCounts{};
        std::array<std::array<uint32_t, 16>, 128> trackCounts{};
        std::array<uint32_t, 128> ownerColors{};

        uint64_t startTick() const {
            return uint64_t(spanTicks) * uint64_t(pageIndex);
        }
    };

    std::vector<KeyboardVisualPage> keyboardVisualPages_;
    uint32_t keyboardVisualGeneration_ = 0;

    // Dedicated SnappySynthV2 browser worker state.
    bool synthReady_ = false;
    bool soundfontLoaded_ = false;
    QString soundfontName_;
    QString synthStatus_ = QStringLiteral("SnappySynthV2 idle");
    int synthSampleRate_ = 0;
    int synthActiveVoices_ = 0;
    int synthFreeVoices_ = 0;
    int synthSteals_ = 0;
    int synthRebalanced_ = 0;
    int synthDroppedNotes_ = 0;
    double synthRenderLoad_ = 0.0;
    double synthRenderLatencyMs_ = 0.0;
    double synthDispatchMs_ = 0.0;
    int synthSimdPercent_ = 0;
    int synthWorkersActive_ = 0;
    double synthAllocMs_ = 0.0;
    int synthFreeRatio_ = 100;
    int synthCcCollapsed_ = 0;
    int synthCcHotNumber_ = 0;
    int synthCcHotPercent_ = 0;
    int synthCcHotCount_ = 0;
    bool synthDebugMetrics_ = false;
    int synthPresampleSeen_ = 0;
    int synthPresampleResampled_ = 0;
    int synthPresampleSkipped_ = 0;
    int synthMissReason_ = 0;
    int synthMissPercent_ = 0;
    double synthWorkerBusyMs_ = 0.0;
    int synthPumpGapMs_ = 0;
    int synthRingFill_ = 0;
    int synthLayers_ = 0;
    int synthRegions_ = 0;
    int synthUnderruns_ = 0;

    // SnappySynth.cfg-equivalent values plus voice.c runtime tuning.
    int synthMaxVoices_ = 16384;
    int synthMinVoices_ = 0;
    int synthBufferFrames_ = 512;
    int synthNumBuffers_ = 16;
    // Legacy UI value retained for compatibility. Browser audio is now strictly
    // realtime and keeps only NumBuffers worth of PCM queued.
    float synthPrebufferSeconds_ = 0.0f;
    int synthVelocityFloor_ = 0;         // user floor; 127 leaves only velocity 127
    int synthEffectiveVelocityFloor_ = 0;
    qint64 synthLastHardResyncElapsedMs_ = -100000;
    int synthRequestedSampleRate_ = 44100; // supplied SnappySynth.cfg default
    int synthChannels_ = 2;
    int synthBitsPerSample_ = 32;
    bool synthRealtimePriority_ = true;
    int synthWorkers_ = 4;             // 4 workers default — sweet spot for WASM
                                     // pthread overhead vs. parallel gain. User can
                                     // override via slider in Controls.qml.
    int synthWorkerCount_ = 0;         // actual source-selected workers
    int synthNoteSharding_ = 0;        // 0 auto, 1 channel, 2 hash
    bool synthStealScoreCache_ = true;
    bool synthFastNoteOff_ = true;
    bool synthValidateState_ = false;
    bool synthSoftClip_ = true;
    bool synthOverlapGain_ = true;
    bool synthGpu_ = false;
    int rollMode_ = 1;
    int rollLookAheadMB_ = 1024;
    bool synthPlaybackPrimed_ = false;
    bool synthWasSoundfontLoaded_ = false;
    bool synthWasStarved_ = false;
    // Adaptive velocity shedding is a last-resort catch-up mechanism. Startup,
    // seek and resync get a grace window so initial mapped-event decoding can
    // never jump the floor straight to 127 before audio has had a chance to
    // prime its rolling PCM ring.
    qint64 synthStarvedSinceElapsedMs_ = -1;
    qint64 synthCatchupGraceUntilElapsedMs_ = 0;
    // Rate-limit adaptive velocity shedding. A high render-cost estimate is
    // not itself an underrun, and the floor must never sprint to 127 merely
    // because the first dense chord is expensive to initialize.
    qint64 synthVelocityLastRaiseElapsedMs_ = -1;
    std::size_t synthGroupCursor_ = 0;
    std::size_t synthSysExCursor_ = 0;
    double synthScheduledUntil_ = 0.0;

    std::vector<uint32_t> synthMessages_;
    std::vector<double> synthTimes_;

    // Incremental +150 ms neural-color lookahead. This replaces rescanning
    // the same dense future event window on every UI frame.
    bool neuralWindowValid_ = false;
    std::size_t neuralFutureLo_ = 0;
    std::size_t neuralFutureHi_ = 0;
    std::array<uint64_t, 16> neuralFutureColors_{};

    float dominantHue_ = 230.0f;
    float neuralActivity_ = 0.0f;
};
