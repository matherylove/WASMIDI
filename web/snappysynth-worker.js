/* global SnappySynthCore */

"use strict";

importScripts("./snappysynth-core.js?v=13.12.0");

let Module = null;
let coreReady = false;

// ---- WebGPU voice rendering (HANDOFF sec. 63) ----------------------------
// Blocks rendered with GPU voices stay unpublished in the ring until their GPU
// part is added, then are published in order.
let gpuWanted = false;
let gpuEngine = null;
let gpuActive = false;
let gpuStatus = "off";
let gpuPending = [];
let gpuPendingFrames = 0;
let gpuWriteFrame = -1;
let gpuSetupSerial = 0;

function setGpuStatus(status) {
    if (status === gpuStatus) return;
    gpuStatus = status;
    postState("gpu", { status });
}

function disableGpu(status) {
    if (Module && coreReady && Module._ssw_wgpu_set_enabled) Module._ssw_wgpu_set_enabled(0);
    gpuActive = false;
    setGpuStatus(status || "off");
}

async function setupGpu() {
    const serial = ++gpuSetupSerial;
    if (!gpuWanted) { disableGpu("off"); return; }
    if (!Module || !coreReady || !soundfontLoaded || !Module._ssw_wgpu_set_enabled) return;
    try {
        if (!self.WasmiSynthGpu)
            importScripts("./snappysynth-webgpu.js?v=13.12.0");
        const voices = Math.max(1, Number(maxVoices) | 0);
        const frames = Math.max(1, preferredRenderFrames() | 0);
        if (!gpuEngine || gpuEngine.lost || gpuEngine.maxVoices !== voices || gpuEngine.blockFrames < frames) {
            if (gpuEngine) { try { gpuEngine.destroy(); } catch (_) {} }
            gpuEngine = null;
            const made = await self.WasmiSynthGpu.create(voices, Math.max(frames, 2048));
            if (serial !== gpuSetupSerial) return;
            if (!made.gpu) { disableGpu("unavailable: " + made.reason); return; }
            gpuEngine = made.gpu;
        }
        const reason = gpuEngine.uploadSamples(Module,
            n => Module._malloc(n), p => Module._free(p));
        if (reason) { disableGpu("unavailable: " + reason); return; }
        Module._ssw_wgpu_set_frame_cap(Math.floor(gpuEngine.scratchFrames * 0.95));
        if (!Module._ssw_wgpu_set_enabled(1)) { disableGpu("unavailable: out of memory"); return; }
        gpuActive = true;
        setGpuStatus("active (" + Math.round(gpuEngine.poolBytes / 1048576) + " MB of samples on the GPU)");
    } catch (error) {
        disableGpu("unavailable: " + (error && error.message ? error.message : String(error)));
    }
}

function dropGpuPending() {
    gpuPending = [];
    gpuPendingFrames = 0;
    gpuWriteFrame = -1;
}

function flushGpuPending() {
    const header = ringHeader();
    if (!header || !Module) { dropGpuPending(); return; }
    while (gpuPending.length && gpuPending[0].done) {
        const entry = gpuPending.shift();
        if (entry.generation !== Atomics.load(header, RING_GENERATION)) { dropGpuPending(); return; }
        const stride = Math.max(1, synthChannels);
        const ptr = audioRingPcmPtr + entry.writeFrame * stride * 4;
        if (entry.result && stride === 2) {
            const pcm = Module.HEAPF32.subarray(ptr >>> 2, (ptr >>> 2) + entry.frames * 2);
            const add = entry.result;
            for (let i = 0; i < add.length && i < pcm.length; ++i) pcm[i] += add[i];
        }
        Module._ssw_output_stage_apply(ptr, entry.frames);
        const nextWrite = entry.writeFrame + entry.frames >= audioRingCapacityFrames
            ? 0 : entry.writeFrame + entry.frames;
        Atomics.store(header, RING_WRITE, nextWrite);
        Atomics.add(header, RING_AVAILABLE, entry.frames);
        gpuPendingFrames -= entry.frames;
    }
    if (!gpuPending.length) gpuWriteFrame = -1;
}

function renderOneBlockGpu(frames) {
    const header = ringHeader();
    if (!header || !audioRingPcmPtr) return false;
    const available = Atomics.load(header, RING_AVAILABLE);
    if (audioRingCapacityFrames - available - gpuPendingFrames < frames) return false;
    if (gpuPending.length > 8) return false;
    const writeFrame = gpuWriteFrame >= 0 ? gpuWriteFrame : Atomics.load(header, RING_WRITE);
    if (writeFrame < 0 || writeFrame + frames > audioRingCapacityFrames) return false;
    const generation = Atomics.load(header, RING_GENERATION);
    const blockStart = renderSongTime;
    const outPtr = audioRingPcmPtr + writeFrame * Math.max(1, synthChannels) * 4;
    if (gpuActive) Module._ssw_wgpu_begin_block();
    if (!renderQueuedWithSysex(outPtr, blockStart, frames))
        throw new Error("SnappySynthV2 shared-ring render failed.");
    if (generation !== Atomics.load(header, RING_GENERATION)) return false;
    const entry = { writeFrame, frames, generation, done: false, result: null };
    gpuPending.push(entry);
    gpuPendingFrames += frames;
    gpuWriteFrame = writeFrame + frames >= audioRingCapacityFrames ? 0 : writeFrame + frames;
    let work;
    try {
        work = gpuActive && gpuEngine ? gpuEngine.submit(Module, frames) : Promise.resolve(null);
    } catch (error) {
        work = Promise.reject(error);
    }
    work.then(result => { entry.result = result; entry.done = true; flushGpuPending(); },
              error => {
                  entry.done = true;
                  disableGpu("unavailable: " + (error && error.message ? error.message : String(error)));
                  flushGpuPending();
              });
    renderSongTime = blockStart + frames / sampleRateHz;
    pendingNeedFrames = Math.max(0, pendingNeedFrames - frames);
    ++pcmBlocksRendered;
    const now = performance.now();
    if (now - lastStatsReport > 250) {
        lastStatsReport = now;
        postState("stats", { activeVoices: Module._ssw_active_voices(), renderSongTime, gpu: gpuStatus });
    }
    return true;
}
let audioPort = null;

let sampleRateHz = 44100;
let blockFrames = 512;
let numBuffers = 16;
// Audio is rendered as a realtime transport queue. There is deliberately no
// seconds-long PCM prerender cache; only the configured NumBuffers worth of
// device safety audio stays resident.
let prebufferSeconds = 0.0;
let midiDuration = 0.0;
let prebufferFrames = 0;
let maxVoices = 16384;
let minVoices = 0;
let synthChannels = 2;
let bitsPerSample = 32;
let realtimePriority = 1;

let requestedWorkers = 0;
let debugMetrics = false;     // 0 = original automatic policy
let noteSharding = 0;         // 0 auto, 1 channel, 2 hash, 3 single
let stealScoreCache = true;   // source default ON
let fastNoteOff = true;       // source default ON
let validateState = false;    // source default OFF
let softClip = true;          // source realtime default ON

