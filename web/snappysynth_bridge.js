(() => {
    "use strict";

    if (globalThis.WasmidiSnappyBridge)
        return;

    const state = {
        ready: false,
        localSynthReady: false,
        localSynthActive: false,
        localSynthLoadMs: 0,
        soundfontLoaded: false,
        soundfontName: "",
        status: "SnappySynthV2 idle",
        sampleRate: 0,
        activeVoices: 0,
        freeVoices: 0,
        steals: 0,
        rebalanced: 0,     // free voices returned to the global pool, per second
        droppedNotes: 0,   // note-ons lost with no sound, cumulative
        underruns: 0,
        layers: 0,
        regions: 0,
        playing: false,
        starved: false,
        audioClock: 0.0,
        audioClockPerf: 0.0,
        audioClockBase: 0.0,
        transportEpoch: 1,

        // SnappySynth.cfg / source-exposed settings.
        maxVoices: 16384,
        renderLoadPercent: 0,
        lastRenderMs: 0,
        lastDispatchMs: 0,
        pathFastVoices: 0,
        pathScalarVoices: 0,
        workersParticipating: 0,
        allocMs: 0,
        ccHotspot: 0,
        ccHotspotCount: 0,
        controllersCollapsed: 0,
        presampleSeen: 0,
        presampleSkipped: 0,
        presampleResampled: 0,
        notesStarted: 0,
        voicesRecycled: 0,
        missInterp: 0,
        missLoop: 0,
        missFilter: 0,
        missOther: 0,
        pathSimdVoiceVoices: 0,
        workerBusyMs: 0,
        ringFillPercent: 0,
        pumpGapMs: 0,
        blocksPerPump: 0,
        renderBudget: 0,
        minVoices: 0,
        blockFrames: 512,
        numBuffers: 16,
        // Legacy field retained for protocol compatibility only. The browser
        // backend renders PCM on AudioWorklet demand and never seconds ahead.
        prebufferSeconds: 0.0,
        midiDuration: 0.0,
        prebufferFrames: 0,
        requestedSampleRate: 44100, // supplied SnappySynth.cfg default
        channels: 2,
        bitsPerSample: 32,
        realtimePriority: true,

        // voice.c runtime tuning envs.
        workers: 0,            // 0 = auto
        workerCount: 0,        // actual source-selected worker count
        noteSharding: 0,       // 0 auto, 1 channel, 2 hash, 3 single
        stealScoreCache: true,
        debugMetrics: false,
        fastNoteOff: true,
        validateState: false,

        // Voice renderer runtime switches.
        softClip: true,
        overlapGain: false,
        gpu: false,
        gpuStatus: "off",

        volume: 1.00
    };

    let context = null;
    let node = null;
    let worker = null;
    let backendPromise = null;
    let workerReadyPromise = null;
    let workerReadyResolve = null;
    let workerReadyReject = null;
    let input = null;

    // The AudioWorklet writes the *actually delivered* PCM frame count here
    // once per render quantum.  Reading this shared counter on the UI thread
    // avoids ~46 ms clock-report quantization, interpolation drift, and—most
    // importantly—stale pre-seek clock messages arriving after a transport
    // reset.  Slot 0 is the transport epoch, slot 1 the delivered frame count,
    // and slot 2 the current starvation flag.
    const CLOCK_EPOCH = 0;
    const CLOCK_FRAMES = 1;
    const CLOCK_STARVED = 2;
    const CLOCK_WORDS = 4;
    let sharedClockBuffer = null;
    let sharedClock = null;

    function nextTransportEpoch() {
        state.transportEpoch = ((Number(state.transportEpoch) >>> 0) + 1) >>> 0;
        if (state.transportEpoch === 0)
            state.transportEpoch = 1;
        return state.transportEpoch >>> 0;
    }

    function resetSharedClock(baseTime, advanceEpoch = true) {
        const epoch = advanceEpoch
            ? nextTransportEpoch()
            : (Number(state.transportEpoch) >>> 0) || 1;
        state.audioClockBase = Math.max(0.0, Number(baseTime) || 0.0);
        state.audioClock = state.audioClockBase;
        state.audioClockPerf = performance.now();
        state.starved = false;
        if (sharedClock) {
            Atomics.store(sharedClock, CLOCK_EPOCH, epoch | 0);
            Atomics.store(sharedClock, CLOCK_FRAMES, 0);
            Atomics.store(sharedClock, CLOCK_STARVED, 0);
        }
        return epoch;
    }

    function updateStatus(text) {
        state.status = String(text || "");
    }

    function getAudioClock() {
        if (!state.soundfontLoaded)
            return -1.0;

        if (sharedClock && state.sampleRate > 0) {
            const epoch = Atomics.load(sharedClock, CLOCK_EPOCH) >>> 0;
            if (epoch === (Number(state.transportEpoch) >>> 0)) {
                const frames = Atomics.load(sharedClock, CLOCK_FRAMES) >>> 0;
                state.starved = Atomics.load(sharedClock, CLOCK_STARVED) !== 0;
                const value = state.audioClockBase + frames / state.sampleRate;
                state.audioClock = value;
                state.audioClockPerf = performance.now();
                return value;
            }
        }

        let value = state.audioClock;

        if (state.playing && !state.starved && state.audioClockPerf > 0.0) {
            value +=
                (performance.now() -
                 state.audioClockPerf) /
                1000.0;
        }

        return value;
    }

    function copyFileName(file) {
        return file && file.name
            ? file.name
            : "soundfont.sf2";
    }

    // Synth MIDI source over a MessagePort. Default: the parser Worker answers the synth
    // directly with admitted events (HANDOFF sec. 70). ?synthsource=feeder: the old JS
    // feeder (HANDOFF §26-27). ?synthsource=relay: batches relayed by the page.
    let feeder = null;
    let feederPortForWorker = null;
    let localVelocityFloor = -1;
    const synthSource = (() => {
        try { return new URL(globalThis.location.href).searchParams.get("synthsource") || ""; }
        catch (_) { return ""; }
    })();
    const localSourceAllowed = synthSource === "feeder";
    const parserSourceAllowed = synthSource !== "feeder" && synthSource !== "relay";

    function deliverFeederPort() {
        if (worker && feederPortForWorker) {
            worker.postMessage({ type: "feederPort", port: feederPortForWorker }, [feederPortForWorker]);
            feederPortForWorker = null;
        }
    }

    function clearLocalMidiSource() {
        if (feeder) { try { feeder.terminate(); } catch (_) {} }
        feeder = null;
        feederPortForWorker = null;
        state.localSynthReady = false;
        state.localSynthActive = false;
        localVelocityFloor = -1;
        if (worker) worker.postMessage({ type: "feederPort", port: null });
    }

    function setLocalMidiSource(file, parserWorker) {
        clearLocalMidiSource();
        if (parserSourceAllowed && parserWorker && typeof MessageChannel === "function") {
            const channel = new MessageChannel();
            parserWorker.postMessage({ type: "feeder-port", port: channel.port1 }, [channel.port1]);
            feederPortForWorker = channel.port2;
            state.localSynthReady = true;
            state.localSynthLoadMs = 0;
            deliverFeederPort();
            return;
        }
        if (!localSourceAllowed || !file || typeof Worker !== "function") return;
        feeder = new Worker("./synth-feeder-worker.js?v=13.12.0");
        const channel = new MessageChannel();
        feeder.postMessage({ type: "port", port: channel.port1 }, [channel.port1]);
        feederPortForWorker = channel.port2;
        const mine = feeder;
        feeder.onmessage = event => {
            if (mine !== feeder) return;
            const d = event.data || {};
            if (d.type === "ready") {
                state.localSynthReady = true;
                state.localSynthLoadMs = Number(d.ms) || 0;
                deliverFeederPort();
            } else if (d.type === "failed") {
                console.warn("SnappySynth feeder unavailable, using parser path:", d.error);
                clearLocalMidiSource();
            }
        };
        feeder.onerror = () => { if (mine === feeder) clearLocalMidiSource(); };
        feeder.postMessage({ type: "load", file });
    }

    function setLocalVelocityFloor(value) {
        const v = Math.max(0, Math.min(127, value | 0));
        if (v === localVelocityFloor) return;
        localVelocityFloor = v;
        if (worker) worker.postMessage({ type: "localVelocityFloor", value: v });
    }

    async function ensureBackend() {
        if (backendPromise)
            return backendPromise;

        backendPromise = (async () => {
            if (!globalThis.crossOriginIsolated) {
                updateStatus(
                    "Preparing SnappySynthV2 audio isolation — reload required");

                throw new Error(
                    "SnappySynthV2 pthread core requires crossOriginIsolated=true. " +
                    "The Pages COI service worker has not taken control yet; reload the page and select the SF2 again.");
            }

            const audioOptions = {
                latencyHint: "interactive"
            };

            // AudioContext supports a requested sample rate in modern browsers.
            // The actual device/context rate is still authoritative and is sent
            // to the synth core after construction so pitch never drifts.
            if (state.requestedSampleRate > 0)
                audioOptions.sampleRate =
                    state.requestedSampleRate;

            context =
                new AudioContext(
                    audioOptions);

            state.sampleRate =
                Math.round(
                    context.sampleRate);

            await context.audioWorklet.addModule(
                "./snappysynth-audio-worklet.js?v=13.12.0");

            node =
                new AudioWorkletNode(
                    context,
                    "snappysynth-output",
                    {
                        numberOfInputs: 0,
                        numberOfOutputs: 1,
                        outputChannelCount: [2]
                    });

            node.connect(
                context.destination);

            if (typeof SharedArrayBuffer === "function") {
                sharedClockBuffer = new SharedArrayBuffer(
                    Int32Array.BYTES_PER_ELEMENT * CLOCK_WORDS);
                sharedClock = new Int32Array(sharedClockBuffer);
                Atomics.store(sharedClock, CLOCK_EPOCH,
                    (Number(state.transportEpoch) >>> 0) | 0);
                Atomics.store(sharedClock, CLOCK_FRAMES, 0);
                Atomics.store(sharedClock, CLOCK_STARVED, 0);
                node.port.postMessage({
                    type: "clockState",
                    buffer: sharedClockBuffer,
                    epoch: Number(state.transportEpoch) >>> 0
                });
            }

            // ?synthlimit=N (0 = off) for A/B tests of the render limit (HANDOFF sec. 42).
            let synthLimitParam = "";
            try {
                const value = new URL(globalThis.location.href).searchParams.get("synthlimit");
                if (value !== null) synthLimitParam = "&synthlimit=" + encodeURIComponent(value);
            } catch (_) {}
            worker =
                new Worker(
                    "./snappysynth-worker.js?v=13.12.0" + synthLimitParam);

            workerReadyPromise =
                new Promise(
                    (resolve, reject) => {
                        workerReadyResolve = resolve;
                        workerReadyReject = reject;
                    });

            const channel =
                new MessageChannel();

            worker.postMessage({
                type: "audioPort",
                port: channel.port1
            }, [channel.port1]);
            if (state.localSynthReady) deliverFeederPort();

            node.port.postMessage({
                type: "workerPort",
                port: channel.port2
            }, [channel.port2]);

            node.port.postMessage({
                type: "config",
                blockFrames:
                    state.blockFrames,
                numBuffers:
                    state.numBuffers
            });

            node.port.postMessage({
                type: "volume",
                value: state.volume
            });

            worker.onmessage = event => {
                const data =
                    event.data || {};

                if (Number.isFinite(data.sampleRate))
                    state.sampleRate =
                        Math.round(
                            data.sampleRate);

                if (Number.isFinite(data.activeVoices))
                    state.activeVoices =
                        Math.max(
                            0,
                            Math.round(
                                data.activeVoices));

                if (Number.isFinite(data.freeVoices))
                    state.freeVoices = Math.max(0, Math.round(data.freeVoices));
                if (Number.isFinite(data.steals))
                    state.steals = Math.max(0, Math.round(data.steals));
                if (Number.isFinite(data.rebalanced))
                    state.rebalanced = Math.max(0, Math.round(data.rebalanced));
                if (Number.isFinite(data.droppedNotes))
                    state.droppedNotes = Math.max(0, Math.round(data.droppedNotes));
                // Render telemetry for the sidebar graphs. Carried here
                // rather than printed: console output from a worker at this
                // rate costs the main thread real time.
                if (Number.isFinite(data.renderLoadPercent))
                    state.renderLoadPercent = data.renderLoadPercent;
                if (Number.isFinite(data.lastRenderMs))
                    state.lastRenderMs = data.lastRenderMs;
                if (Number.isFinite(data.lastDispatchMs))
                    state.lastDispatchMs = data.lastDispatchMs;
                if (Number.isFinite(data.pathFastVoices))
                    state.pathFastVoices = data.pathFastVoices;
                if (Number.isFinite(data.pathScalarVoices))
                    state.pathScalarVoices = data.pathScalarVoices;
                for (const k of ["missInterp","missLoop","missFilter","missOther","workersParticipating","allocMs","notesStarted","voicesRecycled","ccHotspot","ccHotspotCount","controllersCollapsed","presampleSeen","presampleSkipped","presampleResampled"])
                    if (Number.isFinite(data[k])) state[k] = data[k];
                if (Number.isFinite(data.pathSimdVoiceVoices))
                    state.pathSimdVoiceVoices = data.pathSimdVoiceVoices;
                if (Number.isFinite(data.workerBusyMs))
                    state.workerBusyMs = data.workerBusyMs;
                if (Number.isFinite(data.ringFillPercent))
                    state.ringFillPercent = data.ringFillPercent;
                if (Number.isFinite(data.pumpGapMs))
                    state.pumpGapMs = data.pumpGapMs;
                if (Number.isFinite(data.blocksPerPump))
                    state.blocksPerPump = data.blocksPerPump;
                if (Number.isFinite(data.renderBudget))
                    state.renderBudget = data.renderBudget;
                if (Number.isFinite(data.layers))
                    state.layers = Math.max(0, Math.round(data.layers));
                if (Number.isFinite(data.regions))
                    state.regions = Math.max(0, Math.round(data.regions));

                if (Number.isFinite(data.workerCount))
                    state.workerCount = Math.max(0, Math.round(data.workerCount));
                if (Number.isFinite(data.channels))
                    state.channels = Number(data.channels) === 1 ? 1 : 2;
                if (Number.isFinite(data.bitsPerSample))
                    state.bitsPerSample = Number(data.bitsPerSample) === 16 ? 16 : 32;
                if (Number.isFinite(data.numBuffers))
                    state.numBuffers = Math.max(1, Math.round(data.numBuffers));
                if (Number.isFinite(data.prebufferFrames))
                    state.prebufferFrames = Math.max(0, Math.round(data.prebufferFrames));

                switch (data.type) {
                case "ready":
                    state.ready = true;

                    if (workerReadyResolve) {
                        workerReadyResolve(true);
                        workerReadyResolve = null;
                        workerReadyReject = null;
                    }

                    updateStatus(
                        "SnappySynthV2 ready — load an SF2");
                    break;

                case "soundfont":
                    state.soundfontLoaded =
                        !!data.loaded;

                    state.soundfontName =
                        String(
                            data.name || "");

                    state.regions =
                        Math.max(
                            0,
                            Number(data.regions) | 0);

                    updateStatus(
                        state.soundfontLoaded
                            ? "SF2 ready"
                            : "SF2 load failed");
                    settleSoundfontWaiters(state.soundfontLoaded);
                    break;

                case "clock":
                    if (Number.isFinite(data.epoch) &&
                        (Number(data.epoch) >>> 0) !==
                            (Number(state.transportEpoch) >>> 0)) {
                        // Different MessageChannels are used for transport
                        // control and periodic clock reports. A report queued
                        // before a seek can therefore arrive after the seek.
                        // Never let that obsolete clock move the UI transport
                        // back to the previous position.
                        break;
                    }
                    state.audioClock =
                        Math.max(
                            0.0,
                            Number(data.songTime) ||
                            0.0);

                    state.audioClockPerf =
                        performance.now();

                    state.underruns =
                        Math.max(
                            0,
                            Number(data.underruns) |
                            0);

                    state.starved =
                        !!data.starved;
                    break;

                case "configured":
                    if (typeof data.loaded === "boolean")
                        state.soundfontLoaded = data.loaded;

                    if (Number.isFinite(data.maxVoices))
                        state.maxVoices = Math.max(1, Math.round(data.maxVoices));
                    if (Number.isFinite(data.minVoices))
                        state.minVoices = Math.max(0, Math.round(data.minVoices));
                    if (Number.isFinite(data.blockFrames))
                        state.blockFrames = Math.max(1, Math.round(data.blockFrames));
                    if (Number.isFinite(data.requestedWorkers))
                        state.workers = Math.max(0, Math.round(data.requestedWorkers));
                    if (Number.isFinite(data.noteSharding))
                        state.noteSharding = Math.max(0, Math.min(3, Math.round(data.noteSharding)));
                    if (typeof data.stealScoreCache === "boolean")
                        state.stealScoreCache = data.stealScoreCache;
                    if (typeof data.debugMetrics === "boolean")
                        state.debugMetrics = data.debugMetrics;
                    if (typeof data.fastNoteOff === "boolean")
                        state.fastNoteOff = data.fastNoteOff;
                    if (typeof data.validateState === "boolean")
                        state.validateState = data.validateState;
                    if (typeof data.softClip === "boolean")
                        state.softClip = data.softClip;
                    if (typeof data.realtimePriority === "number" ||
                        typeof data.realtimePriority === "boolean")
                        state.realtimePriority = !!data.realtimePriority;

                    updateStatus(
                        state.soundfontLoaded
                            ? "SF2 ready"
                            : "SnappySynthV2 ready — load an SF2");
                    break;

                case "gpu":
                    state.gpuStatus = String(data.status || "off");
                    updateStatus("WasmiSynth GPU: " + state.gpuStatus);
                    if (state.gpuStatus.startsWith("unavailable"))
                        console.warn("[WASMIDI WasmiSynth] GPU", state.gpuStatus);
                    break;

                case "error": {
                    settleSoundfontWaiters(state.soundfontLoaded);
                    const message =
                        String(
                            data.message ||
                            "SnappySynthV2 error");

                    if (!state.ready &&
                        workerReadyReject) {
                        workerReadyReject(
                            new Error(message));

                        workerReadyResolve = null;
                        workerReadyReject = null;
                    }

                    updateStatus(message);

                    console.error(
                        "[WASMIDI SnappySynthV2]",
                        message);
                    break;
                }
                }
            };

            worker.onerror = error => {
                const message =
                    error && error.message
                        ? String(error.message)
                        : "SnappySynthV2 worker error";

                if (!state.ready &&
                    workerReadyReject) {
                    workerReadyReject(
                        new Error(message));

                    workerReadyResolve = null;
                    workerReadyReject = null;
                }

                updateStatus(message);

                console.error(
                    "[WASMIDI SnappySynthV2 worker]",
                    error);
            };

            // Worker core initialization values.
            worker.postMessage({
                type: "configure",
                sampleRate:
                    state.sampleRate,
                maxVoices:
                    state.maxVoices,
                minVoices:
                    state.minVoices,
                blockFrames:
                    state.blockFrames,
                numBuffers:
                    state.numBuffers,
                prebufferSeconds:
                    state.prebufferSeconds,
                midiDuration:
                    state.midiDuration,
                channels:
                    state.channels,
                bitsPerSample:
                    state.bitsPerSample,
                realtimePriority:
                    state.realtimePriority,
                workers:
                    state.workers,
                noteSharding:
                    state.noteSharding,
                stealScoreCache:
                    state.stealScoreCache,
                debugMetrics:
                    state.debugMetrics,
                fastNoteOff:
                    state.fastNoteOff,
                validateState:
                    state.validateState,
                softClip:
                    state.softClip
            });

            worker.postMessage({
                type: "vor",
                overlapGain:
                    state.overlapGain
            });

            updateStatus(
                "Starting SnappySynthV2 core…");

            // Do not let loadSoundfontFile() post a File until the modularized
            // Emscripten core has instantiated and the worker has sent ready.
            await workerReadyPromise;

            return true;
        })();

        try {
            return await backendPromise;
        } catch (error) {
            backendPromise = null;
            workerReadyPromise = null;
            workerReadyResolve = null;
            workerReadyReject = null;
            throw error;
        }
    }

    // Resolved by the worker's "soundfont" answer (or an error) so callers know
    // whether the SF2 actually loaded (HANDOFF sec. 67).
    let soundfontWaiters = [];

    function settleSoundfontWaiters(loaded) {
        const waiters = soundfontWaiters;
        soundfontWaiters = [];
        for (const resolve of waiters) resolve(!!loaded);
    }

    async function loadSoundfontFile(file) {
        if (!file)
            return false;

        const addingLayer = state.soundfontLoaded;
        state.soundfontName = copyFileName(file);

        updateStatus(
            addingLayer ? "Adding SoundFont layer…" : "Loading SF2…");

        try {
            await ensureBackend();

            const answered = new Promise(resolve => soundfontWaiters.push(resolve));
            worker.postMessage({
                type: "loadSoundfont",
                file
            });

            return await answered;
        } catch (error) {
            updateStatus(
                error &&
                error.message
                    ? error.message
                    : "Could not start SnappySynthV2");

            console.error(
                "[WASMIDI] SnappySynthV2 SF2 load failed:",
                error);

            return false;
        }
    }

    function getInput() {
        if (input)
            return input;

        input =
            document.createElement(
                "input");

        input.type = "file";
        input.accept =
            ".sf2,audio/x-soundfont,application/octet-stream";

        input.style.position = "fixed";
        input.style.left = "-10000px";
        input.style.top = "-10000px";
        input.style.width = "1px";
        input.style.height = "1px";
        input.style.opacity = "0";

        document.body.appendChild(input);

        input.onchange = async () => {
            const file =
                input.files &&
                input.files.length
                    ? input.files[0]
                    : null;

            input.value = null;

            if (!file)
                return;

            await loadSoundfontFile(file);
        };

        return input;
    }

    function openSoundfont() {
        // `click()` stays synchronous with the QML button's user gesture.
        const picker =
            getInput();

        picker.click();

        // Start the backend in parallel. The change handler awaits the same
        // promise before handing the File object to WORKERFS.
        ensureBackend().catch(error => {
            console.warn(
                "[WASMIDI] SnappySynth backend not ready:",
                error);
        });
    }

    async function resumeAudio() {
        try {
            await ensureBackend();

            if (context &&
                context.state !== "running") {
                await context.resume();
            }
        } catch (error) {
            console.warn(
                "[WASMIDI] Could not resume SnappySynth audio:",
                error);
        }
    }

    function play(time, reset) {
        const value =
            Math.max(
                0.0,
                Number(time) ||
                0.0);

        state.playing = true;
        state.starved = false;

        let epoch = Number(state.transportEpoch) >>> 0;
        if (reset) {
            epoch = resetSharedClock(value, true);
        }

        // A loaded SoundFont means the backend already exists. Post the reset
        // synchronously so the first schedule batch sent by Qt can never race
        // ahead of it and then be erased by a delayed reset.
        if (reset)
            state.localSynthActive = state.localSynthReady;
        if (worker) {
            worker.postMessage({
                type: "play",
                time: value,
                reset: !!reset,
                local: state.localSynthActive,
                epoch
            });
        }

        if (node) {
            node.port.postMessage({
                type: "play",
                time: value,
                resetClock: !!reset,
                epoch
            });
        }

        // AudioContext.resume() is intentionally independent of control-message
        // ordering. It still runs from the user's Play gesture.
        resumeAudio();
    }

    function pause() {
        // Capture the extrapolated device clock before clearing `playing`;
        // otherwise getAudioClock() would return the last periodic report and
        // jump backwards by up to one report interval on pause.
        const pausedClock =
            getAudioClock();

        state.playing = false;
        state.audioClock =
            pausedClock;
        state.audioClockPerf =
            performance.now();

        if (worker)
            worker.postMessage({
                type: "pause"
            });

        if (node)
            node.port.postMessage({
                type: "pause"
            });
    }

    function seek(time) {
        const value =
            Math.max(
                0.0,
                Number(time) ||
                0.0);

        const epoch = resetSharedClock(value, true);

        state.localSynthActive = state.localSynthReady;
        if (worker)
            worker.postMessage({
                type: "seek",
                time: value,
                local: state.localSynthActive,
                epoch
            });

        if (node) {
            node.port.postMessage({
                type: "flush",
                time: value,
                epoch
            });
        }
    }

    function stop() {
        state.playing = false;
        const epoch = resetSharedClock(0.0, true);

        if (worker)
            worker.postMessage({
                type: "stop",
                epoch
            });

        if (node) {
            node.port.postMessage({
                type: "pause"
            });

            node.port.postMessage({
                type: "flush",
                time: 0.0,
                epoch
            });
        }
    }

    function syncVisualClock(time) {
        // Compatibility no-op. SnappySynth's AudioWorklet intentionally runs
        // from its own PCM/device clock; renderer timing must never gate audio.
        void time;
    }

    function schedule(messages, times, safeUntil) {
        if (!worker ||
            !state.soundfontLoaded)
            return;

        worker.postMessage({
            type: "schedule",
            messages,
            times,
            safeUntil:
                Number(safeUntil) ||
                0.0
        }, [
            messages.buffer,
            times.buffer
        ]);
    }

    // Atomically publish one mapped-MIDI producer batch.  Posting the short
    // messages and SysEx as two Worker messages let the "schedule" handler
    // satisfy AudioWorklet demand before the following SysEx message was even
    // dispatched.  A GM/GS reset at the same tick could therefore arrive late
    // and reset channels back to program/bank 0 after the correct Program
    // Changes had already been queued.  Keep the two streams separate inside
    // the synth (SysEx is variable length), but cross the Worker boundary once
    // and allow pumping only after both have been admitted.
    function scheduleBatch(messages, times, safeUntil, meta, bytes, sysexTimes) {
        if (!worker || !state.soundfontLoaded ||
            !(messages instanceof Uint32Array) ||
            !(times instanceof Float64Array) ||
            !(meta instanceof Uint32Array) ||
            !(bytes instanceof Uint8Array) ||
            !(sysexTimes instanceof Float64Array)) {
            return;
        }

        worker.postMessage({
            type: "scheduleBatch",
            messages,
            times,
            safeUntil: Number(safeUntil) || 0.0,
            meta,
            bytes,
            sysexTimes
        }, [
            messages.buffer,
            times.buffer,
            meta.buffer,
            bytes.buffer,
            sysexTimes.buffer
        ]);
    }

    function scheduleSysEx(bytes, time) {
        if (!worker || !state.soundfontLoaded || !(bytes instanceof Uint8Array))
            return;
        worker.postMessage({
            type: "sysex",
            bytes,
            time: Math.max(0.0, Number(time) || 0.0)
        }, [bytes.buffer]);
    }

    function scheduleSysExBatch(meta, bytes, times) {
        if (!worker || !state.soundfontLoaded ||
            !(meta instanceof Uint32Array) ||
            !(bytes instanceof Uint8Array) ||
            !(times instanceof Float64Array)) {
            return;
        }
        worker.postMessage({
            type: "sysexBatch",
            meta,
            bytes,
            times
        }, [meta.buffer, bytes.buffer, times.buffer]);
    }

    function clearSoundfonts() {
        const epoch = resetSharedClock(0.0, true);
        state.soundfontLoaded = false;
        state.soundfontName = "";
        state.layers = 0;
        state.regions = 0;
        updateStatus("SnappySynthV2 ready — load an SF2");
        if (worker) worker.postMessage({ type: "clearSoundfonts" });
        if (node) {
            node.port.postMessage({ type: "pause" });
            node.port.postMessage({ type: "flush", time: 0.0, epoch });
        }
    }

    function setVolume(percent) {
        state.volume =
            Math.max(
                0.0,
                Math.min(
                    1.0,
                    Number(percent) /
                    100.0));

        // Preserve Pass 10 exactly: UI volume is post-synth AudioWorklet gain
        // and never overwrites MIDI Universal Master Volume / GS state.
        if (node) {
            node.port.postMessage({
                type: "volume",
                value: state.volume
            });
        }
    }

    // WASMIDI Audio's spectrum visualizer taps the output (HANDOFF sec. 67).
    function getOutput() {
        return { context, node };
    }

    function setGpu(enabled) {
        state.gpu = !!enabled;
        if (worker)
            worker.postMessage({ type: "gpu", enabled: state.gpu });
    }

    function setOverlapGain(enabled) {
        state.overlapGain =
            !!enabled;

        if (worker) {
            worker.postMessage({
                type: "vor",
                overlapGain:
                    state.overlapGain
            });
        }
    }

    function setPrebuffer(seconds, duration) {
        // Legacy API only. Browser audio is strictly demand-rendered into the
        // small NumBuffers transport ring, so callers cannot re-enable a
        // seconds-long PCM cache by passing the old setting.
        void seconds;
        const songDuration = Number(duration);
        state.prebufferSeconds = 0.0;
        state.midiDuration =
            Number.isFinite(songDuration)
                ? Math.max(0.0, songDuration)
                : state.midiDuration;

        if (worker) {
            worker.postMessage({
                type: "prebuffer",
                seconds: 0.0,
                duration: state.midiDuration
            });
        }
    }

    function configure(options, legacyFrames) {
        // Backward compatibility with Pass 9/10 callers.
        if (typeof options !== "object" || options === null) {
            options = {
                maxVoices: options,
                blockFrames: legacyFrames
            };
        }

        if (Number.isFinite(Number(options.maxVoices))) {
            state.maxVoices =
                Math.max(
                    1,
                    Math.min(
                        5000000,
                        Number(options.maxVoices) |
                        0));
        }

        if (Number.isFinite(Number(options.minVoices))) {
            state.minVoices =
                Math.max(
                    0,
                    Math.min(
                        5000000,
                        Number(options.minVoices) |
                        0));
        }

        if (Number.isFinite(Number(options.blockFrames))) {
            state.blockFrames =
                Math.max(
                    1,
                    Math.min(
                        65536,
                        Number(options.blockFrames) |
                        0));
        }

        if (Number.isFinite(Number(options.numBuffers))) {
            state.numBuffers =
                Math.max(
                    1,
                    Math.min(
                        128,
                        Number(options.numBuffers) |
                        0));
        }

        if (Number.isFinite(Number(options.requestedSampleRate))) {
            state.requestedSampleRate =
                Math.max(
                    0,
                    Math.min(
                        384000,
                        Number(options.requestedSampleRate) |
                        0));
        }

        if (Number.isFinite(Number(options.channels)))
            state.channels =
                Number(options.channels) === 1 ? 1 : 2;

        if (Number.isFinite(Number(options.bitsPerSample)))
            state.bitsPerSample =
                Number(options.bitsPerSample) === 16 ? 16 : 32;

        if (typeof options.realtimePriority === "boolean")
            state.realtimePriority =
                options.realtimePriority;

        if (Number.isFinite(Number(options.workers))) {
            state.workers =
                Math.max(
                    0,
                    Math.min(
                        256,
                        Number(options.workers) |
                        0));
        }

        if (Number.isFinite(Number(options.noteSharding))) {
            state.noteSharding =
                Math.max(
                    0,
                    Math.min(
                        3,
                        Number(options.noteSharding) |
                        0));
        }

        if (typeof options.stealScoreCache === "boolean")
            state.stealScoreCache =
                options.stealScoreCache;
        if (typeof options.debugMetrics === "boolean")
            state.debugMetrics = options.debugMetrics;

        if (typeof options.fastNoteOff === "boolean")
            state.fastNoteOff =
                options.fastNoteOff;

        if (typeof options.validateState === "boolean")
            state.validateState =
                options.validateState;

        if (typeof options.softClip === "boolean")
            state.softClip =
                options.softClip;

        if (node) {
            const flushTime = Math.max(0.0, Number(getAudioClock()) || state.audioClock || 0.0);
            const epoch = resetSharedClock(flushTime, true);
            node.port.postMessage({
                type: "pause"
            });

            node.port.postMessage({
                type: "flush",
                time: flushTime,
                epoch
            });

            node.port.postMessage({
                type: "config",
                blockFrames:
                    state.blockFrames,
                numBuffers:
                    state.numBuffers
            });
        }

        if (worker) {
            // Core reinitialization preserves the merged instrument and reapplies
            // presampling at the active AudioContext sample rate.
            state.soundfontLoaded = false;

            worker.postMessage({
                type: "configure",
                sampleRate:
                    state.sampleRate,
                maxVoices:
                    state.maxVoices,
                minVoices:
                    state.minVoices,
                blockFrames:
                    state.blockFrames,
                numBuffers:
                    state.numBuffers,
                prebufferSeconds:
                    state.prebufferSeconds,
                midiDuration:
                    state.midiDuration,
                channels:
                    state.channels,
                bitsPerSample:
                    state.bitsPerSample,
                realtimePriority:
                    state.realtimePriority,
                workers:
                    state.workers,
                noteSharding:
                    state.noteSharding,
                stealScoreCache:
                    state.stealScoreCache,
                debugMetrics:
                    state.debugMetrics,
                fastNoteOff:
                    state.fastNoteOff,
                validateState:
                    state.validateState,
                softClip:
                    state.softClip
            });
        }

        updateStatus(
            worker
                ? "Reconfiguring SnappySynthV2…"
                : "SnappySynthV2 settings ready");
    }

    // Flips diagnostic counters without reconfiguring the engine.
    // Routing this through configure() reloaded the soundfont, so turning the
    // counters on to take a reading destroyed the very state being measured.
    function setDebugMetrics(enabled) {
        state.debugMetrics = !!enabled;
        if (worker)
            worker.postMessage({
                type: "debugMetrics",
                enabled: state.debugMetrics
            });
    }

    globalThis.WasmidiSnappyBridge = {
        state,
        setDebugMetrics,
        setLocalMidiSource,
        clearLocalMidiSource,
        setLocalVelocityFloor,
        ensureBackend,
        loadSoundfontFile,
        openSoundfont,
        play,
        pause,
        stop,
        seek,
        syncVisualClock,
        schedule,
        scheduleBatch,
        scheduleSysEx,
        scheduleSysExBatch,
        clearSoundfonts,
        setVolume,
        setOverlapGain,
        setGpu,
        getOutput,
        setPrebuffer,
        configure,
        getAudioClock
    };
})();
