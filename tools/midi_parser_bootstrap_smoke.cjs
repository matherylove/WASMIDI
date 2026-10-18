"use strict";

// Executes the *generated* modularized parser exactly enough to catch startup
// aborts before GitHub Pages deployment. The target includes ENVIRONMENT=node
// solely for this CI test; production still imports the same file in a Worker.
const fs = require("fs");
const path = require("path");

// Emscripten is intentionally built for both `worker` and `node`. The parser's
// EM_JS progress callbacks are optional in Node, but providing the tiny Worker
// surface here makes the smoke test exercise the same callback path that Pages
// uses instead of failing merely because Node has no `self`/`postMessage`.
const parserProgressMessages = [];
if (typeof globalThis.self === "undefined")
    globalThis.self = globalThis;
if (typeof globalThis.postMessage !== "function") {
    globalThis.postMessage = message => {
        parserProgressMessages.push(message);
    };
}

let parserPointerBits = 0;
let activePagedSource = null;
let pagedReadCalls = 0;
let pagedMaxRead = 0;
const SOURCE_WINDOW_BYTES = 8 * 1024 * 1024;

globalThis.__wasmidiMidiParserReadAt = (offset, count) => {
    if (!activePagedSource)
        throw new Error("Paged parser smoke source is not installed.");

    const sourceSize = activePagedSource instanceof Uint8Array
        ? activePagedSource.length
        : Number(activePagedSource.size);

    if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(count) ||
        !Number.isSafeInteger(sourceSize) ||
        offset < 0 || count < 0 || offset + count > sourceSize) {
        throw new Error("Generated parser requested an invalid source window.");
    }

    ++pagedReadCalls;
    pagedMaxRead = Math.max(pagedMaxRead, count);

    if (activePagedSource instanceof Uint8Array)
        return activePagedSource.subarray(offset, offset + count);

    const result = activePagedSource.read(offset, count);
    if (!(result instanceof Uint8Array) || result.length !== count)
        throw new Error("Virtual paged source returned an invalid window.");
    return result;
};

function parsePaged(Module, source) {
    const size = source instanceof Uint8Array
        ? source.length
        : Number(source && source.size);
    if (!Number.isSafeInteger(size) || size < 0)
        throw new Error("Invalid paged smoke source size.");

    activePagedSource = source;
    pagedReadCalls = 0;
    pagedMaxRead = 0;
    // Like the browser worker (mappedFileReady keeps the File), a loaded source
    // stays readable: the BPFA store reads it on demand after indexing.
    try {
        const ok = Module._wmp_parse_file_js(size);
        if (!ok) activePagedSource = null;
        return ok;
    } catch (error) {
        activePagedSource = null;
        throw error;
    }
}

function sizeNumber(value) {
    const n = typeof value === "bigint" ? Number(value) : Number(value);
    if (!Number.isSafeInteger(n) || n < 0)
        throw new Error("Generated parser returned an unsafe size.");
    return n;
}


function parserErrorText(Module, fallback) {
    const address = Number(Module._wmp_error_ptr_js());
    const byteLength = Number(Module._wmp_error_size_js());
    if (!Number.isSafeInteger(address) || address < 0 ||
        !Number.isSafeInteger(byteLength) || byteLength < 0 ||
        !address || !byteLength) {
        return fallback;
    }
    return new TextDecoder().decode(
        Module.HEAPU8.subarray(address, address + byteLength));
}

function makeDenseCrashMidi(noteCount) {
    // Format-0, one tick containing NOTECOUNT NoteOns followed by matching
    // NoteOffs. This exercises the black-MIDI keyboard compression path that
    // previously allocated an 8-byte sort key for every visual note.
    const trackLength = noteCount * 8 + 4;
    const bytes = new Uint8Array(14 + 8 + trackLength);
    let o = 0;
    const put = (...values) => {
        for (const value of values) bytes[o++] = value;
    };
    const put32 = value => {
        put(
            (value >>> 24) & 255,
            (value >>> 16) & 255,
            (value >>> 8) & 255,
            value & 255);
    };

    put(0x4d, 0x54, 0x68, 0x64);
    put32(6);
    put(0x00, 0x00, 0x00, 0x01, 0x01, 0xe0);
    put(0x4d, 0x54, 0x72, 0x6b);
    put32(trackLength);

    for (let i = 0; i < noteCount; ++i)
        put(0x00, 0x90, 60 + (i & 3), 100);
    for (let i = 0; i < noteCount; ++i)
        put(0x00, 0x80, 60 + (i & 3), 0);

    put(0x00, 0xff, 0x2f, 0x00);
    if (o !== bytes.length)
        throw new Error("Dense MIDI smoke generator length mismatch.");
    return bytes;
}

function makeSysexSeekMidi() {
    const track = Uint8Array.from([
        // GM System On at tick 0.
        0x00, 0xf0, 0x05, 0x7e, 0x7f, 0x09, 0x01, 0xf7,
        // Multi-bank selector state at tick 0: bank MSB 1, LSB 2, program 5.
        // Native SnappySynth commits Bank Select only on Program Change.
        0x00, 0xb0, 0x00, 0x01,
        0x00, 0xb0, 0x20, 0x02,
        0x00, 0xc0, 0x05,
        0x00, 0x90, 0x3c, 0x64,
        0x83, 0x60, 0x80, 0x3c, 0x00,
        0x00, 0xff, 0x2f, 0x00
    ]);
    const bytes = new Uint8Array(22 + track.length);
    let o = 0;
    const put = (...values) => {
        for (const value of values) bytes[o++] = value;
    };
    const put32 = value => put(
        (value >>> 24) & 255,
        (value >>> 16) & 255,
        (value >>> 8) & 255,
        value & 255);

    put(0x4d, 0x54, 0x68, 0x64);
    put32(6);
    put(0x00, 0x00, 0x00, 0x01, 0x01, 0xe0);
    put(0x4d, 0x54, 0x72, 0x6b);
    put32(track.length);
    bytes.set(track, o);
    return bytes;
}