let volume = 1.00;
let vorMode = 1; // 1 = one-voice loudness for exact overlap

let soundfontFiles = [];
let soundfontPaths = [];
let soundfontMounts = [];
let soundfontLoaded = false;
let sysexEvents = [];
let sysexPtr = 0;
let sysexCapacity = 0;

let playing = false;
let renderSongTime = 0.0;
let safeUntil = 0.0;
let pendingNeedFrames = 0;
// After play(reset)/seek, wait until the MIDI producer has acknowledged the
// new timeline at least once. Previously an AudioWorklet `need` could race the
// parser reset and make us fill NumBuffers with valid *silence* before the
// first MIDI batch arrived, producing a multi-buffer startup delay every Play.
let startupWaitingForSchedule = false;

// Schedule batches are copied once into the core's C-side chronological queue.
// This keeps per-event time->sample classification out of the JavaScript audio
// hot path on multi-million-NPS Black MIDIs.
let messagePtr = 0;
let timePtr = 0;
let scratchCapacity = 0;

let pcmBlocksRendered = 0;
let lastStatsReport = 0;

// Shared PCM ring lives directly inside SnappySynth's pthread WebAssembly.Memory.
// AudioWorklet reads it in-place: no per-block ArrayBuffer allocation, copy,
// transfer, recycle message, or queue object is required.
const RING_READ = 0;
const RING_WRITE = 1;
const RING_AVAILABLE = 2;
const RING_GENERATION = 3;
const RING_CAPACITY = 4;
const RING_CHANNELS = 5;
const RING_BLOCK_FRAMES = 6;
const RING_WORDS = 8;

let audioRingHeaderPtr = 0;
let audioRingPcmPtr = 0;
let audioRingCapacityFrames = 0;
let audioRingMemory = null;

function postState(type, extra = {}) {
    postMessage(Object.assign({
        type,
        sampleRate: sampleRateHz,
        activeVoices:
            coreReady && Module ? Module._ssw_active_voices() : 0,
        freeVoices:
            coreReady && Module ? Module._ssw_free_voices() : 0,
        steals: sampleStealRate(),
        rebalanced: sampleRebalanceRate(),
        droppedNotes:
            coreReady && Module ? Module._ssw_dropped_notes() : 0,
        renderLoadPercent:
            coreReady && Module
                ? Module._ssw_render_load_x1000() / 10
                : 0,
        lastRenderMs:
            coreReady && Module
                ? Module._ssw_last_render_us() / 1000
                : 0,
        lastDispatchMs:
            coreReady && Module
                ? Module._ssw_last_dispatch_us() / 1000
                : 0,
        pathFastVoices:
            coreReady && Module && debugMetrics ? Module._ssw_path_fast_voices() : 0,
        pathScalarVoices:
            coreReady && Module && debugMetrics ? Module._ssw_path_scalar_voices() : 0,
        workersParticipating:
            coreReady && Module && debugMetrics ? Module._ssw_workers_participating() : 0,
        allocMs: coreReady && Module && debugMetrics ? Module._ssw_alloc_us() / 1000 : 0,
        ccHotspot: coreReady && Module && debugMetrics ? Module._ssw_cc_hotspot() : 0,
        ccHotspotCount: coreReady && Module && debugMetrics ? Module._ssw_cc_hotspot_count() : 0,
        controllersCollapsed:
            coreReady && Module ? Module._ssw_controllers_collapsed() : 0,
        presampleSeen: coreReady && Module ? Module._ssw_presample_seen() : 0,
        presampleSkipped: coreReady && Module ? Module._ssw_presample_skipped() : 0,
        presampleResampled: coreReady && Module ? Module._ssw_presample_resampled() : 0,
        notesStarted:
            coreReady && Module && debugMetrics ? Module._ssw_notes_started() : 0,
        voicesRecycled:
            coreReady && Module && debugMetrics ? Module._ssw_voices_recycled() : 0,
        missInterp: coreReady && Module && debugMetrics ? Module._ssw_miss_interp() : 0,
        missLoop: coreReady && Module && debugMetrics ? Module._ssw_miss_loop() : 0,
        missFilter: coreReady && Module && debugMetrics ? Module._ssw_miss_filter() : 0,
        missOther: coreReady && Module && debugMetrics ? Module._ssw_miss_other() : 0,
        pathSimdVoiceVoices:
            coreReady && Module ? Module._ssw_path_simd_voice_voices() : 0,
        workerBusyMs:
            coreReady && Module ? Module._ssw_worker_busy_us() / 1000 : 0,
        renderBudget:
            coreReady && Module ? Module._ssw_render_budget() : 0,
        ringFillPercent: telemetryRingFill * 100,
        blocksPerPump: telemetryBlocksPerPump,
        pumpGapMs: telemetryLateMs,
        layers:
            coreReady && Module ? Module._ssw_layer_count() : 0,
        regions:
            coreReady && Module ? Module._ssw_region_count() : 0,
        workerCount:
            coreReady && Module ? Module._ssw_worker_count() : 0,
        channels:
            coreReady && Module ? Module._ssw_channels() : synthChannels,
        bitsPerSample:
            coreReady && Module ? Module._ssw_bits_per_sample() : bitsPerSample,
        numBuffers:
            coreReady && Module ? Module._ssw_num_buffers() : numBuffers,
        prebufferFrames
    }, extra));
}

function setError(error) {
    const message =
        error && error.message
            ? error.message
            : String(error || "Unknown SnappySynthV2 error");

    console.error("[SnappySynthV2 worker]", error);
    postState("error", { message });
}

function ringHeader() {
    if (!Module || !audioRingHeaderPtr)
        return null;
    return new Int32Array(
        Module.HEAPU8.buffer,
        audioRingHeaderPtr,
        RING_WORDS);
}

function resetAudioRing() {
    const header = ringHeader();
    if (!header)
        return;

    Atomics.add(header, RING_GENERATION, 1);
    dropGpuPending();
    Atomics.store(header, RING_READ, 0);
    Atomics.store(header, RING_WRITE, 0);
    Atomics.store(header, RING_AVAILABLE, 0);
}

function freeAudioRing() {
    if (!Module)
        return;

    if (audioRingHeaderPtr)
        Module._free(audioRingHeaderPtr);
    if (audioRingPcmPtr)
        Module._free(audioRingPcmPtr);

    audioRingHeaderPtr = 0;
    audioRingPcmPtr = 0;
    audioRingCapacityFrames = 0;
    audioRingMemory = null;
}

function publishAudioRing(force = false) {
    if (!Module || !audioPort || !audioRingHeaderPtr || !audioRingPcmPtr)
        return;

    const memory = Module.HEAPU8.buffer;

    if (!(memory instanceof SharedArrayBuffer)) {
        throw new Error(
            "SnappySynthV2 pthread memory is not SharedArrayBuffer; " +
            "cross-origin isolation/pthread initialization is incomplete.");
    }

    if (!force && memory === audioRingMemory)
        return;

    audioRingMemory = memory;

    audioPort.postMessage({
        type: "sharedRing",
        memory,
        headerPtr: audioRingHeaderPtr,
        pcmPtr: audioRingPcmPtr,
        capacityFrames: audioRingCapacityFrames,
        channels: synthChannels,
        blockFrames
    });
}

