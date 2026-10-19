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
        noteSharding: 0, softClip: true, stackGain: true, gpu: false, volume: 100
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
        const b = bridge();
        if (b && b.clearLocalMidiSource) b.clearLocalMidiSource();
        S.feedDirect = false;
        // The page query reaches the Worker for its overrides (?helpers=N, ?rawlimit=MiB).
        const query = new URLSearchParams(location.search);
        query.set("v", VERSION);
        S.parser = new Worker("midi-parser-worker.js?" + query.toString());
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
            S.parser.postMessage({ type: "text-events" });
            S.parser.postMessage({ type: "density" });
            {
                // The parser answers the synth directly (HANDOFF sec. 70); the pump
                // below stays for ?synthsource=relay.
                const b = bridge();
                if (b && b.setLocalMidiSource) b.setLocalMidiSource(null, S.parser);
                S.feedDirect = !!(b && b.state && b.state.localSynthReady);
            }
            return;
        case "text-events":
            Lyrics.setEvents(m.events || []);
            return;
        case "density":
            Graphs.setDensity(m.starts, m.counts);
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
        settings.stackGain = $("c-stackgain").checked;
        saveSettings();
    }

    function writeConfigForm() {
        $("c-voices").value = settings.maxVoices;
        $("c-workers").value = settings.workers;
        $("c-block").value = settings.blockFrames;
        $("c-buffers").value = settings.numBuffers;
        $("c-sharding").value = String(settings.noteSharding);
        $("c-softclip").checked = !!settings.softClip;
        $("c-stackgain").checked = !!settings.stackGain;
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
        b.setOverlapGain(!!settings.stackGain);
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

    async function loadSfzFolder(files) {
        if (!files || !files.length) return;
        try {
            const choices = bridge().sfzChoices(files);
            if (!choices.length) throw new Error("no .sfz file in that folder");
            log("Loading SFZ " + choices[0] + " (" + files.length + " files" +
                (choices.length > 1 ? ", " + choices.length + " .sfz found" : "") + ")");
            const t0 = performance.now();
            await ensureSynth();
            const ok = await bridge().loadSoundfontFolder(files, choices[0]);
            if (ok === false || !bridge().state.soundfontLoaded)
                throw new Error(bridge().state.status || "load failed");
            $("i-sf2").textContent = choices[0].split("/").pop();
            log("SFZ ready in " + fmtMs(performance.now() - t0) + " (" +
                (bridge().state.regions | 0) + " regions)", "ok");
            reapplyGpu();
        } catch (error) {
            log("SFZ failed: " + (error && error.message ? error.message : error), "err");
        }
        updateButtons();
    }

    async function droppedFiles(items) {
        const out = [];
        async function walk(entry, prefix) {
            if (entry.isFile) {
                const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
                out.push({ path: prefix + entry.name, file });
            } else if (entry.isDirectory) {
                const reader = entry.createReader();
                for (;;) {
                    const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
                    if (!batch.length) break;
                    for (const child of batch) await walk(child, prefix + entry.name + "/");
                }
            }
        }
        const entries = Array.from(items || []).map(item => item.webkitGetAsEntry && item.webkitGetAsEntry()).filter(Boolean);
        for (const entry of entries) await walk(entry, "");
        return out;
    }

    function reapplyGpu() {
        const b = bridge();
        if (b && settings.gpu) b.setGpu(true);
    }

    // ---- Transport (same protocol as mainwindow.cpp) -----------------------
    function resetSynth(seconds) {
        if (S.feedDirect) return;
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
        if (!S.playing || !b || !S.parser || S.feedDirect) return;
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
        Visualizer.attach();
        kick();
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
        Graphs.reset();
        kick();
        if (S.playing) {
            resetSynth(t);
            bridge().seek(t);
        }
    }

    // ---- Windows: drag, minimize, restore, persisted layout ------------------
    const Windows = (() => {
        const LAYOUT_KEY = "wasmidiAudioLayout";
        const defaults = {
            main: { x: 24, y: 24, open: true },
            vis: { x: 24, y: 300, open: false },
            info: { x: 24, y: 520, open: true },
            lyrics: { x: 610, y: 24, open: false },
            graphs: { x: 610, y: 350, open: false }
        };
        let layout = {};
        try { layout = JSON.parse(localStorage.getItem(LAYOUT_KEY) || "{}"); } catch (_) { layout = {}; }
        const el = key => document.querySelector('[data-win="' + key + '"]');
        const flowLayout = () => window.matchMedia("(max-width: 900px)").matches;
        let z = 10;

        function save() { try { localStorage.setItem(LAYOUT_KEY, JSON.stringify(layout)); } catch (_) {} }

        function place(key) {
            const w = el(key), L = layout[key];
            if (!w || flowLayout()) return;
            const maxX = Math.max(0, document.documentElement.clientWidth - 80);
            w.style.left = Math.max(0, Math.min(maxX, L.x)) + "px";
            w.style.top = Math.max(0, L.y) + "px";
        }

        function isOpen(key) { return !!(layout[key] && layout[key].open); }

        function setOpen(key, open) {
            if (key === "main") return;
            layout[key].open = !!open;
            el(key).hidden = !open;
            const btn = document.querySelector('[data-toggle="' + key + '"]');
            if (btn) btn.setAttribute("aria-pressed", open ? "true" : "false");
            save();
            onToggle(key, open);
        }

        function focus(w) { w.style.zIndex = String(++z); }

        function makeDraggable(w, key) {
            const bar = w.querySelector(".win-title");
            let start = null;
            bar.addEventListener("pointerdown", e => {
                if (flowLayout() || e.button !== 0 || e.target.closest(".cap-btn")) return;
                focus(w);
                start = { x: e.clientX, y: e.clientY, left: w.offsetLeft, top: w.offsetTop };
                bar.setPointerCapture(e.pointerId);
                w.classList.add("dragging");
            });
            bar.addEventListener("pointermove", e => {
                if (!start) return;
                layout[key].x = Math.round(start.left + e.clientX - start.x);
                layout[key].y = Math.round(start.top + e.clientY - start.y);
                place(key);
            });
            const end = () => { if (start) { start = null; w.classList.remove("dragging"); save(); } };
            bar.addEventListener("pointerup", end);
            bar.addEventListener("pointercancel", end);
            w.addEventListener("pointerdown", () => focus(w), true);
        }

        function init() {
            for (const key of Object.keys(defaults)) {
                layout[key] = Object.assign({}, defaults[key], layout[key] || {});
                const w = el(key);
                if (!w) continue;
                makeDraggable(w, key);
                place(key);
                if (key !== "main") {
                    w.hidden = !layout[key].open;
                    for (const b of w.querySelectorAll("[data-min]"))
                        b.addEventListener("click", () => setOpen(key, false));
                }
            }
            layout.main.open = true;
            for (const btn of document.querySelectorAll("[data-toggle]")) {
                const key = btn.dataset.toggle;
                btn.setAttribute("aria-pressed", isOpen(key) ? "true" : "false");
                btn.addEventListener("click", () => setOpen(key, !isOpen(key)));
            }
            window.addEventListener("resize", () => { for (const key of Object.keys(layout)) place(key); });
            for (const key of Object.keys(defaults)) if (key !== "main" && isOpen(key)) onToggle(key, true);
        }

        function onToggle(key, open) {
            if (key === "vis") { if (open) { Visualizer.attach(); Visualizer.resize(); } else Visualizer.detach(); }
            if (key === "graphs") { if (open) { Graphs.reset(); Graphs.resize(); } }
            if (key === "lyrics" && open) Lyrics.update(S.time, true);
            if (key === "info" && open) updateReadouts();
            if (open) kick();
        }

        return { init, isOpen, setOpen };
    })();

    // ---- Lyrics and markers ---------------------------------------------------
    const Lyrics = (() => {
        let lines = [];      // { t, end, parts: [{ t, text }] }
        let markers = [];    // { t, text }
        let lineIndex = -2, sungParts = -1, markerIndex = -2;

        function clean(text) { return text.replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/g, ""); }

        function setEvents(events) {
            lines = [];
            markers = [];
            const lyricKind = events.some(e => e.kind === 5) ? 5 : 1; // .kar files use text events
            let line = null;
            const startLine = t => { line = { t, parts: [] }; lines.push(line); };
            for (const e of events) {
                if (e.kind === 6) { markers.push({ t: e.t, text: clean(e.text).trim() }); continue; }
                if (e.kind !== lyricKind) continue;
                let text = e.text;
                if (lyricKind === 1 && /^@/.test(text)) continue; // .kar header tags
                let breakBefore = false;
                if (/^[\\/]/.test(text)) { breakBefore = true; text = text.slice(1); }
                if (breakBefore || !line) startLine(e.t);
                const breakAfter = /[\r\n]$/.test(text);
                text = clean(text.replace(/[\r\n]+/g, ""));
                if (text) line.parts.push({ t: e.t, text });
                if (breakAfter) line = null;
            }
            lines = lines.filter(l => l.parts.length);
            for (let i = 0; i < lines.length; ++i) lines[i].end = i + 1 < lines.length ? lines[i + 1].t : Infinity;
            renderMarkers();
            lineIndex = -2; markerIndex = -2; sungParts = -1;
            update(S.time, true);
            if ((lines.length || markers.length) && !Windows.isOpen("lyrics"))
                log("This MIDI has " + lines.length + " lyric lines and " + markers.length + " markers (LYR)");
        }

        function find(list, t) {
            let lo = 0, hi = list.length - 1, ans = -1;
            while (lo <= hi) { const mid = (lo + hi) >> 1; if (list[mid].t <= t) { ans = mid; lo = mid + 1; } else hi = mid - 1; }
            return ans;
        }

        function renderMarkers() {
            const box = $("lyr-markers");
            box.textContent = "";
            if (!markers.length) { box.innerHTML = '<div class="lyr-empty">No markers</div>'; return; }
            const frag = document.createDocumentFragment();
            markers.forEach((m, i) => {
                const b = document.createElement("button");
                b.type = "button";
                b.dataset.i = String(i);
                const tm = document.createElement("b");
                tm.textContent = fmtTime(m.t);
                b.append(tm, document.createTextNode(m.text || "(marker)"));
                b.addEventListener("click", () => { seek(m.t); if (!S.playing) { S.time = m.t; update(m.t, true); } });
                frag.appendChild(b);
            });
            box.appendChild(frag);
        }

        function update(t, force) {
            const li = find(lines, t);
            let sung = -1;
            if (li >= 0) { const parts = lines[li].parts; sung = find(parts, t); }
            if (force || li !== lineIndex || sung !== sungParts) {
                lineIndex = li; sungParts = sung;
                const now = $("lyr-now");
                now.textContent = "";
                if (!lines.length) now.innerHTML = '<span class="lyr-empty">No lyrics</span>';
                else if (li >= 0) {
                    const parts = lines[li].parts;
                    const a = document.createElement("span"); a.className = "sung";
                    a.textContent = parts.slice(0, sung + 1).map(p => p.text).join("");
                    now.append(a, document.createTextNode(parts.slice(sung + 1).map(p => p.text).join("")));
                }
                const next = lines[li + 1];
                $("lyr-next").textContent = next ? next.parts.map(p => p.text).join("") : "";
            }
            const mi = find(markers, t);
            if (force || mi !== markerIndex) {
                const box = $("lyr-markers");
                const old = box.querySelector("button.now");
                if (old) old.classList.remove("now");
                markerIndex = mi;
                if (mi >= 0) {
                    const b = box.querySelector('button[data-i="' + mi + '"]');
                    if (b) { b.classList.add("now"); if (b.offsetTop < box.scrollTop || b.offsetTop > box.scrollTop + box.clientHeight - 20) box.scrollTop = b.offsetTop - 40; }
                }
            }
        }

        function init() {}
        return { init, setEvents, update };
    })();

    // ---- Graphs (Winamp-style strip charts) --------------------------------------
    const Graphs = (() => {
        const SERIES = [
            { key: "nps", label: "NPS", color: "#f0abfc" },
            { key: "voices", label: "VOICES", color: "#a78bfa" },
            { key: "load", label: "LOAD %", color: "#ffc371" },
            { key: "buffer", label: "BUFFER %", color: "#7cf29a" }
        ];
        const STEP_MS = 50;       // one column every 50 ms (20 Hz); pure scroll-blit
        const COL = 2;            // pixels per column
        let canvas = null, ctx = null, w = 0, h = 0, dpr = 1;
        let starts = null, counts = null;
        let last = 0, maxima = { nps: 1, voices: 1, load: 100, buffer: 100 };
        let prev = null;

        function setDensity(s, c) { starts = s; counts = c; reset(); }

        function nps(t) {
            if (!starts || !counts || !counts.length) return 0;
            const a = t - 0.125, b = t + 0.125;
            let lo = 0, hi = counts.length - 1;
            while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (starts[mid] <= a) lo = mid; else hi = mid - 1; }
            let sum = 0;
            for (let i = lo; i < counts.length && starts[i] < b; ++i) {
                const s0 = starts[i], s1 = starts[i + 1];
                const span = Math.max(1e-6, s1 - s0);
                const overlap = Math.max(0, Math.min(b, s1) - Math.max(a, s0));
                sum += counts[i] * overlap / span;
            }
            return sum / 0.25;
        }

        function resize() {
            canvas = $("graphs-canvas");
            if (!canvas) return;
            dpr = Math.min(2, window.devicePixelRatio || 1);
            const r = canvas.getBoundingClientRect();
            w = Math.max(1, Math.round(r.width * dpr));
            h = Math.max(1, Math.round(r.height * dpr));
            canvas.width = w; canvas.height = h;
            ctx = canvas.getContext("2d", { alpha: false });
            reset();
        }

        function reset() {
            prev = null;
            if (!ctx) return;
            ctx.fillStyle = "#04031a";
            ctx.fillRect(0, 0, w, h);
            drawFrame();
            ctx.font = Math.round(13 * dpr) + "px VT323, monospace";
            SERIES.forEach((sd, i) => { ctx.fillStyle = sd.color; ctx.fillText(sd.label, 4 * dpr, rowRect(i).y + 13 * dpr); });
        }

        function rowRect(i) {
            const rh = Math.floor(h / SERIES.length);
            return { y: i * rh, h: rh };
        }

        function drawFrame() {
            ctx.strokeStyle = "#1a1640";
            ctx.lineWidth = 1;
            for (let i = 1; i < SERIES.length; ++i) {
                const r = rowRect(i);
                ctx.beginPath(); ctx.moveTo(0, r.y + 0.5); ctx.lineTo(w, r.y + 0.5); ctx.stroke();
            }
        }

        function sample(time) {
            const b = bridge(), st = b ? b.state : {};
            return {
                nps: nps(time),
                voices: Number(st.activeVoices) || 0,
                load: Number(st.renderLoadPercent) || 0,
                buffer: Number(st.ringFillPercent) || 0
            };
        }

        function update(now, time, playing) {
            if (!ctx) resize();
            if (!ctx || !playing || now - last < STEP_MS) return;
            const steps = Math.min(8, Math.floor((now - last) / STEP_MS)) || 1;
            last = now;
            const v = sample(time);
            const b = bridge();
            maxima.voices = Math.max(maxima.voices, b ? (b.state.maxVoices || 1) : 1);
            maxima.nps = Math.max(maxima.nps * 0.999, v.nps, 1);
            const dx = COL * steps * dpr;
            ctx.drawImage(canvas, -dx, 0);
            ctx.fillStyle = "#04031a";
            ctx.fillRect(w - dx, 0, dx, h);
            drawFrame();
            ctx.font = Math.round(13 * dpr) + "px VT323, monospace";
            SERIES.forEach((sd, i) => {
                const r = rowRect(i);
                const value = v[sd.key];
                const y = r.y + r.h - 2 * dpr - (r.h - 18 * dpr) * Math.min(1, value / maxima[sd.key]);
                ctx.strokeStyle = sd.color;
                ctx.lineWidth = 1.5 * dpr;
                ctx.beginPath();
                ctx.moveTo(w - dx - 1, prev ? prev[i] : y);
                ctx.lineTo(w - 1, y);
                ctx.stroke();
                ctx.fillStyle = "#04031a";
                ctx.fillRect(0, r.y + 1, 150 * dpr, 15 * dpr);
                ctx.fillStyle = sd.color;
                ctx.fillText(sd.label + " " + Math.round(value).toLocaleString(), 4 * dpr, r.y + 13 * dpr);
                (prev || (prev = []))[i] = y;
            });
        }

        return { setDensity, update, reset, resize };
    })();

    // ---- Visualizer: WebGL2 mirrored spectrum ------------------------------------
    const Visualizer = (() => {
        const BARS = 72;
        let analyser = null, freq = null, connected = false;
        let gl = null, prog = null, vao = null, heightBuf = null, canvas = null, uLoc = {};
        const heights = new Float32Array(BARS * 2); // bars, then peaks
        const peakVel = new Float32Array(BARS);
        let bins = null, lastNow = 0, energy = 0;

        function attach() {
            const b = bridge();
            const out = b && b.getOutput ? b.getOutput() : null;
            if (!out || !out.context || !out.node || !Windows.isOpen("vis")) return;
            if (!analyser || analyser.context !== out.context) {
                analyser = out.context.createAnalyser();
                analyser.fftSize = 4096;
                analyser.smoothingTimeConstant = 0.55;
                analyser.minDecibels = -92;
                analyser.maxDecibels = -18;
                freq = new Uint8Array(analyser.frequencyBinCount);
                bins = null;
                connected = false;
            }
            if (!connected) { try { out.node.connect(analyser); connected = true; } catch (_) {} }
        }

        function detach() {
            if (analyser && connected) {
                const out = bridge() && bridge().getOutput ? bridge().getOutput() : null;
                try { if (out && out.node) out.node.disconnect(analyser); } catch (_) {}
            }
            connected = false; // the analyser does no audio-thread work while disconnected
        }

        function makeBins() {
            const rate = analyser.context.sampleRate, n = analyser.frequencyBinCount;
            bins = new Uint16Array(BARS + 1);
            const f0 = 30, f1 = Math.min(18000, rate / 2);
            for (let i = 0; i <= BARS; ++i) {
                const f = f0 * Math.pow(f1 / f0, i / BARS);
                bins[i] = Math.min(n - 1, Math.max(1, Math.round(f / (rate / 2) * n)));
            }
        }

        function initGl() {
            canvas = $("vis-canvas");
            gl = canvas.getContext("webgl2", { antialias: false, alpha: false, preserveDrawingBuffer: false, powerPreference: "low-power" });
            if (!gl) return false;
            const vs = `#version 300 es
in float aHeight;
uniform float uBars;
uniform float uGap;
uniform int uPeaks;
uniform float uSign;
out float vY;
const vec2 C[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                          vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
void main() {
  vec2 c = C[gl_VertexID];
  float bar = float(gl_InstanceID);
  float x = mix(bar, bar + 1.0 - uGap, c.x) / uBars;
  float h = clamp(aHeight, 0.0, 1.0) * 0.94;
  if (uPeaks == 1 && h < 0.004) { vY = 0.0; gl_Position = vec4(2.0, 2.0, 0.0, 1.0); return; }
  float y = uPeaks == 0 ? mix(-h, h, c.y) : uSign * mix(h + 0.012, h + 0.035, c.y);
  vY = y;
  gl_Position = vec4(x * 2.0 - 1.0, y, 0.0, 1.0);
}`;
            const fs = `#version 300 es
precision mediump float;
precision highp int;
in float vY;
uniform int uPeaks;
out vec4 color;
void main() {
  float a = abs(vY);
  vec3 lo = vec3(0.42, 0.28, 0.85), mid = vec3(0.94, 0.67, 0.99), hi = vec3(1.0, 0.97, 1.0);
  vec3 c = a < 0.65 ? mix(lo, mid, a / 0.65) : mix(mid, hi, (a - 0.65) / 0.35);
  if (uPeaks == 1) c = vec3(1.0, 0.95, 1.0);
  color = vec4(c * (vY < 0.0 ? 0.38 : 1.0), 1.0);
}`;
            const sh = (type, src) => { const o = gl.createShader(type); gl.shaderSource(o, src); gl.compileShader(o); if (!gl.getShaderParameter(o, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(o)); return o; };
            prog = gl.createProgram();
            gl.attachShader(prog, sh(gl.VERTEX_SHADER, vs));
            gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, fs));
            gl.linkProgram(prog);
            if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog));
            vao = gl.createVertexArray();
            gl.bindVertexArray(vao);
            heightBuf = gl.createBuffer();
            gl.bindBuffer(gl.ARRAY_BUFFER, heightBuf);
            gl.bufferData(gl.ARRAY_BUFFER, heights.byteLength, gl.DYNAMIC_DRAW);
            const loc = gl.getAttribLocation(prog, "aHeight");
            gl.enableVertexAttribArray(loc);
            gl.vertexAttribPointer(loc, 1, gl.FLOAT, false, 0, 0);
            gl.vertexAttribDivisor(loc, 1);
            uLoc = { bars: gl.getUniformLocation(prog, "uBars"), gap: gl.getUniformLocation(prog, "uGap"),
                     peaks: gl.getUniformLocation(prog, "uPeaks"), sign: gl.getUniformLocation(prog, "uSign"),
                     height: loc };
            canvas.addEventListener("webglcontextlost", e => { e.preventDefault(); gl = null; });
            return true;
        }

        function resize() {
            canvas = $("vis-canvas");
            if (!canvas) return;
            const dpr = Math.min(2, window.devicePixelRatio || 1);
            const r = canvas.getBoundingClientRect();
            canvas.width = Math.max(1, Math.round(r.width * dpr));
            canvas.height = Math.max(1, Math.round(r.height * dpr));
            if (gl) gl.viewport(0, 0, canvas.width, canvas.height);
        }

        function active() { return energy > 0.002; }

        function update(now, playing) {
            if (!gl && !initGl()) return;
            if (canvas.width <= 1) resize();
            const dt = Math.min(0.1, lastNow ? (now - lastNow) / 1000 : 0.016);
            lastNow = now;
            if (playing && analyser && connected) {
                if (!bins) makeBins();
                analyser.getByteFrequencyData(freq);
                for (let i = 0; i < BARS; ++i) {
                    let m = 0;
                    for (let k = bins[i]; k <= bins[i + 1]; ++k) if (freq[k] > m) m = freq[k];
                    heights[i] = m / 255;
                }
            } else {
                for (let i = 0; i < BARS; ++i) heights[i] = Math.max(0, heights[i] - dt * 2.5);
            }
            energy = 0;
            for (let i = 0; i < BARS; ++i) {
                const p = BARS + i;
                if (heights[i] >= heights[p]) { heights[p] = heights[i]; peakVel[i] = 0; }
                else { peakVel[i] += dt * 1.8; heights[p] = Math.max(0, heights[p] - peakVel[i] * dt); }
                energy += heights[i] + heights[p];
            }
            gl.useProgram(prog);
            gl.bindVertexArray(vao);
            gl.bindBuffer(gl.ARRAY_BUFFER, heightBuf);
            gl.bufferSubData(gl.ARRAY_BUFFER, 0, heights);
            gl.clearColor(0.016, 0.012, 0.102, 1);
            gl.clear(gl.COLOR_BUFFER_BIT);
            gl.uniform1f(uLoc.bars, BARS);
            gl.uniform1f(uLoc.gap, 0.22);
            gl.uniform1i(uLoc.peaks, 0);
            gl.uniform1f(uLoc.sign, 1);
            gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, BARS);
            // Peak caps (mirrored): same instanced quad, heights from the second half.
            gl.vertexAttribPointer(uLoc.height, 1, gl.FLOAT, false, 0, BARS * 4);
            gl.uniform1i(uLoc.peaks, 1);
            gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, BARS);
            gl.uniform1f(uLoc.sign, -1);
            gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, BARS);
            gl.vertexAttribPointer(uLoc.height, 1, gl.FLOAT, false, 0, 0);
        }

        function debugFeed(values) { for (let i = 0; i < BARS; ++i) heights[i] = values[i % values.length]; }
        return { attach, detach, update, resize, active, debugFeed };
    })();

    globalThis.__wasmidiAudioTest = { Lyrics, Graphs, Visualizer, Windows };

    // ---- Frame loop ----------------------------------------------------------
    // rAF runs only while something animates (playback, or the visualizer decaying);
    // otherwise a 4 Hz timer keeps the readouts current. Minimized windows do no work.
    let rafId = 0;
    let idleTimer = 0;
    let lastReadouts = 0;

    function wantFrames() {
        return S.playing || (Windows.isOpen("vis") && Visualizer.active());
    }

    function kick() {
        if (!rafId && !document.hidden) rafId = requestAnimationFrame(frame);
    }

    function transportTick() {
        const b = bridge();
        if (!S.playing || !b) return;
        const clock = b.getAudioClock();
        if (Number.isFinite(clock) && clock >= 0) S.time = Math.min(S.duration, clock);
        scheduleAhead();
        if (S.duration > 0 && S.time >= S.duration - 1e-3 && clock >= S.duration + 0.3) {
            stop();
            log("End of song");
        }
    }

    function frame(now) {
        rafId = 0;
        transportTick();
        $("clock").textContent = fmtTime(S.time);
        if (!S.seeking) $("seek").value = String(S.time);
        if (now - lastReadouts > 100) { lastReadouts = now; updateReadouts(); }
        if (Windows.isOpen("lyrics")) Lyrics.update(S.time);
        if (Windows.isOpen("graphs")) Graphs.update(now, S.time, S.playing);
        if (Windows.isOpen("vis")) Visualizer.update(now, S.playing);
        if (wantFrames()) kick();
    }

    function updateReadouts() {
        const b = bridge();
        if (!b) return;
        const st = b.state;
        $("r-voices").textContent = String(st.activeVoices | 0);
        $("r-load").textContent = Math.round(Number(st.renderLoadPercent) || 0) + "%";
        $("r-buffer").textContent = Math.round(Number(st.ringFillPercent) || 0) + "%";
        $("r-underruns").textContent = String(st.underruns | 0);
        if (Windows.isOpen("info")) {
            $("r-dropped").textContent = Number(st.droppedNotes || 0).toLocaleString();
            $("r-steals").textContent = Number(st.steals || 0).toLocaleString();
            $("r-workers").textContent = String(st.workerCount || st.workersParticipating || 0);
            $("r-engine").textContent = settings.gpu ? "GPU (" + String(st.gpuStatus || "off") + ")" : "CPU";
        }
        if (!S.loading) $("meter-load").style.width = Math.max(0, Math.min(100, Number(st.renderLoadPercent) || 0)) + "%";
        $("synth-status").textContent = String(st.status || "").replace(/SnappySynthV2/g, "WasmiSynth");
    }

    // A hidden page keeps playing (the feed and the render live in Workers) but draws
    // nothing; this timer only keeps the transport going.
    function idleTick() {
        if (document.hidden) { transportTick(); return; }
        if (!rafId) {
            updateReadouts();
            if (Windows.isOpen("lyrics")) Lyrics.update(S.time);
        }
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
        $("sfz-input").addEventListener("change", () => {
            const files = Array.from($("sfz-input").files || []);
            $("sfz-input").value = "";
            void loadSfzFolder(files);
        });
        $("open-sfz").addEventListener("click", () => $("sfz-input").click());
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
        const main = $("w-main");
        main.addEventListener("dragover", e => { e.preventDefault(); main.classList.add("drag"); });
        main.addEventListener("dragleave", () => main.classList.remove("drag"));
        main.addEventListener("drop", async e => {
            e.preventDefault();
            main.classList.remove("drag");
            const files = Array.from(e.dataTransfer.files || []);
            const items = Array.from(e.dataTransfer.items || []);
            const hasFolder = items.some(item => { const en = item.webkitGetAsEntry && item.webkitGetAsEntry(); return en && en.isDirectory; });
            for (const file of files) {
                if (/\.(mid|midi|smf|kar)$/i.test(file.name)) void loadMidi(file);
                else if (/\.(sf2|sf3)$/i.test(file.name)) void loadSf2(file);
            }
            if (hasFolder) void loadSfzFolder(await droppedFiles(items));
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
        Windows.init();
        Lyrics.init();
        idleTimer = setInterval(idleTick, 250);
        document.addEventListener("visibilitychange", () => { if (!document.hidden && wantFrames()) kick(); });
        kick();
    }

    if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
    else init();
})();