function encodeVarLen(value) {
    let buffer = value & 0x7f;
    const bytes = [];
    while ((value >>>= 7) !== 0) {
        buffer <<= 8;
        buffer |= ((value & 0x7f) | 0x80);
    }
    while (true) {
        bytes.push(buffer & 0xff);
        if (buffer & 0x80)
            buffer >>>= 8;
        else
            break;
    }
    return bytes;
}

function makeHugeSparseMidiSource(payloadBytes = 240 * 1024 * 1024) {
    if (!Number.isSafeInteger(payloadBytes) ||
        payloadBytes <= 0 || payloadBytes > 0x0fffffff) {
        throw new Error("Invalid virtual MIDI payload size.");
    }

    const metaPrefix = Uint8Array.from([
        0x00, 0xff, 0x7f,
        ...encodeVarLen(payloadBytes)
    ]);
    const eot = Uint8Array.from([0x00, 0xff, 0x2f, 0x00]);
    const trackLength =
        metaPrefix.length + payloadBytes +
        metaPrefix.length + payloadBytes +
        eot.length;

    const header = new Uint8Array(22);
    let o = 0;
    const put = (...values) => {
        for (const value of values) header[o++] = value;
    };
    const put32 = value => put(
        (value >>> 24) & 255,
        (value >>> 16) & 255,
        (value >>> 8) & 255,
        value & 255);

    put(0x4d, 0x54, 0x68, 0x64);
    put32(6);
    put(0x00, 0x00, 0x00, 0x01, 0x01, 0xe0);
    put(0x4d, 0x54, 0x72, 0x6b);
    put32(trackLength);

    const trackStart = header.length;
    const prefix1 = trackStart;
    const prefix2 = prefix1 + metaPrefix.length + payloadBytes;
    const eotOffset = prefix2 + metaPrefix.length + payloadBytes;
    const size = eotOffset + eot.length;

    const segments = [
        [0, header],
        [prefix1, metaPrefix],
        [prefix2, metaPrefix],
        [eotOffset, eot]
    ];

    return {
        size,
        read(offset, count) {
            const out = new Uint8Array(count); // zero-filled skipped payload
            const readEnd = offset + count;
            for (const [segmentOffset, data] of segments) {
                const segmentEnd = segmentOffset + data.length;
                const begin = Math.max(offset, segmentOffset);
                const end = Math.min(readEnd, segmentEnd);
                if (begin >= end)
                    continue;
                out.set(
                    data.subarray(begin - segmentOffset, end - segmentOffset),
                    begin - offset);
            }
            return out;
        }
    };
}

// Synth feed (HANDOFF sec. 70): the sequential feed and a second module instance acting
// as a decode helper (compressed slabs shared as SharedArrayBuffers) must produce the same
// events, and admission must keep every note-off paired.
function feedWords(Module) {
    const count = sizeNumber(Module._wmp_event_batch_count_js());
    if (!count) return [];
    const ptr = sizeNumber(Module._wmp_event_batch_ptr_js());
    return Array.from(new Uint32Array(Module.HEAPU8.buffer, ptr, count * 2));
}