/*
 * Steals are reported as a rate, not a running total.
 *
 * ssw_steals() is cumulative, which makes it useless for telling "the pool is
 * saturated right now" from "something stole a lot two minutes ago". A rate
 * reads zero while playback is healthy and rises the moment the pool cannot
 * keep up, which is what the number is actually consulted for.
 */
let stealRatePerSecond = 0;
let stealRateHasBaseline = false;
let lastStealCount = 0;
let lastStealSampleMs = 0;

/*
 * Same treatment for the free-voice rebalance counter: ssw_rebalanced() is
 * cumulative, the panel wants "is it happening now". Dropped notes stay a
 * running total: a note that never sounded is something the user wants to see
 * stick, not fade.
 */
let rebalanceRatePerSecond = 0;
let rebalanceRateHasBaseline = false;
let lastRebalanceCount = 0;
let lastRebalanceSampleMs = 0;

/*
 * Render telemetry.
 *
 * These used to be console lines. Printing from a worker at this rate is itself
 * a cost the browser pays on the main thread, so the numbers are carried in the
 * stats payload instead and the UI graphs them. Nothing here formats a string
 * unless something is genuinely wrong.
 *
 * The pump gap is the important one and it is easy to misread. This code only
 * runs from inside pump(), so a stall does not appear as a low load number, it
 * appears as no sample at all. Measuring the gap since the previous pump turns
 * a stall into a value rather than into missing output. A large gap with low
 * load means the thread was stopped, not busy.
 */
let telemetryLastPumpMs = 0;
let telemetryLateMs = 0;
let telemetryRingFill = 0;
let telemetryBlocksPerPump = 0;

/*
 * How late the pump was, not how long it slept.
 *
 * The first version of this reported the raw gap between pump() calls, which is
 * backwards: when load is low the ring reaches its target, the pump exits early
 * and is not invited again until the worklet drains, so healthy idling produced
 * the largest gaps. Under heavy load the pump runs constantly and the gaps are
 * tiny. The number lit up red exactly when nothing was wrong.
 *
 * A gap only matters measured against what the ring could cover while it
 * lasted. Sitting out 100 ms with 300 ms of audio buffered is free; sitting out
 * 100 ms with 20 ms buffered is an underrun. So the reported value is the
 * overrun past that cover, and it is zero whenever the buffer was deep enough,
 * however long the pause.
 */
function noteTelemetry(ringFillFraction, blocksThisPump, ringMsAtEntry) {
    const now =
        typeof performance !== "undefined" && performance.now
            ? performance.now()
            : Date.now();
    if (telemetryLastPumpMs > 0) {
        const gap = now - telemetryLastPumpMs;
        const late = Math.max(0, gap - ringMsAtEntry);
        // Decay, so one real stall stays readable for a moment rather than
        // being erased by the next healthy pump.
        telemetryLateMs = Math.max(late, telemetryLateMs * 0.6);
    }
    telemetryLastPumpMs = now;
    telemetryRingFill = ringFillFraction;
    telemetryBlocksPerPump = blocksThisPump;
}

/*
 * A worker count below what was asked for has two unrelated causes and the
 * count alone cannot tell them apart: the engine clamped to a core count the
 * browser under-reports (Brave farbles navigator.hardwareConcurrency), or
 * thread creation failed because the pool is too small. Reported once at init,
 * not per frame.
 */
function reportWorkerPool() {
    if (!coreReady || !Module)
        return;
    const running = Module._ssw_worker_count();
    if (requestedWorkers > 0 && running < requestedWorkers) {
        postState("workerPool", {
            requestedWorkers,
            workerCount: running,
            detectedCores: Module._ssw_detected_cores(),
            workerThreadFailures: Module._ssw_worker_thread_failures()
        });
    }
}

function resetStealRate() {
    stealRatePerSecond = 0;
    stealRateHasBaseline = false;
    lastStealCount = 0;
    lastStealSampleMs = 0;
    rebalanceRatePerSecond = 0;
    rebalanceRateHasBaseline = false;
    lastRebalanceCount = 0;
    lastRebalanceSampleMs = 0;
}

function sampleRebalanceRate() {
    if (!coreReady || !Module)
        return 0;

    const now =
        typeof performance !== "undefined" && performance.now
            ? performance.now()
            : Date.now();
    const total = Module._ssw_rebalanced();

    if (!rebalanceRateHasBaseline) {
        rebalanceRateHasBaseline = true;
        lastRebalanceCount = total;
        lastRebalanceSampleMs = now;
        return 0;
    }

    const elapsedMs = now - lastRebalanceSampleMs;
    if (elapsedMs < 100)
        return Math.round(rebalanceRatePerSecond);

    let delta = total - lastRebalanceCount;
    if (delta < 0)
        delta = 0;

    const instant = (delta * 1000) / elapsedMs;
    rebalanceRatePerSecond += 0.4 * (instant - rebalanceRatePerSecond);
    if (rebalanceRatePerSecond < 0.5)
        rebalanceRatePerSecond = 0;

    lastRebalanceCount = total;
    lastRebalanceSampleMs = now;
    return Math.round(rebalanceRatePerSecond);
}

function sampleStealRate() {
    if (!coreReady || !Module)
        return 0;

    const now =
        typeof performance !== "undefined" && performance.now
            ? performance.now()
            : Date.now();
    const total = Module._ssw_steals();

    if (!stealRateHasBaseline) {
        stealRateHasBaseline = true;
        lastStealCount = total;
        lastStealSampleMs = now;
        return 0;
    }

    const elapsedMs = now - lastStealSampleMs;
    // Stats arrive every 250 ms, but postState() is also called for unrelated
    // events. Ignore samples too short to carry a meaningful rate and reuse the
    // last one instead of dividing by a near-zero interval.
    if (elapsedMs < 100)
        return Math.round(stealRatePerSecond);

    // A reinit or reset restarts the engine counter; never report a negative.
    let delta = total - lastStealCount;
    if (delta < 0)
        delta = 0;

    const instant = (delta * 1000) / elapsedMs;
    // Light smoothing so the readout is legible rather than flickering.
    stealRatePerSecond += 0.4 * (instant - stealRatePerSecond);
    if (stealRatePerSecond < 0.5)
        stealRatePerSecond = 0;

    lastStealCount = total;
    lastStealSampleMs = now;
    return Math.round(stealRatePerSecond);
}

function preferredRenderFrames() {
    // Match the supplied SnappySynthV2 driver's BufferSize semantics exactly.
    // The native driver invokes voice_render_float() one configured source
    // block at a time. Larger browser-only mega-blocks reduced message/futex
    // traffic, but they also lengthened a single synchronous render enough to
    // starve control/event delivery on dense files.
    return Math.max(1, blockFrames);
}

