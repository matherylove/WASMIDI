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

// Page overrides (HANDOFF sec. 71): ?helpers=N decode helpers, ?rawlimit=MiB raw source
// limit, ?visualstream=0 roll and keyboard without the visual stream.
function pageOverride(name) {
    let query = self.__wasmidiPageQuery;
    if (query == null) {
        try { query = self.location.search; } catch (_) { query = ""; }
    }
    const value = new URLSearchParams(query || "").get(name);
    return value !== null && value !== "" && Number.isFinite(Number(value)) ? Number(value) : null;
}

// zstd only when keeping the MIDI raw could exceed the parser heap (16 GiB) or the
// machine's RAM (HANDOFF sec. 54). navigator.deviceMemory is coarse and capped at 8 GiB.
function rawSourceLimitBytes(wholeFileCopied) {
    const forced = pageOverride("rawlimit");
    if (forced !== null) return Math.max(0, forced) * 1024 * 1024;
    const GiB = 1024 * 1024 * 1024;
    const heapRoom = 16 * GiB - 2 * GiB;
    const deviceGiB = Number(self.navigator && self.navigator.deviceMemory);
    const ram = Number.isFinite(deviceGiB) && deviceGiB > 0 ? deviceGiB * GiB : 8 * GiB;
    let ramRoom = ram * 0.75;
    // While loading, the contiguous JS copy and the raw store coexist.
    if (wholeFileCopied) ramRoom /= 2;
    return Math.max(0, Math.min(heapRoom, ramRoom));
}

// Parallel load (HANDOFF sec. 60): helper Workers index contiguous track ranges and
// this Worker merges their partial indexes in track order.
const PARALLEL_MIN_BYTES = 128 * 1024 * 1024;
const PARALLEL_MAX_HELPERS = 12;