async function synthFeedSmoke(Module, createParser, denseMidi) {
    for (const name of ["_wmp_feed_config_js", "_wmp_feed_reset_js", "_wmp_feed_build_js",
        "_wmp_feed_safe_seconds_js", "_wmp_feed_complete_js", "_wmp_feed_block_js",
        "_wmp_feed_block_seconds_js", "_wmp_feed_buffer_js", "_wmp_feed_buffer_size_js",
        "_wmp_feed_buffer_ptr_js", "_wmp_feed_image_js", "_wmp_source_slab_count_js",
        "_wmp_source_slab_ptr_js", "_wmp_source_slab_bytes_js", "_wmp_feed_request_js",
        "_wmp_feed_stitch_js", "_wmp_feed_helper_init_js", "_wmp_feed_decode_js"]) {
        if (typeof Module[name] !== "function")
            throw new Error("Missing generated export " + name);
    }
    // Compressed source, as for multi-GB files.
    Module._wmp_set_raw_source_limit_js(0);
    if (!parsePaged(Module, denseMidi))
        throw new Error("Feed smoke could not parse: " + parserErrorText(Module, "?"));
    const run = (voices, admission) => {
        Module._wmp_feed_config_js(voices, admission, 48000, 512, 0, 4);
        Module._wmp_feed_reset_js(0);
        const words = [];
        let guard = 0;
        do {
            if (!Module._wmp_feed_build_js(10, 1 << 20))
                throw new Error("Feed build failed: " + parserErrorText(Module, "?"));
            words.push(...feedWords(Module));
        } while (!Module._wmp_feed_complete_js() && ++guard < 10000);
        return words;
    };
    // Admission off: every note survives (600k copies of one key fold into stacks).
    const all = run(1024, 0);
    let ons = 0, offs = 0;
    for (let i = 1; i < all.length; i += 2) {
        const m = all[i] >>> 0, n = (m >>> 24) + 1, type = m & 0xf0;
        if (type === 0x90) ons += n; else if (type === 0x80) offs += n;
    }
    if (ons !== 600000 || offs !== 600000)
        throw new Error("Feed without admission lost notes: " + ons + " on / " + offs + " off");
    const admitted = run(1024, 25);

    // Helper instance.
    if (!Module._wmp_feed_image_js())
        throw new Error("Feed image unavailable with a compressed source");
    const image = Module.HEAPU8.slice(sizeNumber(Module._wmp_feed_buffer_ptr_js()),
        sizeNumber(Module._wmp_feed_buffer_ptr_js()) + sizeNumber(Module._wmp_feed_buffer_size_js()));
    const slabBytes = sizeNumber(Module._wmp_source_slab_bytes_js());
    const slabs = [];
    for (let i = 0; i < sizeNumber(Module._wmp_source_slab_count_js()); ++i) {
        const ptr = sizeNumber(Module._wmp_source_slab_ptr_js(i));
        const sab = new SharedArrayBuffer(slabBytes);
        new Uint8Array(sab).set(Module.HEAPU8.subarray(ptr, ptr + slabBytes));
        slabs.push(sab);
    }
    const Helper = await createParser({ noInitialRun: true, noExitRuntime: true, print() {}, printErr() {} });
    globalThis.__wasmidiFeedSlabs = slabs;
    globalThis.__wasmidiFeedSlabBytes = slabBytes;
    Helper.HEAPU8.set(image, sizeNumber(Helper._wmp_feed_buffer_js(image.length)));
    if (!Helper._wmp_feed_helper_init_js(image.length))
        throw new Error("Feed helper init failed: " + parserErrorText(Helper, "?"));
    Module._wmp_feed_config_js(1024, 25, 48000, 512, 0, 4);
    Module._wmp_feed_reset_js(0);
    const parallel = [];
    let block = Number(Module._wmp_feed_block_js());
    for (let guard = 0; guard < 10000 && !Module._wmp_feed_complete_js(); ++guard) {
        if (!Module._wmp_feed_request_js(block, 4, 2))
            throw new Error("Feed request failed: " + parserErrorText(Module, "?"));
        const request = Module.HEAPU8.slice(sizeNumber(Module._wmp_feed_buffer_ptr_js()),
            sizeNumber(Module._wmp_feed_buffer_ptr_js()) + sizeNumber(Module._wmp_feed_buffer_size_js()));
        Helper.HEAPU8.set(request, sizeNumber(Helper._wmp_feed_buffer_js(request.length)));
        if (!Helper._wmp_feed_decode_js(request.length))
            throw new Error("Feed helper decode failed: " + parserErrorText(Helper, "?"));
        const result = Helper.HEAPU8.slice(sizeNumber(Helper._wmp_feed_buffer_ptr_js()),
            sizeNumber(Helper._wmp_feed_buffer_ptr_js()) + sizeNumber(Helper._wmp_feed_buffer_size_js()));
        Module.HEAPU8.set(result, sizeNumber(Module._wmp_feed_buffer_js(result.length)));
        if (!Module._wmp_feed_stitch_js(result.length))
            throw new Error("Feed stitch failed: " + parserErrorText(Module, "?"));
        parallel.push(...feedWords(Module));
        block += 8;
    }
    if (parallel.length !== admitted.length || parallel.some((w, i) => w !== admitted[i]))
        throw new Error("Feed helper output differs from the sequential feed (" +
            parallel.length + " vs " + admitted.length + " words)");
    Module._wmp_set_raw_source_limit_js(512 * 1024 * 1024);
}

