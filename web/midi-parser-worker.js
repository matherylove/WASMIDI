/* global WasmidiMidiParserCore */

"use strict";

// Pass 13.11.0: SharpMIDI-style mapped-source parser. The browser File remains
// outside the WASM heap; only compact indexes/checkpoints remain resident in
// Memory64, and render/playback data are decoded into bounded pages on demand.
const WASMIDI_MIDI_PARSER_BOOTSTRAP = "13.11.0";
const RESULT_CHUNK_BYTES = 16 * 1024 * 1024;
const SYNTH_EVENT_BATCH_EVENTS = 262144;
const FAST_SOURCE_MIN_BYTES = 256 * 1024 * 1024;
const FAST_SOURCE_DEFAULT_BYTES = 512 * 1024 * 1024;
const FAST_SOURCE_MAX_BYTES = 1024 * 1024 * 1024;

// zstd only when keeping the MIDI raw could exceed the parser heap (16 GiB) or the
// machine's RAM (HANDOFF sec. 54). navigator.deviceMemory is coarse and capped at 8 GiB.
function rawSourceLimitBytes(wholeFileCopied) {
    const GiB = 1024 * 1024 * 1024;
    const heapRoom = 16 * GiB - 2 * GiB;
    const deviceGiB = Number(self.navigator && self.navigator.deviceMemory);
    const ram = Number.isFinite(deviceGiB) && deviceGiB > 0 ? deviceGiB * GiB : 8 * GiB;
    let ramRoom = ram * 0.75;
    // While loading, the contiguous JS copy and the raw store coexist.
    if (wholeFileCopied) ramRoom /= 2;
    return Math.max(0, Math.min(heapRoom, ramRoom));
}

function fastSourceLimitBytes() {
    // MPWGL2 reads the whole MIDI once and then parses a contiguous Uint8Array,
    // which is much faster than repeated Blob.slice/FileReaderSync crossings.
    // Keep that behavior on desktop when memory permits, but retain the mapped
    // fallback for multi-gigabyte files. navigator.deviceMemory is deliberately
    // treated only as a coarse budget hint and allocation failure falls back.
    const deviceGiB = Number(self.navigator && self.navigator.deviceMemory);
    if (!Number.isFinite(deviceGiB) || deviceGiB <= 0)
        return FAST_SOURCE_DEFAULT_BYTES;
    const eighthOfRam = Math.floor(deviceGiB * 1024 * 1024 * 1024 / 8);
    return Math.max(
        FAST_SOURCE_MIN_BYTES,
        Math.min(FAST_SOURCE_MAX_BYTES, eighthOfRam));
}

self.__wasmidiMidiParserStage = "Loading parser core";
self.__wasmidiMidiParserPercent = 14;

// When MainWindow fetches this Worker with cache: no-store and launches it
// from a Blob URL, relative importScripts() would resolve against blob:. Keep
// the real deployment directory in a tiny prelude so the generated parser
// core is always fetched from the current site and current bootstrap version.
const WASMIDI_MIDI_PARSER_BASE_URL = (() => {
    const configured = self.__wasmidiMidiParserBaseUrl;
    if (configured)
        return String(configured);
    try {
        return new URL("./", self.location.href).href;
    } catch (_) {
        return "./";
    }
})();

importScripts(new URL(
    "wasmidi-midi-parser.js?v=" +
        encodeURIComponent(WASMIDI_MIDI_PARSER_BOOTSTRAP),
    WASMIDI_MIDI_PARSER_BASE_URL).href);

let modulePromise = null;
let lastAbortReason = "";
let lastRuntimeError = "";
let pointerBits = 0;

function currentStage() {
    return String(
        self.__wasmidiMidiParserStage ||
        "Initializing parser core");
}

function conciseError(value) {
    if (!value)
        return "";
    if (value instanceof Error)
        return value.message || String(value);
    return String(value);
}

function enrichedCoreError(error) {
    const primary = conciseError(error);
    const detail =
        lastAbortReason ||
        lastRuntimeError ||
        primary ||
        "unknown parser runtime error";

    return new Error(
        "Background MIDI parser failed while " +
        currentStage() +
        ": " + detail);
}

function isMemory64() {
    return pointerBits === 64;
}


function pointerToNumber(value, label) {
    const n = typeof value === "bigint" ? Number(value) : Number(value);
    if (!Number.isSafeInteger(n) || n < 0)
        throw new Error((label || "WASM pointer") + " exceeds JavaScript's exact integer range.");
    return n;
}

function sizeToNumber(value, label) {
    const n = typeof value === "bigint" ? Number(value) : Number(value);
    if (!Number.isSafeInteger(n) || n < 0)
        throw new Error((label || "WASM size") + " exceeds JavaScript's exact integer range.");
    return n;
}


function parserErrorText(Module, fallback) {
    const address = Number(Module._wmp_error_ptr_js());
    const byteLength = Number(Module._wmp_error_size_js());

    if (!Number.isSafeInteger(address) || address < 0 ||
        !Number.isSafeInteger(byteLength) || byteLength < 0 ||
        !address || !byteLength) {
        return String(fallback || "Could not parse MIDI");
    }

    const end = address + byteLength;
    if (!Number.isSafeInteger(end) || end > Module.HEAPU8.length)
        return String(fallback || "Could not parse MIDI");

    try {
        return new TextDecoder("utf-8", { fatal: false }).decode(
            Module.HEAPU8.subarray(address, end));
    } catch (_) {
        return String(fallback || "Could not parse MIDI");
    }
}