function requestedPrebufferFrames() {
    const transportFrames =
        Math.max(1, blockFrames) *
        Math.max(1, numBuffers);
    const quantum = Math.max(1, preferredRenderFrames());
    const aligned = Math.ceil(transportFrames / quantum) * quantum;
    return Math.max(quantum, aligned);
}

function allocateAudioRing() {
    if (!Module)
        return;

    freeAudioRing();

    // The shared ring is only the realtime device queue. It is never sized from
    // MIDI duration or a seconds-long prerender preference.
    const minimumTransportFrames = 256;
    audioRingCapacityFrames =
        Math.max(
            minimumTransportFrames,
            requestedPrebufferFrames());

    // Keep capacity an exact source-block multiple so wrap points can never
    // split one sample-accurate synth block.
    audioRingCapacityFrames =
        Math.ceil(
            audioRingCapacityFrames /
            Math.max(1, blockFrames)) *
        Math.max(1, blockFrames);

    prebufferFrames = audioRingCapacityFrames;

    audioRingHeaderPtr =
        Module._malloc(RING_WORDS * 4);
    audioRingPcmPtr =
        Module._malloc(
            audioRingCapacityFrames *
            Math.max(1, synthChannels) *
            4);

    if (!audioRingHeaderPtr || !audioRingPcmPtr) {
        freeAudioRing();
        throw new Error(
            "Could not allocate the shared SnappySynth audio ring.");
    }

    const header = ringHeader();
    for (let i = 0; i < RING_WORDS; ++i)
        Atomics.store(header, i, 0);
    Atomics.store(header, RING_CAPACITY, audioRingCapacityFrames);
    Atomics.store(header, RING_CHANNELS, synthChannels);
    Atomics.store(header, RING_BLOCK_FRAMES, blockFrames);

    publishAudioRing(true);
}

function resetEventQueue() {
    if (coreReady && Module && Module._ssw_clear_events)
        Module._ssw_clear_events();
    sysexEvents.length = 0;
    safeUntil = renderSongTime;
}

function freeScratch() {
    if (!Module)
        return;

    if (messagePtr)
        Module._free(messagePtr);
    if (timePtr)
        Module._free(timePtr);

    messagePtr = 0;
    timePtr = 0;
    scratchCapacity = 0;
}

function ensureScratch(required) {
    if (required <= scratchCapacity)
        return;

    let capacity = Math.max(4096, scratchCapacity || 4096);
    while (capacity < required)
        capacity *= 2;

    const newMessagePtr = Module._malloc(capacity * 4);
    const newTimePtr = Module._malloc(capacity * 8);
    if (!newMessagePtr || !newTimePtr) {
        if (newMessagePtr) Module._free(newMessagePtr);
        if (newTimePtr) Module._free(newTimePtr);
        throw new Error(
            "Could not allocate SnappySynth schedule scratch buffers.");
    }

    if (messagePtr) Module._free(messagePtr);
    if (timePtr) Module._free(timePtr);
    messagePtr = newMessagePtr;
    timePtr = newTimePtr;
    scratchCapacity = capacity;
}

function ensureDir(path) {
    try { Module.FS.mkdir(path); } catch (_) {}
}

function mountSoundfontFile(file) {
    if (!Module || !file)
        return "";
    if (!Module.FS)
        throw new Error("Emscripten FS is not available in SnappySynthV2 core.");
    if (!Module.WORKERFS)
        throw new Error("WORKERFS is not available in SnappySynthV2 core.");

    ensureDir("/soundfonts");
    const index = soundfontFiles.length;
    const mount = "/soundfonts/layer" + index;
    ensureDir(mount);

    Module.FS.mount(
        Module.WORKERFS,
        { files: [file] },
        mount);

    const path = mount + "/" + (file.name || ("layer" + index + ".sf2"));
    const stat = Module.FS.stat(path);
    if (!stat || stat.size <= 0) {
        try { Module.FS.unmount(mount); } catch (_) {}
        throw new Error("Selected SF2 is empty or WORKERFS could not expose it.");
    }

    soundfontFiles.push(file);
    soundfontPaths.push(path);
    soundfontMounts.push(mount);
    return path;
}

function unmountAllSoundfonts() {
    for (let i = soundfontMounts.length - 1; i >= 0; --i) {
        try { Module.FS.unmount(soundfontMounts[i]); } catch (_) {}
    }
    soundfontFiles.length = 0;
    soundfontPaths.length = 0;
    soundfontMounts.length = 0;
}

function callLoadSoundfont(path) {
    const bytes =
        Module.lengthBytesUTF8(path) + 1;

    const ptr =
        Module._malloc(bytes);

    if (!ptr)
        throw new Error("Could not allocate SF2 path string.");

    try {
        Module.stringToUTF8(
            path,
            ptr,
            bytes);

        return Module._ssw_load_sf2(ptr);
    } finally {
        Module._free(ptr);
    }
}

function applyCoreSettings() {
    try {
        const limit = new URL(self.location.href).searchParams.get("synthlimit");
        if (limit !== null && Module && Module._ssw_set_render_limit_percent)
            Module._ssw_set_render_limit_percent(Math.max(0, Number(limit) | 0));
    } catch (_) {}
    // MIDI master volume belongs to the synth and may be changed by SysEx.
    // UI volume is applied after synthesis in the AudioWorklet.
    Module._ssw_set_vor_mode(vorMode);
}

function initCore() {
    if (!Module)
        return;

    resetStealRate();
    if (Module._ssw_set_debug_metrics) Module._ssw_set_debug_metrics(debugMetrics ? 1 : 0);

    // `GetSystemInfo()` inside Emscripten currently reports a single logical
    // processor in some Chromium/Brave worker configurations.  That made the
    // UI's Workers=0 (Auto) path silently construct one SSv2 worker even though
    // the pthread pool had many workers available.  Keep 0 as the persisted UI
    // value, but resolve Auto at the browser boundary where the browser's own
    // reported hardwareConcurrency is available.  An explicit user value still
    // wins unchanged and is deliberately not clipped (Brave may farble the
    // reported core count).
    const autoWorkers = (() => {
        if (requestedWorkers > 0)
            return requestedWorkers;
        const reported =
            (typeof navigator !== "undefined" && navigator.hardwareConcurrency)
                ? Number(navigator.hardwareConcurrency) | 0
                : 0;
        // Leave cores for UI, MIDI parser/prefetch, visual cache and audio (HANDOFF §25).
        const reserve = reported >= 8 ? 4 : (reported >= 4 ? 2 : 0);
        return Math.max(1, Math.min(256, (reported || 1) - reserve));
    })();

    const initialized = Module._ssw_init_ex(
        sampleRateHz,
        synthChannels,
        bitsPerSample,
        blockFrames,
        numBuffers,
        realtimePriority,
        maxVoices,
        minVoices,
        autoWorkers,
        noteSharding,
        stealScoreCache ? 1 : 0,
        fastNoteOff ? 1 : 0,
        validateState ? 1 : 0,
        softClip ? 1 : 0);

    if (!initialized)
        throw new Error("SnappySynthV2 could not materialize the current soundfont at this sample rate.");

    applyCoreSettings();
    coreReady = true;
    allocateAudioRing();
    dropGpuPending();
    gpuActive = false;
    if (gpuWanted) void setupGpu();
}