// Visual stream (HANDOFF sec. 71): three tracks with echo copies and overlapping notes.
function makeVisualMidi() {
    const vlq = (out, value) => {
        const stack = [value & 0x7f];
        while ((value >>>= 7) > 0) stack.push((value & 0x7f) | 0x80);
        while (stack.length) out.push(stack.pop());
    };
    const tracks = [];
    for (let t = 0; t < 3; ++t) {
        const events = [];
        for (let i = 0; i < 6000; ++i) {
            const tick = i * 7 + t, key = 36 + ((i * 11 + t * 5) % 48), copies = t === 0 ? 3 : 1;
            for (let c = 0; c < copies; ++c) {
                events.push([tick, [0x90 | t, key, 64 + (i & 63)]]);
                events.push([tick + 20 + (i % 13), [0x80 | t, key, 0]]);
            }
            if (i % 97 === 0) events.push([tick, [0xb0 | t, 7, i & 127]]);
        }
        events.sort((a, b) => a[0] - b[0]);
        const body = [];
        let last = 0;
        for (const [tick, bytes] of events) { vlq(body, tick - last); last = tick; body.push(...bytes); }
        body.push(0, 0xff, 0x2f, 0);
        tracks.push(body);
    }
    const out = [0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 1, 0, 3, 0x01, 0xe0];
    for (const body of tracks) {
        const n = body.length;
        out.push(0x4d, 0x54, 0x72, 0x6b, (n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255);
        for (const b of body) out.push(b);
    }
    return Uint8Array.from(out);
}

function bufferBytes(M) {
    const ptr = sizeNumber(M._wmp_feed_buffer_ptr_js());
    return M.HEAPU8.slice(ptr, ptr + sizeNumber(M._wmp_feed_buffer_size_js()));
}

function putBuffer(M, bytes) {
    M.HEAPU8.set(bytes, sizeNumber(M._wmp_feed_buffer_js(bytes.length)));
}

function liveWords(M) {
    const count = sizeNumber(M._wmp_key_snapshot_word_count_js());
    return Array.from(new Uint32Array(M.HEAPU8.buffer, sizeNumber(M._wmp_key_snapshot_ptr_js()), count));
}

async function visualStreamSmoke(Module, createParser, denseMidi) {
    for (const name of ["_wmp_visual_config_js", "_wmp_visual_head_js", "_wmp_visual_done_js",
        "_wmp_visual_span_end_js", "_wmp_visual_request_js", "_wmp_visual_decode_js",
        "_wmp_visual_append_js"]) {
        if (typeof Module[name] !== "function")
            throw new Error("Missing generated export " + name);
    }
    Module._wmp_set_raw_source_limit_js(0);
    if (!parsePaged(Module, makeVisualMidi()))
        throw new Error("Visual smoke could not parse: " + parserErrorText(Module, "?"));
    const maxTick = new Float64Array(Module.HEAPU8.buffer, sizeNumber(Module._wmp_document_info_js()), 6)[0];
    if (!Module._wmp_feed_image_js())
        throw new Error("Visual smoke: no helper image");
    const image = bufferBytes(Module);
    const slabBytes = sizeNumber(Module._wmp_source_slab_bytes_js());
    const slabs = [];
    for (let i = 0; i < sizeNumber(Module._wmp_source_slab_count_js()); ++i) {
        const ptr = sizeNumber(Module._wmp_source_slab_ptr_js(i));
        const sab = new SharedArrayBuffer(slabBytes);
        new Uint8Array(sab).set(Module.HEAPU8.subarray(ptr, ptr + slabBytes));
        slabs.push(sab);
    }
    globalThis.__wasmidiFeedSlabs = slabs;
    globalThis.__wasmidiFeedSlabBytes = slabBytes;
    const helper = async () => {
        const H = await createParser({ noInitialRun: true, noExitRuntime: true, print() {}, printErr() {} });
        putBuffer(H, image);
        if (!H._wmp_feed_helper_init_js(image.length))
            throw new Error("Visual helper init failed: " + parserErrorText(H, "?"));
        return H;
    };
    const Spans = await helper();

    const sweep = (stream, spans) => {
        Module._wmp_visual_config_js(stream ? 1 : 0, spans ? 0 : 1);
        Module._wmp_reset_render_cursor_js(0, 0);
        const appends = [], closes = [];
        for (let guard = 0; guard < 100000; ++guard) {
            if (spans && !Module._wmp_visual_done_js()) {
                const head = Number(Module._wmp_visual_head_js());
                const end = Number(Module._wmp_visual_span_end_js(head, 3000));
                if (!Module._wmp_visual_request_js(head, end))
                    throw new Error("Visual request failed: " + parserErrorText(Module, "?"));
                putBuffer(Spans, bufferBytes(Module));
                if (!Spans._wmp_visual_decode_js(sizeNumber(Module._wmp_feed_buffer_size_js())))
                    throw new Error("Visual decode failed: " + parserErrorText(Spans, "?"));
                putBuffer(Module, bufferBytes(Spans));
                if (!Module._wmp_visual_append_js(sizeNumber(Spans._wmp_feed_buffer_size_js())))
                    throw new Error("Visual append failed: " + parserErrorText(Module, "?"));
            }
            if (!Module._wmp_build_render_sweep_js(maxTick, 2048))
                throw new Error("Visual sweep failed: " + parserErrorText(Module, "?"));
            const na = sizeNumber(Module._wmp_render_append_count_js());
            const nc = sizeNumber(Module._wmp_render_close_count_js());
            if (na) for (const w of new Uint32Array(Module.HEAPU8.buffer, sizeNumber(Module._wmp_render_append_ptr_js()), na * 3)) appends.push(w);
            if (nc) for (const w of new Uint32Array(Module.HEAPU8.buffer, sizeNumber(Module._wmp_render_close_ptr_js()), nc * 2)) closes.push(w);
            if (Module._wmp_render_batch_complete_js()) return { appends, closes };
        }
        throw new Error("Visual sweep did not finish");
    };
    const same = (a, b) => a.length === b.length && a.every((w, i) => w === b[i]);
    const merge = sweep(false, false), local = sweep(true, false), spans = sweep(true, true);
    if (!merge.appends.length)
        throw new Error("Visual smoke: the sweep produced no notes");
    for (const [name, r] of [["local", local], ["helper", spans]]) {
        if (!same(r.appends, merge.appends) || !same(r.closes, merge.closes))
            throw new Error("Visual stream (" + name + ") differs from the merge sweep: " +
                r.appends.length / 3 + "/" + merge.appends.length / 3 + " notes");
    }

    // Joint spans (HANDOFF sec. 85): one helper decode gives the feed result and the visual span.
    for (const name of ["_wmp_joint_decode_js", "_wmp_feed_block_tick_js", "_wmp_feed_block_at_tick_js"]) {
        if (typeof Module[name] !== "function")
            throw new Error("Missing generated export " + name);
    }
    const jointSweep = () => {
        Module._wmp_visual_config_js(1, 0);
        Module._wmp_reset_render_cursor_js(0, 0);
        Module._wmp_feed_config_js(1024, 25, 48000, 512, 0, 4);
        Module._wmp_feed_reset_js(0);
        let block = Number(Module._wmp_feed_block_js());
        if (Number(Module._wmp_feed_block_tick_js(block)) !== Number(Module._wmp_visual_head_js()))
            throw new Error("Joint smoke: the feed and the visual stream do not start together");
        const appends = [], closes = [];
        for (let guard = 0, k = 0; guard < 100000; ++guard) {
            if (!Module._wmp_visual_done_js()) {
                const chunks = 1 + (k++ % 3);
                if (!Module._wmp_feed_request_js(block, 4, chunks))
                    throw new Error("Joint request failed: " + parserErrorText(Module, "?"));
                const request = bufferBytes(Module);
                putBuffer(Spans, request);
                if (!Spans._wmp_feed_decode_js(request.length))
                    throw new Error("Feed decode failed: " + parserErrorText(Spans, "?"));
                const feed = bufferBytes(Spans);
                putBuffer(Spans, request);
                if (!Spans._wmp_joint_decode_js(request.length))
                    throw new Error("Joint decode failed: " + parserErrorText(Spans, "?"));
                const joint = bufferBytes(Spans);
                const feedSize = Number(new DataView(joint.buffer).getBigUint64(0, true));
                if (feedSize !== feed.length || !same(joint.subarray(8, 8 + feedSize), feed))
                    throw new Error("Joint feed result differs from the feed decode");
                const visual = joint.slice(8 + feedSize);
                putBuffer(Module, visual);
                if (!Module._wmp_visual_append_js(visual.length))
                    throw new Error("Joint visual append failed: " + parserErrorText(Module, "?"));
                block += 4 * chunks;
                const tick = Number(Module._wmp_feed_block_tick_js(block));
                const at = Number(Module._wmp_feed_block_at_tick_js(tick, 0));
                if (at > block || Number(Module._wmp_feed_block_tick_js(at)) !== tick ||
                    (at > 0 && Number(Module._wmp_feed_block_tick_js(at - 1)) >= tick))
                    throw new Error("Feed block at tick " + tick + " is " + at + ", expected the first block of " + block);
            }
            if (!Module._wmp_build_render_sweep_js(maxTick, 2048))
                throw new Error("Visual sweep failed: " + parserErrorText(Module, "?"));
            const na = sizeNumber(Module._wmp_render_append_count_js());
            const nc = sizeNumber(Module._wmp_render_close_count_js());
            if (na) for (const w of new Uint32Array(Module.HEAPU8.buffer, sizeNumber(Module._wmp_render_append_ptr_js()), na * 3)) appends.push(w);
            if (nc) for (const w of new Uint32Array(Module.HEAPU8.buffer, sizeNumber(Module._wmp_render_close_ptr_js()), nc * 2)) closes.push(w);
            if (Module._wmp_render_batch_complete_js()) return { appends, closes };
        }
        throw new Error("Joint sweep did not finish");
    };
    const joint = jointSweep();
    if (!same(joint.appends, merge.appends) || !same(joint.closes, merge.closes))
        throw new Error("Visual stream (joint) differs from the merge sweep: " +
            joint.appends.length / 3 + "/" + merge.appends.length / 3 + " notes");

    // Live state read from the helper-decoded stream against the merge path.
    const ticks = [];
    for (let t = 1000; t < maxTick - 100; t += 333) ticks.push(t);
    const snapshots = (stream) => {
        Module._wmp_visual_config_js(stream ? 1 : 0, stream ? 0 : 1);
        Module._wmp_reset_render_cursor_js(0, 0);
        return ticks.map((t, i) => {
            while (stream && !Module._wmp_visual_done_js() && Number(Module._wmp_visual_head_js()) <= t + 500) {
                const head = Number(Module._wmp_visual_head_js());
                if (!Module._wmp_visual_request_js(head, Number(Module._wmp_visual_span_end_js(head, 3000))))
                    throw new Error("Visual request failed: " + parserErrorText(Module, "?"));
                putBuffer(Spans, bufferBytes(Module));
                if (!Spans._wmp_visual_decode_js(sizeNumber(Module._wmp_feed_buffer_size_js())))
                    throw new Error("Visual decode failed: " + parserErrorText(Spans, "?"));
                putBuffer(Module, bufferBytes(Spans));
                if (!Module._wmp_visual_append_js(sizeNumber(Spans._wmp_feed_buffer_size_js())))
                    throw new Error("Visual append failed: " + parserErrorText(Module, "?"));
            }
            if (!Module._wmp_build_live_snapshot_js(t, Math.max(0, t - 120), Math.max(0, t - 480), i === 0 ? 1 : 0))
                throw new Error("Live snapshot failed: " + parserErrorText(Module, "?"));
            return liveWords(Module);
        });
    };
    const mergeLive = snapshots(false), streamLive = snapshots(true);
    for (let i = 0; i < ticks.length; ++i) {
        if (!same(streamLive[i], mergeLive[i]))
            throw new Error("Live state from the visual stream differs from the merge path at tick " + ticks[i]);
    }

    // Keyboard Worker (HANDOFF sec. 83): the same snapshots on a third instance that gets the
    // spans and the keys rebuilt by the coordinator on a reset.
    for (const name of ["_wmp_live_buffer_js", "_wmp_live_buffer_ptr_js", "_wmp_live_buffer_size_js",
        "_wmp_live_image_js", "_wmp_live_stream_export_js", "_wmp_live_rebuild_js", "_wmp_live_external_js",
        "_wmp_live_external_position_js", "_wmp_live_init_js", "_wmp_live_stream_import_js", "_wmp_live_restart_js",
        "_wmp_live_step_js", "_wmp_live_resume_js", "_wmp_live_position_js"]) {
        if (typeof Module[name] !== "function")
            throw new Error("Missing generated export " + name);
    }
    const liveBytes = M => {
        const ptr = sizeNumber(M._wmp_live_buffer_ptr_js());
        return M.HEAPU8.slice(ptr, ptr + sizeNumber(M._wmp_live_buffer_size_js()));
    };
    const putLive = (M, bytes) => M.HEAPU8.set(bytes, sizeNumber(M._wmp_live_buffer_js(bytes.length)));
    const Keys = await createParser({ noInitialRun: true, noExitRuntime: true, print() {}, printErr() {} });
    Module._wmp_visual_config_js(1, 0);
    Module._wmp_reset_render_cursor_js(0, 0);
    if (!Module._wmp_live_image_js())
        throw new Error("Keyboard image failed: " + parserErrorText(Module, "?"));
    const keysImage = liveBytes(Module);
    putLive(Keys, keysImage);
    if (!Keys._wmp_live_init_js(keysImage.length))
        throw new Error("Keyboard init failed: " + parserErrorText(Keys, "?"));
    if (!Module._wmp_live_stream_export_js())
        throw new Error("Visual stream export failed: " + parserErrorText(Module, "?"));
    const keysStream = liveBytes(Module);
    putLive(Keys, keysStream);
    if (!Keys._wmp_live_stream_import_js(keysStream.length))
        throw new Error("Visual stream import failed: " + parserErrorText(Keys, "?"));
    Module._wmp_live_external_js(1);
    let rebuilds = 0;
    const keysLive = ticks.map((t, i) => {
        while (!Module._wmp_visual_done_js() && Number(Module._wmp_visual_head_js()) <= t + 500) {
            const head = Number(Module._wmp_visual_head_js());
            if (!Module._wmp_visual_request_js(head, Number(Module._wmp_visual_span_end_js(head, 3000))))
                throw new Error("Visual request failed: " + parserErrorText(Module, "?"));
            putBuffer(Spans, bufferBytes(Module));
            if (!Spans._wmp_visual_decode_js(sizeNumber(Module._wmp_feed_buffer_size_js())))
                throw new Error("Visual decode failed: " + parserErrorText(Spans, "?"));
            const result = bufferBytes(Spans);
            putBuffer(Module, result);
            putBuffer(Keys, result);
            if (!Module._wmp_visual_append_js(result.length) || !Keys._wmp_visual_append_js(result.length))
                throw new Error("Visual append failed: " + parserErrorText(Module, "?") + " " + parserErrorText(Keys, "?"));
        }
        const nps = Math.max(0, t - 120), cc = Math.max(0, t - 480);
        let step = Keys._wmp_live_step_js(t, nps, cc, i === 0 ? 1 : 0);
        if (step === 2 || step === 3) {
            ++rebuilds;
            if (!Module._wmp_live_rebuild_js(t, nps, cc, step === 3 ? 1 : 0))
                throw new Error("Keyboard rebuild failed: " + parserErrorText(Module, "?"));
            const rebuilt = liveBytes(Module);
            putLive(Keys, rebuilt);
            step = Keys._wmp_live_resume_js(rebuilt.length, t, nps, cc) ? 1 : 0;
        }
        if (step !== 1)
            throw new Error("Keyboard step failed: " + parserErrorText(Keys, "?"));
        Module._wmp_live_external_position_js(Number(Keys._wmp_live_position_js()));
        return liveWords(Keys);
    });
    Module._wmp_live_external_js(0);
    if (!rebuilds)
        throw new Error("Keyboard Worker smoke never rebuilt the keys");
    for (let i = 0; i < ticks.length; ++i) {
        if (!same(keysLive[i], mergeLive[i]))
            throw new Error("Keyboard Worker state differs from the merge path at tick " + ticks[i]);
    }
    Module._wmp_visual_config_js(1, 1);
    Module._wmp_set_raw_source_limit_js(512 * 1024 * 1024);
    // The checks after this one use the dense file.
    if (!parsePaged(Module, denseMidi))
        throw new Error("Visual smoke could not restore the dense file: " + parserErrorText(Module, "?"));
}

async function main() {
    const jsPath = path.resolve(
        process.argv[2] ||
        path.join("build", "wasmidi-midi-parser.js"));

    if (!fs.existsSync(jsPath))
        throw new Error("Missing generated parser module: " + jsPath);

    delete require.cache[require.resolve(jsPath)];
    const createParser = require(jsPath);

    if (typeof createParser !== "function")
        throw new Error("Generated parser did not export a module factory.");

    let abortReason = "";
    const Module = await createParser({
        noInitialRun: true,
        noExitRuntime: true,
        onAbort(reason) {
            abortReason = String(reason || "unknown abort");
        },
        print() {},
        printErr(text) {
            if (text)
                process.stderr.write(String(text) + "\n");
        }
    });

    if (abortReason)
        throw new Error("Parser aborted during bootstrap: " + abortReason);

    for (const name of [
        "_wmp_parse_file_js",
        "_wmp_pack",
        "_wmp_result_ptr_js",
        "_wmp_result_size_js",
        "_wmp_release_result",
        "_wmp_build_visual_page_js",
        "_wmp_visual_page_ptr_js",
        "_wmp_visual_page_count_js",
        "_wmp_build_key_snapshot_js",
        "_wmp_build_live_snapshot_js",
        "_wmp_key_snapshot_ptr_js",
        "_wmp_key_snapshot_word_count_js",
        "_wmp_reset_event_cursor_js",
        "_wmp_build_event_batch_js",
        "_wmp_event_batch_ptr_js",
        "_wmp_event_batch_count_js",
        "_wmp_event_batch_time_ptr_js",
        "_wmp_event_batch_complete_js",
        "_wmp_event_batch_next_tick_js",
        "_wmp_event_batch_has_next_tick_js",
        "_wmp_build_historical_sysex_js",
        "_wmp_build_historical_selector_state_js",
        "_wmp_historical_selector_state_ptr_js",
        "_wmp_historical_selector_state_count_js",
        "_wmp_sysex_batch_event_ptr_js",
        "_wmp_sysex_batch_event_count_js",
        "_wmp_sysex_batch_data_ptr_js",
        "_wmp_sysex_batch_data_size_js",
        "_wmp_reset_render_cursor_js",
        "_wmp_build_render_sweep_js",
        "_wmp_render_append_ptr_js",
        "_wmp_render_append_count_js",
        "_wmp_render_append_base_js",
        "_wmp_render_close_ptr_js",
        "_wmp_render_close_count_js",
        "_wmp_render_batch_complete_js",
        "_wmp_render_batch_next_tick_js",
        "_wmp_render_batch_has_next_tick_js",
        "_wmp_tick_to_seconds_js",
        "_wmp_error_ptr_js",
        "_wmp_error_size_js",
        "_wmp_pointer_bits",
    ]) {
        if (typeof Module[name] !== "function")
            throw new Error("Missing generated export " + name);
    }

    parserPointerBits = Number(Module._wmp_pointer_bits()) | 0;
    if (parserPointerBits !== 64)
        throw new Error("Generated Pass 13.11.0 parser is not Memory64.");

    // Valid format-0 MIDI: header + one track containing only EndOfTrack.
    const midi = Uint8Array.from([
        0x4d, 0x54, 0x68, 0x64,
        0x00, 0x00, 0x00, 0x06,
        0x00, 0x00,
        0x00, 0x01,
        0x01, 0xe0,
        0x4d, 0x54, 0x72, 0x6b,
        0x00, 0x00, 0x00, 0x04,
        0x00, 0xff, 0x2f, 0x00
    ]);

    // Exercise the production paged-source path: raw MIDI bytes stay outside
    // the wasm heap and C++ requests only bounded source windows.
    if (!parsePaged(Module, midi)) {
        throw new Error(
            "Parser rejected smoke-test MIDI: " +
            parserErrorText(Module, "unknown parse failure"));
    }
    if (pagedReadCalls === 0 || pagedMaxRead > SOURCE_WINDOW_BYTES)
        throw new Error("Paged parser bootstrap exceeded its 8 MiB source window.");

    if (!Module._wmp_pack()) {
        throw new Error(
            "Parser could not pack smoke-test MIDI: " +
            parserErrorText(Module, "unknown pack failure"));
    }

    const resultPtr = Module._wmp_result_ptr_js();
    const resultSizeRaw = Module._wmp_result_size_js();
    if (typeof resultPtr !== "number" || typeof resultSizeRaw !== "number")
        throw new Error("Parser result facade leaked a non-Number wasm64 value.");
    const resultSize = sizeNumber(resultSizeRaw);
    if (!resultPtr || resultSize === 0)
        throw new Error("Parser produced an empty serialized document.");

    Module._wmp_release_result();

    const validProgress = parserProgressMessages.some(message =>
        message &&
        message.type === "progress" &&
        typeof message.stage === "string" &&
        message.stage.length > 0 &&
        Number.isFinite(Number(message.percent)));
    if (!validProgress) {
        throw new Error(
            "Parser smoke test did not receive a decoded string progress stage.");
    }

    // Memory64 progress callbacks must legalize const char* to Number before
    // entering EM_JS. A raw wasm64 pointer would surface as BigInt and make
    // Emscripten's UTF8ToString assert before this point.
    if (parserProgressMessages.some(message =>
            message && typeof message.stage !== "string")) {
        throw new Error(
            "Parser progress callback leaked a non-string Memory64 stage value.");
    }

    // Mapped SysEx stays resident beside the Memory64 event store rather than
    // being copied into Qt's packed metadata. Exercise both forward playback
    // and historical seek recovery so a GM/GS state regression fails CI.
    const sysexMidi = makeSysexSeekMidi();
    if (!parsePaged(Module, sysexMidi)) {
        throw new Error(
            "Parser rejected SysEx seek MIDI: " +
            parserErrorText(Module, "unknown SysEx parse failure"));
    }
    Module._wmp_reset_event_cursor_js(0);
    if (!Module._wmp_build_event_batch_js(0, 64) ||
        sizeNumber(Module._wmp_sysex_batch_event_count_js()) !== 1 ||
        sizeNumber(Module._wmp_sysex_batch_data_size_js()) !== 6) {
        throw new Error("Mapped playback batch lost tick-0 GM SysEx.");
    }
    if (!Module._wmp_build_historical_sysex_js(1) ||
        sizeNumber(Module._wmp_sysex_batch_event_count_js()) !== 1 ||
        sizeNumber(Module._wmp_sysex_batch_data_size_js()) !== 6) {
        throw new Error("Mapped seek history lost GM SysEx state.");
    }
    if (!Module._wmp_build_historical_selector_state_js(1))
        throw new Error("Mapped seek could not rebuild bank/program selector state.");
    const selectorCount = sizeNumber(
        Module._wmp_historical_selector_state_count_js());
    const selectorPtr = Number(Module._wmp_historical_selector_state_ptr_js());
    if (!selectorPtr || selectorCount < 3)
        throw new Error("Mapped seek returned no historical bank/program state.");
    let sawMsb = false;
    let sawLsb = false;
    let sawProgram = false;
    for (let i = 0; i < selectorCount; ++i) {
        const packed = Module.HEAPU32[(selectorPtr + i * 8 + 4) >>> 2] >>> 0;
        if ((packed & 0x00ffffff) === 0x000100b0) sawMsb = true;
        if ((packed & 0x00ffffff) === 0x000220b0) sawLsb = true;
        if ((packed & 0x0000ffff) === 0x000005c0) sawProgram = true;
    }
    if (!sawMsb || !sawLsb || !sawProgram)
        throw new Error("Mapped seek did not preserve SF2 bank/program latch state.");

    // Regression for the browser failure that motivated Pass 12.8/12.9: the old
    // Worker tried to grow a 64 MiB heap directly to ~500 MiB just to hold the
    // raw file. This virtual ~480 MiB MIDI contains two large skipped meta
    // payloads but never exists as one JS/WASM allocation. The generated module
    // must parse it through <=8 MiB windows and produce a tiny document.
    const hugeSparseMidi = makeHugeSparseMidiSource();
    if (hugeSparseMidi.size < 480 * 1024 * 1024)
        throw new Error("Huge paged-source smoke MIDI is unexpectedly small.");

    if (!parsePaged(Module, hugeSparseMidi)) {
        throw new Error(
            "Parser rejected huge paged-source smoke MIDI: " +
            parserErrorText(Module, "unknown parse failure"));
    }
    if (pagedReadCalls < 3 || pagedMaxRead > SOURCE_WINDOW_BYTES) {
        throw new Error(
            "Huge MIDI parsing escaped the 8 MiB bounded-source window.");
    }

    if (!Module._wmp_pack()) {
        throw new Error(
            "Parser could not pack huge paged-source MIDI: " +
            parserErrorText(Module, "unknown pack failure"));
    }
    const hugeResultSize = sizeNumber(Module._wmp_result_size_js());
    if (!Module._wmp_result_ptr_js() || hugeResultSize === 0 ||
        hugeResultSize > 1024 * 1024) {
        throw new Error(
            "Huge sparse MIDI produced an unexpected document size: " +
            hugeResultSize);
    }
    Module._wmp_release_result();

    // Exercise a real dense crashpoint after the bootstrap MIDI. Keeping this
    // reasonably small makes CI fast, while still covering counted keyboard
    // starts/ends, zero-length minimum duration, phased parse/pack, and reuse
    // after releasing the previous serialized result.
    const denseMidi = makeDenseCrashMidi(600000);
    if (!parsePaged(Module, denseMidi)) {
        throw new Error(
            "Parser rejected dense smoke-test MIDI: " +
            parserErrorText(Module, "unknown parse failure"));
    }

    // The dense source remains larger than the old 4 MiB page size and must still
    // go through the read-at facade rather than being copied into the wasm heap.
    // Pass 13.2.3 deliberately widened the mapped-store cache page to 8 MiB to
    // reduce FileReaderSync/JS<->WASM crossings, so this ~4.8 MiB source may fit
    // in one mapped page. The huge sparse probe above is the multi-page check.
    if (denseMidi.length <= 4 * 1024 * 1024 ||
        pagedReadCalls < 1 ||
        pagedMaxRead > SOURCE_WINDOW_BYTES) {
        throw new Error(
            "Dense paged-source smoke test did not honor the 8 MiB window cap.");
    }

    // Pass 13's critical invariant: the parsed source remains a mapped store,
    // not a monolithic event/note document. Exercise every bounded consumer
    // directly against the same File-backed index before packing metadata.
    if (!Module._wmp_build_visual_page_js(0, 1) ||
        sizeNumber(Module._wmp_visual_page_count_js()) === 0) {
        throw new Error(
            "Mapped parser could not build a visual page after dense indexing: " +
            parserErrorText(Module, "unknown visual-page failure"));
    }

    // Pass 13.2.2 regression: Emscripten 3.1.56 MEMORY64 used to calculate
    // a fractional Memory.grow page count here (for example
    // 10.999984741210938) and BigInt() aborted. The dense visual page is large
    // enough to force the 64 MiB initial heap to grow.
    // The BPFA store streams the source into compressed blocks and may not need
    // more than the initial heap; force the growth path explicitly.
    if (Module.HEAPU8.buffer.byteLength <= 64 * 1024 * 1024 &&
        !Module._wmp_memory_growth_probe_js(96)) {
        throw new Error("Memory64 growth probe allocation failed.");
    }
    const grownHeapBytes = Module.HEAPU8.buffer.byteLength;
    if (grownHeapBytes <= 64 * 1024 * 1024 ||
        (grownHeapBytes % (64 * 1024)) !== 0) {
        throw new Error(
            "Memory64 growth regression probe did not produce an aligned heap " +
            "larger than 64 MiB: " + grownHeapBytes);
    }

    if (!Module._wmp_build_key_snapshot_js(0) ||
        sizeNumber(Module._wmp_key_snapshot_word_count_js()) !== 384) {
        throw new Error(
            "Mapped parser did not produce the fixed 384-word keyboard state.");
    }

    Module._wmp_reset_event_cursor_js(0);
    if (!Module._wmp_build_event_batch_js(0, 65536) ||
        sizeNumber(Module._wmp_event_batch_count_js()) === 0) {
        throw new Error(
            "Mapped parser could not stream a bounded playback batch.");
    }

    await synthFeedSmoke(Module, createParser, denseMidi);
    await visualStreamSmoke(Module, createParser, denseMidi);

    Module._wmp_reset_render_cursor_js(0, 1);
    if (!Module._wmp_build_render_sweep_js(1, 65536) ||
        sizeNumber(Module._wmp_render_append_count_js()) === 0) {
        throw new Error(
            "Mapped parser could not stream SharpMIDI renderer ring deltas.");
    }

    if (!Module._wmp_pack()) {
        throw new Error(
            "Parser could not pack dense smoke-test MIDI: " +
            parserErrorText(Module, "unknown pack failure"));
    }

    const denseResultSize = sizeNumber(Module._wmp_result_size_js());
    if (!Module._wmp_result_ptr_js() || denseResultSize === 0)
        throw new Error("Dense parser smoke test produced an empty document.");

    // Pass 13 sends metadata only. Even 600k source notes must not make the
    // Qt wire image scale with event/note count.
    if (denseResultSize > 2 * 1024 * 1024)
        throw new Error(
            "Dense parser smoke-test document unexpectedly expanded to " +
            denseResultSize + " bytes.");

    Module._wmp_release_result();

    console.log(
        "MIDI parser Pass 13.11.0 SharpMIDI-ring/streaming-playback smoke test OK");
}

main().catch(error => {
    console.error(error && error.stack ? error.stack : error);
    process.exitCode = 1;
});