function getModule() {
    if (!modulePromise) {
        lastAbortReason = "";
        lastRuntimeError = "";
        self.__wasmidiMidiParserStage = "Initializing Memory64 parser core";
        self.__wasmidiMidiParserPercent = 15;

        modulePromise = Promise.resolve().then(() =>
            WasmidiMidiParserCore({
                noInitialRun: true,
                noExitRuntime: true,
                onAbort(reason) {
                    lastAbortReason =
                        conciseError(reason) ||
                        "Emscripten aborted without a reason";
                    console.error(
                        "[WASMIDI MIDI parser] abort during " +
                        currentStage() + ":",
                        lastAbortReason);
                },
                print(text) {
                    if (text)
                        console.log("[WASMIDI MIDI parser]", text);
                },
                printErr(text) {
                    if (!text)
                        return;
                    lastRuntimeError = String(text);
                    console.error(
                        "[WASMIDI MIDI parser runtime]",
                        text);
                }
            })
        ).then(Module => {
            if (!Module ||
                typeof Module._wmp_parse_file_js !== "function" ||
                typeof Module._wmp_pack !== "function" ||
                typeof Module._wmp_result_ptr_js !== "function" ||
                typeof Module._wmp_result_size_js !== "function" ||
                typeof Module._wmp_release_result !== "function" ||
                typeof Module._wmp_error_ptr_js !== "function" ||
                typeof Module._wmp_error_size_js !== "function" ||
                typeof Module._wmp_build_visual_page_js !== "function" ||
                typeof Module._wmp_visual_page_ptr_js !== "function" ||
                typeof Module._wmp_visual_page_count_js !== "function" ||
                typeof Module._wmp_build_key_snapshot_js !== "function" ||
                typeof Module._wmp_build_live_snapshot_js !== "function" ||
                typeof Module._wmp_key_snapshot_ptr_js !== "function" ||
                typeof Module._wmp_key_snapshot_word_count_js !== "function" ||
                typeof Module._wmp_reset_event_cursor_js !== "function" ||
                typeof Module._wmp_build_event_batch_js !== "function" ||
                typeof Module._wmp_event_batch_ptr_js !== "function" ||
                typeof Module._wmp_event_batch_count_js !== "function" ||
                typeof Module._wmp_event_batch_complete_js !== "function" ||
                typeof Module._wmp_event_batch_next_tick_js !== "function" ||
                typeof Module._wmp_event_batch_has_next_tick_js !== "function" ||
                typeof Module._wmp_build_historical_sysex_js !== "function" ||
                typeof Module._wmp_sysex_batch_event_ptr_js !== "function" ||
                typeof Module._wmp_sysex_batch_event_count_js !== "function" ||
                typeof Module._wmp_sysex_batch_data_ptr_js !== "function" ||
                typeof Module._wmp_sysex_batch_data_size_js !== "function" ||
                typeof Module._wmp_reset_render_cursor_js !== "function" ||
                typeof Module._wmp_build_render_sweep_js !== "function" ||
                typeof Module._wmp_render_append_ptr_js !== "function" ||
                typeof Module._wmp_render_append_count_js !== "function" ||
                typeof Module._wmp_render_append_base_js !== "function" ||
                typeof Module._wmp_render_close_ptr_js !== "function" ||
                typeof Module._wmp_render_close_count_js !== "function" ||
                typeof Module._wmp_render_batch_complete_js !== "function" ||
                typeof Module._wmp_render_batch_next_tick_js !== "function" ||
                typeof Module._wmp_render_batch_has_next_tick_js !== "function" ||
                typeof Module._wmp_tick_to_seconds_js !== "function" ||
                typeof Module._wmp_pointer_bits !== "function" ||
                !Module.HEAPU8) {
                throw new Error(
                    "parser WASM initialized without its complete exported API");
            }

            pointerBits = Number(Module._wmp_pointer_bits()) | 0;
            if (pointerBits !== 64) {
                throw new Error(
                    "Pass 13.11.0 parser was built without Memory64 (pointer width " +
                    pointerBits + ").");
            }

            self.__wasmidiMidiParserStage = "Memory64 parser core ready";
            self.__wasmidiMidiParserPercent = 15;
            return Module;
        }).catch(error => {
            modulePromise = null;
            pointerBits = 0;
            throw enrichedCoreError(error);
        });
    }
    return modulePromise;
}

function progress(percent, stage) {
    const bounded = Math.max(0, Math.min(100, percent | 0));
    const text = String(stage || "Loading MIDI");
    self.__wasmidiMidiParserStage = text;
    self.__wasmidiMidiParserPercent = bounded;
    postMessage({
        type: "progress",
        percent: bounded,
        stage: text
    });
}

async function streamPackedResult(Module, file, resultPtr, resultSize) {
    const base = pointerToNumber(resultPtr, "Parsed-document pointer");
    const total = sizeToNumber(resultSize, "Parsed-document size");

    if (!base || total <= 0)
        throw new Error("MIDI parser returned an empty document.");

    postMessage({
        type: "result-begin",
        name: String(file.name || "browser.mid"),
        size: total,
        parserPointerBits: pointerBits
    });

    for (let offset = 0; offset < total; offset += RESULT_CHUNK_BYTES) {
        const end = Math.min(total, offset + RESULT_CHUNK_BYTES);
        const chunk = Module.HEAPU8.slice(base + offset, base + end);

        postMessage({
            type: "result-chunk",
            offset,
            data: chunk.buffer
        }, [chunk.buffer]);

        if ((offset & ((64 * 1024 * 1024) - 1)) === 0)
            await new Promise(resolve => setTimeout(resolve, 0));
    }

    postMessage({
        type: "result-end",
        name: String(file.name || "browser.mid"),
        size: total
    });
}

// Explicit source-version handshake. MainWindow does not send the File until
// this arrives, so a stale Worker can never silently execute an older loading
// path after a GitHub Pages deployment.
postMessage({
    type: "worker-ready",
    bootstrap: WASMIDI_MIDI_PARSER_BOOTSTRAP,
    pagedSource: true,
    mappedStore: true
});


