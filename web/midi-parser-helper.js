/* global WasmidiMidiParserCore */

"use strict";

// Parallel MIDI load helper (HANDOFF sec. 60): indexes a contiguous range of tracks
// with the same Memory64 parser core and returns the partial index as one buffer. The
// same script runs the synth feed helpers (HANDOFF sec. 70), the visual span helpers
// (HANDOFF sec. 71) and the keyboard Worker (HANDOFF sec. 83).

let corePromise = null;

function loadCore(baseUrl, version) {
    if (!corePromise) {
        importScripts(new URL(
            "wasmidi-midi-parser.js?v=" + encodeURIComponent(version),
            baseUrl).href);
        corePromise = WasmidiMidiParserCore({
            noInitialRun: true,
            noExitRuntime: true,
            print() {},
            printErr(text) {
                if (text)
                    console.error("[WASMIDI MIDI parser helper]", text);
            }
        });
    }
    return corePromise;
}

function errorText(Module, fallback) {
    try {
        const ptr = Number(Module._wmp_error_ptr_js());
        const size = Number(Module._wmp_error_size_js());
        if (ptr && size > 0)
            return new TextDecoder().decode(Module.HEAPU8.slice(ptr, ptr + size));
    } catch (_) {}
    return fallback;
}

// Synth feed helper (HANDOFF sec. 70): decodes spans of admission blocks for the parser
// Worker from the compressed source slabs it shares (SharedArrayBuffers).
async function feedMessage(m) {
    try {
        const Module = await loadCore(m.baseUrl || feedBase.baseUrl, m.version || feedBase.version);
        if (m.type === "feed-init") {
            feedBase.baseUrl = m.baseUrl;
            feedBase.version = m.version;
            self.__wasmidiFeedSlabs = m.slabs;
            self.__wasmidiFeedSlabBytes = m.slabBytes;
            const image = new Uint8Array(m.image);
            const ptr = Number(Module._wmp_feed_buffer_js(image.length));
            if (!ptr)
                throw new Error(errorText(Module, "No memory for the synth feed image"));
            Module.HEAPU8.set(image, ptr);
            if (!Module._wmp_feed_helper_init_js(image.length))
                throw new Error(errorText(Module, "Invalid synth feed image"));
            postMessage({ type: "feed-ready" });
            return;
        }
        const visual = m.type === "visual-decode";
        const joint = m.type === "joint-decode";
        const request = new Uint8Array(m.request);
        const ptr = Number(Module._wmp_feed_buffer_js(request.length));
        if (!ptr)
            throw new Error(errorText(Module, "No memory for the synth feed request"));
        Module.HEAPU8.set(request, ptr);
        const decode = visual ? Module._wmp_visual_decode_js : joint ? Module._wmp_joint_decode_js : Module._wmp_feed_decode_js;
        if (!decode(request.length))
            throw new Error(errorText(Module, visual ? "Visual span decode failed" : joint ? "Joint span decode failed" : "Synth feed decode failed"));
        const out = Number(Module._wmp_feed_buffer_ptr_js());
        const size = Number(Module._wmp_feed_buffer_size_js());
        if (joint) {
            // [u64 feed size][feed result][visual result] (HANDOFF sec. 85)
            const feedSize = Number(new DataView(Module.HEAPU8.buffer, out, 8).getBigUint64(0, true));
            const feed = Module.HEAPU8.slice(out + 8, out + 8 + feedSize);
            const spans = Module.HEAPU8.slice(out + 8 + feedSize, out + size);
            postMessage({ type: "joint-result", id: m.id, feed: feed.buffer, visual: spans.buffer }, [feed.buffer, spans.buffer]);
            return;
        }
        const result = Module.HEAPU8.slice(out, out + size);
        postMessage({ type: visual ? "visual-result" : "feed-result", id: m.id, result: result.buffer }, [result.buffer]);
    } catch (error) {
        postMessage({ type: "feed-error", message: error && error.message ? error.message : String(error) });
    }
}
const feedBase = { baseUrl: "", version: "" };

// Keyboard Worker (HANDOFF sec. 83): keeps a copy of the visual stream and answers the
// key-state requests; on a reset it asks the parser Worker for the rebuilt keys. Messages run
// one at a time, a request waiting for its rebuild included.
let liveChain = Promise.resolve();
let liveRebuildWaiter = null;
let liveGen = 0;

function liveMessage(m) {
    if (m.type === "live-rebuilt") {
        const waiter = liveRebuildWaiter;
        liveRebuildWaiter = null;
        if (waiter) waiter(m);
        return;
    }
    liveChain = liveChain.then(() => liveHandle(m)).catch(error => {
        postMessage({ type: "live-error", message: error && error.message ? error.message : String(error) });
    });
}