async function parallelParse(Module, file, total, compressed) {
    const cores = Number(self.navigator && self.navigator.hardwareConcurrency) || 1;
    if (total < PARALLEL_MIN_BYTES || cores < 4 ||
        typeof Module._wmp_track_table_js !== "function" ||
        typeof Module._wmp_merge_begin_js !== "function")
        return false;
    const count = Number(Module._wmp_track_table_js(total));
    if (count < 6)
        return false;
    const tablePtr = Number(Module._wmp_track_table_ptr_js());
    const table = Array.from(new Float64Array(Module.HEAPU8.buffer, tablePtr, count));
    const [format, trackCount, ticksPerBeat, blockSize] = table;
    const chunk = table.slice(4);
    const helpersWanted = Math.min(cores - 2, PARALLEL_MAX_HELPERS, trackCount);
    if (helpersWanted < 2)
        return false;
    const cuts = [0];
    for (let k = 1; k < helpersWanted; ++k) {
        const target = total * k / helpersWanted;
        let t = cuts[cuts.length - 1];
        while (t < trackCount && chunk[t] < target) ++t;
        if (t > cuts[cuts.length - 1] && t < trackCount)
            cuts.push(t);
    }
    cuts.push(trackCount);
    const parts = cuts.length - 1;
    if (parts < 2)
        return false;

    if (!Module._wmp_merge_begin_js(total, format, trackCount, ticksPerBeat, blockSize, compressed ? 1 : 0))
        return false;

    const stage = "BPFA: parallel load (" + parts + " workers)";
    const helpers = [];
    const percent = new Array(parts).fill(0);
    const weight = [];
    const results = [];
    const helperUrl = new URL(
        "midi-parser-helper.js?v=" + encodeURIComponent(WASMIDI_MIDI_PARSER_BOOTSTRAP),
        WASMIDI_MIDI_PARSER_BASE_URL).href;
    try {
        for (let k = 0; k < parts; ++k) {
            const rangeBegin = k === 0 ? 0 : chunk[cuts[k]];
            const rangeEnd = chunk[cuts[k + 1]];
            weight.push((rangeEnd - rangeBegin) / total);
            const helper = new Worker(helperUrl);
            helpers.push(helper);
            results.push(new Promise((resolve, reject) => {
                helper.onmessage = event => {
                    const m = event.data || {};
                    if (m.type === "progress") {
                        percent[k] = Math.max(0, Math.min(100, (Number(m.percent) - 15) / 0.79));
                        let done = 0;
                        for (let i = 0; i < parts; ++i) done += weight[i] * percent[i];
                        progress(Math.floor(15 + 0.79 * done), stage);
                    } else if (m.type === "partial-done") {
                        resolve(m.blob);
                    } else if (m.type === "partial-error") {
                        reject(new Error(m.message || "Parallel MIDI helper failed"));
                    }
                };
                helper.onerror = event => reject(new Error(
                    (event && event.message) || "Parallel MIDI helper crashed"));
            }));
            helper.postMessage({
                type: "partial",
                file,
                baseUrl: WASMIDI_MIDI_PARSER_BASE_URL,
                version: WASMIDI_MIDI_PARSER_BOOTSTRAP,
                size: total,
                format,
                trackCount,
                ticksPerBeat,
                firstTrack: cuts[k],
                endTrack: cuts[k + 1],
                firstChunkPosition: chunk[cuts[k]],
                rangeBegin,
                rangeEnd,
                blockSize,
                compress: compressed,
                snapshotBudget: Math.floor(16 * 1024 * 1024 * (rangeEnd - rangeBegin) / total)
            });
        }
        progress(15, stage);
        if (!compressed && !Module._wmp_merge_read_raw_js())
            throw new Error(parserErrorText(Module, "Could not read MIDI source"));
        for (let k = 0; k < parts; ++k) {
            const blob = await results[k];
            helpers[k].terminate();
            helpers[k] = null;
            const ptr = Number(Module._wmp_merge_buffer_js(blob.byteLength));
            if (!ptr)
                throw new Error(parserErrorText(Module, "Could not allocate the parallel merge buffer"));
            Module.HEAPU8.set(new Uint8Array(blob), ptr);
            results[k] = null;
            if (!Module._wmp_merge_part_js())
                throw new Error(parserErrorText(Module, "Could not merge a partial MIDI index"));
        }
        progress(93, "BPFA: merging partial indexes");
        if (!Module._wmp_merge_finish_js())
            throw new Error(parserErrorText(Module, "Could not finish the parallel MIDI load"));
        return true;
    } catch (error) {
        console.warn("[WASMIDI MIDI parser] parallel load failed; falling back to one thread", error);
        return false;
    } finally {
        for (const helper of helpers)
            if (helper) helper.terminate();
    }
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
// The fold of colors per track (HANDOFF sec. 108): the roll's window in ticks and width in pixels.
let sharpRenderFoldWindow = 0;
let sharpRenderFoldPixels = 0;
let sharpRenderCompletedTarget = 0;
let sharpRenderStarved = false;

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

// setTimeout(0) in a loop is clamped to 4 ms after a few nested calls: ~250 sweeps a second,
// fewer than the densest parts need (HANDOFF sec. 89). A message to ourselves yields without the
// clamp; ?sweepyield=0 goes back to setTimeout.
const sweepYieldChannel = typeof MessageChannel === "function" ? new MessageChannel() : null;
const sweepYieldWaiters = [];
if (sweepYieldChannel)
    sweepYieldChannel.port1.onmessage = () => { const resolve = sweepYieldWaiters.shift(); if (resolve) resolve(); };
function sweepYield() {
    if (!sweepYieldChannel || pageOverride("sweepyield") === 0)
        return new Promise(resolve => setTimeout(resolve, 0));
    return new Promise(resolve => {
        sweepYieldWaiters.push(resolve);
        sweepYieldChannel.port2.postMessage(0);
    });
}

// Roll summaries: 1 in channel colors (HANDOFF sec. 108), 2 in colors per track for the roll's fold
// (sec. 113), 0 with no fold or ?rollsummary=0 (the sweep reads every word).
function rollSummaryMode() {
    if (pageOverride("rollsummary") === 0) return 0;
    if (!sharpRenderPerTrack) return 1;
    return sharpRenderFoldWindow && sharpRenderFoldPixels ? 2 : 0;
}

// The held pieces whose summary is for other colors or another fold go to the helpers again, one at
// a time and ahead of the visual spans, from the sweep on (HANDOFF sec. 113).
function rollPump(Module) {
    const pool = feedPool;
    if (!pool || !pool.ready || pool.rollBusy || typeof Module._wmp_roll_request_js !== "function") return;
    if (!rollSummaryMode() || !Module._wmp_roll_request_js()) return;
    const request = copyWasmBytes(Module, Module._wmp_feed_buffer_ptr_js(),
        sizeToNumber(Module._wmp_feed_buffer_size_js(), "roll request size"));
    pool.lastId = (pool.lastId || 0) + 1;
    pool.rollBusy = { id: pool.lastId, kind: "roll", request, result: null };
    pool.rollPending.push(pool.rollBusy);
}

function rollOnResult(entry, m) {
    const pool = feedPool;
    if (pool && pool.rollBusy === entry) pool.rollBusy = null;
    getModule().then(Module => {
        if (m.result && m.result.byteLength) {
            const result = new Uint8Array(m.result);
            const ptr = pointerToNumber(Module._wmp_feed_buffer_js(result.length), "roll buffer");
            if (ptr) {
                Module.HEAPU8.set(result, ptr);
                if (!Module._wmp_roll_attach_js(result.length))
                    console.warn("[WASMIDI visual] roll summary not attached:", parserErrorText(Module, "?"));
            }
        } else if (m.message) {
            console.warn("[WASMIDI visual] roll summary failed:", m.message);
        }
        rollPump(Module);
        feedDispatch();
    }).catch(error => console.warn("[WASMIDI visual] roll summary:", error));
}

async function drainSharpRenderer() {
    if (sharpRenderRunning)
        return;
    sharpRenderRunning = true;

    try {
        const Module = await getModule();
        sharpRenderStarved = false;
        while (mappedFileReady && sharpRenderReady &&
               sharpRenderCompletedTarget < sharpRenderTarget) {
            visualPump(Module);
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
            const sweepStart = performance.now();
            const swept = Module._wmp_build_render_sweep_js(target, sourceBudget);
            visualStats.sweepMs += performance.now() - sweepStart;
            ++visualStats.sweeps;
            visualStatsTick();
            if (!swept) {
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
            } else if (visualPoolActive && hasNextTick &&
                       nextTick >= sizeToNumber(Module._wmp_visual_head_js(), "visual head") &&
                       !Module._wmp_visual_done_js()) {
                // The helpers have not returned the next span yet; visualPump resumes.
                sharpRenderStarved = true;
                ++visualStats.starved;
                break;
            }

            // Always return to the event loop after one bounded resident sweep.
            // Pass 13.8 allowed urgent render bursts to monopolize this shared
            // mapped Worker for up to ~6 ms at a time; that directly delayed
            // synth admission and live-state requests.  Prefetch depth is now
            // maintained by watermarks on the main thread, not by long bursts.
            await sweepYield();
        }
    } finally {
        sharpRenderRunning = false;
        if (mappedFileReady && sharpRenderReady && !sharpRenderStarved &&
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
        // ?rollfold=0: colors per track send every note, also those that cover no pixel (HANDOFF sec. 108).
        const fold = pageOverride("rollfold") !== 0;
        sharpRenderFoldWindow = fold ? Number(message.foldWindow) >>> 0 : 0;
        sharpRenderFoldPixels = fold ? Number(message.foldPixels) >>> 0 : 0;
        if (typeof Module._wmp_roll_summary_js === "function")
            Module._wmp_roll_summary_js(rollSummaryMode());
        if (typeof Module._wmp_render_fold_js === "function")
            Module._wmp_render_fold_js(sharpRenderFoldWindow, sharpRenderFoldPixels);
        Module._wmp_reset_render_cursor_js(startTick, perTrack ? 1 : 0);
        // A start inside the held stream keeps it and the spans in flight (HANDOFF sec. 89).
        const kept = typeof Module._wmp_visual_kept_js === "function" && Module._wmp_visual_kept_js() === 1;
        if (!kept) visualRestart(Module);
        liveRestartStream(Module);
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

    if (!feedPool)
        feedStartPool().catch(error => console.warn("[WASMIDI visual] helper pool unavailable", error));
    void drainSharpRenderer();
}

async function buildKeyState(message) {
    const Module = await getModule();
    if (!mappedFileReady)
        return;
    const tick = Math.max(0, Math.min(0xffffffff, Number(message.tick) || 0));
    const npsStartTick = Math.max(0, Math.min(tick, Number(message.npsStartTick) || 0));
    const ccStartTick = Math.max(0, Math.min(tick, Number(message.ccStartTick) || 0));
    const liveStart = performance.now();
    if (!Module._wmp_build_live_snapshot_js(
            tick, npsStartTick, ccStartTick, message.reset ? 1 : 0))
        throw new Error(parserErrorText(Module, "Could not build mapped live state"));
    visualStats.liveMs += performance.now() - liveStart;
    ++visualStats.lives;
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

// Synth feed (HANDOFF sec. 70): this Worker answers WasmiSynth's feeder port directly
// (reset/fill/batch, the same protocol as synth-feeder-worker.js), so the main thread is
// out of the audio path. The C++ store applies the synth's block admission before the
// events leave the parser. With a compressed source, helper Workers decode spans of
// admission blocks in parallel from shared copies of the compressed slabs and this
// Worker stitches their results in order.
const FEED_SPAN_BLOCKS = 4;
const FEED_PREFETCH_SECONDS = 1.0;
const FEED_FILL_BUDGET_MS = 12;
const FEED_MAX_EVENTS = 262144;
const FEED_HELPERS_MAX = 8;
let feedPort = null;
let feedGeneration = 0;
let feedReady = false;
let feedConfig = { voices: 1024, admission: 25, sampleRate: 48000, blockFrames: 512, velocityFloor: 0 };
let feedHistory = null;
let feedFill = null;
let feedComplete = false;
let feedPool = null;
let feedPlanned = 0;
let feedQueue = [];
let feedSequence = 0;
let feedSpanChunks = 2;
let feedStarted = false;

function feedApplyConfig(Module, d) {
    if (d) {
        if (Number(d.voices) > 0) feedConfig.voices = Number(d.voices) | 0;
        if (Number(d.sampleRate) > 0) feedConfig.sampleRate = Number(d.sampleRate);
        if (Number(d.blockFrames) > 0) feedConfig.blockFrames = Number(d.blockFrames) | 0;
        if (d.velocityFloor != null) feedConfig.velocityFloor = Math.max(0, Math.min(127, d.velocityFloor | 0));
    }
    Module._wmp_feed_config_js(feedConfig.voices, feedConfig.admission, feedConfig.sampleRate,
        feedConfig.blockFrames, feedConfig.velocityFloor, FEED_SPAN_BLOCKS);
}

function feedTakeBatch(Module, out) {
    const count = sizeToNumber(Module._wmp_event_batch_count_js(), "feed batch count");
    if (count) {
        const ptr = pointerToNumber(Module._wmp_event_batch_ptr_js(), "feed batch pointer");
        const timePtr = pointerToNumber(Module._wmp_event_batch_time_ptr_js(), "feed time pointer");
        const words = new Uint32Array(Module.HEAPU8.buffer, ptr, count * 2);
        const messages = new Uint32Array(count);
        for (let i = 0; i < count; ++i) messages[i] = words[i * 2 + 1];
        out.messages.push(messages);
        out.times.push(new Float64Array(Module.HEAPU8.buffer, timePtr, count).slice());
    }
    const sxCount = sizeToNumber(Module._wmp_sysex_batch_event_count_js(), "feed SysEx count");
    const sxBytes = sizeToNumber(Module._wmp_sysex_batch_data_size_js(), "feed SysEx bytes");
    if (sxCount && sxBytes) {
        const meta = new Uint32Array(copyWasmBytes(Module, Module._wmp_sysex_batch_event_ptr_js(), sxCount * 12).buffer);
        const data = copyWasmBytes(Module, Module._wmp_sysex_batch_data_ptr_js(), sxBytes);
        out.sysex.push({ meta, data });
    }
    out.safe = Math.max(out.safe, Number(Module._wmp_feed_safe_seconds_js()) || 0);
    if (Module._wmp_feed_complete_js()) out.complete = true;
}

function feedPostBatch(Module, generation, out) {
    let total = 0;
    for (const m of out.messages) total += m.length;
    const prefix = feedHistory;
    feedHistory = null;
    const selector = prefix ? prefix.selector : null;
    const selectorCount = selector ? selector.length : 0;
    const messages = new Uint32Array(selectorCount + total);
    const times = new Float64Array(selectorCount + total);
    if (selectorCount) {
        messages.set(selector, 0);
        times.fill(prefix.time, 0, selectorCount);
    }
    let at = selectorCount;
    for (let k = 0; k < out.messages.length; ++k) {
        messages.set(out.messages[k], at);
        times.set(out.times[k], at);
        at += out.messages[k].length;
    }
    const parts = [];
    if (prefix && prefix.sysexMeta && prefix.sysexData)
        parts.push({ meta: prefix.sysexMeta, data: prefix.sysexData, time: prefix.time });
    for (const p of out.sysex) parts.push(p);
    let sxCount = 0, sxBytes = 0;
    for (const p of parts) { sxCount += Math.floor(p.meta.length / 3); sxBytes += p.data.length; }
    const sysexMeta = new Uint32Array(sxCount * 3);
    const sysexData = new Uint8Array(sxBytes);
    const sysexTimes = new Float64Array(sxCount);
    let mi = 0, di = 0;
    let lastTick = 0xffffffff, lastTime = 0;
    for (const p of parts) {
        const n = Math.floor(p.meta.length / 3);
        sysexData.set(p.data, di);
        for (let i = 0; i < n; ++i, ++mi) {
            const tick = p.meta[i * 3] >>> 0;
            sysexMeta[mi * 3] = tick;
            sysexMeta[mi * 3 + 1] = (p.meta[i * 3 + 1] + di) >>> 0;
            sysexMeta[mi * 3 + 2] = p.meta[i * 3 + 2] >>> 0;
            if (p.time != null) {
                sysexTimes[mi] = p.time;
            } else {
                if (tick !== lastTick) {
                    lastTick = tick;
                    lastTime = Math.max(0, Number(Module._wmp_tick_to_seconds_js(tick)) || 0);
                }
                sysexTimes[mi] = lastTime;
            }
        }
        di += p.data.length;
    }
    feedPort.postMessage({
        type: "batch",
        generation,
        messages, times, sysexMeta, sysexData, sysexTimes,
        safeUntil: out.complete ? Math.max(out.safe, 1e9) : out.safe
    }, [messages.buffer, times.buffer, sysexMeta.buffer, sysexData.buffer, sysexTimes.buffer]);
}

async function feedReset(d) {
    const Module = await getModule();
    if (!mappedFileReady) return;
    feedGeneration = d.generation >>> 0;
    feedFill = null;
    feedComplete = false;
    feedQueue = [];
    ++feedSequence;
    // Joint spans still carry the visual stream's part.
    if (feedPool) feedPool.pending = feedPool.pending.filter(e => e.kind === "joint" && e.visual.sequence === visualSequence);
    feedStarted = true;
    const time = Math.max(0, Number(d.time) || 0);
    const tick = Math.max(0, Math.floor(Number(Module._wmp_seconds_to_tick_js(time)) || 0));
    feedApplyConfig(Module, d);
    Module._wmp_feed_reset_js(tick);
    feedPlanned = Number(Module._wmp_feed_block_js()) || 0;

    // Seek state, as resetSynthCursor() restores it for the relayed path.
    const history = { time: Math.max(0, Number(Module._wmp_tick_to_seconds_js(tick)) || 0) };
    if (!Module._wmp_build_historical_sysex_js(tick))
        throw new Error(parserErrorText(Module, "Could not restore mapped SysEx state"));
    const hCount = sizeToNumber(Module._wmp_sysex_batch_event_count_js(), "historical SysEx count");
    const hBytes = sizeToNumber(Module._wmp_sysex_batch_data_size_js(), "historical SysEx bytes");
    if (hCount && hBytes) {
        history.sysexMeta = new Uint32Array(copyWasmBytes(Module, Module._wmp_sysex_batch_event_ptr_js(), hCount * 12).buffer);
        history.sysexData = copyWasmBytes(Module, Module._wmp_sysex_batch_data_ptr_js(), hBytes);
    }
    if (!Module._wmp_build_historical_selector_state_js(tick))
        throw new Error(parserErrorText(Module, "Could not restore mapped bank/program state"));
    const sCount = sizeToNumber(Module._wmp_historical_selector_state_count_js(), "selector count");
    if (sCount) {
        const ptr = pointerToNumber(Module._wmp_historical_selector_state_ptr_js(), "selector pointer");
        const words = new Uint32Array(Module.HEAPU8.buffer, ptr, sCount * 2);
        history.selector = new Uint32Array(sCount);
        for (let i = 0; i < sCount; ++i) history.selector[i] = words[i * 2 + 1];
    }
    feedHistory = history;
}

async function feedOnFill(d) {
    const Module = await getModule();
    if (!mappedFileReady || (d.generation >>> 0) !== feedGeneration) return;
    if ((d.velocityFloor != null && (d.velocityFloor | 0) !== feedConfig.velocityFloor) ||
        (Number(d.voices) > 0 && (Number(d.voices) | 0) !== feedConfig.voices))
        feedApplyConfig(Module, d);
    feedFill = { generation: feedGeneration, target: Math.max(0, Number(d.target) || 0) };
    if (feedPool && feedPool.ready) feedPump(Module);
    else feedFillSequential(Module);
}

function feedFillSequential(Module) {
    const fill = feedFill;
    if (!fill) return;
    feedFill = null;
    const out = { messages: [], times: [], sysex: [], safe: 0, complete: false };
    const started = performance.now();
    let events = 0;
    do {
        if (!Module._wmp_feed_build_js(fill.target, FEED_MAX_EVENTS))
            throw new Error(parserErrorText(Module, "Could not build the synth feed"));
        feedTakeBatch(Module, out);
        events += sizeToNumber(Module._wmp_event_batch_count_js(), "feed batch count");
    } while (!out.complete && out.safe < fill.target && events < FEED_MAX_EVENTS &&
             performance.now() - started < FEED_FILL_BUDGET_MS);
    feedPostBatch(Module, fill.generation, out);
}

// ---- Visual stream (HANDOFF sec. 71) ----
// With the helper pool the folded stream that the roll sweep and the keyboard/counters read
// is decoded in spans by the same helpers (feed spans go first) and appended here in order;
// the sweep waits for the next span.
const VISUAL_SPAN_WORDS = 1 << 19;
// Each span resumes every track, so with thousands of tracks the spans are longer
// (HANDOFF sec. 82); after a restart they grow from VISUAL_SPAN_WORDS so the first comes soon.
let visualSpanMax = VISUAL_SPAN_WORDS;
let visualSpanWords = VISUAL_SPAN_WORDS;
let visualPoolActive = false;
let visualStreamOn = true;      // ?visualstream=0: the roll and keyboard read the old merge path
// The compact stream lets the helpers run ahead of the roll up to the plan, in words past the
// sweep and the keyboard (HANDOFF sec. 88): 32 M words (~47 MB per copy) per GiB of RAM, up to
// 1024 M. ?visualplan=M sets it in millions of words; ?visualahead=0 plans only up to the roll's
// target, with the old 32 M words.
let visualAhead = true;
let visualPlanWords = 32 * 1048576;
// Roll modes (HANDOFF sec. 89), from the parse message or ?rollmode=: 1 look-ahead (a RAM limit,
// lookAheadMB or ?lookahead=MB, decoded before the MIDI opens), 2 the whole song decoded at load
// and kept (seeks reuse it), 0 the rev. 92 run-ahead with no prefill.
let rollMode = 0;
let visualPrefilling = false;
const VISUAL_BYTES_PER_WORD = 1.5;
// The keyboard Worker gets each piece when it is this many words from it.
const LIVE_FORWARD_WORDS = 32 * 1048576;
function rollModeFor(message) {
    const forced = pageOverride("rollmode");
    const mode = forced !== null ? forced : Number(message.rollMode);
    return mode === 1 || mode === 2 ? mode : 0;
}
function visualPlanDefault(message) {
    const forced = pageOverride("visualplan");
    if (forced) return Math.max(1, forced) * 1048576;
    if (rollMode === 2) return 1e10;
    if (rollMode === 1) {
        const forcedMB = pageOverride("lookahead");
        const mb = forcedMB !== null ? forcedMB : Number(message.lookAheadMB);
        const limit = Number.isFinite(mb) && mb > 0 ? Math.min(16384, Math.max(64, mb)) : 1024;
        return Math.round(limit * 1048576 / VISUAL_BYTES_PER_WORD);
    }
    if (!visualAhead) return 32 * 1048576;
    const deviceGiB = Number(self.navigator && self.navigator.deviceMemory);
    const gib = Number.isFinite(deviceGiB) && deviceGiB > 0 ? deviceGiB : 8;
    return Math.round(Math.min(1024, Math.max(32, 32 * gib))) * 1048576;
}
// ?visualdebug=1: once a second, where this Worker's time went (sweep, live, appends).
const visualStats = { at: 0, sweepMs: 0, sweeps: 0, liveMs: 0, lives: 0, appendMs: 0, spans: 0, bytes: 0,
    starved: 0, local: 0, keyMs: 0, keys: 0, joints: 0, helperMs: 0, handlers: {} };
function visualStatsHandler(type, ms) {
    const h = visualStats.handlers;
    const e = h[type] || (h[type] = { ms: 0, n: 0, max: 0 });
    e.ms += ms; ++e.n; e.max = Math.max(e.max, ms);
}
function visualStatsTick() {
    if (pageOverride("visualdebug") !== 1) return;
    const now = performance.now();
    if (!visualStats.at) visualStats.at = now;
    if (now - visualStats.at < 1000) return;
    const v = visualStats;
    console.info("[WASMIDI visual] " + Math.round(now - v.at) + " ms: sweep " + v.sweepMs.toFixed(0) + " ms/" + v.sweeps +
        ", live " + v.liveMs.toFixed(0) + " ms/" + v.lives + ", append " + v.appendMs.toFixed(0) + " ms/" + v.spans +
        " spans (" + v.joints + " joint) " + (v.bytes / 1048576).toFixed(1) + " MiB, starved " + v.starved + ", pool " + (visualPoolActive ? "on" : "off") +
        (liveKeys ? ", keyboard Worker " + v.keyMs.toFixed(0) + " ms/" + v.keys : "") +
        ", queue " + visualQueue.length + ", feed pending " + (feedPool ? feedPool.pending.length : 0) +
        (feedPool && feedPool.helpers.length ? ", helpers " + Math.round(100 * v.helperMs / ((now - v.at) * feedPool.helpers.length)) + "% busy" : "") +
        Object.entries(v.handlers).filter(([, e]) => e.ms >= 20)
            .map(([type, e]) => ", " + type + " " + e.ms.toFixed(0) + " ms/" + e.n + " (max " + e.max.toFixed(0) + ")").join(""));
    Object.assign(v, { at: now, sweepMs: 0, sweeps: 0, liveMs: 0, lives: 0, appendMs: 0, spans: 0, bytes: 0, starved: 0,
        keyMs: 0, keys: 0, joints: 0, helperMs: 0, handlers: {} });
}
let visualQueue = [];
let visualSequence = 0;
let visualPlanned = 0;

function visualRestart(Module) {
    ++visualSequence;
    visualQueue = [];
    if (feedPool) {
        feedPool.visualPending = [];
        feedPool.pending = feedPool.pending.filter(e => e.kind !== "joint" || e.feed.sequence === feedSequence);
        if (feedPool.rollPending.length) {
            feedPool.rollPending = [];
            feedPool.rollBusy = null;
        }
    }
    visualPlanned = sizeToNumber(Module._wmp_visual_head_js(), "visual head");
    visualSpanWords = VISUAL_SPAN_WORDS;
}

function visualPump(Module) {
    const pool = feedPool;
    if (!visualStreamOn) return;
    if (!visualPoolActive) {
        if (!pool || !pool.ready) return;
        Module._wmp_visual_config_js(1, 0);
        if (typeof Module._wmp_visual_plan_js === "function") Module._wmp_visual_plan_js(visualPlanWords);
        visualPoolActive = true;
        visualRestart(Module);
        liveStart(Module);
    }
    if (!pool || !pool.ready) return;
    let appended = false;
    const appendStart = performance.now();
    while (visualQueue.length && visualQueue[0].result) {
        const entry = visualQueue.shift();
        ++visualStats.spans;
        visualStats.bytes += entry.result.length;
        const ptr = pointerToNumber(Module._wmp_feed_buffer_js(entry.result.length), "visual buffer");
        if (!ptr) throw new Error(parserErrorText(Module, "Could not allocate the visual buffer"));
        Module.HEAPU8.set(entry.result, ptr);
        if (!Module._wmp_visual_append_js(entry.result.length))
            throw new Error(parserErrorText(Module, "Could not append the visual span"));
        appended = true;
    }
    if (appended) liveFeedPump(Module);
    visualStats.appendMs += performance.now() - appendStart;
    rollPump(Module);
    jointPlan(Module);
    // Past the roll's target, while the synth feed runs, one helper stays free for it.
    const queueLimit = visualQueueLimit(pool);
    let aheadLimit = feedStarted && !feedComplete ? Math.max(1, pool.helpers.length - 1) : queueLimit;
    // Past half the plan, one span at a time: the sweep, the keyboard and the page need the cores
    // more than the refill (HANDOFF sec. 89). Not while prefilling.
    if (!visualPrefilling && pageOverride("refillcap") !== 0 && typeof Module._wmp_visual_ahead_words_js === "function" &&
        Number(Module._wmp_visual_ahead_words_js()) >= visualPlanWords / 2)
        aheadLimit = 1;
    while (visualQueue.length < (visualPlanned <= sharpRenderTarget ? queueLimit : visualAhead ? aheadLimit : 0)) {
        let end = sizeToNumber(Module._wmp_visual_span_end_js(visualPlanned, visualSpanWords), "visual span end");
        if (end <= visualPlanned) break;
        if (jointActive()) {
            if (feedBlockTick(Module, feedPlanned) === visualPlanned) break;
            end = visualJointEnd(Module, end);
        }
        visualSpanWords = Math.min(visualSpanMax, visualSpanWords * 2);
        if (!Module._wmp_visual_request_js(visualPlanned, end))
            throw new Error(parserErrorText(Module, "Could not plan the visual span"));
        const request = copyWasmBytes(Module, Module._wmp_feed_buffer_ptr_js(),
            sizeToNumber(Module._wmp_feed_buffer_size_js(), "visual request size"));
        pool.lastId = (pool.lastId || 0) + 1;
        const entry = { id: pool.lastId, kind: "visual", sequence: visualSequence, request, result: null };
        visualQueue.push(entry);
        pool.visualPending.push(entry);
        visualPlanned = end;
    }
    feedDispatch();
    if (appended && sharpRenderStarved && !sharpRenderRunning) {
        sharpRenderStarved = false;
        void drainSharpRenderer();
    }
}

function visualQueueLimit(pool) {
    return pool.helpers.length + (visualSpanMax > VISUAL_SPAN_WORDS ? 1 : pool.helpers.length);
}

// Roll modes (HANDOFF sec. 89): before the MIDI opens, look-ahead decodes its plan from the start
// and the whole-song mode decodes all of it. The whole song falls back to look-ahead when it does
// not fit in the parser heap (16 GiB) or half the machine's RAM.
async function visualPrefill(Module) {
    if (typeof Module._wmp_visual_prefill_local_js !== "function") return;
    const GiB = 1024 * 1024 * 1024;
    const infoPtr = pointerToNumber(Module._wmp_document_info_js(), "document info");
    const maxTick = infoPtr ? new Float64Array(Module.HEAPU8.buffer, infoPtr, 6)[0] : 0;
    const deviceGiB = Number(self.navigator && self.navigator.deviceMemory);
    const ram = (Number.isFinite(deviceGiB) && deviceGiB > 0 ? deviceGiB : 8) * GiB;
    const used = typeof Module._wmp_heap_used_js === "function" ? Number(Module._wmp_heap_used_js()) : 0;
    const budget = Math.max(64 * 1048576, Math.min(14.5 * GiB - (used || Module.HEAPU8.length), ram / 2));
    let full = rollMode === 2;
    if (!full) visualPlanWords = Math.min(visualPlanWords, Math.max(1048576, Math.floor(budget / VISUAL_BYTES_PER_WORD)));
    Module._wmp_visual_keep_all_js(full ? 1 : 0);
    Module._wmp_visual_plan_js(visualPlanWords);
    Module._wmp_reset_render_cursor_js(0, 0);
    const started = performance.now();
    let shown = -1;
    const report = () => {
        const head = Number(Module._wmp_visual_head_js());
        const songPart = maxTick > 0 ? head / maxTick : 1;
        const fraction = full ? songPart : Math.max(songPart, Number(Module._wmp_visual_ahead_words_js()) / visualPlanWords);
        const percent = Module._wmp_visual_done_js() ? 100 : Math.min(99, Math.floor(fraction * 100));
        if (percent === shown) return;
        shown = percent;
        progress(94, (full ? "Decoding the whole roll " : "Prefilling the roll look-ahead ") + percent + "%");
    };
    const checkBudget = () => {
        if (!full || Number(Module._wmp_visual_held_bytes_js()) <= budget) return;
        // What is decoded becomes the look-ahead.
        full = false;
        rollMode = 1;
        visualPlanWords = Math.max(1048576, Math.floor(budget / VISUAL_BYTES_PER_WORD));
        Module._wmp_visual_keep_all_js(0);
        Module._wmp_visual_plan_js(visualPlanWords);
        console.warn("[WASMIDI roll] the whole song does not fit in " + (budget / GiB).toFixed(1) +
            " GiB; using look-ahead");
    };
    report();
    visualPrefilling = true;
    try {
        await visualPrefillRun(Module, report, checkBudget);
    } finally {
        visualPrefilling = false;
    }
    console.info("[WASMIDI roll] " + (full ? "whole song" : "look-ahead") + " ready: " +
        (Number(Module._wmp_visual_held_bytes_js()) / 1048576).toFixed(0) + " MiB, to " +
        (Number(Module._wmp_tick_to_seconds_js(Number(Module._wmp_visual_head_js()))) || 0).toFixed(1) + " s, " +
        ((performance.now() - started) / 1000).toFixed(1) + " s");
}

async function visualPrefillRun(Module, report, checkBudget) {
    await feedStartPool().catch(() => {});
    if (feedPool && feedPool.settled) {
        await feedPool.settled;
        await new Promise(resolve => setTimeout(resolve, 0));
    }
    if (feedPool && feedPool.ready) {
        visualPump(Module);
        while (visualPoolActive) {
            await new Promise(resolve => setTimeout(resolve, 50));
            report();
            checkBudget();
            const stopped = Module._wmp_visual_done_js() === 1 ||
                sizeToNumber(Module._wmp_visual_span_end_js(visualPlanned, 1), "visual span end") <= visualPlanned;
            if (stopped && !visualQueue.length) break;
            visualPump(Module);
        }
    }
    // Without helpers (raw source, few cores) this Worker decodes.
    let yielded = performance.now();
    while (!visualPoolActive && Module._wmp_visual_prefill_local_js(VISUAL_SPAN_WORDS)) {
        checkBudget();
        if (performance.now() - yielded > 100) {
            report();
            await new Promise(resolve => setTimeout(resolve, 0));
            yielded = performance.now();
        }
    }
}

// ---- Joint spans (HANDOFF sec. 85) ----
// Where the synth feed and the visual stream meet on a feed chunk edge (chunks of
// FEED_SPAN_BLOCKS blocks counted from feedPlanned), one helper decodes the span once for
// both. Visual-only spans end on chunk edges and feed-only spans run to the visual head, past
// the synth's horizon if needed, so the two meet; from there joint spans go as far as either
// one asks. ?jointdecode=0 keeps them apart.
const JOINT_FEED_QUEUE_MAX = 256;
const JOINT_SPAN_SECONDS = 0.75;
let jointOn = true;

function jointActive() {
    return jointOn && feedStarted && !feedComplete && visualPoolActive && !!feedPool && feedPool.ready;
}

function feedBlockTick(Module, block) {
    return Number(Module._wmp_feed_block_tick_js(block)) || 0;
}

function feedEdgeAtOrAfter(Module, tick) {
    const block = Number(Module._wmp_feed_block_at_tick_js(tick, feedPlanned)) || feedPlanned;
    return feedPlanned + Math.ceil(Math.max(0, block - feedPlanned) / FEED_SPAN_BLOCKS) * FEED_SPAN_BLOCKS;
}

function visualJointEnd(Module, end) {
    const meet = feedBlockTick(Module, feedPlanned);
    if (visualPlanned < meet) return Math.min(end, meet);
    const up = feedEdgeAtOrAfter(Module, end);
    const upTick = feedBlockTick(Module, up);
    if (upTick === end) return end;
    if (up - FEED_SPAN_BLOCKS >= feedPlanned) {
        const downTick = feedBlockTick(Module, up - FEED_SPAN_BLOCKS);
        if (downTick > visualPlanned) return downTick;
    }
    return upTick > visualPlanned ? upTick : end;
}

function jointMaxChunks(blockSeconds) {
    return Math.max(feedSpanChunks, Math.floor(JOINT_SPAN_SECONDS / (FEED_SPAN_BLOCKS * blockSeconds)));
}

function jointPlan(Module) {
    const pool = feedPool;
    if (!jointActive()) return;
    const blockSeconds = Number(Module._wmp_feed_block_seconds_js()) || (512 / 48000);
    const horizon = (feedFill ? feedFill.target : 0) + FEED_PREFETCH_SECONDS;
    const maxChunks = jointMaxChunks(blockSeconds);
    while (feedBlockTick(Module, feedPlanned) === visualPlanned) {
        const natural = sizeToNumber(Module._wmp_visual_span_end_js(visualPlanned, visualSpanWords), "visual span end");
        if (natural <= visualPlanned) return;
        const feedWants = feedQueue.length < pool.helpers.length * 2 && feedPlanned * blockSeconds < horizon;
        const visualWants = visualQueue.length < visualQueueLimit(pool) &&
            (visualAhead || visualPlanned <= sharpRenderTarget) &&
            feedQueue.length < JOINT_FEED_QUEUE_MAX;
        if (!feedWants && !visualWants) return;
        let chunks = visualWants
            ? Math.max(1, Math.min(maxChunks, (feedEdgeAtOrAfter(Module, natural) - feedPlanned) / FEED_SPAN_BLOCKS))
            : feedSpanChunks;
        if (feedWants) chunks = Math.min(chunks, feedSpanChunks);
        visualSpanWords = Math.min(visualSpanMax, visualSpanWords * 2);
        if (!Module._wmp_feed_request_js(feedPlanned, FEED_SPAN_BLOCKS, chunks))
            throw new Error(parserErrorText(Module, "Could not plan the joint span"));
        const request = copyWasmBytes(Module, Module._wmp_feed_buffer_ptr_js(),
            sizeToNumber(Module._wmp_feed_buffer_size_js(), "joint request size"));
        pool.lastId = (pool.lastId || 0) + 1;
        const feed = { id: pool.lastId, sequence: feedSequence, request: null, result: null };
        const visual = { id: pool.lastId, kind: "visual", sequence: visualSequence, request: null, result: null };
        feedQueue.push(feed);
        visualQueue.push(visual);
        pool.pending.push({ id: pool.lastId, kind: "joint", request, feed, visual });
        ++visualStats.joints;
        feedPlanned += chunks * FEED_SPAN_BLOCKS;
        visualPlanned = feedBlockTick(Module, feedPlanned);
    }
}

function visualOnResult(entry, m) {
    if (entry.sequence !== visualSequence) return;
    entry.result = new Uint8Array(m.result);
    getModule().then(Module => visualPump(Module)).catch(error => {
        console.error("[WASMIDI visual]", error);
        postMessage({ type: "runtime-error", requestType: "sharp-render",
            message: error && error.message ? error.message : String(error) });
    });
}

function visualPoolStopped(Module) {
    if (!visualPoolActive) return;
    visualPoolActive = false;
    liveStop(Module);
    visualQueue = [];
    ++visualSequence;
    // Local decoding resumes where the appended spans end.
    Module._wmp_visual_config_js(1, 1);
    if (sharpRenderStarved) {
        sharpRenderStarved = false;
        void drainSharpRenderer();
    }
}

// ---- Keyboard Worker (HANDOFF sec. 83) ----
// With the helper pool the keyboard/counter snapshot runs on its own parser instance: it gets
// every appended span and restart, answers the key-state requests and asks this Worker only
// for the keys rebuilt on a reset. ?keyworker=0 keeps the keyboard here.
let liveKeys = null;
let liveStreamGen = 0;

function liveStart(Module) {
    if (liveKeys || pageOverride("keyworker") === 0) return;
    if (!Module._wmp_live_image_js()) return;
    const image = copyWasmBytes(Module, Module._wmp_live_buffer_ptr_js(),
        sizeToNumber(Module._wmp_live_buffer_size_js(), "keyboard image size"));
    // The keyboard Worker starts with an empty stream and gets the pieces as it nears them.
    if (!Module._wmp_live_forward_begin_js()) return;
    const stream = copyWasmBytes(Module, Module._wmp_live_buffer_ptr_js(),
        sizeToNumber(Module._wmp_live_buffer_size_js(), "visual stream size"));
    const url = new URL("midi-parser-helper.js?v=" + encodeURIComponent(WASMIDI_MIDI_PARSER_BOOTSTRAP),
        WASMIDI_MIDI_PARSER_BASE_URL).href;
    const worker = new Worker(url);
    const keys = { worker, ready: false, inFlight: null };
    liveKeys = keys;
    worker.onmessage = event => {
        if (liveKeys !== keys) return;
        liveOnMessage(keys, event.data || {}).catch(error => liveFailed(keys, error));
    };
    worker.onerror = event => liveFailed(keys, new Error((event && event.message) || "Keyboard Worker crashed"));
    worker.postMessage({
        type: "live-init", baseUrl: WASMIDI_MIDI_PARSER_BASE_URL, version: WASMIDI_MIDI_PARSER_BOOTSTRAP,
        image: image.buffer, stream: stream.buffer, gen: liveStreamGen, plan: 2 * LIVE_FORWARD_WORDS
    }, [image.buffer, stream.buffer]);
    liveFeedPump(Module);
}

// After a reset the keyboard Worker's stream restarts where forwarding does: the piece of the
// start, or the start itself.
function liveRestartStream(Module) {
    if (!liveKeys) return;
    ++liveStreamGen;
    liveKeys.worker.postMessage({
        type: "live-restart", startTick: Number(Module._wmp_live_forward_tick_js()),
        base: Number(Module._wmp_live_forward_base_js()), gen: liveStreamGen
    });
    liveFeedPump(Module);
}

function liveFeedPump(Module) {
    if (!liveKeys) return;
    while (Module._wmp_live_forward_js(LIVE_FORWARD_WORDS)) {
        const piece = copyWasmBytes(Module, Module._wmp_live_buffer_ptr_js(),
            sizeToNumber(Module._wmp_live_buffer_size_js(), "visual piece size"));
        liveKeys.worker.postMessage({ type: "live-append", result: piece.buffer }, [piece.buffer]);
    }
}

function liveForward(message) {
    liveKeys.inFlight = message;
    liveKeys.worker.postMessage({
        type: "live-request",
        tick: Number(message.tick) || 0,
        npsStartTick: Number(message.npsStartTick) || 0,
        ccStartTick: Number(message.ccStartTick) || 0,
        reset: !!message.reset,
        requestId: Number(message.requestId) >>> 0,
        generation: Number(message.generation) >>> 0
    });
}

async function liveOnMessage(keys, m) {
    const Module = await getModule();
    if (m.type === "live-ready") {
        keys.ready = true;
        Module._wmp_live_external_js(1);
        return;
    }
    if (m.type === "live-rebuild") {
        const started = performance.now();
        if (!Module._wmp_live_rebuild_js(Number(m.tick) || 0, Number(m.npsStartTick) || 0,
                                         Number(m.ccStartTick) || 0, m.density ? 1 : 0))
            throw new Error(parserErrorText(Module, "Could not rebuild the keyboard"));
        visualStats.liveMs += performance.now() - started;
        ++visualStats.lives;
        const data = copyWasmBytes(Module, Module._wmp_live_buffer_ptr_js(),
            sizeToNumber(Module._wmp_live_buffer_size_js(), "keyboard rebuild size"));
        keys.worker.postMessage({ type: "live-rebuilt", data: data.buffer }, [data.buffer]);
        return;
    }
    if (m.type === "live-state") {
        keys.inFlight = null;
        visualStats.keyMs += Number(m.ms) || 0;
        ++visualStats.keys;
        Module._wmp_live_external_position_js((m.gen >>> 0) === liveStreamGen ? Number(m.position) : -1);
        liveFeedPump(Module);
        postMessage({
            type: "live-state",
            tick: m.tick,
            requestId: m.requestId >>> 0,
            generation: m.generation >>> 0,
            data: m.data
        }, [m.data]);
        visualPump(Module);
        return;
    }
    if (m.type === "live-error")
        throw new Error(m.message || "Keyboard Worker failed");
}

// Back to the keyboard on this Worker; a request the keyboard Worker still owed is answered here.
function liveStop(Module) {
    const keys = liveKeys;
    if (!keys) return;
    liveKeys = null;
    try { keys.worker.terminate(); } catch (_) {}
    Module._wmp_live_external_js(0);
    if (keys.inFlight) {
        buildKeyState(keys.inFlight).catch(error => postMessage({ type: "runtime-error", requestType: "key-state",
            message: error && error.message ? error.message : String(error) }));
    }
}

function liveFailed(keys, error) {
    if (liveKeys !== keys) return;
    console.warn("[WASMIDI keyboard] keyboard Worker stopped; the keyboard runs on the parser Worker:", error);
    getModule().then(Module => liveStop(Module));
}

// ---- Helper pool ----

function feedHelperCount() {
    const forced = pageOverride("helpers");
    if (forced !== null)
        return Math.min(FEED_HELPERS_MAX, Math.max(0, Math.floor(forced)));
    const cores = Number(self.navigator && self.navigator.hardwareConcurrency) || 1;
    return Math.min(FEED_HELPERS_MAX, Math.max(0, Math.floor(cores / 3)));
}

async function feedStartPool() {
    const Module = await getModule();
    const helpers = feedHelperCount();
    if (feedPool || helpers < 2 || typeof SharedArrayBuffer !== "function" || !self.crossOriginIsolated)
        return;
    if (!Module._wmp_feed_image_js()) return;
    const image = copyWasmBytes(Module, Module._wmp_feed_buffer_ptr_js(), sizeToNumber(Module._wmp_feed_buffer_size_js(), "feed image size"));
    // Longer spans with many tracks: each span resumes every track from a checkpoint.
    const trackCount = new Uint32Array(image.buffer, 0, 2)[1];
    feedSpanChunks = trackCount < 256 ? 2 : trackCount < 2048 ? 4 : 8;
    visualSpanMax = Math.min(1 << 23, Math.max(VISUAL_SPAN_WORDS, trackCount * 1024));
    const slabBytes = sizeToNumber(Module._wmp_source_slab_bytes_js(), "slab size");
    const slabCount = sizeToNumber(Module._wmp_source_slab_count_js(), "slab count");
    const slabs = [];
    try {
        for (let i = 0; i < slabCount; ++i) {
            const ptr = pointerToNumber(Module._wmp_source_slab_ptr_js(i), "slab pointer");
            const sab = new SharedArrayBuffer(slabBytes);
            new Uint8Array(sab).set(Module.HEAPU8.subarray(ptr, ptr + slabBytes));
            slabs.push(sab);
        }
    } catch (error) {
        console.warn("[WASMIDI synth feed] no memory for shared source slabs; decoding on one thread", error);
        return;
    }
    const url = new URL("midi-parser-helper.js?v=" + encodeURIComponent(WASMIDI_MIDI_PARSER_BOOTSTRAP),
        WASMIDI_MIDI_PARSER_BASE_URL).href;
    const pool = { ready: false, failed: false, helpers: [], pending: [], visualPending: [], rollPending: [], rollBusy: null };
    feedPool = pool;
    const inits = [];
    for (let h = 0; h < helpers; ++h) {
        const worker = new Worker(url);
        const helper = { worker, busy: null };
        pool.helpers.push(helper);
        inits.push(new Promise((resolve, reject) => {
            worker.onmessage = event => {
                const m = event.data || {};
                if (m.type === "feed-ready") resolve();
                else if (m.type === "feed-result" || m.type === "visual-result" || m.type === "joint-result" ||
                         m.type === "roll-result")
                    feedOnResult(helper, m);
                else if (m.type === "feed-error") {
                    reject(new Error(m.message || "Synth feed helper failed"));
                    feedPoolFailed(m.message);
                }
            };
            worker.onerror = event => {
                const text = (event && event.message) || "Synth feed helper crashed";
                reject(new Error(text));
                feedPoolFailed(text);
            };
        }));
        const copy = image.slice();
        worker.postMessage({
            type: "feed-init", baseUrl: WASMIDI_MIDI_PARSER_BASE_URL, version: WASMIDI_MIDI_PARSER_BOOTSTRAP,
            image: copy.buffer, slabs, slabBytes
        }, [copy.buffer]);
    }
    pool.settled = Promise.allSettled(inits);
    try {
        await Promise.all(inits);
        if (feedPool !== pool || pool.failed) return;
        pool.ready = true;
        console.info("[WASMIDI synth feed] " + helpers + " decode helpers, " +
            (slabCount * slabBytes / 1048576).toFixed(0) + " MiB shared source");
        postMessage({ type: "helpers-ready", helpers });
        if (feedFill) feedPump(Module);
        if (sharpRenderReady) void drainSharpRenderer();
    } catch (_) {}
}

function feedPoolFailed(message) {
    const pool = feedPool;
    if (!pool || pool.failed) return;
    pool.failed = true;
    pool.ready = false;
    console.warn("[WASMIDI synth feed] helpers stopped; decoding on one thread:", message);
    for (const h of pool.helpers) { try { h.worker.terminate(); } catch (_) {} }
    feedQueue = [];
    ++feedSequence;
    // Restart the sequential feed exactly where the stitch stopped.
    getModule().then(Module => {
        visualPoolStopped(Module);
        if (feedFill) feedFillSequential(Module);
    });
}

function feedDispatch() {
    const pool = feedPool;
    if (!pool || !pool.ready) return;
    for (const helper of pool.helpers) {
        if (helper.busy) continue;
        const entry = pool.pending.shift() || pool.rollPending.shift() || pool.visualPending.shift();
        if (!entry) return;
        helper.busy = entry;
        helper.since = performance.now();
        const type = entry.kind === "visual" ? "visual-decode" : entry.kind === "joint" ? "joint-decode" :
            entry.kind === "roll" ? "roll-summarize" : "feed-decode";
        helper.worker.postMessage({ type, id: entry.id, request: entry.request.buffer, rollMode: rollSummaryMode(),
            foldWindow: sharpRenderFoldWindow, foldPixels: sharpRenderFoldPixels }, [entry.request.buffer]);
        entry.request = null;
    }
}

function feedOnResult(helper, m) {
    const entry = helper.busy;
    helper.busy = null;
    if (helper.since) visualStats.helperMs += performance.now() - helper.since;
    if (entry && entry.id === m.id && entry.kind === "joint") {
        if (entry.feed.sequence === feedSequence) {
            entry.feed.result = new Uint8Array(m.feed);
            getModule().then(Module => feedPump(Module));
        }
        visualOnResult(entry.visual, { result: m.visual });
    } else if (entry && entry.id === m.id && entry.kind === "visual") {
        visualOnResult(entry, m);
    } else if (entry && entry.id === m.id && entry.kind === "roll") {
        rollOnResult(entry, m);
    } else if (entry && entry.id === m.id && entry.sequence === feedSequence) {
        entry.result = new Uint8Array(m.result);
        getModule().then(Module => feedPump(Module));
    }
    feedDispatch();
}

function feedPump(Module) {
    const pool = feedPool;
    if (!pool || !pool.ready) return;
    const blockSeconds = Number(Module._wmp_feed_block_seconds_js()) || (512 / 48000);
    const fill = feedFill;
    // Plan spans ahead of what the synth asked for.
    const horizon = (fill ? fill.target : 0) + FEED_PREFETCH_SECONDS;
    // The sequential path may have moved the feed while the pool was starting.
    if (!feedQueue.length) feedPlanned = Number(Module._wmp_feed_block_js()) || 0;
    jointPlan(Module);
    while (!feedComplete) {
        let chunks = feedSpanChunks;
        let wants = feedQueue.length < pool.helpers.length * 2 && feedPlanned * blockSeconds < horizon;
        if (jointActive()) {
            const meet = feedBlockTick(Module, feedPlanned);
            if (meet === visualPlanned &&
                sizeToNumber(Module._wmp_visual_span_end_js(visualPlanned, 1), "visual span end") > visualPlanned) break;
            if (meet < visualPlanned) {
                // Past the horizon too: the visual head is where the joint spans start.
                if (!wants && pool.pending.length < pool.helpers.length && feedQueue.length < JOINT_FEED_QUEUE_MAX) {
                    wants = true;
                    chunks = jointMaxChunks(blockSeconds);
                }
                chunks = Math.max(1, Math.min(chunks, (feedEdgeAtOrAfter(Module, visualPlanned) - feedPlanned) / FEED_SPAN_BLOCKS));
            }
        }
        if (!wants) break;
        if (!Module._wmp_feed_request_js(feedPlanned, FEED_SPAN_BLOCKS, chunks))
            throw new Error(parserErrorText(Module, "Could not plan the synth feed"));
        const request = copyWasmBytes(Module, Module._wmp_feed_buffer_ptr_js(),
            sizeToNumber(Module._wmp_feed_buffer_size_js(), "feed request size"));
        pool.lastId = (pool.lastId || 0) + 1;
        const entry = { id: pool.lastId, sequence: feedSequence, request, result: null };
        feedQueue.push(entry);
        pool.pending.push(entry);
        feedPlanned += chunks * FEED_SPAN_BLOCKS;
    }
    feedDispatch();
    if (!fill) return;
    // Stitch finished spans in order until the fill is covered.
    const out = { messages: [], times: [], sysex: [], safe: 0, complete: false };
    let stitched = false;
    while (feedQueue.length && feedQueue[0].result && !out.complete) {
        const entry = feedQueue.shift();
        const ptr = pointerToNumber(Module._wmp_feed_buffer_js(entry.result.length), "feed buffer");
        if (!ptr) throw new Error(parserErrorText(Module, "Could not allocate the feed buffer"));
        Module.HEAPU8.set(entry.result, ptr);
        if (!Module._wmp_feed_stitch_js(entry.result.length))
            throw new Error(parserErrorText(Module, "Could not stitch the synth feed"));
        feedTakeBatch(Module, out);
        stitched = true;
        if (out.safe >= fill.target) break;
    }
    if (out.complete) feedComplete = true;
    if (!stitched) return;
    feedFill = null;
    feedPostBatch(Module, fill.generation, out);
    if (feedQueue.length < pool.helpers.length * 2) feedPump(Module);
}

function feedAttachPort(port) {
    if (feedPort) { try { feedPort.close(); } catch (_) {} }
    feedPort = port || null;
    if (!feedPort) return;
    feedPort.onmessage = event => {
        const d = event.data || {};
        const run = async () => {
            const started = performance.now();
            if (d.type === "reset") await feedReset(d);
            else if (d.type === "fill") await feedOnFill(d);
            visualStatsHandler("feed-" + d.type, performance.now() - started);
        };
        run().catch(error => {
            console.error("[WASMIDI synth feed]", error);
            postMessage({ type: "runtime-error", requestType: "synth-feed",
                message: error && error.message ? error.message : String(error) });
        });
    };
    feedPort.start();
    feedStartPool().catch(error => console.warn("[WASMIDI synth feed] helper pool unavailable", error));
}

self.onmessage = async event => {
    const started = performance.now();
    await handleMessage(event);
    visualStatsHandler(String((event.data || {}).type), performance.now() - started);
};

async function handleMessage(event) {
    const message = event.data || {};
    if (message.type === "feeder-port") {
        feedAttachPort(message.port);
        return;
    }
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
            if (liveKeys && liveKeys.ready) liveForward(message);
            else await buildKeyState(message);
            return;
        }
        // WASMIDI Audio (HANDOFF sec. 64): timeline queries and seconds-based requests.
        if (message.type === "song-peaks") {
            const Module = await getModule();
            const ptr = typeof Module._wmp_song_peaks_js === "function" ? Number(Module._wmp_song_peaks_js()) : 0;
            const p = ptr ? new Float64Array(Module.HEAPU8.buffer, ptr, 4) : new Float64Array(4);
            postMessage({ type: "song-peaks", nps: p[0], npsTime: p[1], polyphony: p[2], polyphonyTime: p[3] });
            return;
        }
        if (message.type === "doc-info") {
            const Module = await getModule();
            const ptr = Number(Module._wmp_document_info_js());
            const info = ptr ? new Float64Array(Module.HEAPU8.buffer, ptr, 6) : new Float64Array(6);
            postMessage({
                type: "doc-info",
                maxTick: info[0], duration: info[1], ticksPerBeat: info[2],
                notes: info[3], tracks: info[4], format: info[5]
            });
            return;
        }
        // Song seconds to ticks for a list of times (visual-bench.html).
        if (message.type === "tick-at") {
            const Module = await getModule();
            const seconds = Array.from(message.seconds || []);
            const ticks = new Float64Array(seconds.map(t => Number(Module._wmp_seconds_to_tick_js(Number(t) || 0))));
            postMessage({ type: "tick-at", ticks }, [ticks.buffer]);
            return;
        }
        if (message.type === "text-events") {
            const Module = await getModule();
            const ptr = Number(Module._wmp_text_events_js());
            const size = Number(Module._wmp_text_events_size_js());
            const events = [];
            if (ptr && size >= 4) {
                const bytes = Module.HEAPU8.slice(ptr, ptr + size);
                const view = new DataView(bytes.buffer);
                const utf8 = new TextDecoder("utf-8", { fatal: true });
                const latin1 = new TextDecoder("latin1");
                const count = view.getUint32(0, true);
                let o = 4;
                for (let i = 0; i < count && o + 12 <= size; ++i) {
                    const seconds = view.getFloat64(o, true);
                    const kind = view.getUint8(o + 8);
                    const length = view.getUint16(o + 10, true);
                    const raw = bytes.subarray(o + 12, o + 12 + length);
                    let text;
                    try { text = utf8.decode(raw); } catch (_) { text = latin1.decode(raw); }
                    events.push({ t: seconds, kind, text });
                    o += 12 + length;
                }
            }
            postMessage({ type: "text-events", events });
            return;
        }
        if (message.type === "density") {
            const Module = await getModule();
            const ptr = Number(Module._wmp_density_js());
            if (!ptr) { postMessage({ type: "density", starts: null, counts: null }); return; }
            const n = new Float64Array(Module.HEAPU8.buffer, ptr, 1)[0] | 0;
            const all = new Float64Array(Module.HEAPU8.buffer, ptr, 1 + (n + 1) + n).slice();
            const starts = all.slice(1, n + 2);
            const counts = new Float32Array(all.subarray(n + 2));
            postMessage({ type: "density", starts, counts }, [starts.buffer, counts.buffer]);
            return;
        }
        if ((message.type === "synth-reset" || message.type === "synth-pump") &&
            (Number.isFinite(message.seconds) || Number.isFinite(message.endSeconds))) {
            const Module = await getModule();
            if (Number.isFinite(message.seconds))
                message.tick = Math.floor(Number(Module._wmp_seconds_to_tick_js(message.seconds)));
            if (Number.isFinite(message.endSeconds))
                message.endTick = Math.ceil(Number(Module._wmp_seconds_to_tick_js(message.endSeconds)));
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

        const rawLimit = rawSourceLimitBytes(!!self.__wasmidiMidiParserWholeFile);
        if (typeof Module._wmp_set_raw_source_limit_js === "function")
            Module._wmp_set_raw_source_limit_js(rawLimit);
        visualStreamOn = pageOverride("visualstream") !== 0;
        rollMode = visualStreamOn ? rollModeFor(message) : 0;
        visualAhead = rollMode !== 0 || pageOverride("visualahead") !== 0;
        visualPlanWords = visualPlanDefault(message);
        // Running ahead, the visual head is far from the synth feed: joint spans would make the
        // feed chase it. ?jointdecode=1 keeps them.
        jointOn = pageOverride("jointdecode") === 1 || (!visualAhead && pageOverride("jointdecode") !== 0);
        Module._wmp_visual_config_js(visualStreamOn ? 1 : 0, 1);
        if (typeof Module._wmp_roll_summary_js === "function")
            Module._wmp_roll_summary_js(rollSummaryMode());
        const ok = await parallelParse(Module, file, total, total > rawLimit) ||
            Module._wmp_parse_file_js(total);
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

        if (rollMode !== 0)
            await visualPrefill(Module);

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
}