let mappedFileReady = false;
let visualBuiltGeneration = 0;
let visualBuiltSpan = 0;
const visualBuiltPages = new Set();
const visualQueuedPages = new Set();
let visualBuildRunning = false;
let visualLatestFirst = 0;
let visualLatestCount = 0;
let visualLatestCurrent = 0;
let synthPumpActive = false;
let synthPriorityUntil = 0;
// Seek/reset transaction id for the independent SnappySynth event cursor.
// A dense pump yields between batches, so a later seek can be processed while
// the old async pump is still suspended. Generation checks prevent that old
// pump from touching the newly reset cursor or publishing stale events.
let synthGeneration = 0;
let synthHistoricalSelectorMessages = null;
let synthHistoricalSysExMeta = null;
let synthHistoricalSysExData = null;
let synthHistoricalSelectorTime = 0.0;

// SharpMIDI-raylib renderer port. There are no visual pages. The Memory64
// worker walks the same compact event stream with its own persistent cursor and
// publishes only ring appends + EndTick patches to the Qt/WebGL2 renderer.
// Resident Memory64 traversal is much cheaper than the JS/WebGL handoff. Walk
// a larger source chunk per yield so several future viewports can be ready in
// advance instead of reaching a dense transition just-in-time.
// Renderer preprocessing shares this Worker with the audio event cursor and
// live keyboard/stat state.  Large resident sweeps are efficient in aggregate
// but a 500K-event synchronous slice can occupy the Worker for a full display
// frame on wasm.  Stay finely time-sliced while safely ahead, and only use the
// larger source window when the prepared frontier falls inside the emergency
// watermark supplied by the renderer.
const SHARP_RENDER_SOURCE_BUDGET = 131072;
const SHARP_RENDER_URGENT_SOURCE_BUDGET = 262144;
let sharpRenderGeneration = 0;
let sharpRenderTarget = 0;
let sharpRenderUrgentThrough = 0;
let sharpRenderSafeThrough = 0;
let sharpRenderRunning = false;
let sharpRenderReady = false;
let sharpRenderPerTrack = false;
let sharpRenderCompletedTarget = 0;

function copyWasmBytes(Module, address, byteLength) {
    const base = pointerToNumber(address, "mapped result pointer");
    const size = sizeToNumber(byteLength, "mapped result size");
    if (!base || size < 0 || base + size > Module.HEAPU8.length)
        throw new Error("Mapped result points outside the Memory64 heap.");
    return Module.HEAPU8.slice(base, base + size);
}

function visualPagePriority(page) {
    const current = visualLatestCurrent;
    // Two history pages can still intersect retained/long-note geometry. Build
    // the nearest history tile first, then current and forward pages in strict
    // order. Once the current neighborhood exists this naturally becomes a
    // SharpMIDI-style forward sweep instead of repeatedly restarting at "now".
    if (current > 0 && page === current - 1) return 0;
    if (page === current) return 1;
    if (page === current + 1) return 2;
    if (page > current) return 3 + (page - current);
    return 1000 + (current - page);
}

function pageStillWanted(page) {
    return page >= visualLatestFirst &&
        page < visualLatestFirst + visualLatestCount;
}

async function drainVisualPages() {
    if (visualBuildRunning)
        return;
    visualBuildRunning = true;

    try {
        const Module = await getModule();
        while (mappedFileReady && visualQueuedPages.size > 0) {
            // Audio event delivery wins over speculative visual look-ahead. A
            // single visual screen can be expensive on a Black MIDI, so give
            // the next synth-pump task a short grace window before starting it.
            if (synthPumpActive || performance.now() < synthPriorityUntil) {
                await new Promise(resolve => setTimeout(resolve, 1));
                continue;
            }

            let pageIndex = null;
            let bestPriority = Number.POSITIVE_INFINITY;
            for (const page of visualQueuedPages) {
                if (!pageStillWanted(page))
                    continue;
                const priority = visualPagePriority(page);
                if (priority < bestPriority) {
                    bestPriority = priority;
                    pageIndex = page;
                }
            }

            if (pageIndex == null) {
                visualQueuedPages.clear();
                break;
            }

            visualQueuedPages.delete(pageIndex);
            const generation = visualBuiltGeneration;
            const span = visualBuiltSpan;
            const start = pageIndex * span;
            const end = Math.min(
                0xffffffff,
                start + Math.max(0, span - 1));

            if (!Module._wmp_build_visual_page_js(start, end)) {
                throw new Error(parserErrorText(
                    Module,
                    "Could not build mapped visual page"));
            }

            const noteCount = sizeToNumber(
                Module._wmp_visual_page_count_js(),
                "visual page count");
            const bytes = noteCount
                ? copyWasmBytes(
                    Module,
                    Module._wmp_visual_page_ptr_js(),
                    noteCount * 12)
                : new Uint8Array(0);

            // A seek/span change can only be observed after the synchronous C++
            // page build yields back to JS. Check the transaction again before
            // publishing so an obsolete page is never installed into a new
            // cache generation.
            if (generation === visualBuiltGeneration &&
                span === visualBuiltSpan &&
                pageStillWanted(pageIndex)) {
                visualBuiltPages.add(pageIndex);
                postMessage({
                    type: "visual-page",
                    generation,
                    spanTicks: span,
                    pageIndex,
                    sourceCount: noteCount,
                    difficulty: noteCount,
                    data: bytes.buffer
                }, [bytes.buffer]);
            }

            // One complete screen is the largest uninterrupted visual job. Yield
            // after every page so synth-reset/synth-pump messages get priority
            // before speculative future screens continue filling the 64-screen
            // horizon. Crucially, the remaining pages stay queued across normal
            // forward motion instead of being cancelled and rebuilt.
            await new Promise(resolve => setTimeout(resolve, 0));
        }
    } finally {
        visualBuildRunning = false;
        if (mappedFileReady && visualQueuedPages.size > 0)
            setTimeout(() => { void drainVisualPages(); }, 0);
    }
}