function reinitializeCore() {
    if (!Module)
        return;

    freeScratch();
    freeAudioRing();
    coreReady = false;

    // ssw_init() reinitializes only the voice engine and deliberately preserves
    // the already-loaded merged instrument, matching SnappySynth config reload.
    initCore();

    soundfontLoaded =
        Module._ssw_region_count() > 0;

    postState("configured", {
        maxVoices,
        minVoices,
        blockFrames,
        numBuffers,
        channels: synthChannels,
        bitsPerSample,
        realtimePriority,
        requestedWorkers,
        workerCount: Module._ssw_worker_count(),
        detectedCores: Module._ssw_detected_cores(),
        workerThreadFailures: Module._ssw_worker_thread_failures(),
        noteSharding,
        stealScoreCache,
        fastNoteOff,
        validateState,
        softClip,
        loaded: soundfontLoaded,
        layers: Module._ssw_layer_count(),
        regions: Module._ssw_region_count(),
        prebufferFrames
    });

    renderSongTime = 0.0;
    Module._ssw_set_song_time(0.0);
    resetEventQueue();
}

function ensureSysexScratch(required) {
    if (required <= sysexCapacity)
        return;
    let capacity = Math.max(256, sysexCapacity || 256);
    while (capacity < required) capacity *= 2;
    if (sysexPtr) Module._free(sysexPtr);
    sysexPtr = Module._malloc(capacity);
    if (!sysexPtr) throw new Error("Could not allocate SysEx scratch buffer.");
    sysexCapacity = capacity;
}

function admitSysExBatch(meta, bytes, times) {
    if (!(meta instanceof Uint32Array) ||
        !(bytes instanceof Uint8Array) ||
        !(times instanceof Float64Array)) {
        return;
    }

    const count = Math.min(times.length, Math.floor(meta.length / 3));
    for (let i = 0; i < count; ++i) {
        const offset = meta[i * 3 + 1] >>> 0;
        const length = meta[i * 3 + 2] >>> 0;
        if (!length || offset > bytes.length || length > bytes.length - offset)
            continue;
        const event = {
            // subarray retains one transferred arena; no per-SysEx payload
            // copy is needed on the realtime worker.
            bytes: bytes.subarray(offset, offset + length),
            time: Math.max(0.0, Number(times[i]) || 0.0)
        };
        let pos = sysexEvents.length;
        while (pos > 0 && sysexEvents[pos - 1].time > event.time) --pos;
        sysexEvents.splice(pos, 0, event);
    }
}

function admitShortSchedule(messages, times) {
    if (!messages || !times ||
        messages.length !== times.length || messages.length <= 0) {
        return;
    }

    const count = messages.length | 0;
    ensureScratch(count);
    Module.HEAPU32.set(messages, messagePtr >>> 2);
    Module.HEAPF64.set(times, timePtr >>> 3);
    if (!Module._ssw_queue_events(messagePtr, timePtr, count)) {
        throw new Error(
            "SnappySynthV2 could not queue the MIDI schedule batch.");
    }
}

function renderQueuedWithSysex(outPtr, blockStart, frames) {
    const blockEnd = blockStart + frames / sampleRateHz;
    const strideBytes = Math.max(1, synthChannels) * 4;
    let cursorFrame = 0;

    // SysEx is normally sparse, so splitting only at its exact sample costs
    // essentially nothing in note-heavy MIDIs while preventing a GM/GS/XG
    // reset or tuning message near the end of a block from affecting the
    // beginning of that block. CC/program/pitch segmentation happens inside
    // ssw_render_queued_into() and remains on the same absolute sample clock.
    while (sysexEvents.length > 0 && sysexEvents[0].time < blockEnd) {
        const event = sysexEvents.shift();
        const bytes = event.bytes;
        const frame = Math.max(0, Math.min(frames - 1,
            Math.round((event.time - blockStart) * sampleRateHz)));

        if (frame > cursorFrame) {
            if (gpuActive) Module._ssw_wgpu_set_frame_base(cursorFrame);
            if (!Module._ssw_render_queued_into(
                    outPtr + cursorFrame * strideBytes,
                    frame - cursorFrame)) {
                return false;
            }
            cursorFrame = frame;
        }

        if (bytes instanceof Uint8Array && bytes.length > 0) {
            ensureSysexScratch(bytes.length);
            Module.HEAPU8.set(bytes, sysexPtr);
            // The C render cursor is now exactly at this SysEx sample.
            Module._ssw_send_sysex(sysexPtr, bytes.length, 0);
        }
    }

    if (cursorFrame < frames) {
        if (gpuActive) Module._ssw_wgpu_set_frame_base(cursorFrame);
        return !!Module._ssw_render_queued_into(
            outPtr + cursorFrame * strideBytes,
            frames - cursorFrame);
    }
    return true;
}

function renderOneBlock(frames) {
    if (gpuActive || gpuPending.length) {
        if (!gpuActive) return false; // finish the GPU blocks before CPU-only ones
        return renderOneBlockGpu(frames);
    }
    const blockStart =
        renderSongTime;

    const blockEnd =
        blockStart +
        frames / sampleRateHz;

    // Never gate PCM rendering on MIDI event coverage. The original
    // SnappySynth realtime backend keeps rendering the device timeline even
    // when the producer has not yet supplied a future MIDI event. `safeUntil`
    // is retained only as a scheduling/diagnostic watermark; using it as a hard
    // PCM stop is what produced the deterministic "about one second, then
    // silence" failure in WASMIDI. The C-side scheduled-event queue clamps late
    // events to the first not-yet-rendered sample while the multi-second event
    // scheduler normally keeps that path well ahead of the device.

    const header = ringHeader();
    if (!header || !audioRingPcmPtr)
        return false;

    const available =
        Atomics.load(
            header,
            RING_AVAILABLE);

    const freeFrames =
        audioRingCapacityFrames -
        available;

    if (freeFrames < frames)
        return false;

    const writeFrame =
        Atomics.load(
            header,
            RING_WRITE);

    // Capacity is an integer number of synth blocks, so a full synth render
    // never straddles the ring boundary. Keep a defensive guard anyway.
    if (writeFrame < 0 ||
        writeFrame + frames > audioRingCapacityFrames) {
        return false;
    }

    const generation =
        Atomics.load(
            header,
            RING_GENERATION);

    const outPtr =
        audioRingPcmPtr +
        writeFrame *
        Math.max(1, synthChannels) *
        4;

    const rendered = renderQueuedWithSysex(outPtr, blockStart, frames);

    if (!rendered)
        throw new Error(
            "SnappySynthV2 shared-ring render failed.");

    // A seek/flush can be issued by the AudioWorklet while pthread rendering
    // is in progress. In that case discard this old-generation block. The
    // worker's queued seek/reset will reset the synth state immediately after
    // this synchronous render returns.
    if (generation !==
        Atomics.load(
            header,
            RING_GENERATION)) {
        return false;
    }

    const nextWrite =
        writeFrame + frames >= audioRingCapacityFrames
            ? 0
            : writeFrame + frames;

    // Atomics publish the already-written PCM with sequential consistency.
    Atomics.store(
        header,
        RING_WRITE,
        nextWrite);
    Atomics.add(
        header,
        RING_AVAILABLE,
        frames);

    renderSongTime = blockEnd;
    pendingNeedFrames =
        Math.max(
            0,
            pendingNeedFrames - frames);

    ++pcmBlocksRendered;

    const now = performance.now();

    if (now - lastStatsReport > 250) {
        lastStatsReport = now;

        postState("stats", {
            activeVoices:
                Module._ssw_active_voices(),
            renderSongTime
        });
    }

    return true;
}