function liveCopyIn(Module, buffer) {
    const bytes = new Uint8Array(buffer);
    const ptr = Number(Module._wmp_live_buffer_js(bytes.length));
    if (!ptr) throw new Error(errorText(Module, "No memory for the keyboard Worker"));
    Module.HEAPU8.set(bytes, ptr);
    return bytes.length;
}

async function liveHandle(m) {
    if (m.type === "live-init") {
        feedBase.baseUrl = m.baseUrl;
        feedBase.version = m.version;
    }
    const Module = await loadCore(feedBase.baseUrl, feedBase.version);
    if (m.type === "live-init") {
        if (!Module._wmp_live_init_js(liveCopyIn(Module, m.image)))
            throw new Error(errorText(Module, "Invalid keyboard image"));
        if (m.stream && !Module._wmp_live_stream_import_js(liveCopyIn(Module, m.stream)))
            throw new Error(errorText(Module, "Invalid visual stream"));
        if (m.plan && typeof Module._wmp_visual_plan_js === "function") Module._wmp_visual_plan_js(Number(m.plan));
        liveGen = m.gen >>> 0;
        postMessage({ type: "live-ready" });
        return;
    }
    if (m.type === "live-restart") {
        Module._wmp_live_restart_js(Number(m.startTick) || 0);
        liveGen = m.gen >>> 0;
        return;
    }
    if (m.type === "live-append") {
        const result = new Uint8Array(m.result);
        const ptr = Number(Module._wmp_feed_buffer_js(result.length));
        if (!ptr) throw new Error(errorText(Module, "No memory for the visual span"));
        Module.HEAPU8.set(result, ptr);
        if (!Module._wmp_visual_append_js(result.length))
            throw new Error(errorText(Module, "Could not append the visual span"));
        return;
    }
    if (m.type !== "live-request") return;
    const started = performance.now();
    const tick = Number(m.tick) || 0, nps = Number(m.npsStartTick) || 0, cc = Number(m.ccStartTick) || 0;
    let step = Module._wmp_live_step_js(tick, nps, cc, m.reset ? 1 : 0);
    if (step === 2 || step === 3) {
        const rebuilt = await new Promise(resolve => {
            liveRebuildWaiter = resolve;
            postMessage({ type: "live-rebuild", tick, npsStartTick: nps, ccStartTick: cc, density: step === 3 ? 1 : 0 });
        });
        step = Module._wmp_live_resume_js(liveCopyIn(Module, rebuilt.data), tick, nps, cc) ? 1 : 0;
    }
    if (step !== 1) throw new Error(errorText(Module, "Keyboard state failed"));
    const ptr = Number(Module._wmp_key_snapshot_ptr_js());
    const words = Number(Module._wmp_key_snapshot_word_count_js());
    const data = Module.HEAPU8.slice(ptr, ptr + words * 4);
    postMessage({
        type: "live-state", tick, requestId: m.requestId >>> 0, generation: m.generation >>> 0, data: data.buffer,
        position: Number(Module._wmp_live_position_js()), gen: liveGen, ms: performance.now() - started
    }, [data.buffer]);
}

self.onmessage = async event => {
    const m = event.data || {};
    if (m.type === "feed-init" || m.type === "feed-decode" || m.type === "visual-decode" || m.type === "joint-decode") {
        await feedMessage(m);
        return;
    }
    if (typeof m.type === "string" && m.type.startsWith("live-")) {
        liveMessage(m);
        return;
    }
    if (m.type !== "partial")
        return;
    try {
        const Module = await loadCore(m.baseUrl, m.version);
        self.__wasmidiMidiParserFile = m.file;
        const ok = Module._wmp_partial_index_js(
            m.size, m.format, m.trackCount, m.ticksPerBeat,
            m.firstTrack, m.endTrack, m.firstChunkPosition,
            m.rangeBegin, m.rangeEnd, m.blockSize, m.compress ? 1 : 0,
            m.snapshotBudget);
        if (!ok)
            throw new Error(errorText(Module, "Partial MIDI index failed"));
        const ptr = Number(Module._wmp_partial_blob_ptr_js());
        const size = Number(Module._wmp_partial_blob_size_js());
        const blob = new Uint8Array(size);
        blob.set(Module.HEAPU8.subarray(ptr, ptr + size));
        Module._wmp_partial_release_js();
        postMessage({ type: "partial-done", blob: blob.buffer }, [blob.buffer]);
    } catch (error) {
        postMessage({
            type: "partial-error",
            message: error && error.message ? error.message : String(error)
        });
    }
};