async function buildVisualPages(message) {
    await getModule();
    if (!mappedFileReady)
        return;

    const generation = Number(message.generation) >>> 0;
    const span = Math.max(1, Number(message.spanTicks) >>> 0);
    const first = Number(message.firstPage) >>> 0;
    const count = Math.max(0, Math.min(68, Number(message.count) >>> 0));
    const current = Number(message.currentPage) >>> 0;
    const missingLo = Number(message.missingLo) >>> 0;
    const missingHi = Number(message.missingHi) >>> 0;
    const missingTop = Number(message.missingTop) >>> 0;

    const transactionChanged =
        generation !== visualBuiltGeneration ||
        span !== visualBuiltSpan;

    if (transactionChanged) {
        visualBuiltGeneration = generation;
        visualBuiltSpan = span;
        visualBuiltPages.clear();
        visualQueuedPages.clear();
    }

    visualLatestFirst = first;
    visualLatestCount = count;
    visualLatestCurrent = current;

    // Visible/history pages remain resident in Qt. Worker bookkeeping only
    // tracks the current rolling horizon and may forget old source results.
    for (const page of visualBuiltPages) {
        if (!pageStillWanted(page))
            visualBuiltPages.delete(page);
    }
    for (const page of visualQueuedPages) {
        if (!pageStillWanted(page))
            visualQueuedPages.delete(page);
    }

    const requested = i =>
        i < 32
            ? ((missingLo >>> i) & 1) !== 0
            : i < 64
                ? ((missingHi >>> (i - 32)) & 1) !== 0
                : ((missingTop >>> (i - 64)) & 1) !== 0;

    for (let i = 0; i < count; ++i) {
        if (requested(i))
            visualQueuedPages.add(first + i);
    }

    // Do not await the whole 64-page drain from the message handler. Keeping
    // this as a persistent background queue lets audio/key requests run between
    // individual screen builds and prevents a new prime from cancelling work.
    void drainVisualPages();
}

async function drainSharpRenderer() {
    if (sharpRenderRunning)
        return;
    sharpRenderRunning = true;

    try {
        const Module = await getModule();
        while (mappedFileReady && sharpRenderReady &&
               sharpRenderCompletedTarget < sharpRenderTarget) {
            // Keep SnappySynth responsive without demoting the renderer to the
            // old page-at-the-last-second model. One bounded source sweep is
            // followed by an event-loop yield.
            // A visible frame never waits for WasmiSynth (HANDOFF sec. 48).
            const visibleFrameMissing = sharpRenderSafeThrough < sharpRenderUrgentThrough;
            if (!visibleFrameMissing &&
                (synthPumpActive || performance.now() < synthPriorityUntil)) {
                await new Promise(resolve => setTimeout(resolve, 0));
                continue;
            }

            const generation = sharpRenderGeneration;
            const target = sharpRenderTarget >>> 0;
            const sourceBudget = sharpRenderSafeThrough < sharpRenderUrgentThrough
                ? SHARP_RENDER_URGENT_SOURCE_BUDGET
                : SHARP_RENDER_SOURCE_BUDGET;
            if (!Module._wmp_build_render_sweep_js(
                    target, sourceBudget)) {
                throw new Error(parserErrorText(
                    Module,
                    "Could not sweep SharpMIDI renderer events"));
            }

            const appendCount = sizeToNumber(
                Module._wmp_render_append_count_js(),
                "SharpMIDI append count");
            const closeCount = sizeToNumber(
                Module._wmp_render_close_count_js(),
                "SharpMIDI close count");
            const appendBase = sizeToNumber(
                Module._wmp_render_append_base_js(),
                "SharpMIDI append base");
            const complete = !!Module._wmp_render_batch_complete_js();
            const hasNextTick = !!Module._wmp_render_batch_has_next_tick_js();
            const nextTick = hasNextTick
                ? sizeToNumber(
                    Module._wmp_render_batch_next_tick_js(),
                    "SharpMIDI next tick")
                : 0;

            const appendBytes = appendCount
                ? copyWasmBytes(
                    Module,
                    Module._wmp_render_append_ptr_js(),
                    appendCount * 12)
                : new Uint8Array(0);
            const closeBytes = closeCount
                ? copyWasmBytes(
                    Module,
                    Module._wmp_render_close_ptr_js(),
                    closeCount * 8)
                : new Uint8Array(0);

            if (generation !== sharpRenderGeneration)
                continue;

            // For partial batches this is an EXCLUSIVE tick boundary: every
            // event strictly before nextTick is complete, while nextTick itself
            // may still be split across Worker yields (including tick 0). For
            // complete batches it is the inclusive requested target.
            let safeThrough = target;
            if (!complete && hasNextTick)
                safeThrough = nextTick;
            sharpRenderSafeThrough = complete
                ? Math.max(sharpRenderSafeThrough, safeThrough)
                : Math.max(sharpRenderSafeThrough, safeThrough > 0 ? safeThrough - 1 : 0);

            postMessage({
                type: "sharp-render-delta",
                generation,
                appendBase,
                appends: appendBytes.buffer,
                closes: closeBytes.buffer,
                safeThrough,
                complete
            }, [appendBytes.buffer, closeBytes.buffer]);

            if (complete) {
                sharpRenderCompletedTarget = Math.max(
                    sharpRenderCompletedTarget, target);
                if (target === sharpRenderTarget)
                    break;
            }

            // Always return to the event loop after one bounded resident sweep.
            // Pass 13.8 allowed urgent render bursts to monopolize this shared
            // mapped Worker for up to ~6 ms at a time; that directly delayed
            // synth admission and live-state requests.  Prefetch depth is now
            // maintained by watermarks on the main thread, not by long bursts.
            await new Promise(resolve => setTimeout(resolve, 0));
        }
    } finally {
        sharpRenderRunning = false;
        if (mappedFileReady && sharpRenderReady &&
            sharpRenderCompletedTarget < sharpRenderTarget)
            setTimeout(() => { void drainSharpRenderer(); }, 0);
    }
}