// Ring occupancy to keep ahead of the worklet. Below this the pump treats the
// shortfall as demand in its own right rather than waiting to be asked.
const PUMP_RING_TARGET_FRACTION = 0.75;
// Wall clock a single pump call may spend rendering before yielding. Roughly
// one 512 frame block's worth of realtime, so a slice can never stall message
// handling for longer than the audio it just produced.
const PUMP_SLICE_BUDGET_MS = 12;
// Safety ceiling only; the time budget is what normally ends a slice.
const PUMP_MAX_BLOCKS_PER_CALL = 64;

function ringTargetFrames() {
    return Math.floor(audioRingCapacityFrames * PUMP_RING_TARGET_FRACTION);
}

// Dedicated MIDI feeder (HANDOFF §26): same batch format as the parser path.
let feederPort = null;
let localActive = false;
let localGeneration = 0;
let localInFlight = false;
let localVelocityFloor = 0;
const LOCAL_AHEAD_SECONDS = 1.5;
const LOCAL_REFILL_SECONDS = 0.75;

function localReset(time) {
    localGeneration = (localGeneration + 1) >>> 0;
    localInFlight = false;
    if (!localActive || !feederPort) return;
    feederPort.postMessage({ type: "reset", generation: localGeneration, time });
    localRequestFill();
}

function localRequestFill() {
    if (!localActive || !feederPort || localInFlight || !playing) return;
    if (safeUntil >= renderSongTime + LOCAL_REFILL_SECONDS) return;
    localInFlight = true;
    feederPort.postMessage({
        type: "fill",
        generation: localGeneration,
        target: Math.max(safeUntil, renderSongTime) + LOCAL_AHEAD_SECONDS,
        velocityFloor: localVelocityFloor
    });
}

function onFeederMessage(d) {
    if (!d || d.type !== "batch" || (d.generation >>> 0) !== localGeneration || !localActive) return;
    localInFlight = false;
    admitSysExBatch(d.sysexMeta, d.sysexData, d.sysexTimes);
    admitShortSchedule(d.messages, d.times);
    safeUntil = Math.max(safeUntil, Number(d.safeUntil) || renderSongTime);
    startupWaitingForSchedule = false;
    pump();
    localRequestFill();
}

function pump() {
    localRequestFill();
    if (!Module ||
        !coreReady ||
        !soundfontLoaded ||
        !playing ||
        startupWaitingForSchedule ||
        !audioPort ||
        !audioRingHeaderPtr ||
        pendingNeedFrames <= 0) {
        return;
    }

    try {
        let guard = 0;
        let produced = 0;
        const header = ringHeader();
        const renderQuantum = Math.max(1, preferredRenderFrames());

        // Keep a cushion ahead of AudioWorklet demand, in slices.
        //
        // This used to render strictly on demand, with a hard ceiling of 16
        // blocks per call. Telemetry on a dense file showed that ceiling being
        // hit on every single call: 16 blocks at roughly 7.5 ms each is about
        // 120 ms of synchronous rendering in one go, during which this worker
        // cannot process scheduleBatch at all. Average load was only 45-69%,
        // so the CPU was not the limit; the ring still sagged from about 70%
        // down to 3% on a spike because production came in bursts rather than
        // steadily.
        //
        // So two changes. Fill toward a target occupancy instead of only
        // covering the immediate request, which is the cushion BPFA's
        // SynthAudio keeps ahead of its device buffers. And cap a single call
        // by wall clock rather than by block count, handing control back so
        // messages get processed, then resume immediately. Same total work,
        // spread evenly, with the message thread able to breathe between
        // slices.
        const sliceStartedMs = performance.now();
        let stoppedForTimeSlice = false;
        // Depth of the ring before this call renders anything, in milliseconds
        // of audio. This is what the pump had to cover the pause it just woke
        // from.
        const ringMsAtEntry = header
            ? (Math.max(0, Atomics.load(header, RING_AVAILABLE)) * 1000) /
                Math.max(1, sampleRateHz)
            : 0;

        while (header && guard++ < PUMP_MAX_BLOCKS_PER_CALL) {
            const available =
                Math.max(0, Atomics.load(header, RING_AVAILABLE));
            const freeFrames = audioRingCapacityFrames - available;

            if (freeFrames < Math.max(1, blockFrames))
                break;

            // Past the target, only keep going while there is real demand.
            if (produced > 0 &&
                available >= ringTargetFrames() &&
                pendingNeedFrames <= 0)
                break;

            if (produced > 0 &&
                performance.now() - sliceStartedMs >= PUMP_SLICE_BUDGET_MS) {
                stoppedForTimeSlice = true;
                break;
            }

            const writeFrame =
                Math.max(0, Atomics.load(header, RING_WRITE));
            const contiguous =
                Math.max(0, audioRingCapacityFrames - writeFrame);

            // Below the target the cushion itself is the demand, so do not
            // let a small pendingNeedFrames cap the slice.
            const wanted =
                Math.max(
                    pendingNeedFrames,
                    ringTargetFrames() - available);

            let frames =
                Math.min(
                    renderQuantum,
                    freeFrames,
                    contiguous,
                    Math.max(blockFrames, wanted));

            frames =
                Math.floor(frames / Math.max(1, blockFrames)) *
                Math.max(1, blockFrames);

            if (frames <= 0 || !renderOneBlock(frames))
                break;

            produced += frames;
        }

        if (header) {
            const availableNow =
                Math.max(0, Atomics.load(header, RING_AVAILABLE));
            noteTelemetry(
                audioRingCapacityFrames > 0
                    ? availableNow / audioRingCapacityFrames
                    : 0,
                Math.floor(produced / Math.max(1, blockFrames)),
                ringMsAtEntry);
        }

        if (produced > 0) {
            publishAudioRing();
            audioPort.postMessage({
                type: "filled",
                frames: produced
            });
        }

        const available =
            header
                ? Math.max(0, Atomics.load(header, RING_AVAILABLE))
                : 0;
        const room = audioRingCapacityFrames - available;

        const wantsMore =
            pendingNeedFrames > 0 || available < ringTargetFrames();

        if (wantsMore && room >= Math.max(1, blockFrames)) {
            // A time-sliced stop means work is still owed right now; anything
            // else can wait for the next turn of the event loop.
            setTimeout(pump, stoppedForTimeSlice ? 0 : 0);
        }
    } catch (error) {
        setError(error);
        playing = false;
    }
}

