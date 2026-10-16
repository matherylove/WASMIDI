/* WASMIDI Audio (HANDOFF sec. 64): the WASMIDI parser worker and WasmiSynth with
 * the same scheduling protocol as the Qt player (mainwindow.cpp), and no piano
 * roll, keyboard or graphs, so audio problems can be isolated. */
(() => {
    "use strict";

    const $ = id => document.getElementById(id);
    const bridge = () => globalThis.WasmidiSnappyBridge;
    const VERSION = (() => {
        try { return new URL(document.currentScript.src).searchParams.get("v") || "dev"; }
        catch (_) { return "dev"; }
    })();
    const STORAGE_KEY = "wasmidiAudio";

    const S = {
        parser: null,
        parserReady: null,
        parserReadyResolve: null,
        loaded: false,
        loading: false,
        midiName: "",
        midiSize: 0,
        info: null,
        duration: 0,
        playing: false,
        paused: false,
        time: 0,
        generation: 0,
        resetPending: false,
        pumpPending: false,
        queued: null,
        scheduledUntil: 0,
        loadT0: 0,
        stage: "",
        stageT0: 0,
        seeking: false,
        configApplied: false
    };

    const settings = Object.assign({
        maxVoices: 1024, workers: 0, blockFrames: 512, numBuffers: 16,
        noteSharding: 0, softClip: true, gpu: false, volume: 100
    }, (() => { try { return JSON.parse(localStorage.getItem(STORAGE_KEY) || "{}"); } catch (_) { return {}; } })());

    function saveSettings() {
        try { localStorage.setItem(STORAGE_KEY, JSON.stringify(settings)); } catch (_) {}
    }

    // ---- UI helpers -------------------------------------------------------
    function log(text, cls) {
        const box = $("log");
        const line = document.createElement("div");
        if (cls) line.className = cls;
        const t = new Date();
        line.textContent = "[" + t.toTimeString().slice(0, 8) + "] " + text;
        box.appendChild(line);
        while (box.childNodes.length > 300) box.removeChild(box.firstChild);
        box.scrollTop = box.scrollHeight;
    }

    function fmtTime(seconds) {
        const s = Math.max(0, Math.floor(seconds || 0));
        const m = Math.floor(s / 60);
        return String(m).padStart(2, "0") + ":" + String(s % 60).padStart(2, "0");
    }

    function fmtMs(ms) { return ms >= 10000 ? (ms / 1000).toFixed(1) + " s" : Math.round(ms) + " ms"; }

    function setStatus(text) { $("status").textContent = text; }

    function setTitle(text) {
        const box = $("marquee");
        box.firstElementChild.textContent = text;
        box.classList.add("still");
        // Scroll only when the text does not fit.
        requestAnimationFrame(() => box.classList.toggle("still", box.firstElementChild.scrollWidth <= box.clientWidth - 12));
    }

    function setState(kind, label) {
        const el = $("state");
        el.className = "state " + kind;
        el.lastElementChild.textContent = label;
    }

    // ---- Parser worker ----------------------------------------------------
    function startParser() {
        if (S.parser) S.parser.terminate();
        S.parser = new Worker("midi-parser-worker.js?v=" + encodeURIComponent(VERSION));
        S.parserReady = new Promise(resolve => { S.parserReadyResolve = resolve; });
        S.parser.onmessage = event => onParserMessage(event.data || {});
        S.parser.onerror = event => log("Parser worker error: " + (event.message || event), "err");
    }

    function closeStage(now) {
        if (S.stage) log(S.stage + ": " + fmtMs(now - S.stageT0));
    }

    function onParserMessage(m) {
        const now = performance.now();
        switch (m.type) {
        case "worker-ready":
            if (S.parserReadyResolve) S.parserReadyResolve();
            return;
        case "progress":
            if (m.stage !== S.stage) {
                closeStage(now);
                S.stage = m.stage;
                S.stageT0 = now;
            }
            setStatus(Math.round(Number(m.percent) || 0) + "% " + m.stage);
            $("meter-load").style.width = Math.max(0, Math.min(100, Number(m.percent) || 0)) + "%";
            return;
        case "result-begin":
            closeStage(now);
            S.stage = "";
            return;
        case "result-end":
            S.parser.postMessage({ type: "doc-info" });
            return;
        case "doc-info":
            S.info = m;
            S.duration = Math.max(0, Number(m.duration) || 0);
            S.loaded = true;
            S.loading = false;
            S.time = 0;
            $("seek").max = String(S.duration);
            $("seek").value = "0";
            $("total").textContent = fmtTime(S.duration);
            $("i-tracks").textContent = String(m.tracks | 0);
            $("i-notes").textContent = Number(m.notes).toLocaleString();
            $("i-ppq").textContent = String(m.ticksPerBeat | 0);
            $("i-duration").textContent = fmtTime(S.duration);
            $("i-format").textContent = String(m.format | 0);
            log("MIDI ready in " + fmtMs(now - S.loadT0) + " (" + Number(m.notes).toLocaleString() +
                " notes, " + (m.tracks | 0) + " tracks)", "ok");
            setStatus("MIDI ready");
            setState("stop", "STOP");
            updateButtons();
            return;
        case "error":
            S.loading = false;
            log("MIDI load failed: " + (m.message || "unknown error"), "err");
            setStatus("MIDI load failed");
            updateButtons();
            return;
        case "runtime-error":
            log("Parser: " + (m.message || "runtime error"), "err");
            S.resetPending = false;
            S.pumpPending = false;
            S.queued = null;
            return;
        case "synth-cursor-reset":
            if ((Number(m.generation) >>> 0) !== S.generation) return;
            S.resetPending = false;
            S.pumpPending = false;
            dispatchQueued();
            return;
        case "synth-pump-deferred":
            if ((Number(m.generation) >>> 0) !== S.generation) return;
            queueFurthest({
                type: "synth-pump", generation: S.generation,
                endTick: Number(m.endTick) || 0, safeUntil: Number(m.safeUntil) || 0,
                velocityFloor: Number(m.velocityFloor) | 0, maxBatches: 8
            });
            S.pumpPending = false;
            S.resetPending = true;
            return;
        case "synth-batch": {
            if ((Number(m.generation) >>> 0) !== S.generation) return;
            const b = bridge();
            if (b && b.scheduleBatch) {
                b.scheduleBatch(
                    new Uint32Array(m.messages || new ArrayBuffer(0)),
                    new Float64Array(m.times || new ArrayBuffer(0)),
                    Number(m.safeUntil) || 0.0,
                    new Uint32Array(m.sysexMeta || new ArrayBuffer(0)),
                    new Uint8Array(m.sysexData || new ArrayBuffer(0)),
                    new Float64Array(m.sysexTimes || new ArrayBuffer(0)));
            }
            if (m.complete) {
                S.pumpPending = false;
                if (!S.resetPending) dispatchQueued();
            }
            return;
        }
        case "synth-more":
            if ((Number(m.generation) >>> 0) !== S.generation) return;
            S.parser.postMessage({
                type: "synth-pump", generation: S.generation,
                endTick: Number(m.endTick) || 0, safeUntil: Number(m.safeUntil) || 0,
                velocityFloor: Number(m.velocityFloor) | 0, maxBatches: 8
            });
            return;
        default:
            return;
        }
    }

    async function loadMidi(file) {
        if (!file) return;
        stop();
        S.loaded = false;
        S.loading = true;
        S.info = null;
        S.midiName = file.name || "MIDI";
        S.midiSize = file.size || 0;
        setTitle(S.midiName);
        $("i-file").textContent = S.midiName;
        $("i-size").textContent = (S.midiSize / 1048576).toFixed(1) + " MB";
        for (const id of ["i-tracks", "i-notes", "i-ppq", "i-duration", "i-format"]) $(id).textContent = "…";
        log("Loading " + S.midiName + " (" + (S.midiSize / 1048576).toFixed(1) + " MB)");
        updateButtons();
        startParser();
        await S.parserReady;
        S.loadT0 = performance.now();
        S.stage = "";
        S.parser.postMessage({ type: "parse", file });
    }

    // ---- WasmiSynth ---------------------------------------------------------
    function readConfigForm() {
        settings.maxVoices = Math.max(1, Number($("c-voices").value) | 0);
        settings.workers = Math.max(0, Number($("c-workers").value) | 0);
        settings.blockFrames = Math.max(64, Number($("c-block").value) | 0);
        settings.numBuffers = Math.max(2, Number($("c-buffers").value) | 0);
        settings.noteSharding = Number($("c-sharding").value) | 0;
        settings.softClip = $("c-softclip").checked;
        saveSettings();
    }

    function writeConfigForm() {
        $("c-voices").value = settings.maxVoices;
        $("c-workers").value = settings.workers;
        $("c-block").value = settings.blockFrames;
        $("c-buffers").value = settings.numBuffers;
        $("c-sharding").value = String(settings.noteSharding);
        $("c-softclip").checked = !!settings.softClip;
        $("volume").value = settings.volume;
        $("gpu").setAttribute("aria-pressed", settings.gpu ? "true" : "false");
        $("gpu").textContent = settings.gpu ? "GPU" : "CPU";
    }

    function applyConfig() {
        const b = bridge();
        if (!b) return;
        b.configure({
            maxVoices: settings.maxVoices,
            blockFrames: settings.blockFrames,
            numBuffers: settings.numBuffers,
            workers: settings.workers,
            noteSharding: settings.noteSharding,
            softClip: settings.softClip
        });
        b.setVolume(settings.volume);
        b.setGpu(!!settings.gpu);
        S.configApplied = true;
    }

    async function ensureSynth() {
        const b = bridge();
        if (!b) throw new Error("snappysynth_bridge.js did not load");
        await b.ensureBackend();
        if (!S.configApplied) applyConfig();
    }

    async function loadSf2(file) {
        if (!file) return;
        try {
            log("Loading SoundFont " + file.name + " (" + (file.size / 1048576).toFixed(1) + " MB)");
            const t0 = performance.now();
            await ensureSynth();
            const ok = await bridge().loadSoundfontFile(file);
            if (ok === false || !bridge().state.soundfontLoaded)
                throw new Error(bridge().state.status || "load failed");
            $("i-sf2").textContent = file.name;
            log("SoundFont ready in " + fmtMs(performance.now() - t0) + " (" +
                (bridge().state.regions | 0) + " regions)", "ok");
            reapplyGpu();
        } catch (error) {
            log("SoundFont failed: " + (error && error.message ? error.message : error), "err");
        }
        updateButtons();
    }

    function reapplyGpu() {
        const b = bridge();
        if (b && settings.gpu) b.setGpu(true);
    }

    // ---- Transport (same protocol as mainwindow.cpp) -----------------------
    function resetSynth(seconds) {
        S.generation = ((S.generation + 1) >>> 0) || 1;
        S.resetPending = true;
        S.pumpPending = false;
        S.queued = null;
        S.scheduledUntil = seconds;
        S.parser.postMessage({ type: "synth-reset", generation: S.generation, seconds });
    }

    // Requests only move forward in time, so the newest queued one is the furthest.
    function queueFurthest(request) {
        S.queued = request;
    }

    function dispatchQueued() {
        if (!S.queued || S.pumpPending || S.resetPending) return;
        const request = S.queued;
        S.queued = null;
        if ((request.generation >>> 0) !== S.generation) return;
        S.pumpPending = true;
        S.parser.postMessage(request);
    }

    function scheduleAhead() {
        const b = bridge();
        if (!S.playing || !b || !S.parser) return;
        const rate = b.state.sampleRate > 0 ? b.state.sampleRate : 44100;
        const blockSeconds = Math.max(1, b.state.blockFrames || settings.blockFrames) / rate;
        const transportSeconds = blockSeconds * Math.max(1, b.state.numBuffers || settings.numBuffers);
        const ahead = Math.min(4, Math.max(2, transportSeconds * 8 + blockSeconds * 4));
        const clock = b.getAudioClock();
        const base = Math.max(S.time, Number.isFinite(clock) && clock >= 0 ? clock : S.time);
        const target = Math.min(S.duration, base + ahead);
        if (target <= S.scheduledUntil + 0.015) return;
        let safeUntil = target;
        if (target >= S.duration - 1e-7) safeUntil += Math.max(0.05, blockSeconds * 2);
        const request = {
            type: "synth-pump", generation: S.generation,
            endSeconds: target, safeUntil, velocityFloor: 0, maxBatches: 8
        };
        S.scheduledUntil = target;
        if (S.resetPending || S.pumpPending) { queueFurthest(request); return; }
        S.pumpPending = true;
        S.parser.postMessage(request);
    }

    async function play() {
        if (!S.loaded) { log("Open a MIDI first", "err"); return; }
        try { await ensureSynth(); } catch (error) { log(String(error.message || error), "err"); return; }
        const b = bridge();
        if (!b.state.soundfontLoaded) { log("Load a SoundFont first", "err"); return; }
        if (S.playing) return;
        if (S.time >= S.duration - 0.05) S.time = 0;
        resetSynth(S.time);
        b.play(S.time, true);
        S.playing = true;
        S.paused = false;
        setState("play", "PLAY");
        updateButtons();
    }

    function pause() {
        const b = bridge();
        if (!S.playing || !b) return;
        const clock = b.getAudioClock();
        if (Number.isFinite(clock) && clock >= 0) S.time = Math.min(S.duration, clock);
        b.pause();
        S.playing = false;
        S.paused = true;
        setState("pause", "PAUSE");
        updateButtons();
    }

    function stop() {
        const b = bridge();
        if (b && (S.playing || S.paused)) b.stop();
        S.playing = false;
        S.paused = false;
        S.time = 0;
        if (S.loaded) setState("stop", "STOP");
        updateButtons();
    }

    function seek(seconds) {
        const t = Math.max(0, Math.min(S.duration, seconds));
        S.time = t;
        if (S.playing) {
            resetSynth(t);
            bridge().seek(t);
        }
    }

    // ---- Frame loop ----------------------------------------------------------
    function tick() {
        const b = bridge();
        if (S.playing && b) {
            const clock = b.getAudioClock();
            if (Number.isFinite(clock) && clock >= 0) S.time = Math.min(S.duration, clock);
            scheduleAhead();
            if (S.duration > 0 && S.time >= S.duration - 1e-3 && clock >= S.duration + 0.3) {
                stop();
                log("End of song");
            }
        }
        $("clock").textContent = fmtTime(S.time);
        if (!S.seeking) $("seek").value = String(S.time);
        if (b) {
            const st = b.state;
            $("r-voices").textContent = String(st.activeVoices | 0);
            $("r-load").textContent = Math.round(Number(st.renderLoadPercent) || 0) + "%";
            $("r-buffer").textContent = Math.round(Number(st.ringFillPercent) || 0) + "%";
            $("r-underruns").textContent = String(st.underruns | 0);
            $("r-dropped").textContent = Number(st.droppedNotes || 0).toLocaleString();
            $("r-steals").textContent = Number(st.steals || 0).toLocaleString();
            $("r-workers").textContent = String(st.workerCount || st.workersParticipating || 0);
            $("r-engine").textContent = settings.gpu ? "GPU" : "CPU";
            $("meter-load").style.width = S.loading ? $("meter-load").style.width
                : Math.max(0, Math.min(100, Number(st.renderLoadPercent) || 0)) + "%";
            $("synth-status").textContent = String(st.status || "").replace(/SnappySynthV2/g, "WasmiSynth");
        }
        requestAnimationFrame(tick);
    }

    function updateButtons() {
        $("play").disabled = !S.loaded;
        $("pause").disabled = !S.playing;
        $("stop").disabled = !(S.playing || S.paused);
        $("restart").disabled = !S.loaded;
        $("seek").disabled = !S.loaded;
        $("apply").disabled = S.playing;
    }

    // ---- Wiring ----------------------------------------------------------------
    function pick(input, handler) {
        input.addEventListener("change", () => {
            const file = input.files && input.files[0];
            input.value = "";
            if (file) handler(file);
        });
    }

    function init() {
        writeConfigForm();
        pick($("midi-input"), loadMidi);
        pick($("sf2-input"), loadSf2);
        $("open-midi").addEventListener("click", () => $("midi-input").click());
        $("open-sf2").addEventListener("click", () => $("sf2-input").click());
        $("play").addEventListener("click", () => { void play(); });
        $("pause").addEventListener("click", pause);
        $("stop").addEventListener("click", stop);
        $("restart").addEventListener("click", () => { seek(0); if (!S.playing) S.time = 0; });
        const seekBar = $("seek");
        seekBar.addEventListener("input", () => { S.seeking = true; $("clock").textContent = fmtTime(Number(seekBar.value)); });
        seekBar.addEventListener("change", () => { S.seeking = false; seek(Number(seekBar.value)); });
        $("volume").addEventListener("input", () => {
            settings.volume = Number($("volume").value) | 0;
            saveSettings();
            const b = bridge();
            if (b) b.setVolume(settings.volume);
        });
        $("gpu").addEventListener("click", () => {
            settings.gpu = !settings.gpu;
            saveSettings();
            writeConfigForm();
            const b = bridge();
            if (b) b.setGpu(settings.gpu);
            log("Engine: " + (settings.gpu ? "GPU (WebGPU) requested" : "CPU"));
        });
        $("apply").addEventListener("click", async () => {
            readConfigForm();
            try {
                await ensureSynth();
                applyConfig();
                log("WasmiSynth reconfigured: " + settings.maxVoices + " voices, " +
                    (settings.workers || "auto") + " workers, block " + settings.blockFrames +
                    ", " + settings.numBuffers + " buffers", "ok");
            } catch (error) {
                log("Configure failed: " + (error.message || error), "err");
            }
        });
        const main = $("main");
        main.addEventListener("dragover", e => { e.preventDefault(); main.classList.add("drag"); });
        main.addEventListener("dragleave", () => main.classList.remove("drag"));
        main.addEventListener("drop", e => {
            e.preventDefault();
            main.classList.remove("drag");
            for (const file of Array.from(e.dataTransfer.files || [])) {
                if (/\.(mid|midi|smf|kar)$/i.test(file.name)) void loadMidi(file);
                else if (/\.(sf2|sfz|sf3)$/i.test(file.name)) void loadSf2(file);
            }
        });
        document.addEventListener("keydown", e => {
            if (e.target && /input|select|textarea/i.test(e.target.tagName)) return;
            if (e.key === " ") { e.preventDefault(); if (S.playing) pause(); else void play(); }
        });
        if (!globalThis.crossOriginIsolated) {
            // coi-serviceworker reloads the page once on the first visit.
            setTimeout(() => {
                if (!globalThis.crossOriginIsolated)
                    log("This page is not cross-origin isolated, so WasmiSynth cannot start its threads. Reload the page.", "err");
            }, 3000);
        }
        setTitle("Open a MIDI and a SoundFont");
        setState("stop", "IDLE");
        updateButtons();
        requestAnimationFrame(tick);
    }

    if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
    else init();
})();