async function requestSharpRenderer(message) {
    const Module = await getModule();
    if (!mappedFileReady)
        return;

    const generation = Number(message.generation) >>> 0;
    const startTick = Math.max(
        0, Math.min(0xffffffff, Number(message.startTick) || 0));
    const endTick = Math.max(
        startTick, Math.min(0xffffffff, Number(message.endTick) || 0));
    const perTrack = !!message.perTrack;
    const reset = !!message.reset ||
        generation !== sharpRenderGeneration ||
        perTrack !== sharpRenderPerTrack;

    if (reset) {
        sharpRenderGeneration = generation;
        sharpRenderPerTrack = perTrack;
        sharpRenderTarget = endTick >>> 0;
        sharpRenderUrgentThrough = Math.min(
            sharpRenderTarget,
            Math.max(startTick, Number(message.urgentThrough) || startTick)) >>> 0;
        sharpRenderCompletedTarget = startTick > 0 ? (startTick - 1) >>> 0 : 0;
        sharpRenderSafeThrough = sharpRenderCompletedTarget;
        sharpRenderReady = true;
        Module._wmp_reset_render_cursor_js(startTick, perTrack ? 1 : 0);
        postMessage({
            type: "sharp-render-reset",
            generation,
            startTick: startTick >>> 0
        });
    } else if (endTick > sharpRenderTarget) {
        sharpRenderTarget = endTick >>> 0;
    }

    sharpRenderUrgentThrough = Math.min(
        sharpRenderTarget,
        Math.max(sharpRenderUrgentThrough,
            Math.max(startTick, Number(message.urgentThrough) || startTick))) >>> 0;

    void drainSharpRenderer();
}

async function buildKeyState(message) {
    const Module = await getModule();
    if (!mappedFileReady)
        return;
    const tick = Math.max(0, Math.min(0xffffffff, Number(message.tick) || 0));
    const npsStartTick = Math.max(0, Math.min(tick, Number(message.npsStartTick) || 0));
    const ccStartTick = Math.max(0, Math.min(tick, Number(message.ccStartTick) || 0));
    if (!Module._wmp_build_live_snapshot_js(
            tick, npsStartTick, ccStartTick, message.reset ? 1 : 0))
        throw new Error(parserErrorText(Module, "Could not build mapped live state"));
    const words = sizeToNumber(Module._wmp_key_snapshot_word_count_js(), "live snapshot words");
    const bytes = words
        ? copyWasmBytes(Module, Module._wmp_key_snapshot_ptr_js(), words * 4)
        : new Uint8Array(0);
    postMessage({
        type: "live-state",
        tick,
        requestId: Number(message.requestId) >>> 0,
        generation: Number(message.generation) >>> 0,
        data: bytes.buffer
    }, [bytes.buffer]);
}

async function resetSynthCursor(message) {
    const Module = await getModule();
    const tick = Math.max(0, Math.min(0xffffffff, Number(message.tick) || 0));
    synthGeneration = Number(message.generation) >>> 0;
    Module._wmp_reset_event_cursor_js(tick);
    synthHistoricalSelectorMessages = null;
    synthHistoricalSysExMeta = null;
    synthHistoricalSysExData = null;
    synthHistoricalSelectorTime = Math.max(
        0, Number(Module._wmp_tick_to_seconds_js(tick)) || 0);

    // Restore mapped GM/GS/XG state without copying every SysEx into Qt's
    // wasm32 document. Payloads were retained beside the Memory64 event store
    // during the mandatory parse pass, so a seek never returns to Blob I/O.
    if (!Module._wmp_build_historical_sysex_js(tick))
        throw new Error(parserErrorText(Module, "Could not restore mapped SysEx state"));
    const historyCount = sizeToNumber(
        Module._wmp_sysex_batch_event_count_js(), "historical SysEx count");
    const historyBytes = sizeToNumber(
        Module._wmp_sysex_batch_data_size_js(), "historical SysEx bytes");
    if (historyCount > 0 && historyBytes > 0) {
        const metaBytes = copyWasmBytes(
            Module, Module._wmp_sysex_batch_event_ptr_js(), historyCount * 12);
        synthHistoricalSysExMeta = new Uint32Array(metaBytes.buffer);
        synthHistoricalSysExData = copyWasmBytes(
            Module, Module._wmp_sysex_batch_data_ptr_js(), historyBytes);
    }

    // Native SnappySynth keeps Bank Select MSB/LSB pending and commits them
    // only on Program Change.  A mapped seek used to restore SysEx but not this
    // channel selector state, which made multi-preset/multi-bank SF2 files fall
    // back to bank/program 0 until the next explicit Program Change.  The C++
    // mapped store folds only sparse B0/C0/E0 state events from the mandatory
    // parse pass and emits the minimal sequence that recreates both the applied
    // bank/program and any still-pending Bank Select values.
    if (!Module._wmp_build_historical_selector_state_js(tick))
        throw new Error(parserErrorText(Module, "Could not restore mapped bank/program state"));
    const selectorCount = sizeToNumber(
        Module._wmp_historical_selector_state_count_js(),
        "historical selector-state count");
    if (selectorCount > 0) {
        const selectorPtr = pointerToNumber(
            Module._wmp_historical_selector_state_ptr_js(),
            "historical selector-state pointer");
        const restored = new Uint32Array(selectorCount);
        for (let i = 0; i < selectorCount; ++i) {
            // EventWord = { uint32 tick, uint32 packed }. Only the packed MIDI
            // word is needed by SnappySynth; every restore event is scheduled
            // at the seek point after historical reset SysEx has been applied.
            restored[i] = Module.HEAPU32[(selectorPtr + i * 8 + 4) >>> 2] >>> 0;
        }
        synthHistoricalSelectorMessages = restored;
    }
    postMessage({
        type: "synth-cursor-reset",
        generation: synthGeneration,
        tick
    });
}