function onAudioPortMessage(data) {
    if (!data)
        return;

    if (data.type === "need") {
        // Repeated low-water notifications describe the same missing queue
        // capacity; use max rather than addition to avoid duplicate demand.
        pendingNeedFrames =
            Math.max(
                pendingNeedFrames,
                Math.max(
                    0,
                    Number(data.frames) | 0));

        // Demand is idempotent: keep the largest outstanding deficit. The
        // Worker always renders PCM continuously; MIDI coverage must never turn
        // this into a hard stop. A successful fill is the acknowledgement the
        // Worklet actually needs.
        pump();
        return;
    }

    if (data.type === "clock") {
        postState("clock", {
            epoch:
                Number(data.epoch) >>> 0,
            songTime:
                Number(data.songTime) || 0.0,
            underruns:
                Number(data.underruns) | 0,
            starved:
                !!data.starved
        });

        // A demand can remain outstanding while the ring was temporarily full
        // or while a long synth block was rendering. Clock messages provide a
        // cheap periodic retry after the device has consumed more frames.
        pump();
    }
}

onmessage = async event => {
    const data = event.data || {};

    try {
        if (data.type === "audioPort" && data.port) {
            audioPort = data.port;
            audioPort.onmessage =
                e =>
                    onAudioPortMessage(
                        e.data || {});
            audioPort.start();
            publishAudioRing(true);
            pump();
            return;
        }

        if (data.type === "feederPort") {
            if (feederPort) { try { feederPort.close(); } catch (_) {} }
            feederPort = data.port || null;
            if (feederPort) {
                feederPort.onmessage = e => onFeederMessage(e.data || {});
                feederPort.start();
            } else {
                localActive = false;
                localInFlight = false;
            }
            return;
        }

        if (data.type === "localVelocityFloor") {
            localVelocityFloor = Math.max(0, Math.min(127, data.value | 0));
            return;
        }

        if (data.type === "loadSoundfont") {
            if (!Module || !coreReady)
                throw new Error("SnappySynthV2 core is still starting.");

            const file = data.file;
            if (!file)
                throw new Error("No SF2 file was provided.");

            const path = mountSoundfontFile(file);
            const regions = callLoadSoundfont(path);
            if (regions > 0) {
                Module._ssw_warmup();
            }
            if (regions <= 0) {
                // Roll back the failed layer mount/list entry.
                const mount = soundfontMounts.pop();
                soundfontPaths.pop();
                soundfontFiles.pop();
                try { Module.FS.unmount(mount); } catch (_) {}
                throw new Error(
                    "SnappySynthV2 could not parse and fully materialize " +
                    (file.name || "soundfont.sf2") +
                    " before playback (" + Number(file.size || 0) + " bytes).");
            }

            soundfontLoaded = true;
            renderSongTime = 0.0;
            Module._ssw_set_song_time(0.0);
            resetEventQueue();
            resetAudioRing();
            if (gpuWanted) void setupGpu();
            // SF2 sample/preset allocation may grow Shared WebAssembly.Memory.
            // Refresh the AudioWorklet's memory object without copying PCM.
            publishAudioRing(true);

            const layers = Module._ssw_layer_count();
            postState("soundfont", {
                loaded: true,
                name: layers > 1
                    ? (file.name || "soundfont.sf2") + " (+" + (layers - 1) + " layer" + (layers > 2 ? "s" : "") + ")"
                    : (file.name || "soundfont.sf2"),
                layers,
                regions: Module._ssw_region_count()
            });

            pump();
            return;
        }

        if (data.type === "clearSoundfonts") {
            playing = false;
            resetEventQueue();
            Module._ssw_clear_soundfonts();
            unmountAllSoundfonts();
            soundfontLoaded = false;
            if (gpuActive) disableGpu(gpuWanted ? "waiting for a soundfont" : "off");
            renderSongTime = 0.0;
            Module._ssw_set_song_time(0.0);
            resetAudioRing();
            postState("soundfont", {
                loaded: false,
                name: "",
                layers: 0,
                regions: 0
            });
            return;
        }

        if (data.type === "sysex") {
            const bytes = data.bytes;
            if (bytes instanceof Uint8Array && bytes.length > 0) {
                const event = {
                    bytes,
                    time: Math.max(0.0, Number(data.time) || 0.0)
                };
                // C++ sends in chronological order; retain a defensive ordered insertion.
                let pos = sysexEvents.length;
                while (pos > 0 && sysexEvents[pos - 1].time > event.time) --pos;
                sysexEvents.splice(pos, 0, event);
            }
            return;
        }

        if (data.type === "sysexBatch") {
            if (localActive) return;
            admitSysExBatch(data.meta, data.bytes, data.times);
            pump();
            return;
        }

        if (data.type === "scheduleBatch") {
            if (localActive) return;
            // Admit both streams before pumping.  This preserves sample timing
            // while preventing a same-batch GM/GS/XG reset from being delivered
            // after Program Change/Bank Select audio has already rendered.
            admitSysExBatch(data.meta, data.bytes, data.sysexTimes);
            admitShortSchedule(data.messages, data.times);

            safeUntil = Math.max(
                safeUntil,
                Number(data.safeUntil) || renderSongTime);
            startupWaitingForSchedule = false;
            pump();
            return;
        }

        if (data.type === "schedule") {
            if (localActive) return;
            const messages =
                data.messages;

            const times =
                data.times;

            admitShortSchedule(messages, times);

            safeUntil =
                Math.max(
                    safeUntil,
                    Number(data.safeUntil) ||
                    renderSongTime);

            // Even an empty schedule batch is a valid producer ACK (for a
            // silent interval). It is now safe to satisfy AudioWorklet demand
            // without accidentally pre-filling the ring from the old/reset
            // state.
            startupWaitingForSchedule = false;

            pump();
            return;
        }

        if (data.type === "play") {
            const time =
                Math.max(
                    0.0,
                    Number(data.time) || 0.0);

            if (data.reset) {
                Module._ssw_reset();
            resetStealRate();
                renderSongTime = time;
                Module._ssw_set_song_time(renderSongTime);
                resetAudioRing();
                // Do not clear pendingNeedFrames here. The AudioWorklet sends
                // its fresh request on a different MessagePort and that request
                // may arrive just before this reset. Clearing it would leave the
                // worklet believing audio is on the way and cause a deadlock.
                resetEventQueue();
                startupWaitingForSchedule = true;
                localActive = !!data.local && !!feederPort;
                playing = true;
                localReset(time);
            }

            playing = true;
            pump();
            return;
        }

        if (data.type === "pause") {
            playing = false;
            return;
        }

        if (data.type === "seek") {
            const time =
                Math.max(
                    0.0,
                    Number(data.time) || 0.0);

            Module._ssw_reset();
            resetStealRate();
            renderSongTime = time;
            Module._ssw_set_song_time(renderSongTime);
            resetAudioRing();
            // Preserve cross-port demand for the same reason as play(reset).
            resetEventQueue();
            startupWaitingForSchedule = playing;
            localActive = !!data.local && !!feederPort;
            if (localActive) startupWaitingForSchedule = true;
            localReset(time);
            return;
        }

        if (data.type === "stop") {
            playing = false;
            Module._ssw_reset();
            resetStealRate();
            renderSongTime = 0.0;
            Module._ssw_set_song_time(0.0);
            resetAudioRing();
            pendingNeedFrames = 0;
            resetEventQueue();
            startupWaitingForSchedule = false;
            return;
        }

        if (data.type === "volume") {
            volume = Math.max(0.0, Math.min(1.0, Number(data.value) || 0.0));
            return;
        }

        if (data.type === "gpu") {
            gpuWanted = !!data.enabled;
            void setupGpu();
            return;
        }

        if (data.type === "vor") {
            vorMode =
                data.overlapGain
                    ? 0
                    : 1;

            if (coreReady)
                Module._ssw_set_vor_mode(vorMode);

            return;
        }

        if (data.type === "prebuffer") {
            // Protocol compatibility only. PCM pre-rendering no longer exists.
            // Most importantly, receiving this legacy setting must NOT tear
            // down/reset the live shared ring: MainWindow sends configuration
            // updates while a MIDI is being installed, and reallocating here
            // used to erase perfectly good realtime audio and leave the
            // AudioWorklet underrunning until another demand cycle happened.
            prebufferSeconds = 0.0;

            midiDuration =
                Math.max(
                    0.0,
                    Number(data.duration) || 0.0);

            postState("configured", {
                prebufferFrames
            });
            pump();
            return;
        }

        if (data.type === "debugMetrics" ||
            (data.type === "configure" && data.maxVoices === undefined &&
             typeof data.debugMetrics === "boolean")) {
            // Debug toggle only: must not touch the synth config (rev. 40.1, see HANDOFF §20).
            debugMetrics = data.type === "debugMetrics" ? !!data.enabled : data.debugMetrics;
            if (coreReady && Module && Module._ssw_set_debug_metrics)
                Module._ssw_set_debug_metrics(debugMetrics ? 1 : 0);
            return;
        }

        if (data.type === "configure") {
            sampleRateHz =
                Math.max(
                    8000,
                    Math.min(
                        384000,
                        Number(data.sampleRate) ||
                        sampleRateHz));

            maxVoices =
                Math.max(
                    1,
                    Math.min(
                        5000000,
                        Number(data.maxVoices) |
                        0));

            minVoices =
                Math.max(
                    0,
                    Math.min(
                        5000000,
                        Number(data.minVoices) |
                        0));

            blockFrames =
                Math.max(
                    1,
                    Number(data.blockFrames) |
                    0);

            numBuffers =
                Math.max(
                    1,
                    Math.min(
                        128,
                        Number(data.numBuffers) |
                        0));

            prebufferSeconds = 0.0;

            midiDuration =
                Math.max(
                    0.0,
                    Number(data.midiDuration) || midiDuration);

            synthChannels =
                Number(data.channels) === 1
                    ? 1
                    : 2;

            bitsPerSample =
                Number(data.bitsPerSample) === 16
                    ? 16
                    : 32;

            realtimePriority =
                data.realtimePriority
                    ? 1
                    : 0;

            if (typeof data.debugMetrics === "boolean") {
                debugMetrics = data.debugMetrics;
                if (coreReady && Module && Module._ssw_set_debug_metrics)
                    Module._ssw_set_debug_metrics(debugMetrics ? 1 : 0);
            }

            requestedWorkers =
                Math.max(
                    0,
                    Number(data.workers) |
                    0);

            noteSharding =
                Math.max(
                    0,
                    Math.min(
                        3,
                        Number(data.noteSharding) |
                        0));

            stealScoreCache =
                data.stealScoreCache !== false;

            fastNoteOff =
                data.fastNoteOff !== false;

            validateState =
                !!data.validateState;

            softClip =
                data.softClip !== false;

            // The bridge intentionally sends configuration immediately after
            // creating this Worker so startup settings are available before the
            // Emscripten core is instantiated. During that bootstrap window
            // Module is still null. Keep the settings above, but do not touch
            // exported WASM functions until SnappySynthCore(...).then() has
            // completed. initCore() below will consume these already-updated
            // values on its first initialization.
            if (!Module || !coreReady)
                return;

            playing = false;
            reinitializeCore();

            if (audioPort) {
                audioPort.postMessage({
                    type: "engineConfig",
                    blockFrames,
                    numBuffers
                });
            }

            postState("configured", {
                maxVoices,
                minVoices,
                blockFrames,
                numBuffers,
                channels: synthChannels,
                bitsPerSample,
                realtimePriority,
                requestedWorkers,
                workerCount: Module._ssw_worker_count(),
                noteSharding,
                stealScoreCache,
                fastNoteOff,
                validateState,
                softClip,
                prebufferFrames
            });
            return;
        }

        if (data.type === "softClip") {
            softClip =
                data.enabled !== false;

            if (coreReady)
                Module._ssw_set_soft_clip(
                    softClip ? 1 : 0);

            postState("configured", {
                softClip
            });
            return;
        }
    } catch (error) {
        setError(error);
    }
};

