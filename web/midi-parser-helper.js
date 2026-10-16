/* global WasmidiMidiParserCore */

"use strict";

// Parallel MIDI load helper (HANDOFF sec. 60): indexes a contiguous range of tracks
// with the same Memory64 parser core and returns the partial index as one buffer. The
// same script runs the synth feed helpers (HANDOFF sec. 70).

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
        const request = new Uint8Array(m.request);
        const ptr = Number(Module._wmp_feed_buffer_js(request.length));
        if (!ptr)
            throw new Error(errorText(Module, "No memory for the synth feed request"));
        Module.HEAPU8.set(request, ptr);
        if (!Module._wmp_feed_decode_js(request.length))
            throw new Error(errorText(Module, "Synth feed decode failed"));
        const out = Number(Module._wmp_feed_buffer_ptr_js());
        const size = Number(Module._wmp_feed_buffer_size_js());
        const result = Module.HEAPU8.slice(out, out + size);
        postMessage({ type: "feed-result", id: m.id, result: result.buffer }, [result.buffer]);
    } catch (error) {
        postMessage({ type: "feed-error", message: error && error.message ? error.message : String(error) });
    }
}
const feedBase = { baseUrl: "", version: "" };

self.onmessage = async event => {
    const m = event.data || {};
    if (m.type === "feed-init" || m.type === "feed-decode") {
        await feedMessage(m);
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