async function pumpSynthWindow(message) {
    const Module = await getModule();
    if (!mappedFileReady)
        return;
    const generation = Number(message.generation) >>> 0;
    if (generation !== synthGeneration) {
        // Defensive protocol response. The main thread normally waits for the
        // synth-cursor-reset ACK before posting a pump, but never silently drop
        // a same-session request: a silent return can leave the caller's
        // synthPumpPending latch set forever and produce valid all-zero PCM.
        postMessage({
            type: "synth-pump-deferred",
            generation,
            workerGeneration: synthGeneration >>> 0,
            endTick: Math.max(0, Math.min(0xffffffff, Number(message.endTick) || 0)),
            safeUntil: Math.max(0, Number(message.safeUntil) || 0),
            velocityFloor: Math.max(0, Math.min(127, Number(message.velocityFloor) | 0))
        });
        return;
    }
    const endTick = Math.max(0, Math.min(0xffffffff, Number(message.endTick) || 0));
    const velocityFloor = Math.max(0, Math.min(127, Number(message.velocityFloor) | 0));
    const safeUntil = Math.max(0, Number(message.safeUntil) || 0);
    const maxBatches = Math.max(1, Math.min(64, Number(message.maxBatches) | 0 || 8));

    for (let batchIndex = 0; batchIndex < maxBatches; ++batchIndex) {
        if (generation !== synthGeneration)
            return;

        // Only the synchronous resident-event scan is an audio-critical section.
        // Clear the flag before yielding so the faithful SharpMIDI renderer may
        // consume one of its own bounded sweeps between synth batches. The old
        // code held synthPumpActive across the entire multi-batch async request,
        // which starved rendering for seconds in dense passages even though both
        // consumers were already walking resident Memory64 arrays.
        synthPumpActive = true;
        let built = false;
        try {
            built = !!Module._wmp_build_event_batch_js(
                endTick, SYNTH_EVENT_BATCH_EVENTS);
        } finally {
            synthPumpActive = false;
        }
        if (generation !== synthGeneration)
            return;
        if (!built)
            throw new Error(parserErrorText(Module, "Could not build mapped synth batch"));
        const count = sizeToNumber(Module._wmp_event_batch_count_js(), "event batch count");
        const complete = !!Module._wmp_event_batch_complete_js();
        const hasNextTick = !!Module._wmp_event_batch_has_next_tick_js();
        const nextTick = hasNextTick
            ? sizeToNumber(Module._wmp_event_batch_next_tick_js(), "event batch next tick")
            : 0;
        const ptr = pointerToNumber(Module._wmp_event_batch_ptr_js(), "event batch pointer");
        const timePtr = pointerToNumber(
            Module._wmp_event_batch_time_ptr_js(), "event batch time pointer");
        const sysExCount = sizeToNumber(
            Module._wmp_sysex_batch_event_count_js(), "SysEx batch count");
        const sysExByteCount = sizeToNumber(
            Module._wmp_sysex_batch_data_size_js(), "SysEx batch bytes");
        // The parser core converts the complete batch to seconds in one native
        // pass. This removes one JS/WASM call per distinct MIDI tick.
        const sourceTimes = count
            ? new Float64Array(Module.HEAPU8.buffer, timePtr, count)
            : null;
        const messageScratch = new Uint32Array(count);
        const timeScratch = new Float64Array(count);
        let outCount = 0;

        let i = 0;
        while (i < count) {
            const byte = ptr + i * 8;
            const tick = Module.HEAPU32[byte >>> 2] >>> 0;
            const packed = Module.HEAPU32[(byte >>> 2) + 1] >>> 0;
            const status = packed & 255;
            const command = status & 0xf0;
            const data1 = (packed >>> 8) & 127;
            const data2 = (packed >>> 16) & 127;
            const eventTime = sourceTimes ? sourceTimes[i] : 0.0;

            if (command === 0x90 && data2 !== 0 && data2 < velocityFloor) {
                ++i;
                continue;
            }

            let messageWord = status | (data1 << 8) | (data2 << 16);
            if (command === 0x90 && data2 !== 0) {
                // The mapped C++ cursor already folds consecutive identical
                // NoteOns into SnappySynthV2's native high-byte overlap count.
                // Preserve that count here and opportunistically join adjacent
                // compressed words when their combined stack still fits in one
                // byte. This is lossless and never crosses controller/event
                // ordering boundaries.
                let stackCount = ((packed >>> 24) & 255) + 1;
                while (i + 1 < count && stackCount < 256) {
                    const nextByte = ptr + (i + 1) * 8;
                    const nextEventTick = Module.HEAPU32[nextByte >>> 2] >>> 0;
                    const nextPacked = Module.HEAPU32[(nextByte >>> 2) + 1] >>> 0;
                    if (nextEventTick !== tick ||
                        (nextPacked & 0x00ffffff) !== (packed & 0x00ffffff)) {
                        break;
                    }
                    const nextStackCount = ((nextPacked >>> 24) & 255) + 1;
                    if (stackCount + nextStackCount > 256)
                        break;
                    stackCount += nextStackCount;
                    ++i;
                }
                if (stackCount > 1)
                    messageWord |= (stackCount - 1) << 24;
            }

            messageScratch[outCount] = messageWord >>> 0;
            timeScratch[outCount] = eventTime;
            ++outCount;
            ++i;
        }

        let msgArray =
            outCount === count
                ? messageScratch
                : messageScratch.slice(0, outCount);
        let timeArray =
            outCount === count
                ? timeScratch
                : timeScratch.slice(0, outCount);

        // Selector restoration belongs to the first real schedule batch of the
        // reset generation.  Do not publish it as a standalone schedule ACK:
        // doing so can let the AudioWorklet begin consuming PCM before the
        // mapped producer has supplied actual MIDI coverage.  Prepending here
        // preserves startup/seek gating and guarantees historical SysEx arrives
        // first, then bank/program/pitch state, then source events at/after tick.
        if (synthHistoricalSelectorMessages &&
            synthHistoricalSelectorMessages.length > 0) {
            const prefix = synthHistoricalSelectorMessages;
            const mergedMessages = new Uint32Array(prefix.length + msgArray.length);
            const mergedTimes = new Float64Array(prefix.length + timeArray.length);
            mergedMessages.set(prefix, 0);
            mergedMessages.set(msgArray, prefix.length);
            mergedTimes.fill(synthHistoricalSelectorTime, 0, prefix.length);
            mergedTimes.set(timeArray, prefix.length);
            msgArray = mergedMessages;
            timeArray = mergedTimes;
            synthHistoricalSelectorMessages = null;
        }
        let sysExMetaBytes = sysExCount
            ? copyWasmBytes(
                Module, Module._wmp_sysex_batch_event_ptr_js(), sysExCount * 12)
            : new Uint8Array(0);
        let sysExData = sysExByteCount
            ? copyWasmBytes(
                Module, Module._wmp_sysex_batch_data_ptr_js(), sysExByteCount)
            : new Uint8Array(0);
        let sysExTimes = new Float64Array(sysExCount);
        if (sysExCount) {
            const words = new Uint32Array(
                sysExMetaBytes.buffer, sysExMetaBytes.byteOffset, sysExCount * 3);
            let lastSysExTick = 0xffffffff;
            let lastSysExTime = 0.0;
            for (let sx = 0; sx < sysExCount; ++sx) {
                const sxTick = words[sx * 3] >>> 0;
                if (sxTick !== lastSysExTick) {
                    lastSysExTick = sxTick;
                    lastSysExTime = Math.max(
                        0, Number(Module._wmp_tick_to_seconds_js(sxTick)) || 0);
                }
                sysExTimes[sx] = lastSysExTime;
            }
        }

        // Historical reset/tuning SysEx and restored Bank/Program state must
        // cross into SnappySynth in the same producer transaction.  A separate
        // history message can be observed by the realtime worker before the
        // first short-message batch, creating subtle startup/seek ordering
        // differences on full GM/GS soundfonts.  Merge history into the first
        // batch and schedule it at the seek sample, before the restored selector
        // prefix and source events.
        if (synthHistoricalSysExMeta && synthHistoricalSysExMeta.length > 0 &&
            synthHistoricalSysExData && synthHistoricalSysExData.length > 0) {
            const prefixMeta = synthHistoricalSysExMeta;
            const prefixData = synthHistoricalSysExData;
            const currentMeta = new Uint32Array(
                sysExMetaBytes.buffer, sysExMetaBytes.byteOffset, sysExCount * 3);
            const mergedMeta = new Uint32Array(prefixMeta.length + currentMeta.length);
            const mergedData = new Uint8Array(prefixData.length + sysExData.length);
            const prefixCount = Math.floor(prefixMeta.length / 3);
            const mergedTimes = new Float64Array(prefixCount + sysExTimes.length);
            mergedData.set(prefixData, 0);
            mergedData.set(sysExData, prefixData.length);
            mergedTimes.fill(synthHistoricalSelectorTime, 0, prefixCount);
            mergedTimes.set(sysExTimes, prefixCount);
            mergedMeta.set(prefixMeta, 0);
            for (let sx = 0; sx < sysExCount; ++sx) {
                const src = sx * 3;
                const dst = prefixMeta.length + src;
                mergedMeta[dst] = currentMeta[src] >>> 0;
                mergedMeta[dst + 1] = (currentMeta[src + 1] + prefixData.length) >>> 0;
                mergedMeta[dst + 2] = currentMeta[src + 2] >>> 0;
            }
            sysExMetaBytes = new Uint8Array(mergedMeta.buffer);
            sysExData = mergedData;
            sysExTimes = mergedTimes;
            synthHistoricalSysExMeta = null;
            synthHistoricalSysExData = null;
        }
        // Do not hold audio at the seek point until the entire look-ahead
        // horizon is decoded. If another event remains, all events strictly
        // before its tick have already been transferred and are safe to render.
        // For a batch split inside one same-tick crashpoint this naturally stays
        // at that tick until every event at the tick has arrived.
        let batchSafeUntil = 0.0;
        if (complete) {
            // Coverage is defined by the first event we have NOT transferred,
            // not by the arbitrary scheduler request edge. If the next source
            // event lies beyond endTick, there are provably no MIDI events in
            // that gap, so the realtime synth may render safely up to that next
            // event boundary without falling silent at a one-second horizon.
            batchSafeUntil = safeUntil;
            if (hasNextTick) {
                batchSafeUntil = Math.max(
                    batchSafeUntil,
                    Math.max(0.0, Number(Module._wmp_tick_to_seconds_js(nextTick)) || 0.0));
            }
        } else if (hasNextTick) {
            batchSafeUntil = Math.min(
                safeUntil,
                Math.max(0.0, Number(Module._wmp_tick_to_seconds_js(nextTick)) || 0.0));
        }

        postMessage({
            type: "synth-batch",
            generation,
            messages: msgArray.buffer,
            times: timeArray.buffer,
            sysexMeta: sysExMetaBytes.buffer,
            sysexData: sysExData.buffer,
            sysexTimes: sysExTimes.buffer,
            safeUntil: batchSafeUntil,
            complete
        }, [
            msgArray.buffer,
            timeArray.buffer,
            sysExMetaBytes.buffer,
            sysExData.buffer,
            sysExTimes.buffer
        ]);

        if (complete)
            return;

        // The parser/render/live-state consumers share one Memory64 Worker.
        // Reserve the next short event-loop slice for the continuation of a
        // dense synth pump so speculative geometry can never reduce audio
        // coverage/NPS throughput.
        // One short grace slice is enough to let a continuation request reach the
        // front of the event loop without locking speculative renderer work out
        // for multiple milliseconds after every 262K-event batch.
        synthPriorityUntil = performance.now() + 0.5;
        await new Promise(resolve => setTimeout(resolve, 0));
    }

    // More data remains. Main thread will request another pump without moving
    // safeUntil, so SnappySynth can never render beyond a partial dense tick.
    if (generation === synthGeneration) {
        postMessage({
            type: "synth-more",
            generation,
            endTick,
            safeUntil,
            velocityFloor
        });
    }
}