const coreScriptUrl =
    new URL(
        "./snappysynth-core.js",
        self.location.href).href;

console.info(
    "[SnappySynthV2 worker] core script:",
    coreScriptUrl);

SnappySynthCore({
    /*
     * This module is MODULARIZE + pthreads and is itself instantiated from
     * snappysynth-worker.js. In that nested-worker scenario Emscripten cannot
     * reliably infer the URL of the main generated JS file.
     *
     * Without this value the generated snappysynth-core.worker.js receives
     * urlOrBlob=undefined and executes URL.createObjectURL(undefined), which
     * is exactly the Chrome "Overload resolution failed" seen in the console.
     */
    mainScriptUrlOrBlob:
        coreScriptUrl,

    locateFile(path) {
        return new URL(
            path,
            self.location.href).href;
    },
    print(text) {
        if (text)
            console.log("[SnappySynthV2]", text);
    },
    printErr(text) {
        if (text)
            console.warn("[SnappySynthV2]", text);
    }
}).then(module => {
    Module = module;
    initCore();

    postState("ready", {
        ready: true,
        maxVoices,
        minVoices,
        blockFrames,
        numBuffers,
        channels: synthChannels,
        bitsPerSample,
        realtimePriority,
        requestedWorkers,
        workerCount: Module._ssw_worker_count(),
        detectedCores: Module._ssw_detected_cores(),
        workerThreadFailures: Module._ssw_worker_thread_failures(),
        noteSharding,
        stealScoreCache,
        fastNoteOff,
        validateState,
        softClip,
        prebufferFrames
    });

    reportWorkerPool();

    pump();
}).catch(setError);
