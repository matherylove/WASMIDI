/* global WasmidiMidiParserCore */

"use strict";

// Parallel MIDI load helper (HANDOFF sec. 60): indexes a contiguous range of tracks
// with the same Memory64 parser core and returns the partial index as one buffer.

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

self.onmessage = async event => {
    const m = event.data || {};
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