self.onmessage = async event => {
    const message = event.data || {};
    try {
        if (message.type === "sharp-render") {
            await requestSharpRenderer(message);
            return;
        }
        if (message.type === "visual-prime") {
            await buildVisualPages(message);
            return;
        }
        if (message.type === "key-state") {
            await buildKeyState(message);
            return;
        }
        if (message.type === "synth-reset") {
            synthPriorityUntil = performance.now() + 1;
            await resetSynthCursor(message);
            return;
        }
        if (message.type === "synth-pump") {
            try {
                await pumpSynthWindow(message);
            } finally {
                synthPumpActive = false;
                synthPriorityUntil = performance.now() + 1;
                if (visualQueuedPages.size > 0)
                    setTimeout(() => { void drainVisualPages(); }, 0);
            }
            return;
        }
    } catch (error) {
        postMessage({
            type: "runtime-error",
            requestType: String(message.type || ''),
            requestId: Number(message.requestId) >>> 0,
            message: error && error.message ? error.message : String(error)
        });
        return;
    }

    if (message.type !== "parse" || !message.file)
        return;

    let Module = null;
    let resultOwned = false;

    try {
        const file = message.file;
        const total = Number(file.size);
        if (!Number.isSafeInteger(total) || total < 0)
            throw new Error("Invalid MIDI file size.");

        Module = await getModule();

        // Keep the browser File outside the wasm heap. The mapped path requests
        // only bounded 8 MiB windows through FileReaderSync as its source scan
        // moves through track ranges, so multi-gigabyte files never require a
        // raw allocation proportional to file size.
        self.__wasmidiMidiParserFile = file;
        self.__wasmidiMidiParserFileReader = null;
        self.__wasmidiMidiParserReadError = "";
        self.__wasmidiMidiParserWholeFile = null;

        // MPWGL2 was fast partly because it parsed from one contiguous buffer.
        // Keep that fast path for ordinary/large desktop MIDIs while retaining
        // the paged File-backed path for truly huge Black MIDI sources.
        const fastSourceBytes = fastSourceLimitBytes();
        if (total > 0 && total <= fastSourceBytes &&
            typeof FileReaderSync === "function") {
            self.__wasmidiMidiParserStage = "Reading MIDI into fast source cache";
            progress(1, "Reading MIDI into fast source cache");
            try {
                const reader = new FileReaderSync();
                self.__wasmidiMidiParserWholeFile =
                    new Uint8Array(reader.readAsArrayBuffer(file));
            } catch (error) {
                // Allocation/read failure is not fatal; fall back to the bounded
                // File-backed path used for giant sources.
                self.__wasmidiMidiParserWholeFile = null;
                console.warn("[WASMIDI MIDI parser] contiguous source cache unavailable; using paged reads", error);
            }
        }

        self.__wasmidiMidiParserStage = self.__wasmidiMidiParserWholeFile
            ? "Parsing MIDI from contiguous source cache"
            : "Parsing MIDI from bounded file windows";
        progress(2, self.__wasmidiMidiParserWholeFile
            ? "Parsing MIDI from memory"
            : "Opening MIDI as a paged source");

        if (typeof Module._wmp_set_raw_source_limit_js === "function")
            Module._wmp_set_raw_source_limit_js(
                rawSourceLimitBytes(!!self.__wasmidiMidiParserWholeFile));
        const ok = Module._wmp_parse_file_js(total);
        if (!ok) {
            const parserText = parserErrorText(Module, "Could not parse MIDI");
            const readText = String(self.__wasmidiMidiParserReadError || "");
            throw new Error(
                readText && !parserText.includes(readText)
                    ? parserText + ": " + readText
                    : parserText);
        }

        // Pass 13 keeps the File as the browser equivalent of SharpMIDI's
        // MemoryMappedFile. Only metadata is packed to Qt; render/playback
        // pages continue to read bounded windows from this File after loading.
        mappedFileReady = true;
        // The BPFA store keeps its own copy of the source (raw or zstd) and never reads
        // the file after indexing: drop the contiguous JS copy (HANDOFF sec. 53).
        self.__wasmidiMidiParserWholeFile = null;

        self.__wasmidiMidiParserStage = "Packing mapped MIDI metadata";
        const packed = Module._wmp_pack();
        if (!packed) {
            throw new Error(
                parserErrorText(Module, "Could not pack parsed MIDI"));
        }
        resultOwned = true;

        progress(95, "Streaming mapped MIDI metadata to player");

        await streamPackedResult(
            Module,
            file,
            Module._wmp_result_ptr_js(),
            Module._wmp_result_size_js());

        // Every chunk has been copied to a transferable buffer. Drop the giant
        // parser-side capacity immediately; the next MIDI starts from a clean
        // memory budget.
        Module._wmp_release_result();
        resultOwned = false;

        // result-end is the ownership handoff to the Qt player. Do not post a
        // lower progress value after it: the main thread installs the metadata
        // synchronously and may already have published 100%/Ready by the time
        // the next Worker message is dispatched. A late 96% message would
        // reopen the loading overlay forever even though the MIDI is ready.
    } catch (error) {
        const failure =
            (lastAbortReason || lastRuntimeError)
                ? enrichedCoreError(error)
                : error;

        postMessage({
            type: "error",
            message: failure && failure.message
                ? failure.message
                : String(failure || "Could not parse MIDI")
        });
    } finally {
        if (!mappedFileReady) {
            self.__wasmidiMidiParserFile = null;
            self.__wasmidiMidiParserFileReader = null;
            self.__wasmidiMidiParserWholeFile = null;
        }
        self.__wasmidiMidiParserReadAt = null;
        if (resultOwned && Module) {
            try { Module._wmp_release_result(); } catch (_) {}
        }
    }
};
