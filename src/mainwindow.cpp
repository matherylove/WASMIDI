#include "mainwindow.hpp"

#include "midi/midi_document_codec.hpp"

#include <QFile>
#include <QFileInfo>
#include <QTimer>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace {

MainWindow* g_browserMainWindow = nullptr;

const QColor DefaultAccent(0x6b, 0x55, 0xd4);

const QColor DefaultChannelColors[16] = {
    QColor("#818cf8"), QColor("#a78bfa"), QColor("#c4b5fd"), QColor("#fb923c"),
    QColor("#4ade80"), QColor("#38bdf8"), QColor("#f472b6"), QColor("#facc15"),
    QColor("#f87171"), QColor("#34d399"), QColor("#60a5fa"), QColor("#e879f9"),
    QColor("#fb7185"), QColor("#a3e635"), QColor("#22d3ee"), QColor("#fbbf24")
};

} // namespace

#ifdef __EMSCRIPTEN__

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_browser_file_selected(
    const unsigned char* data,
    int dataSize,
    const char* fileName,
    int fileNameSize)
{
    if (!g_browserMainWindow ||
        !data ||
        dataSize <= 0) {
        return;
    }

    const QString name =
        fileName && fileNameSize > 0
            ? QString::fromUtf8(
                fileName,
                fileNameSize)
            : QStringLiteral("browser.mid");

    g_browserMainWindow->loadMidiRaw(
        data,
        static_cast<std::size_t>(dataSize),
        name);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_browser_colors_imported(const uint32_t* rgb, int count)
{
    if (g_browserMainWindow && rgb && count > 0)
        g_browserMainWindow->applyImportedColors(rgb, count);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_browser_loading_progress(
    int percent,
    const char* stage,
    int stageSize)
{
    if (!g_browserMainWindow)
        return;

    const QString text =
        stage && stageSize > 0
            ? QString::fromUtf8(stage, stageSize)
            : QStringLiteral("Loading MIDI");

    g_browserMainWindow->setMidiLoadingProgress(
        percent,
        text);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_browser_loading_failed(
    const char* message,
    int messageSize)
{
    if (!g_browserMainWindow)
        return;

    g_browserMainWindow->failMidiLoading(
        message && messageSize > 0
            ? QString::fromUtf8(message, messageSize)
            : QStringLiteral("Could not parse MIDI"));
}

extern "C" EMSCRIPTEN_KEEPALIVE
int wasmidi_browser_parsed_midi_selected(
    const unsigned char* data,
    double dataSize,
    const char* fileName,
    int fileNameSize)
{
    if (!g_browserMainWindow ||
        !data ||
        !std::isfinite(dataSize) ||
        dataSize <= 0.0 ||
        dataSize > static_cast<double>(
            std::numeric_limits<std::size_t>::max())) {
        return 0;
    }

    const QString name =
        fileName && fileNameSize > 0
            ? QString::fromUtf8(fileName, fileNameSize)
            : QStringLiteral("browser.mid");

    return g_browserMainWindow->loadMidiSerializedRaw(
        data,
        static_cast<std::size_t>(dataSize),
        name)
        ? 1
        : 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_visual_key_page_ready(
    uint32_t generation,
    uint32_t spanTicks,
    uint32_t pageIndex,
    const uint32_t* words,
    uint32_t wordCount)
{
    if (!g_browserMainWindow)
        return;

    g_browserMainWindow->receiveKeyboardVisualPage(
        generation,
        spanTicks,
        pageIndex,
        words,
        wordCount);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_remote_key_state_ready(
    double tick,
    const uint32_t* words,
    uint32_t wordCount)
{
    if (!g_browserMainWindow)
        return;

    g_browserMainWindow->receiveRemoteKeyState(
        tick,
        words,
        wordCount);
}

// Browser MIDI loading is delegated to a cache-busted Worker. The Worker keeps
// the File outside WebAssembly and lets the parser read bounded windows, so no
// allocation proportional to the raw MIDI file size occurs on the Qt thread.
EM_JS(void, wasmidi_browser_open_file_picker, (int kind), {
    /*
     * One single native browser file picker for BOTH MIDI and SF2.
     *
     * kind == 0 -> MIDI
     * kind == 1 -> SoundFont 2
     *
     * This is deliberately the exact same DOM/user-gesture path for both
     * formats. The only differences are the accept filter and what happens
     * after a File has already been selected.
     */
    const isSfz =
        (kind | 0) === 2;
    const isSf2 =
        (kind | 0) === 1 || isSfz;

    console.debug(
        '[WASMIDI picker] opening',
        isSf2 ? 'SF2' : 'MIDI');

    const input =
        document.createElement(
            'input');

    input.type = 'file';
    if (isSfz) {
        input.webkitdirectory = true;
        input.multiple = true;
    }

    input.accept =
        isSf2
            ? '.sf2'
            : '.mid,.midi,audio/midi,audio/x-midi';

    input.style.position = 'fixed';
    input.style.left = '-10000px';
    input.style.top = '-10000px';
    input.style.width = '1px';
    input.style.height = '1px';
    input.style.opacity = '0';

    document.body.appendChild(
        input);

    const cleanup = () => {
        try {
            input.onchange = null;

            if (input.parentNode)
                input.parentNode.removeChild(
                    input);
        } catch (_) {}
    };

    input.onchange = async () => {
        const file =
            input.files &&
            input.files.length
                ? input.files[0]
                : null;

        if (!file) {
            cleanup();
            return;
        }

        if (isSf2) {
            try {
                let bridge =
                    globalThis.WasmidiSnappyBridge;

                if (!bridge) {
                    await new Promise(
                        (resolve, reject) => {
                            const script =
                                document.createElement(
                                    'script');

                            script.src =
                                './snappysynth_bridge.js?v=13.12.0';

                            script.async = false;
                            script.onload = resolve;
                            script.onerror = reject;

                            document.head.appendChild(
                                script);
                        });

                    bridge =
                        globalThis.WasmidiSnappyBridge;
                }

                if (!bridge ||
                    typeof bridge.loadSoundfontFile !==
                        'function') {
                    throw new Error(
                        'SnappySynthV2 bridge is unavailable.');
                }

                if (isSfz)
                    await bridge.loadSoundfontFolder(
                        Array.from(input.files));
                else
                    await bridge.loadSoundfontFile(
                        file);
            } catch (error) {
                console.error(
                    '[WASMIDI] Could not load selected SF2:',
                    error);
            } finally {
                cleanup();
            }

            return;
        }

        const callWithUtf8 =
            (callback, first, text) => {
                const bytes =
                    new TextEncoder().encode(
                        String(text || ''));

                const ptr =
                    _malloc(
                        Math.max(1, bytes.length));

                if (!ptr)
                    return;

                if (bytes.length)
                    HEAPU8.set(bytes, ptr);

                try {
                    callback(first, ptr, bytes.length);
                } finally {
                    _free(ptr);
                }
            };

        const setProgress =
            (percent, stage) => {
                callWithUtf8(
                    _wasmidi_browser_loading_progress,
                    Math.max(0, Math.min(100, percent | 0)),
                    stage || 'Loading MIDI');
            };

        let worker = null;
        let parsedInstallPtr = 0;
        let parsedInstallSize = 0;
        let parsedInstallOffset = 0;
        let parsedInstallName = '';
        // Once Qt has accepted the mapped metadata, this loader transaction is
        // terminal. The persistent parser Worker remains alive for runtime page
        // requests, so stale/queued progress messages must not be allowed to
        // reopen the loading overlay.
        let installComplete = false;

        const releaseParsedInstall = () => {
            if (parsedInstallPtr) {
                try { _free(parsedInstallPtr); } catch (_) {}
            }
            parsedInstallPtr = 0;
            parsedInstallSize = 0;
            parsedInstallOffset = 0;
            parsedInstallName = '';
        };

        const failLoading =
            (text) => {
                releaseParsedInstall();
                const bytes =
                    new TextEncoder().encode(
                        String(text || 'Could not parse MIDI'));
                const ptr = _malloc(Math.max(1, bytes.length));
                if (!ptr) {
                    _wasmidi_browser_loading_failed(0, 0);
                    return;
                }
                if (bytes.length)
                    HEAPU8.set(bytes, ptr);
                try {
                    _wasmidi_browser_loading_failed(ptr, bytes.length);
                } finally {
                    _free(ptr);
                }
            };

        try {
            if (typeof Worker !== 'function') {
                throw new Error(
                    'This browser does not support the MIDI parser Worker.');
            }

            setProgress(0, 'Starting MIDI loader');

            // Only one mapped MIDI source may own the player at a time. The
            // previous Worker holds the old File/Blob and its sparse indexes;
            // terminate it before opening another file so RAM is released
            // immediately instead of waiting for GC.
            const previousMapped = globalThis.__wasmidiMappedMidi;
            if (previousMapped && previousMapped.worker) {
                try { previousMapped.worker.terminate(); } catch (_) {}
            }
            if (previousMapped && previousMapped.visualWorker) {
                try { previousMapped.visualWorker.terminate(); } catch (_) {}
            }
            globalThis.__wasmidiMappedMidi = null;
            if (globalThis.WasmidiSnappyBridge && globalThis.WasmidiSnappyBridge.clearLocalMidiSource)
                globalThis.WasmidiSnappyBridge.clearLocalMidiSource();

            // Fetch the Worker source explicitly with cache:no-store instead of
            // trusting the browser/Pages HTTP cache for a Worker constructor.
            // This prevents a successful earlier deployment from silently running
            // the old monolithic "allocate file.size" loader.
            const parserWorkerUrl =
                new URL('./midi-parser-worker.js?v=13.11.0', window.location.href);
            const parserWorkerResponse =
                await fetch(parserWorkerUrl.href, { cache: 'no-store' });
            if (!parserWorkerResponse.ok) {
                throw new Error(
                    'Could not fetch current MIDI parser Worker (' +
                    parserWorkerResponse.status + ').');
            }

            const parserWorkerSource = await parserWorkerResponse.text();
            if (!parserWorkerSource.includes(
                    'WASMIDI_MIDI_PARSER_BOOTSTRAP = "13.11.0"')) {
                throw new Error(
                    'GitHub Pages returned a stale MIDI parser Worker. ' +
                    'Expected bootstrap 13.11.0.');
            }

            const parserBaseUrl =
                new URL('./', window.location.href).href;
            // The page query reaches the Worker for its overrides (?helpers=N, ?rawlimit=MiB).
            const parserWorkerPrelude =
                'self.__wasmidiMidiParserBaseUrl = ' +
                JSON.stringify(parserBaseUrl) + ';\n' +
                'self.__wasmidiPageQuery = ' +
                JSON.stringify(window.location.search) + ';\n';
            const createParserWorker = () => {
                const blobUrl = URL.createObjectURL(
                    new Blob(
                        [parserWorkerPrelude, parserWorkerSource],
                        { type: 'text/javascript' }));
                try {
                    return new Worker(blobUrl);
                } finally {
                    URL.revokeObjectURL(blobUrl);
                }
            };

            worker = createParserWorker();
            // Keep a stable identity for the long-lived mapped Worker. The
            // mutable `worker` variable is deliberately cleared after the
            // result is installed so cleanup does not terminate the mapped
            // source. Any later onmessage callback must therefore never use
            // that mutable variable to authenticate the sender.
            const parserWorker = worker;

            const deliverVisualPage = (message) => {
                const payload = new Uint8Array(
                    message.data || new ArrayBuffer(0));
                const count = Math.floor(payload.byteLength / 12);
                let ptr = 0;
                try {
                    if (payload.byteLength) {
                        ptr = _malloc(payload.byteLength);
                        if (!ptr) return;
                        HEAPU8.set(payload, ptr);
                    }
                    _wasmidi_visual_page_ready(
                        message.generation >>> 0,
                        message.spanTicks >>> 0,
                        message.pageIndex >>> 0,
                        ptr,
                        count >>> 0,
                        Number(message.sourceCount) >>> 0,
                        Number(message.difficulty) || 0.0);
                } finally {
                    if (ptr) _free(ptr);
                }
            };

            const deliverSharpRenderDelta = (message) => {
                const appendPayload = new Uint8Array(
                    message.appends || new ArrayBuffer(0));
                const closePayload = new Uint8Array(
                    message.closes || new ArrayBuffer(0));
                const appendCount = Math.floor(appendPayload.byteLength / 12);
                const closeCount = Math.floor(closePayload.byteLength / 8);
                let appendPtr = 0;
                let closePtr = 0;
                try {
                    if (appendPayload.byteLength) {
                        appendPtr = _malloc(appendPayload.byteLength);
                        if (!appendPtr) return;
                        HEAPU8.set(appendPayload, appendPtr);
                    }
                    if (closePayload.byteLength) {
                        closePtr = _malloc(closePayload.byteLength);
                        if (!closePtr) return;
                        HEAPU8.set(closePayload, closePtr);
                    }
                    _wasmidi_sharp_render_delta_ready(
                        message.generation >>> 0,
                        Number(message.appendBase) >>> 0,
                        appendPtr,
                        appendCount >>> 0,
                        closePtr,
                        closeCount >>> 0,
                        Number(message.safeThrough) >>> 0,
                        message.complete ? 1 : 0);
                } finally {
                    if (appendPtr) _free(appendPtr);
                    if (closePtr) _free(closePtr);
                }
            };

            const deliverKeyState = (message) => {
                const payload = new Uint8Array(
                    message.data || new ArrayBuffer(0));
                const wordCount = Math.floor(payload.byteLength / 4);
                let ptr = 0;
                try {
                    if (payload.byteLength) {
                        ptr = _malloc(payload.byteLength);
                        if (!ptr) return;
                        HEAPU8.set(payload, ptr);
                    }
                    _wasmidi_remote_key_state_ready(
                        Number(message.tick) || 0.0,
                        ptr,
                        wordCount >>> 0);
                } finally {
                    if (ptr) _free(ptr);
                }
            };

            const finishKeyRequest = (ownerWorker) => {
                const mapped = globalThis.__wasmidiMappedMidi;
                if (!mapped || mapped.keyOwner !== ownerWorker)
                    return;

                mapped.keyPending = false;
                mapped.keyOwner = null;
                mapped.keyInFlightRequestId = 0;
                if (!mapped.keyQueued)
                    return;

                const request = mapped.keyQueued;
                mapped.keyQueued = null;
                const target = mapped.worker;
                if (!target)
                    return;
                mapped.keyPending = true;
                mapped.keyOwner = target;
                mapped.keyInFlightRequestId = Number(request.requestId) >>> 0;
                target.postMessage(request);
            };

            parserWorker.onmessage =
                (event) => {
                    const message =
                        event.data || {};

                    // Pass 13 keeps this Worker alive after loading. It is the
                    // browser equivalent of SharpMIDI's memory-mapped backing
                    // store and serves bounded renderer/keyboard/audio pages.
                    if (message.type === 'sharp-render-reset') {
                        _wasmidi_sharp_render_reset_ready(
                            message.generation >>> 0);
                        return;
                    }

                    if (message.type === 'sharp-render-delta') {
                        deliverSharpRenderDelta(message);
                        return;
                    }

                    if (message.type === 'visual-page') {
                        deliverVisualPage(message);
                        return;
                    }

                    if (message.type === 'live-state') {
                        const mapped = globalThis.__wasmidiMappedMidi;
                        if (mapped && mapped.worker === parserWorker &&
                            (Number(message.requestId) >>> 0) ===
                                (Number(mapped.keyInFlightRequestId) >>> 0) &&
                            (Number(message.generation) >>> 0) ===
                                (Number(mapped.keyGeneration) >>> 0)) {
                            deliverKeyState(message);
                        }
                        finishKeyRequest(parserWorker);
                        return;
                    }

                    if (message.type === 'synth-sysex-history') {
                        const mapped = globalThis.__wasmidiMappedMidi;
                        if (!mapped || mapped.worker !== parserWorker ||
                            (Number(message.generation) >>> 0) !==
                                (Number(mapped.synthGeneration) >>> 0)) {
                            return;
                        }
                        const bridge = globalThis.WasmidiSnappyBridge;
                        if (bridge && typeof bridge.scheduleSysExBatch === 'function') {
                            const meta = new Uint32Array(message.meta || new ArrayBuffer(0));
                            const bytes = new Uint8Array(message.data || new ArrayBuffer(0));
                            const count = Math.floor(meta.length / 3);
                            const times = new Float64Array(count);
                            times.fill(Math.max(0, Number(message.time) || 0));
                            bridge.scheduleSysExBatch(meta, bytes, times);
                        }
                        return;
                    }

                    if (message.type === 'synth-pump-deferred') {
                        const mapped = globalThis.__wasmidiMappedMidi;
                        if (!mapped || mapped.worker !== parserWorker ||
                            (Number(message.generation) >>> 0) !==
                                (Number(mapped.synthGeneration) >>> 0)) {
                            return;
                        }
                        // Keep the logical request queued and release the
                        // in-flight latch. The matching reset ACK will dispatch
                        // it after the Worker commits the generation/cursor.
                        const deferred = {
                            type: 'synth-pump',
                            generation: Number(message.generation) >>> 0,
                            endTick: Math.max(0, Number(message.endTick) || 0),
                            safeUntil: Math.max(0, Number(message.safeUntil) || 0),
                            velocityFloor: Math.max(0, Math.min(127,
                                Number(message.velocityFloor) | 0)),
                            maxBatches: 8
                        };
                        if (!mapped.synthQueued ||
                            deferred.endTick >= Number(mapped.synthQueued.endTick || 0)) {
                            mapped.synthQueued = deferred;
                        }
                        mapped.synthPumpPending = false;
                        mapped.synthResetPending = true;
                        return;
                    }

                    if (message.type === 'synth-cursor-reset') {
                        const mapped = globalThis.__wasmidiMappedMidi;
                        if (!mapped || mapped.worker !== parserWorker ||
                            (Number(message.generation) >>> 0) !==
                                (Number(mapped.synthGeneration) >>> 0)) {
                            return;
                        }

                        // A Worker onmessage handler may be async; posting
                        // synth-reset followed immediately by synth-pump does
                        // NOT guarantee that resetSynthCursor() has completed
                        // before pumpSynthWindow() begins.  Treat this ACK as a
                        // real scheduling barrier.  Without it the first pump
                        // can observe the previous generation, return silently,
                        // and leave synthPumpPending latched forever: the
                        // AudioWorklet then renders valid silence with 0 active
                        // voices and 0 underruns.
                        mapped.synthResetPending = false;
                        mapped.synthPumpPending = false;

                        if (mapped.synthQueued) {
                            const queued = mapped.synthQueued;
                            mapped.synthQueued = null;
                            if ((Number(queued.generation) >>> 0) ===
                                (Number(mapped.synthGeneration) >>> 0)) {
                                mapped.synthPumpPending = true;
                                mapped.worker.postMessage(queued);
                            }
                        }
                        return;
                    }

                    if (message.type === 'synth-batch') {
                        const mapped = globalThis.__wasmidiMappedMidi;
                        if (!mapped || mapped.worker !== parserWorker ||
                            (Number(message.generation) >>> 0) !==
                                (Number(mapped.synthGeneration) >>> 0)) {
                            return;
                        }
                        const bridge = globalThis.WasmidiSnappyBridge;
                        if (bridge && typeof bridge.scheduleBatch === 'function') {
                            bridge.scheduleBatch(
                                new Uint32Array(message.messages || new ArrayBuffer(0)),
                                new Float64Array(message.times || new ArrayBuffer(0)),
                                Number(message.safeUntil) || 0.0,
                                new Uint32Array(message.sysexMeta || new ArrayBuffer(0)),
                                new Uint8Array(message.sysexData || new ArrayBuffer(0)),
                                new Float64Array(message.sysexTimes || new ArrayBuffer(0)));
                        } else {
                            // Compatibility with an older cached bridge.  Queue
                            // SysEx first so startup cannot render a late GM/GS
                            // reset after the short-message batch.
                            if (bridge && typeof bridge.scheduleSysExBatch === 'function') {
                                const meta = new Uint32Array(
                                    message.sysexMeta || new ArrayBuffer(0));
                                const bytes = new Uint8Array(
                                    message.sysexData || new ArrayBuffer(0));
                                const times = new Float64Array(
                                    message.sysexTimes || new ArrayBuffer(0));
                                if (meta.length && bytes.length && times.length)
                                    bridge.scheduleSysExBatch(meta, bytes, times);
                            }
                            if (bridge && typeof bridge.schedule === 'function') {
                                bridge.schedule(
                                    new Uint32Array(message.messages || new ArrayBuffer(0)),
                                    new Float64Array(message.times || new ArrayBuffer(0)),
                                    Number(message.safeUntil) || 0.0);
                            }
                        }

                        if (message.complete) {
                            if (mapped && mapped.worker === parserWorker) {
                                mapped.synthPumpPending = false;
                                if (!mapped.synthResetPending && mapped.synthQueued) {
                                    const queued = mapped.synthQueued;
                                    mapped.synthQueued = null;
                                    mapped.synthPumpPending = true;
                                    mapped.worker.postMessage(queued);
                                }
                            }
                        }
                        return;
                    }

                    if (message.type === 'synth-more') {
                        const mapped = globalThis.__wasmidiMappedMidi;
                        if (mapped && mapped.worker === parserWorker &&
                            (Number(message.generation) >>> 0) ===
                                (Number(mapped.synthGeneration) >>> 0)) {
                            mapped.worker.postMessage({
                                type: 'synth-pump',
                                generation: mapped.synthGeneration >>> 0,
                                endTick: Number(message.endTick) || 0,
                                safeUntil: Number(message.safeUntil) || 0,
                                velocityFloor: Number(message.velocityFloor != null
                                    ? message.velocityFloor
                                    : mapped.synthVelocityFloor) || 0,
                                maxBatches: 8
                            });
                        }
                        return;
                    }

                    if (message.type === 'runtime-error') {
                        const mapped = globalThis.__wasmidiMappedMidi;
                        if (mapped && mapped.worker === parserWorker) {
                            mapped.synthResetPending = false;
                            mapped.synthPumpPending = false;
                            mapped.synthQueued = null;
                            // Never leave the per-frame live-state coalescer
                            // latched after a worker-side exception. The old
                            // keyPending latch was one way the piano could
                            // remain forever on its first successful frame.
                            if (String(message.requestType || '') === 'key-state')
                                finishKeyRequest(parserWorker);
                        }
                        console.error(
                            '[WASMIDI mapped MIDI worker]',
                            message.message || 'runtime error');
                        return;
                    }

                    if (message.type === 'worker-ready') {
                        console.info(
                            '[WASMIDI MIDI parser] worker bootstrap',
                            String(message.bootstrap || '?'),
                            message.pagedSource === true ? 'paged-source' : 'legacy-source');
                        if (String(message.bootstrap || '') !== '13.11.0' ||
                            message.pagedSource !== true ||
                            message.mappedStore !== true) {
                            failLoading(
                                'Stale or incompatible MIDI parser Worker loaded. ' +
                                'Expected mapped-source bootstrap 13.11.0.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                        }
                        return;
                    }

                    if (message.type === 'progress') {
                        if (!installComplete) {
                            setProgress(
                                Number(message.percent) || 0,
                                message.stage || 'Loading MIDI');
                        }
                        return;
                    }

                    if (message.type === 'error') {
                        failLoading(
                            message.message || 'Could not parse MIDI');

                        if (worker) {
                            worker.terminate();
                            worker = null;
                        }
                        cleanup();
                        return;
                    }

                    if (message.type === 'result-begin') {
                        releaseParsedInstall();

                        const total = Number(message.size);
                        if (!Number.isSafeInteger(total) || total <= 0) {
                            failLoading('Parser returned an invalid document size.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                            return;
                        }

                        // Qt itself is still wasm32. Give it the full 4 GiB
                        // address space instead of the historical 2 GiB default,
                        // but never allow JS -> i32 truncation for a >4 GiB wire
                        // image. The Memory64 parser can be much larger because
                        // it no longer shares this heap.
                        if (total > 0xffffffff) {
                            failLoading(
                                'Parsed MIDI document is larger than the 4 GiB Qt wasm32 address space. ' +
                                'The Memory64 parser accepted it, but this file requires segmented player residency.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                            return;
                        }

                        parsedInstallPtr =
                            _malloc(Math.max(1, total)) >>> 0;
                        if (!parsedInstallPtr) {
                            failLoading(
                                'The browser/OS could not commit enough memory to install the parsed MIDI in the player.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                            return;
                        }

                        parsedInstallSize = total;
                        parsedInstallOffset = 0;
                        parsedInstallName =
                            String(message.name || file.name || 'browser.mid');
                        setProgress(95, 'Streaming parsed MIDI into player memory');
                        return;
                    }

                    if (message.type === 'result-chunk') {
                        if (!parsedInstallPtr ||
                            !(message.data instanceof ArrayBuffer)) {
                            failLoading('Unexpected parsed-MIDI chunk.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                            return;
                        }

                        const offset = Number(message.offset);
                        const bytes = new Uint8Array(message.data);
                        if (!Number.isSafeInteger(offset) ||
                            offset !== parsedInstallOffset ||
                            offset + bytes.length > parsedInstallSize) {
                            failLoading('Parsed-MIDI transfer became out of order.');
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                            return;
                        }

                        HEAPU8.set(bytes, parsedInstallPtr + offset);
                        parsedInstallOffset += bytes.length;
                        return;
                    }

                    if (message.type !== 'result-end')
                        return;

                    if (!parsedInstallPtr ||
                        parsedInstallOffset !== parsedInstallSize ||
                        Number(message.size) !== parsedInstallSize) {
                        failLoading('Parsed-MIDI transfer ended before all bytes arrived.');
                        if (worker) worker.terminate();
                        worker = null;
                        cleanup();
                        return;
                    }

                    const nameBytes =
                        new TextEncoder().encode(
                            parsedInstallName || file.name || 'browser.mid');
                    const namePtr =
                        _malloc(Math.max(1, nameBytes.length));

                    if (!namePtr) {
                        failLoading('Could not allocate MIDI filename memory.');
                        if (worker) worker.terminate();
                        worker = null;
                        cleanup();
                        return;
                    }

                    if (nameBytes.length)
                        HEAPU8.set(nameBytes, namePtr);

                    setProgress(96, 'Installing parsed MIDI');

                    try {
                        const installed =
                            _wasmidi_browser_parsed_midi_selected(
                                parsedInstallPtr,
                                parsedInstallSize,
                                namePtr,
                                nameBytes.length);

                        if (!installed) {
                            // loadMidiSerializedRaw() has already surfaced the
                            // detailed C++ error. Do not publish a mapped runtime
                            // whose Qt-side metadata failed to install.
                            if (worker) worker.terminate();
                            worker = null;
                            cleanup();
                            return;
                        }

                        // Metadata is now installed in Qt, while the authoritative
                        // channel-event/file store remains in this Memory64 Worker.
                        // Reuse the already-built sparse index for visual, keyboard
                        // and synth requests instead of parsing the File a second time.
                        const primaryWorker = parserWorker;
                        const mappedState = {
                            worker: primaryWorker,
                            visualWorker: null,
                            visualReady: false,
                            visualPrime: null,
                            bootstrap: '13.11.0',
                            mappedStore: true,
                            keyPending: false,
                            keyOwner: null,
                            keyQueued: null,
                            keyRequestId: 0,
                            keyInFlightRequestId: 0,
                            keyGeneration: 1,
                            synthResetPending: false,
                            synthPumpPending: false,
                            synthQueued: null,
                            synthVelocityFloor: 0,
                            synthGeneration: 0
                        };
                        globalThis.__wasmidiMappedMidi = mappedState;
                        worker = null;
                        installComplete = true;
                        const synthBridge = globalThis.WasmidiSnappyBridge;
                        if (synthBridge && typeof synthBridge.setLocalMidiSource === 'function')
                            synthBridge.setLocalMidiSource(file, primaryWorker);

                        // Make the JS transaction terminal as well as the C++
                        // transaction. This is intentionally after mappedState is
                        // published, so the first frame can request its live state
                        // immediately when the loading overlay disappears.
                        setProgress(100, 'Ready');

                        // One authoritative mapped parser Worker owns the source.
                        // Visual pages, keyboard snapshots and synth event batches are
                        // short cooperative jobs on this same indexed store. The visual
                        // prefetch loop yields after every screen, so audio/key requests
                        // can run between screens without indexing the entire MIDI twice.
                    } finally {
                        _free(namePtr);
                        releaseParsedInstall();
                        if (worker) worker.terminate();
                        worker = null;
                        cleanup();
                    }
                };

            worker.onerror =
                (event) => {
                    failLoading(
                        event && event.message
                            ? event.message
                            : 'MIDI parser Worker failed.');
                    if (worker) worker.terminate();
                    worker = null;
                    cleanup();
                };

            // Roll mode and look-ahead limit (HANDOFF sec. 89), as the controls saved them.
            const rollSetting = (key, fallback) => {
                try {
                    const raw = localStorage.getItem('wasmidi.settings.v1.' + key);
                    const value = Number(raw);
                    return raw != null && Number.isFinite(value) ? value : fallback;
                } catch (_) {
                    return fallback;
                }
            };
            worker.postMessage({
                type: 'parse',
                file,
                name: file.name || 'browser.mid',
                rollMode: rollSetting('rollMode', 1) === 2 ? 2 : 1,
                lookAheadMB: rollSetting('rollLookAheadMB', 1024)
            });
        } catch (error) {
            failLoading(
                error && error.message
                    ? error.message
                    : String(error));

            if (worker)
                worker.terminate();

            cleanup();
        }
    };

    /*
     * IMPORTANT: this is synchronous and happens before AudioContext,
     * Worker creation, promises, bridge loading, or file processing.
     */
    input.click();
});

// Persistent mapped-MIDI Worker controls. These calls are intentionally tiny:
// the File and compact sparse indexes never cross into Qt's wasm32 heap.
EM_JS(void, wasmidi_mapped_shutdown, (), {
    const mapped = globalThis.__wasmidiMappedMidi;
    if (mapped && mapped.worker) {
        try { mapped.worker.terminate(); } catch (_) {}
    }
    if (mapped && mapped.visualWorker) {
        try { mapped.visualWorker.terminate(); } catch (_) {}
    }
    globalThis.__wasmidiMappedMidi = null;
    if (globalThis.WasmidiSnappyBridge && globalThis.WasmidiSnappyBridge.clearLocalMidiSource)
        globalThis.WasmidiSnappyBridge.clearLocalMidiSource();
});

EM_JS(void, wasmidi_mapped_request_key_state,
    (double tick, double npsStartTick, double ccStartTick, int forceReset), {
    const mapped = globalThis.__wasmidiMappedMidi;
    if (!mapped || !mapped.worker || !mapped.mappedStore)
        return;
    const targetTick = Math.max(0, Number(tick) || 0);
    if (forceReset) {
        mapped.keyGeneration = ((Number(mapped.keyGeneration) >>> 0) + 1) >>> 0;
        if (mapped.keyGeneration === 0)
            mapped.keyGeneration = 1;
    }
    const request = {
        type: 'key-state',
        tick: targetTick,
        npsStartTick: Math.max(0, Math.min(targetTick, Number(npsStartTick) || 0)),
        ccStartTick: Math.max(0, Math.min(targetTick, Number(ccStartTick) || 0)),
        requestId: ((Number(mapped.keyRequestId) >>> 0) + 1) >>> 0,
        generation: Number(mapped.keyGeneration) >>> 0,
        reset: !!forceReset
    };
    if (request.requestId === 0)
        request.requestId = 1;
    mapped.keyRequestId = request.requestId;
    if (mapped.keyPending) {
        // One outstanding Worker job plus exactly one newest queued frame. Old
        // GUI samples are useless once a newer transport position exists. A
        // seek/reset bit is sticky until that queued request actually runs.
        if (mapped.keyQueued && mapped.keyQueued.reset)
            request.reset = true;
        mapped.keyQueued = request;
        return;
    }
    mapped.keyPending = true;
    mapped.keyOwner = mapped.worker;
    mapped.keyInFlightRequestId = request.requestId >>> 0;
    mapped.worker.postMessage(request);
});

EM_JS(void, wasmidi_mapped_synth_reset, (double tick), {
    const mapped = globalThis.__wasmidiMappedMidi;
    if (!mapped || !mapped.worker || !mapped.mappedStore)
        return;
    const localBridge = globalThis.WasmidiSnappyBridge;
    if (localBridge && localBridge.state && localBridge.state.localSynthActive)
        return;
    mapped.synthGeneration = ((Number(mapped.synthGeneration) >>> 0) + 1) >>> 0;
    if (mapped.synthGeneration === 0)
        mapped.synthGeneration = 1;
    // Do not allow a new event pump to race this asynchronous Worker reset.
    // The Worker replies with synth-cursor-reset only after its resident event
    // cursor and generation are committed. wasmidi_mapped_synth_pump() queues
    // the furthest requested horizon until that ACK arrives.
    mapped.synthResetPending = true;
    mapped.synthPumpPending = false;
    mapped.synthQueued = null;
    mapped.worker.postMessage({
        type: 'synth-reset',
        generation: mapped.synthGeneration >>> 0,
        tick: Math.max(0, Number(tick) || 0)
    });
});

EM_JS(void, wasmidi_mapped_synth_pump,
      (double endTick, double safeUntil, int velocityFloor), {
    const mapped = globalThis.__wasmidiMappedMidi;
    if (!mapped || !mapped.worker || !mapped.mappedStore)
        return;
    // The synth has its own MIDI feeder (HANDOFF sec. 26); only the velocity floor crosses here.
    const localBridge = globalThis.WasmidiSnappyBridge;
    if (localBridge && localBridge.state && localBridge.state.localSynthActive) {
        localBridge.setLocalVelocityFloor(velocityFloor | 0);
        return;
    }

    const request = {
        type: 'synth-pump',
        generation: Number(mapped.synthGeneration) >>> 0,
        endTick: Math.max(0, Number(endTick) || 0),
        safeUntil: Math.max(0, Number(safeUntil) || 0),
        velocityFloor: Math.max(0, Math.min(127, velocityFloor | 0)),
        maxBatches: 8
    };
    mapped.synthVelocityFloor = request.velocityFloor;

    const queueFurthest = () => {
        if (!mapped.synthQueued ||
            request.endTick >= Number(mapped.synthQueued.endTick || 0)) {
            mapped.synthQueued = request;
        }
    };

    // resetSynthCursor() is async. Never post a pump for the new generation
    // until the Worker explicitly ACKs that reset; otherwise the pump may see
    // the old generation and be dropped while synthPumpPending remains stuck.
    if (mapped.synthResetPending) {
        queueFurthest();
        return;
    }

    if (mapped.synthPumpPending) {
        // Keep only the furthest requested horizon; this mirrors the visual
        // page cache and prevents a dense MIDI from building an unbounded JS
        // message backlog while the Worker is still decoding.
        queueFurthest();
        return;
    }

    mapped.synthPumpPending = true;
    mapped.worker.postMessage(request);
});


EM_JS(int, wasmidi_snappy_ready, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state && b.state.ready ? 1 : 0;
});

EM_JS(int, wasmidi_snappy_soundfont_loaded, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state && b.state.soundfontLoaded ? 1 : 0;
});

EM_JS(int, wasmidi_snappy_sample_rate, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.sampleRate) | 0) : 0;
});

EM_JS(double, wasmidi_snappy_audio_clock, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (!b || typeof b.getAudioClock !== 'function')
        return -1.0;
    const value = Number(b.getAudioClock());
    return Number.isFinite(value) ? value : -1.0;
});

EM_JS(int, wasmidi_snappy_active_voices, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.activeVoices) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_free_voices, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.freeVoices) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_steals, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.steals) | 0) : 0;
});

// Free voices handed back to the global pool per second. Non-zero while the
// per-worker freelists are being rebalanced; zero with notes vanishing means
// the workers are hoarding.
EM_JS(int, wasmidi_snappy_rebalanced, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.rebalanced) | 0) : 0;
});

// Note-ons that produced no sound: no free voice anywhere and nothing to
// steal. Cumulative on purpose; a lost note should stay visible.
EM_JS(int, wasmidi_snappy_dropped_notes, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.droppedNotes) | 0) : 0;
});

// Render telemetry. Carried in the bridge state rather than printed: console
// output from the synth worker at this rate costs the main thread real time.
// Scaled by 10 so the value survives the int round trip with one decimal.
EM_JS(int, wasmidi_snappy_render_load_x10, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? Math.round((Number(b.state.renderLoadPercent) || 0) * 10) : 0;
});

EM_JS(int, wasmidi_snappy_render_latency_x100, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? Math.round((Number(b.state.lastRenderMs) || 0) * 100) : 0;
});

EM_JS(int, wasmidi_snappy_dispatch_x100, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? Math.round((Number(b.state.lastDispatchMs) || 0) * 100) : 0;
});

// Dominant reason voices missed the vectorized path, encoded for one readout:
// 0 none, 1 interpolation, 2 looping, 3 filter, 4 other. Low byte is the
// percentage that reason accounts for.
// Voices recycled as a percentage of note-ons that got a voice, cumulative.
// Trails 100 by roughly the voices currently sounding; drifting steadily
// downward over time means voices are not coming back at all.
// Presampling outcome packed for one readout: resampled << 16 | skipped.
// Skipped regions, or zero resampled on the first load where a reload reports
// many, is the first-load slowdown.
EM_JS(void, wasmidi_snappy_set_debug_metrics, (int enabled), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && typeof b.setDebugMetrics === "function")
        b.setDebugMetrics(!!enabled);
});

EM_JS(int, wasmidi_snappy_cc_hotspot, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.ccHotspot) | 0) : 0;
});
EM_JS(int, wasmidi_snappy_cc_hotspot_count, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.ccHotspotCount) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_cc_collapsed, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.controllersCollapsed) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_presample, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (!b || !b.state) return 0;
    const res = Number(b.state.presampleResampled) || 0;
    const skip = Number(b.state.presampleSkipped) || 0;
    return ((res & 0xffff) << 16) | (skip & 0xffff);
});

EM_JS(int, wasmidi_snappy_presample_seen, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.presampleSeen) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_free_ratio, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (!b || !b.state) return 100;
    const started = Number(b.state.notesStarted) || 0;
    const recycled = Number(b.state.voicesRecycled) || 0;
    if (started <= 0) return 100;
    return Math.round((recycled * 100) / started);
});

EM_JS(int, wasmidi_snappy_alloc_x10, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? Math.round((Number(b.state.allocMs) || 0) * 10) : 0;
});

EM_JS(int, wasmidi_snappy_workers_active, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.workersParticipating) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_miss_reason, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (!b || !b.state) return 0;
    const v = [
        Number(b.state.missInterp) || 0,
        Number(b.state.missLoop) || 0,
        Number(b.state.missFilter) || 0,
        Number(b.state.missOther) || 0
    ];
    const total = v[0] + v[1] + v[2] + v[3];
    if (total <= 0) return 0;
    let best = 0;
    for (let i = 1; i < 4; ++i) if (v[i] > v[best]) best = i;
    const pct = Math.round((v[best] * 100) / total);
    return ((best + 1) << 8) | (pct & 0xff);
});

EM_JS(int, wasmidi_snappy_path_simd_percent, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (!b || !b.state) return 0;
    const fast = Number(b.state.pathSimdVoiceVoices) || 0;
    const scalar = Number(b.state.pathScalarVoices) || 0;
    const total = fast + scalar;
    return total > 0 ? Math.round((fast * 100) / total) : 0;
});

EM_JS(int, wasmidi_snappy_worker_busy_x10, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? Math.round((Number(b.state.workerBusyMs) || 0) * 10) : 0;
});

EM_JS(int, wasmidi_snappy_pump_gap_ms, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? Math.round(Number(b.state.pumpGapMs) || 0) : 0;
});

EM_JS(int, wasmidi_snappy_ring_fill, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? Math.round(Number(b.state.ringFillPercent) || 0) : 0;
});

EM_JS(int, wasmidi_snappy_layers, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.layers) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_regions, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.regions) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_worker_count, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.workerCount) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_underruns, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state ? (Number(b.state.underruns) | 0) : 0;
});

EM_JS(int, wasmidi_snappy_starved, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state && b.state.starved ? 1 : 0;
});

EM_JS(double, wasmidi_snappy_render_load, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state
        ? Math.max(0.0, Number(b.state.renderLoad) || 0.0)
        : 0.0;
});

EM_JS(int, wasmidi_snappy_producer_overloaded, (), {
    const b = globalThis.WasmidiSnappyBridge;
    return b && b.state && b.state.producerOverloaded ? 1 : 0;
});

EM_JS(int, wasmidi_mapped_synth_pending, (), {
    const mapped = globalThis.__wasmidiMappedMidi;
    return mapped && mapped.mappedStore && mapped.synthPumpPending ? 1 : 0;
});

EM_JS(void, wasmidi_snappy_sync_visual_clock, (double time), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.syncVisualClock)
        b.syncVisualClock(Number(time) || 0.0);
});

EM_JS(int, wasmidi_snappy_copy_string, (int which, char* dst, int capacity), {
    if (!dst || capacity <= 0)
        return 0;
    const b = globalThis.WasmidiSnappyBridge;
    let text = '';
    if (b && b.state) {
        text = which === 0
            ? String(b.state.soundfontName || '')
            : String(b.state.status || '');
    }
    stringToUTF8(text, dst, capacity);
    return lengthBytesUTF8(text);
});



EM_JS(void, wasmidi_snappy_clear_soundfonts, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.clearSoundfonts)
        b.clearSoundfonts();
});

EM_JS(void, wasmidi_snappy_schedule_sysex,
      (const uint8_t* data, int length, double time), {
    const b = globalThis.WasmidiSnappyBridge;
    if (!b || !b.scheduleSysEx || !data || length <= 0)
        return;
    const bytes = new Uint8Array(length);
    bytes.set(HEAPU8.subarray(data, data + length));
    b.scheduleSysEx(bytes, Number(time) || 0.0);
});

EM_JS(void, wasmidi_snappy_play, (double time, int reset), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.play)
        b.play(time, !!reset);
});

EM_JS(void, wasmidi_snappy_pause, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.pause)
        b.pause();
});

EM_JS(void, wasmidi_snappy_stop, (), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.stop)
        b.stop();
});

EM_JS(void, wasmidi_snappy_local_floor, (int value), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.setLocalVelocityFloor)
        b.setLocalVelocityFloor(value | 0);
});

EM_JS(void, wasmidi_snappy_seek, (double time), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.seek)
        b.seek(time);
});

EM_JS(void, wasmidi_snappy_set_prebuffer, (double seconds, double duration), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.setPrebuffer)
        b.setPrebuffer(Number(seconds) || 0.0, Number(duration) || 0.0);
});

EM_JS(void, wasmidi_snappy_set_volume, (int percent), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.setVolume)
        b.setVolume(percent);
});

EM_JS(void, wasmidi_snappy_configure,
      (int maxVoices,
       int minVoices,
       int blockFrames,
       int numBuffers,
       int requestedSampleRate,
       int channels,
       int bitsPerSample,
       int realtimePriority,
       int workers,
       int noteSharding,
       int stealScoreCache,
       int debugMetrics,
       int fastNoteOff,
       int validateState,
       int softClip), {
    const b = globalThis.WasmidiSnappyBridge;

    if (!b || !b.configure)
        return;

    b.configure({
        maxVoices,
        minVoices,
        blockFrames,
        numBuffers,
        requestedSampleRate,
        channels,
        bitsPerSample,
        realtimePriority: !!realtimePriority,
        workers,
        noteSharding,
        stealScoreCache: !!stealScoreCache,
        debugMetrics: !!debugMetrics,
        fastNoteOff: !!fastNoteOff,
        validateState: !!validateState,
        softClip: !!softClip
    });
});

EM_JS(void, wasmidi_browser_toggle_full_screen, (), {
    const warn = (e) => console.warn("[WASMIDI] full screen:", e);
    try {
        const p = document.fullscreenElement
            ? document.exitFullscreen()
            : document.documentElement.requestFullscreen({ navigationUI: "hide" });
        if (p && p.catch)
            p.catch(warn);
    } catch (e) {
        warn(e);
    }
});

// Reads the 16 note colors of a config.xml (Visual/Colors/Color R G B), as
// the BPFA-style players write it.
EM_JS(void, wasmidi_browser_import_color_config, (), {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.xml,text/xml,application/xml';
    input.style.position = 'fixed';
    input.style.left = '-10000px';
    document.body.appendChild(input);
    input.onchange = async () => {
        const file = input.files && input.files.length ? input.files[0] : null;
        input.remove();
        if (!file)
            return;
        try {
            const doc = new DOMParser().parseFromString(await file.text(), 'application/xml');
            const colors = [];
            const root = doc.documentElement;
            const visual = root ? Array.from(root.children).find((e) => e.tagName === 'Visual') : null;
            const list = visual ? Array.from(visual.children).find((e) => e.tagName === 'Colors') : null;
            for (const c of list ? Array.from(list.children) : []) {
                if (c.tagName !== 'Color' || colors.length >= 16)
                    continue;
                const v = (n) => Math.max(0, Math.min(255, parseInt(c.getAttribute(n), 10) || 0));
                colors.push((v('R') << 16) | (v('G') << 8) | v('B'));
            }
            if (!colors.length) {
                console.warn('[WASMIDI] config.xml: no Visual/Colors/Color entries in', file.name);
                return;
            }
            const ptr = _malloc(colors.length * 4);
            HEAPU32.set(colors, ptr >>> 2);
            try {
                _wasmidi_browser_colors_imported(ptr, colors.length);
            } finally {
                _free(ptr);
            }
        } catch (e) {
            console.warn('[WASMIDI] config.xml:', e);
        }
    };
    input.click();
});

EM_JS(void, wasmidi_browser_download_text, (const char* name, const char* data, int size), {
    const blob = new Blob([HEAPU8.slice(data, data + size)], { type: 'application/xml' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = UTF8ToString(name);
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
});

EM_JS(void, wasmidi_snappy_set_gpu, (int enabled), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.setGpu)
        b.setGpu(!!enabled);
});

EM_JS(void, wasmidi_snappy_set_overlap_gain, (int enabled), {
    const b = globalThis.WasmidiSnappyBridge;
    if (b && b.setOverlapGain)
        b.setOverlapGain(!!enabled);
});


// Browser settings are deliberately stored in localStorage rather than cookies:
// they never need to be sent with HTTP requests, the capacity is larger, and
// the values survive reloads/restarts under the same site origin.
EM_JS(double, wasmidi_setting_number,
      (const char* key, double fallback), {
    try {
        const name = 'wasmidi.settings.v1.' + UTF8ToString(key);
        const raw = localStorage.getItem(name);
        if (raw == null)
            return fallback;
        const value = Number(raw);
        return Number.isFinite(value) ? value : fallback;
    } catch (_) {
        return fallback;
    }
});

EM_JS(void, wasmidi_setting_set_number,
      (const char* key, double value), {
    try {
        const name = 'wasmidi.settings.v1.' + UTF8ToString(key);
        localStorage.setItem(name, String(Number(value)));
    } catch (_) {}
});

EM_JS(void, wasmidi_snappy_schedule,
      (const uint32_t* messages, const double* times, int count, double safeUntil), {
    const b = globalThis.WasmidiSnappyBridge;
    if (!b || !b.schedule)
        return;

    const n = Math.max(0, count | 0);
    const m = new Uint32Array(n);
    const t = new Float64Array(n);

    if (n > 0) {
        m.set(HEAPU32.subarray(messages >>> 2, (messages >>> 2) + n));
        t.set(HEAPF64.subarray(times >>> 3, (times >>> 3) + n));
    }

    b.schedule(m, t, Number(safeUntil) || 0.0);
});

#endif

#ifdef __EMSCRIPTEN__
#define WASMIDI_PERSIST_SETTING(key, value) \
    wasmidi_setting_set_number((key), static_cast<double>(value))
#else
#define WASMIDI_PERSIST_SETTING(key, value) ((void)0)
#endif

MainWindow::MainWindow(QObject* parent)
    : QObject(parent)
{
    g_browserMainWindow = this;

    channelColors_.reserve(16);

    for (const auto& color : DefaultChannelColors)
        channelColors_.push_back(color);

#ifdef __EMSCRIPTEN__
    // Restore without going through setters: construction should perform one
    // coherent backend configure, not restart the synth once per property.
    noteSpeed_ = std::clamp<float>(
        static_cast<float>(wasmidi_setting_number("noteSpeed", noteSpeed_)),
        0.1f, 60.0f);
    postBuffer_ = std::clamp<float>(
        static_cast<float>(wasmidi_setting_number("postBuffer", postBuffer_)),
        0.0f, 10.0f);
    postBufferAuto_ = wasmidi_setting_number(
        "postBufferAuto", postBufferAuto_ ? 1.0 : 0.0) != 0.0;
    perTrackColors_ = wasmidi_setting_number(
        "perTrackColors", perTrackColors_ ? 1.0 : 0.0) != 0.0;
    adaptColors_ = wasmidi_setting_number(
        "adaptColors", adaptColors_ ? 1.0 : 0.0) != 0.0;
    volume_ = std::clamp<int>(
        static_cast<int>(std::lround(wasmidi_setting_number("volume", volume_))),
        0, 100);

    synthMinVoices_ = std::clamp<int>(static_cast<int>(std::lround(
        wasmidi_setting_number("synthMinVoices", synthMinVoices_))), 0, 5000000);
    synthMaxVoices_ = std::clamp<int>(static_cast<int>(std::lround(
        wasmidi_setting_number("synthMaxVoices", synthMaxVoices_))),
        std::max(1, synthMinVoices_), 5000000);
    synthBufferFrames_ = std::clamp<int>(static_cast<int>(std::lround(
        wasmidi_setting_number("synthBufferFrames", synthBufferFrames_))), 1, 65536);
    synthNumBuffers_ = std::clamp<int>(static_cast<int>(std::lround(
        wasmidi_setting_number("synthNumBuffers", synthNumBuffers_))), 1, 128);
    synthVelocityFloor_ = std::clamp<int>(static_cast<int>(std::lround(
        wasmidi_setting_number("synthVelocityFloor", synthVelocityFloor_))), 0, 127);
    synthEffectiveVelocityFloor_ = synthVelocityFloor_;
    skippedVelocity_ = synthVelocityFloor_;
    const int restoredRate = static_cast<int>(std::lround(
        wasmidi_setting_number("synthRequestedSampleRate", synthRequestedSampleRate_)));
    synthRequestedSampleRate_ = restoredRate <= 0 ? 0 : std::clamp(restoredRate, 8000, 384000);
    synthChannels_ = wasmidi_setting_number("synthChannels", synthChannels_) == 1.0 ? 1 : 2;
    synthBitsPerSample_ = wasmidi_setting_number(
        "synthBitsPerSample", synthBitsPerSample_) == 16.0 ? 16 : 32;
    synthRealtimePriority_ = wasmidi_setting_number(
        "synthRealtimePriority", synthRealtimePriority_ ? 1.0 : 0.0) != 0.0;
    synthWorkers_ = std::clamp<int>(static_cast<int>(std::lround(
        wasmidi_setting_number("synthWorkers", synthWorkers_))), 0, 256);
    synthNoteSharding_ = std::clamp<int>(static_cast<int>(std::lround(
        wasmidi_setting_number("synthNoteSharding", synthNoteSharding_))), 0, 3);
    synthStealScoreCache_ = wasmidi_setting_number(
        "synthStealScoreCache", synthStealScoreCache_ ? 1.0 : 0.0) != 0.0;
    synthFastNoteOff_ = wasmidi_setting_number(
        "synthFastNoteOff", synthFastNoteOff_ ? 1.0 : 0.0) != 0.0;
    synthValidateState_ = wasmidi_setting_number(
        "synthValidateState", synthValidateState_ ? 1.0 : 0.0) != 0.0;
    synthSoftClip_ = wasmidi_setting_number(
        "synthSoftClip", synthSoftClip_ ? 1.0 : 0.0) != 0.0;
    synthOverlapGain_ = wasmidi_setting_number(
        "synthStackGain", synthOverlapGain_ ? 1.0 : 0.0) != 0.0;
    synthGpu_ = wasmidi_setting_number("synthGpu", synthGpu_ ? 1.0 : 0.0) != 0.0;
    rollMode_ = wasmidi_setting_number("rollMode", rollMode_) == 2.0 ? 2 : 1;
    rollLookAheadMB_ = int(std::clamp(wasmidi_setting_number("rollLookAheadMB", rollLookAheadMB_), 64.0, 16384.0));
    neuralRoll_ = float(std::clamp(wasmidi_setting_number("neuralRoll", neuralRoll_), 0.0, 1.0));
    neuralPage_ = float(std::clamp(wasmidi_setting_number("neuralPage", neuralPage_), 0.0, 1.0));

    for (int i = 0; i < channelColors_.size(); ++i) {
        const QByteArray key = QByteArray("channelColor.") + QByteArray::number(i);
        const double packed = wasmidi_setting_number(key.constData(), -1.0);
        if (packed >= 0.0 && packed <= double(0x00ffffffu))
            channelColors_[i] = QColor::fromRgb(static_cast<QRgb>(std::llround(packed)));
    }
#endif

    visualPitchColor_.fill(-1);

    auto* frameTimer = new QTimer(this);
    frameTimer->setTimerType(Qt::PreciseTimer);

    connect(
        frameTimer,
        &QTimer::timeout,
        this,
        &MainWindow::updateCurrentTime);

    frameTimer->start(16);

    // SnappySynth is scheduled in coarse 20 ms batches. dispatchScheduler()
    // walks CompactEvent directly and transfers only the new ~400 ms look-ahead
    // delta to the synth Worker; there is no browser MIDI-output path.
    auto* synthScheduleTimer = new QTimer(this);
    synthScheduleTimer->setTimerType(Qt::PreciseTimer);
    connect(synthScheduleTimer, &QTimer::timeout,
            this, &MainWindow::dispatchScheduler);
    synthScheduleTimer->start(20);

    auto* synthStateTimer = new QTimer(this);
    connect(synthStateTimer, &QTimer::timeout,
            this, &MainWindow::pollSynthState);
    synthStateTimer->start(200);

#ifdef __EMSCRIPTEN__
    wasmidi_snappy_set_volume(volume_);
    wasmidi_snappy_set_overlap_gain(synthOverlapGain_ ? 1 : 0);
    wasmidi_snappy_set_gpu(synthGpu_ ? 1 : 0);
    applySynthConfig();
    publishSynthPrebufferConfig();
#endif
}

MainWindow::~MainWindow()
{
#ifdef __EMSCRIPTEN__
    wasmidi_snappy_stop();
#endif
    if (g_browserMainWindow == this)
        g_browserMainWindow = nullptr;
}

QVariantList MainWindow::channelColorList() const
{
    QVariantList result;
    result.reserve(channelColors_.size());

    for (const auto& color : channelColors_)
        result.push_back(color);

    return result;
}

void MainWindow::setPlaying(bool playing)
{
    if (isPlaying_ == playing)
        return;

    isPlaying_ = playing;
    emit playingChanged();
}

bool MainWindow::loadMidiUrl(const QUrl& url)
{
    const QString localPath =
        url.isLocalFile()
            ? url.toLocalFile()
            : url.toString();

    QFile file(localPath);

    if (!file.open(QIODevice::ReadOnly)) {
        emit loadFailed(
            QStringLiteral("Could not open %1")
                .arg(localPath));
        return false;
    }

    return loadMidiFileNamed(
        file.readAll(),
        QFileInfo(localPath).fileName());
}

bool MainWindow::loadMidiFile(const QByteArray& data)
{
    return loadMidiRaw(
        reinterpret_cast<const uint8_t*>(
            data.constData()),
        static_cast<std::size_t>(
            data.size()),
        fileName_);
}

bool MainWindow::loadMidiFileNamed(
    const QByteArray& data,
    const QString& fileName)
{
    return loadMidiRaw(
        reinterpret_cast<const uint8_t*>(
            data.constData()),
        static_cast<std::size_t>(
            data.size()),
        fileName);
}

bool MainWindow::adoptParsedDocument(
    wasmidi::MidiDocument&& parsed,
    const QString& fileName)
{
    clearKeyboardVisualPageCache();
    document_ = std::move(parsed);
    scheduler_.setDocument(&document_);

    if (!fileName.isEmpty()) {
        fileName_ = fileName;
        emit fileNameChanged();
    }

    publishDocumentMetadata();
    rebuildDerivedStats();

    invalidateLiveTrackers();
    visualStateValid_ = false;

    ++documentRevision_;
    emit documentRevisionChanged();

    publishSynthPrebufferConfig();
    updateLiveStats();
    emit fileLoaded();

    return true;
}

bool MainWindow::loadMidiRaw(
    const uint8_t* data,
    std::size_t size,
    const QString& fileName)
{
    stop();

    wasmidi::MidiDocument parsed;

    if (!parser_.parse(
            data,
            size,
            parsed)) {
        emit loadFailed(
            QString::fromUtf8(
                parser_.error()));
        return false;
    }

    return adoptParsedDocument(
        std::move(parsed),
        fileName);
}

void MainWindow::setMidiLoadingProgress(
    int progress,
    const QString& stage)
{
    const int clamped =
        std::clamp(progress, 0, 100);

    const bool starting =
        !midiLoading_ && clamped < 100;

    if (starting)
        stop();

    const bool nextLoading =
        clamped < 100;

    const bool changed =
        midiLoading_ != nextLoading ||
        midiLoadingProgress_ != clamped ||
        midiLoadingStage_ != stage;

    midiLoading_ = nextLoading;
    midiLoadingProgress_ = clamped;
    midiLoadingStage_ = stage;

    if (changed)
        emit midiLoadingChanged();
}

void MainWindow::failMidiLoading(
    const QString& message)
{
    midiLoading_ = false;
    midiLoadingProgress_ = 0;
    midiLoadingStage_.clear();
    emit midiLoadingChanged();
    emit loadFailed(message);
}

bool MainWindow::loadMidiSerializedRaw(
    const uint8_t* data,
    std::size_t size,
    const QString& fileName)
{
    stop();
    setMidiLoadingProgress(
        97,
        QStringLiteral("Installing MIDI document"));

    wasmidi::MidiDocument parsed;
    std::string error;

    if (!wasmidi::deserializeMidiDocument(
            data,
            size,
            parsed,
            error)) {
        failMidiLoading(
            QString::fromStdString(error));
        return false;
    }

    // Install the large compact arrays immediately, then yield once before the
    // derived-stat pass. That yield lets Qt paint the 98% loading state instead
    // of making the final main-thread work look like another browser freeze.
    clearKeyboardVisualPageCache();
    document_ = std::move(parsed);
    scheduler_.setDocument(&document_);

    if (!fileName.isEmpty()) {
        fileName_ = fileName;
        emit fileNameChanged();
    }

    publishDocumentMetadata();
    rebuildDerivedStats();

    invalidateLiveTrackers();
    visualStateValid_ = false;

    setMidiLoadingProgress(
        99,
        QStringLiteral("Preparing visualizers"));

    ++documentRevision_;
    emit documentRevisionChanged();

    publishSynthPrebufferConfig();
    updateLiveStats();
    emit fileLoaded();

    setMidiLoadingProgress(
        100,
        QStringLiteral("Ready"));
    return true;
}

void MainWindow::openMidiPicker()
{
#ifdef __EMSCRIPTEN__
    wasmidi_browser_open_file_picker(0);
#else
    emit loadFailed(
        QStringLiteral(
            "The browser MIDI picker is available in the WebAssembly build."));
#endif
}

void MainWindow::openSoundfontPicker()
{
#ifdef __EMSCRIPTEN__
    wasmidi_browser_open_file_picker(1);
#else
    synthStatus_ = QStringLiteral("SnappySynthV2 SF2 picker is available in the WebAssembly build.");
    emit synthStateChanged();
#endif
}


void MainWindow::openSfzFolderPicker()
{
#ifdef __EMSCRIPTEN__
    wasmidi_browser_open_file_picker(2);
#else
    synthStatus_ = QStringLiteral("The SFZ folder picker is available in the WebAssembly build.");
    emit synthStateChanged();
#endif
}

void MainWindow::toggleFullScreen()
{
#ifdef __EMSCRIPTEN__
    wasmidi_browser_toggle_full_screen();
#endif
}

void MainWindow::clearSoundfonts()
{
#ifdef __EMSCRIPTEN__
    if (isPlaying_)
        stop();
    wasmidi_snappy_clear_soundfonts();
#endif
    soundfontLoaded_ = false;
    soundfontName_.clear();
    synthLayers_ = 0;
    synthRegions_ = 0;
    synthPlaybackPrimed_ = false;
    synthWasSoundfontLoaded_ = false;
    emit synthStateChanged();
}


void MainWindow::clearVisualState()
{
    visualPitchCount_.fill(0);
    visualPitchMask_.fill(0);
    visualPitchColor_.fill(-1);
    for (auto& counts : visualPitchColorCounts_)
        counts.fill(0);
    visualColorVoices_.fill(0);

    visualActiveVoices_ = 0;
    visualTick_ = 0.0;
    visualKeyStartCursor_ = 0;
    visualKeyEndCursor_ = 0;
    visualKeyOwnerCursor_ = 0;
    visualStateValid_ = false;
}

void MainWindow::clearKeyboardVisualPageCache()
{
    keyboardVisualPages_.clear();
    keyboardVisualGeneration_ = 0;
}

void MainWindow::receiveKeyboardVisualPage(
    uint32_t generation,
    uint32_t spanTicks,
    uint32_t pageIndex,
    const uint32_t* words,
    uint32_t wordCount)
{
    constexpr uint32_t HeaderWords = 3;
    constexpr uint32_t CountWords = 128u * 16u;
    constexpr uint32_t OwnerWords = 128u;
    constexpr uint32_t ExpectedWords =
        HeaderWords + CountWords * 2u + OwnerWords;

    if (!words || spanTicks == 0 || wordCount != ExpectedWords)
        return;

    if (keyboardVisualGeneration_ != generation) {
        keyboardVisualPages_.clear();
        keyboardVisualGeneration_ = generation;
    }

    KeyboardVisualPage page;
    page.generation = generation;
    page.spanTicks = spanTicks;
    page.pageIndex = pageIndex;
    page.startCursor = words[0];
    page.endCursor = words[1];
    page.ownerCursor = words[2];

    uint32_t offset = HeaderWords;
    for (std::size_t pitch = 0; pitch < 128; ++pitch) {
        for (std::size_t color = 0; color < 16; ++color)
            page.globalCounts[pitch][color] = words[offset++];
    }
    for (std::size_t pitch = 0; pitch < 128; ++pitch) {
        for (std::size_t color = 0; color < 16; ++color)
            page.trackCounts[pitch][color] = words[offset++];
    }
    for (std::size_t pitch = 0; pitch < 128; ++pitch)
        page.ownerColors[pitch] = words[offset++];

    auto existing = std::find_if(
        keyboardVisualPages_.begin(),
        keyboardVisualPages_.end(),
        [generation, spanTicks, pageIndex](const KeyboardVisualPage& item) {
            return item.generation == generation &&
                   item.spanTicks == spanTicks &&
                   item.pageIndex == pageIndex;
        });

    if (existing != keyboardVisualPages_.end()) {
        *existing = std::move(page);
        return;
    }

    if (keyboardVisualPages_.size() >= 64) {
        // Keep the 64 snapshots nearest the newest page. This mirrors the roll
        // worker's rolling page cache without tying keyboard correctness to it:
        // a missing page simply falls back to the parser's exact seek index.
        auto victim = keyboardVisualPages_.begin();
        uint64_t farthest = 0;
        const uint64_t incomingTick = page.startTick();
        for (auto it = keyboardVisualPages_.begin();
             it != keyboardVisualPages_.end();
             ++it) {
            const uint64_t tick = it->startTick();
            const uint64_t distance =
                tick > incomingTick ? tick - incomingTick : incomingTick - tick;
            if (it == keyboardVisualPages_.begin() || distance > farthest) {
                victim = it;
                farthest = distance;
            }
        }
        keyboardVisualPages_.erase(victim);
    }

    keyboardVisualPages_.push_back(std::move(page));
}

void MainWindow::receiveRemoteKeyState(
    double tick,
    const uint32_t* words,
    uint32_t wordCount)
{
    constexpr uint32_t HeaderWords = 4u;
    constexpr uint32_t ExpectedWords = HeaderWords + 128u * 3u;
    if (!document_.remoteIndexed || !words || wordCount != ExpectedWords)
        return;

    remoteLiveBaselinePending_ = false;

    RemoteLiveSnapshot snapshot;
    snapshot.tick = tick;
    snapshot.activeVoices = static_cast<int>(std::min<uint32_t>(
        words[0], static_cast<uint32_t>(std::numeric_limits<int>::max())));
    snapshot.nps = static_cast<int>(std::min<uint32_t>(
        words[1], static_cast<uint32_t>(std::numeric_limits<int>::max())));
    snapshot.ccPerSecond = static_cast<int>(std::min<uint32_t>(
        words[2], static_cast<uint32_t>(std::numeric_limits<int>::max())));
    for (std::size_t pitch = 0; pitch < 128; ++pitch) {
        snapshot.counts[pitch] = words[HeaderWords + pitch];
        snapshot.globalColors[pitch] = static_cast<uint8_t>(
            words[HeaderWords + 128u + pitch] & 0x0fu);
        snapshot.trackColors[pitch] = static_cast<uint8_t>(
            words[HeaderWords + 256u + pitch] & 0x0fu);
    }

    // Only one live-state request is in flight, so normal replies are already
    // monotonic.  Keep the common path O(1); a sorted insertion is retained only
    // as a defensive fallback for an unexpected reordered reply.
    if (remoteLiveSnapshots_.empty() ||
        tick > remoteLiveSnapshots_.back().tick + 0.000001) {
        remoteLiveSnapshots_.push_back(std::move(snapshot));
    } else if (std::abs(remoteLiveSnapshots_.back().tick - tick) < 0.000001) {
        remoteLiveSnapshots_.back() = std::move(snapshot);
    } else {
        auto pos = std::lower_bound(
            remoteLiveSnapshots_.begin(), remoteLiveSnapshots_.end(), tick,
            [](const RemoteLiveSnapshot& item, double value) {
                return item.tick < value;
            });
        if (pos != remoteLiveSnapshots_.end() &&
            std::abs(pos->tick - tick) < 0.000001)
            *pos = std::move(snapshot);
        else
            remoteLiveSnapshots_.insert(pos, std::move(snapshot));
    }

    // A two-frame lead only needs a handful of snapshots.
    while (remoteLiveSnapshots_.size() > 8u)
        remoteLiveSnapshots_.pop_front();

    applyRemoteLiveSnapshot(document_.secondsToTick(currentTime_));
}

void MainWindow::applyRemoteLiveSnapshot(double targetTick)
{
    if (!document_.remoteIndexed || remoteLiveSnapshots_.empty())
        return;

    // Select the newest prepared frame at-or-before the authoritative
    // presentation tick without copying/erasing a vector of ~1.5 KiB snapshots
    // every display frame.
    while (remoteLiveSnapshots_.size() > 1u &&
           remoteLiveSnapshots_[1].tick <= targetTick + 0.000001) {
        remoteLiveSnapshots_.pop_front();
    }
    if (remoteLiveSnapshots_.front().tick > targetTick + 0.000001)
        return;
    RemoteLiveSnapshot snapshot = std::move(remoteLiveSnapshots_.front());
    remoteLiveSnapshots_.pop_front();

    const auto oldMask = visualPitchMask_;
    const auto oldColors = visualPitchColor_;

    visualPitchCount_.fill(0);
    visualPitchMask_.fill(0);
    visualPitchColor_.fill(-1);
    for (auto& counts : visualPitchColorCounts_) counts.fill(0);
    visualColorVoices_.fill(0);
    visualActiveVoices_ = snapshot.activeVoices;

    for (std::size_t pitch = 0; pitch < 128; ++pitch) {
        const uint32_t count = snapshot.counts[pitch];
        if (!count) continue;
        const uint8_t color = perTrackColors_
            ? snapshot.trackColors[pitch]
            : snapshot.globalColors[pitch];

        visualPitchCount_[pitch] = count;
        visualPitchMask_[pitch] = 1;
        visualPitchColor_[pitch] = static_cast<int8_t>(color);
        visualPitchColorCounts_[pitch][color] = count;
        visualColorVoices_[color] = static_cast<uint32_t>(
            std::min<uint64_t>(std::numeric_limits<uint32_t>::max(),
                uint64_t(visualColorVoices_[color]) + count));
    }

    visualTick_ = snapshot.tick;
    ++keyboardFrames_;
    visualStateValid_ = true;

    if (oldMask != visualPitchMask_ || oldColors != visualPitchColor_)
        emit activePitchesChanged();
    if (activeVoices_ != snapshot.activeVoices) {
        activeVoices_ = snapshot.activeVoices;
        emit activeVoicesChanged();
    }
    if (nps_ != snapshot.nps) {
        nps_ = snapshot.nps;
        emit npsChanged();
    }
    if (ccPerSecond_ != snapshot.ccPerSecond) {
        ccPerSecond_ = snapshot.ccPerSecond;
        emit ccPerSecondChanged();
    }

    bool peakChanged = false;
    if (snapshot.nps > peakNps_) {
        peakNps_ = snapshot.nps;
        peakNpsTime_ = static_cast<float>(document_.tickToSeconds(snapshot.tick));
        emit peakNpsChanged();
        peakChanged = true;
    }
    if (snapshot.activeVoices > peakPolyphony_) {
        peakPolyphony_ = snapshot.activeVoices;
        peakPolyphonyTime_ = static_cast<float>(document_.tickToSeconds(snapshot.tick));
        emit peakPolyphonyChanged();
        peakChanged = true;
    }
    if (peakChanged)
        emit timelineChanged();

}

void MainWindow::invalidateLiveTrackers()
{
    liveWindowsValid_ = false;
    liveLastTime_ = 0.0f;
    liveHi_ = 0;
    liveNpsLo_ = 0;
    liveCcLo_ = 0;
    liveNpsCount_ = 0;
    liveCcCount_ = 0;

    neuralWindowValid_ = false;
    neuralFutureLo_ = 0;
    neuralFutureHi_ = 0;
    neuralFutureColors_.fill(0);
    remoteLiveSnapshots_.clear();
    remoteLiveBaselinePending_ = false;
}

void MainWindow::clearFile()
{
    stop();

#ifdef __EMSCRIPTEN__
    wasmidi_mapped_shutdown();
#endif

    scheduler_.setDocument(nullptr);
    clearKeyboardVisualPageCache();
    document_ = wasmidi::MidiDocument{};

    fileName_.clear();

    duration_ = 0.0f;
    noteCount_ = 0;
    trackCount_ = 0;
    activeVoices_ = 0;
    activeChannelCount_ = 0;
    nps_ = 0;
    ccPerSecond_ = 0;
    bpm_ = 120.0f;
    midiFormat_ = 0;
    ppq_ = 480;
    tempoChangeCount_ = 0;
    controlEventCount_ = 0;
    peakNps_ = 0;
    peakNpsTime_ = 0.0f;
    peakPolyphony_ = 0;
    peakPolyphonyTime_ = 0.0f;
    pitchRange_ = QStringLiteral("—");
    synthEffectiveVelocityFloor_ = synthVelocityFloor_;
    skippedVelocity_ = synthEffectiveVelocityFloor_;

    npsTimeline_.clear();
    tempoTimes_.clear();
    tempoBpms_.clear();

    clearVisualState();
    invalidateLiveTrackers();

    dominantColor_ = -1;

    ++documentRevision_;

    emit fileNameChanged();
    emit durationChanged();
    emit noteCountChanged();
    emit trackCountChanged();
    emit activeVoicesChanged();
    emit activeChannelCountChanged();
    emit npsChanged();
    emit ccPerSecondChanged();
    emit bpmChanged();
    emit midiFormatChanged();
    emit ppqChanged();
    emit tempoChangeCountChanged();
    emit controlEventCountChanged();
    emit peakNpsChanged();
    emit peakPolyphonyChanged();
    emit pitchRangeChanged();
    emit skippedVelocityChanged();
    emit timelineChanged();
    emit activePitchesChanged();
    emit accentColorChanged();
    emit documentRevisionChanged();
}

void MainWindow::publishDocumentMetadata()
{
    duration_ =
        document_.durationSeconds;

    noteCount_ =
        static_cast<qint64>(
            std::min<uint64_t>(
                document_.noteCount,
                uint64_t(std::numeric_limits<qint64>::max())));

    trackCount_ =
        document_.trackCount;

    midiFormat_ =
        document_.format;

    ppq_ =
        document_.ticksPerBeat;

    tempoChangeCount_ =
        static_cast<int>(
            std::min<std::size_t>(
                document_.tempoMap.size(),
                std::size_t(
                    std::numeric_limits<int>::max())));

    controlEventCount_ =
        static_cast<qint64>(
            std::min<uint64_t>(
                document_.controlEventCount,
                uint64_t(std::numeric_limits<qint64>::max())));

    uint32_t channelMask = 0;

    for (uint32_t mask :
         document_.activeChannelMasks) {
        channelMask |= mask;
    }

    activeChannelCount_ = 0;

    for (int channel = 0;
         channel < 16;
         ++channel) {
        activeChannelCount_ +=
            (channelMask & (1u << channel))
                ? 1
                : 0;
    }

    if (document_.hasPitch) {
        pitchRange_ =
            QStringLiteral("%1–%2")
                .arg(document_.minPitch)
                .arg(document_.maxPitch);
    } else {
        pitchRange_ =
            QStringLiteral("—");
    }

    emit durationChanged();
    emit noteCountChanged();
    emit trackCountChanged();
    emit midiFormatChanged();
    emit ppqChanged();
    emit tempoChangeCountChanged();
    emit controlEventCountChanged();
    emit activeChannelCountChanged();
    emit pitchRangeChanged();
}

void MainWindow::rebuildDerivedStats()
{
    // Loading no longer scans the whole MIDI to manufacture graph data. Keep
    // only the tiny tempo lookup needed to turn the current transport time into
    // BPM; NPS/poly/CC peaks and graph history are accumulated from live frames.
    npsTimeline_.clear();
    tempoTimes_.clear();
    tempoBpms_.clear();
    peakNps_ = 0;
    peakNpsTime_ = 0.0f;
    peakPolyphony_ = 0;
    peakPolyphonyTime_ = 0.0f;

    if (document_.tempoMap.empty()) {
        tempoTimes_.push_back(0.0f);
        tempoBpms_.push_back(120.0f);
    } else {
        tempoTimes_.reserve(document_.tempoMap.size());
        tempoBpms_.reserve(document_.tempoMap.size());
        for (const auto& tempo : document_.tempoMap) {
            tempoTimes_.push_back(static_cast<float>(
                document_.tickToSeconds(tempo.tick)));
            const uint32_t us = tempo.microsecondsPerBeat
                ? tempo.microsecondsPerBeat
                : 500000u;
            tempoBpms_.push_back(60000000.0f / float(us));
        }
    }

    bpm_ = tempoBpms_.empty() ? 120.0f : tempoBpms_.front();
    emit bpmChanged();
    emit peakNpsChanged();
    emit peakPolyphonyChanged();
    emit timelineChanged();
}

void MainWindow::notifyVisualizerFramePresented()
{
    ++visualFrameSerial_;
}

void MainWindow::play()
{
    if (!hasMidi())
        return;

    if (currentTime_ >= duration_)
        currentTime_ = 0.0f;

    playbackAnchorSeconds_ = currentTime_;
    playbackClock_.restart();
    playbackLastElapsedMs_ = 0;
    presentationOffsetSeconds_ = 0.0;
    presentationOffsetLocked_ = false;
    playbackConsumedVisualFrameSerial_ = visualFrameSerial_;
    synthLastHardResyncElapsedMs_ = -100000;
    synthWasStarved_ = false;
    synthStarvedSinceElapsedMs_ = -1;
    synthCatchupGraceUntilElapsedMs_ = 750;
    synthVelocityLastRaiseElapsedMs_ = -1;

    // A previous overload must never leak its adaptive velocity threshold into
    // a new play/resume transaction. Re-evaluate producer pressure from the
    // fresh SSv2 render metrics instead of starting the first note at an old
    // elevated "Skipped vel" value.
    if (synthEffectiveVelocityFloor_ != synthVelocityFloor_) {
        synthEffectiveVelocityFloor_ = synthVelocityFloor_;
        skippedVelocity_ = synthEffectiveVelocityFloor_;
        emit skippedVelocityChanged();
    }

    scheduler_.seek(currentTime_);
    scheduler_.start();

    invalidateLiveTrackers();
    visualStateValid_ = false;

    // Mark playback active before priming the synth so scheduleSynthAhead() can
    // immediately fill the first look-ahead window instead of waiting 20 ms.
    setPlaying(true);

#ifdef __EMSCRIPTEN__
    if (soundfontLoaded_) {
        if (!synthPlaybackPrimed_) {
            // MPWGL2-style transport: audio starts from the same wall-clock
            // transaction as playback and is never gated by renderer/cache
            // readiness.  The mapped scheduler owns its own MIDI look-ahead.
            wasmidi_snappy_local_floor(synthEffectiveVelocityFloor_);
            wasmidi_snappy_play(currentTime_, 1);
            resetSynthSchedule(currentTime_);
            synthPlaybackPrimed_ = true;
        } else {
            wasmidi_snappy_play(currentTime_, 0);
        }
        scheduleSynthAhead();
    }
#endif
}

void MainWindow::pause()
{
    if (!isPlaying_)
        return;

    updateCurrentTime();
    scheduler_.pause();
#ifdef __EMSCRIPTEN__
    if (soundfontLoaded_)
        wasmidi_snappy_pause();
#endif
    setPlaying(false);
}

void MainWindow::stop()
{
    scheduler_.stop();
#ifdef __EMSCRIPTEN__
    wasmidi_snappy_stop();
#endif
    synthPlaybackPrimed_ = false;
    synthGroupCursor_ = 0;
    synthScheduledUntil_ = 0.0;
    ++transportRevision_;
    emit transportRevisionChanged();
    setPlaying(false);

    playbackAnchorSeconds_ = 0.0f;
    playbackLastElapsedMs_ = 0;
    presentationOffsetSeconds_ = 0.0;
    presentationOffsetLocked_ = false;
    playbackConsumedVisualFrameSerial_ = visualFrameSerial_;
    synthWasStarved_ = false;
    synthStarvedSinceElapsedMs_ = -1;
    synthCatchupGraceUntilElapsedMs_ = 0;
    synthVelocityLastRaiseElapsedMs_ = -1;
    if (synthEffectiveVelocityFloor_ != synthVelocityFloor_) {
        synthEffectiveVelocityFloor_ = synthVelocityFloor_;
        skippedVelocity_ = synthEffectiveVelocityFloor_;
        emit skippedVelocityChanged();
    }
    playbackClock_.invalidate();

    if (currentTime_ != 0.0f) {
        currentTime_ = 0.0f;
        emit currentTimeChanged();
    }

    if (activeVoices_ != 0) {
        activeVoices_ = 0;
        emit activeVoicesChanged();
    }

    if (nps_ != 0) {
        nps_ = 0;
        emit npsChanged();
    }

    if (ccPerSecond_ != 0) {
        ccPerSecond_ = 0;
        emit ccPerSecondChanged();
    }

    if (!tempoBpms_.empty() &&
        !qFuzzyCompare(bpm_, tempoBpms_.front())) {
        bpm_ = tempoBpms_.front();
        emit bpmChanged();
    }

    clearVisualState();
    invalidateLiveTrackers();
    emit activePitchesChanged();
}

void MainWindow::seek(float seconds)
{
    const float clamped =
        std::clamp(seconds, 0.0f, duration_);

    currentTime_ = clamped;
    playbackAnchorSeconds_ = clamped;
    ++transportRevision_;
    emit transportRevisionChanged();

    if (isPlaying_) {
        playbackClock_.restart();
        playbackLastElapsedMs_ = 0;
        presentationOffsetSeconds_ = 0.0;
        presentationOffsetLocked_ = false;
        playbackConsumedVisualFrameSerial_ = visualFrameSerial_;
        synthStarvedSinceElapsedMs_ = -1;
        synthCatchupGraceUntilElapsedMs_ = 600;
        synthVelocityLastRaiseElapsedMs_ = -1;
    }

    scheduler_.seek(clamped);
    invalidateLiveTrackers();
    visualStateValid_ = false;

    if (document_.remoteIndexed) {
        // A seek starts a new visual transaction. Never leave the keyboard and
        // graphs displaying a frame from the old transport position while the
        // exact mapped baseline is being rebuilt. The baseline request below
        // repopulates all four values atomically from the new tick.
        clearVisualState();
        if (activeVoices_ != 0) {
            activeVoices_ = 0;
            emit activeVoicesChanged();
        }
        if (nps_ != 0) {
            nps_ = 0;
            emit npsChanged();
        }
        if (ccPerSecond_ != 0) {
            ccPerSecond_ = 0;
            emit ccPerSecondChanged();
        }
        emit activePitchesChanged();
    }

#ifdef __EMSCRIPTEN__
    if (soundfontLoaded_) {
        wasmidi_snappy_local_floor(synthEffectiveVelocityFloor_);
        wasmidi_snappy_seek(clamped);
        resetSynthSchedule(clamped);
        synthPlaybackPrimed_ = true;
        scheduleSynthAhead();
    }
#endif

    emit currentTimeChanged();
    updateLiveStats();
}

void MainWindow::setNoteSpeed(float speed)
{
    const float clamped =
        std::clamp(
            speed,
            0.1f,
            60.0f);

    if (qFuzzyCompare(
            noteSpeed_,
            clamped)) {
        return;
    }

    noteSpeed_ = clamped;
    WASMIDI_PERSIST_SETTING("noteSpeed", noteSpeed_);
    emit noteSpeedChanged();
}

void MainWindow::setPostBuffer(float buffer)
{
    const float clamped =
        std::clamp(
            buffer,
            0.0f,
            10.0f);

    const bool autoWasEnabled =
        postBufferAuto_;

    postBufferAuto_ = false;
    WASMIDI_PERSIST_SETTING("postBufferAuto", 0);

    if (autoWasEnabled)
        emit postBufferAutoChanged();

    if (qFuzzyCompare(
            postBuffer_,
            clamped)) {
        return;
    }

    postBuffer_ = clamped;
    WASMIDI_PERSIST_SETTING("postBuffer", postBuffer_);
    emit postBufferChanged();
}

void MainWindow::setPostBufferAuto()
{
    const bool changed =
        !postBufferAuto_;

    postBufferAuto_ = true;
    WASMIDI_PERSIST_SETTING("postBufferAuto", 1);

    if (!qFuzzyIsNull(postBuffer_)) {
        postBuffer_ = 0.0f;
        WASMIDI_PERSIST_SETTING("postBuffer", postBuffer_);
        emit postBufferChanged();
    }

    if (changed)
        emit postBufferAutoChanged();
}

void MainWindow::setPerTrackColors(bool enable)
{
    if (perTrackColors_ == enable)
        return;

    perTrackColors_ = enable;
    WASMIDI_PERSIST_SETTING("perTrackColors", perTrackColors_ ? 1 : 0);
    visualStateValid_ = false;
    neuralWindowValid_ = false;

    emit perTrackColorsChanged();

    updateLiveStats();
}

void MainWindow::setVolume(int value)
{
    const int clamped =
        std::clamp(
            value,
            0,
            100);

    if (volume_ == clamped)
        return;

    volume_ = clamped;
    WASMIDI_PERSIST_SETTING("volume", volume_);
    emit volumeChanged();
#ifdef __EMSCRIPTEN__
    // UI volume is applied after synthesis in the AudioWorklet. It does not
    // invalidate any queued PCM and therefore must never seek/reset the synth;
    // doing so on a live slider used to create avoidable silent gaps.
    wasmidi_snappy_set_volume(volume_);
#endif
}


void MainWindow::applySynthConfig()
{
    synthPlaybackPrimed_ = false;

#ifdef __EMSCRIPTEN__
    wasmidi_snappy_configure(
        synthMaxVoices_,
        synthMinVoices_,
        synthBufferFrames_,
        synthNumBuffers_,
        synthRequestedSampleRate_,
        synthChannels_,
        synthBitsPerSample_,
        synthRealtimePriority_ ? 1 : 0,
        synthWorkers_,
        synthNoteSharding_,
        synthStealScoreCache_ ? 1 : 0,
        synthDebugMetrics_ ? 1 : 0,
        synthFastNoteOff_ ? 1 : 0,
        synthValidateState_ ? 1 : 0,
        synthSoftClip_ ? 1 : 0);
#endif
}

void MainWindow::setSynthMaxVoices(int value)
{
    // SnappySynth_SetMinimumVoices() treats the minimum as a floor for the
    // effective voice cap. Keep the UI state internally consistent with that
    // source behavior instead of showing Max < Min while the core silently
    // raises Max during initialization.
    const int clamped =
        std::clamp(
            value,
            std::max(1, synthMinVoices_),
            5000000);

    if (synthMaxVoices_ == clamped)
        return;

    synthMaxVoices_ = clamped;
    WASMIDI_PERSIST_SETTING("synthMaxVoices", synthMaxVoices_);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthMinVoices(int value)
{
    const int clamped =
        std::clamp(
            value,
            0,
            5000000);

    bool changed = false;

    if (synthMinVoices_ != clamped) {
        synthMinVoices_ = clamped;
        changed = true;
    }

    if (synthMaxVoices_ < synthMinVoices_) {
        synthMaxVoices_ = synthMinVoices_;
        changed = true;
    }

    if (!changed)
        return;

    WASMIDI_PERSIST_SETTING("synthMinVoices", synthMinVoices_);
    WASMIDI_PERSIST_SETTING("synthMaxVoices", synthMaxVoices_);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthBufferFrames(int value)
{
    // The original SnappySynth configuration accepts any positive BufferSize
    // (examples include 48, 96, 256, 480, 512, 1024). Do not force powers of 2.
    const int clamped =
        std::clamp(
            value,
            1,
            65536);

    if (synthBufferFrames_ == clamped)
        return;

    synthBufferFrames_ = clamped;
    WASMIDI_PERSIST_SETTING("synthBufferFrames", synthBufferFrames_);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthNumBuffers(int value)
{
    const int clamped =
        std::clamp(
            value,
            1,
            128);

    if (synthNumBuffers_ == clamped)
        return;

    synthNumBuffers_ = clamped;
    WASMIDI_PERSIST_SETTING("synthNumBuffers", synthNumBuffers_);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthPrebufferSeconds(float value)
{
    (void)value;

    // Retained only for QML/API compatibility with older deployments. Audio
    // pre-rendering has been removed; the effective value is always zero and
    // changing this legacy property must not flush/reseek the realtime queue.
    if (std::abs(synthPrebufferSeconds_) < 0.0001f)
        return;

    synthPrebufferSeconds_ = 0.0f;
    emit synthConfigChanged();
}

void MainWindow::setSynthVelocityFloor(int value)
{
    const int clamped = std::clamp(value, 0, 127);
    if (synthVelocityFloor_ == clamped)
        return;

    synthVelocityFloor_ = clamped;
    WASMIDI_PERSIST_SETTING("synthVelocityFloor", synthVelocityFloor_);
    synthEffectiveVelocityFloor_ =
        std::max(synthEffectiveVelocityFloor_, synthVelocityFloor_);

    if (!isPlaying_)
        synthEffectiveVelocityFloor_ = synthVelocityFloor_;

    if (skippedVelocity_ != synthEffectiveVelocityFloor_) {
        skippedVelocity_ = synthEffectiveVelocityFloor_;
        emit skippedVelocityChanged();
    }

    emit synthConfigChanged();

#ifdef __EMSCRIPTEN__
    // The velocity floor changes the actual MIDI stream sent to the synth, so
    // already rendered PCM is invalid just like after a seek or DSP change.
    if (soundfontLoaded_ && isPlaying_) {
        wasmidi_snappy_local_floor(synthEffectiveVelocityFloor_);
        wasmidi_snappy_seek(currentTime_);
        resetSynthSchedule(currentTime_);
        synthPlaybackPrimed_ = true;
        scheduleSynthAhead();
    }
#endif
}

void MainWindow::publishSynthPrebufferConfig()
{
#ifdef __EMSCRIPTEN__
    wasmidi_snappy_set_prebuffer(
        synthPrebufferSeconds_,
        duration_);
#endif
}

void MainWindow::setSynthRequestedSampleRate(int value)
{
    const int clamped =
        value <= 0
            ? 0
            : std::clamp(
                value,
                8000,
                384000);

    if (synthRequestedSampleRate_ == clamped)
        return;

    // AudioContext sample rate is chosen at context construction. Keep this
    // configurable before the backend starts; afterward the actual device rate
    // remains authoritative so pitch cannot drift.
    if (synthReady_)
        return;

    synthRequestedSampleRate_ = clamped;
    WASMIDI_PERSIST_SETTING("synthRequestedSampleRate", synthRequestedSampleRate_);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthChannels(int value)
{
    const int clamped =
        value == 1 ? 1 : 2;

    if (synthChannels_ == clamped)
        return;

    synthChannels_ = clamped;
    WASMIDI_PERSIST_SETTING("synthChannels", synthChannels_);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthBitsPerSample(int value)
{
    const int clamped =
        value == 16 ? 16 : 32;

    if (synthBitsPerSample_ == clamped)
        return;

    synthBitsPerSample_ = clamped;
    WASMIDI_PERSIST_SETTING("synthBitsPerSample", synthBitsPerSample_);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthRealtimePriority(bool enabled)
{
    if (synthRealtimePriority_ == enabled)
        return;

    synthRealtimePriority_ = enabled;
    WASMIDI_PERSIST_SETTING("synthRealtimePriority", synthRealtimePriority_ ? 1 : 0);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthWorkers(int value)
{
    const int clamped =
        std::clamp(
            value,
            0,
            256);

    if (synthWorkers_ == clamped)
        return;

    synthWorkers_ = clamped;
    WASMIDI_PERSIST_SETTING("synthWorkers", synthWorkers_);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthNoteSharding(int value)
{
    const int clamped =
        std::clamp(
            value,
            0,
            3);

    if (synthNoteSharding_ == clamped)
        return;

    synthNoteSharding_ = clamped;
    WASMIDI_PERSIST_SETTING("synthNoteSharding", synthNoteSharding_);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthStealScoreCache(bool enabled)
{
    if (synthStealScoreCache_ == enabled)
        return;

    synthStealScoreCache_ = enabled;
    WASMIDI_PERSIST_SETTING("synthStealScoreCache", synthStealScoreCache_ ? 1 : 0);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthFastNoteOff(bool enabled)
{
    if (synthFastNoteOff_ == enabled)
        return;

    synthFastNoteOff_ = enabled;
    WASMIDI_PERSIST_SETTING("synthFastNoteOff", synthFastNoteOff_ ? 1 : 0);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthValidateState(bool enabled)
{
    if (synthValidateState_ == enabled)
        return;

    synthValidateState_ = enabled;
    WASMIDI_PERSIST_SETTING("synthValidateState", synthValidateState_ ? 1 : 0);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthDebugMetrics(bool enabled)
{
    if (synthDebugMetrics_ == enabled)
        return;

    // Off by default. The gated counters cost two clock reads per controller
    // event and an atomic add per voice per block, which is worth paying while
    // diagnosing and pure waste otherwise.
    synthDebugMetrics_ = enabled;
    WASMIDI_PERSIST_SETTING("synthDebugMetrics", synthDebugMetrics_ ? 1 : 0);
    emit synthConfigChanged();
    /*
     * Deliberately NOT applySynthConfig(). Every other flag here reconfigures
     * the engine, which reloads the soundfont; this one only decides whether
     * diagnostic counters are collected. Routing it through the same path meant
     * that turning the counters on to take a reading reloaded the soundfont
     * first, so the reading described the post-reload state rather than the one
     * being investigated -- which is exactly the first-load problem these
     * counters exist to diagnose. The worker is told directly instead.
     */
    wasmidi_snappy_set_debug_metrics(synthDebugMetrics_ ? 1 : 0);
}

void MainWindow::setSynthSoftClip(bool enabled)
{
    if (synthSoftClip_ == enabled)
        return;

    synthSoftClip_ = enabled;
    WASMIDI_PERSIST_SETTING("synthSoftClip", synthSoftClip_ ? 1 : 0);
    emit synthConfigChanged();
    applySynthConfig();
}

void MainWindow::setSynthGpu(bool enabled)
{
    if (synthGpu_ == enabled)
        return;

    synthGpu_ = enabled;
    WASMIDI_PERSIST_SETTING("synthGpu", synthGpu_ ? 1 : 0);
    emit synthConfigChanged();

#ifdef __EMSCRIPTEN__
    wasmidi_snappy_set_gpu(synthGpu_ ? 1 : 0);
#endif
}

void MainWindow::setRollMode(int mode)
{
    mode = mode == 2 ? 2 : 1;
    if (rollMode_ == mode)
        return;
    rollMode_ = mode;
    WASMIDI_PERSIST_SETTING("rollMode", rollMode_);
    emit rollConfigChanged();
}

void MainWindow::setNeuralRoll(float amount)
{
    amount = std::clamp(amount, 0.0f, 1.0f);
    if (neuralRoll_ == amount)
        return;
    neuralRoll_ = amount;
    WASMIDI_PERSIST_SETTING("neuralRoll", neuralRoll_);
    emit neuralBackgroundChanged();
}

void MainWindow::setNeuralPage(float amount)
{
    amount = std::clamp(amount, 0.0f, 1.0f);
    if (neuralPage_ == amount)
        return;
    neuralPage_ = amount;
    WASMIDI_PERSIST_SETTING("neuralPage", neuralPage_);
    emit neuralBackgroundChanged();
}

void MainWindow::setRollLookAheadMB(int megabytes)
{
    megabytes = std::clamp(megabytes, 64, 16384);
    if (rollLookAheadMB_ == megabytes)
        return;
    rollLookAheadMB_ = megabytes;
    WASMIDI_PERSIST_SETTING("rollLookAheadMB", rollLookAheadMB_);
    emit rollConfigChanged();
}

void MainWindow::setSynthOverlapGain(bool enabled)
{
    if (synthOverlapGain_ == enabled)
        return;

    synthOverlapGain_ = enabled;
    WASMIDI_PERSIST_SETTING("synthStackGain", synthOverlapGain_ ? 1 : 0);
    emit synthConfigChanged();

#ifdef __EMSCRIPTEN__
    wasmidi_snappy_set_overlap_gain(
        enabled ? 1 : 0);

    if (soundfontLoaded_ && isPlaying_) {
        wasmidi_snappy_local_floor(synthEffectiveVelocityFloor_);
        wasmidi_snappy_seek(currentTime_);
        resetSynthSchedule(currentTime_);
        synthPlaybackPrimed_ = true;
        scheduleSynthAhead();
    }
#endif
}

void MainWindow::setChannelColor(
    int channel,
    const QColor& color)
{
    if (channel < 0 ||
        channel >= channelColors_.size() ||
        !color.isValid()) {
        return;
    }

    if (channelColors_[channel] == color)
        return;

    channelColors_[channel] = color;

    emit channelColorsChanged();
    if (channel == dominantColor_ && adaptColors_)
        emit accentColorChanged();
    updateDominantColor();
    const QByteArray settingKey = QByteArray("channelColor.") + QByteArray::number(channel);
    WASMIDI_PERSIST_SETTING(settingKey.constData(), color.rgb() & 0x00ffffffu);
}

void MainWindow::addVisualNote(std::size_t sourceIndex)
{
    if (sourceIndex >= document_.visualNotes.size())
        return;

    const auto& note = document_.visualNotes[sourceIndex];
    const uint8_t pitch = static_cast<uint8_t>((note.packedData >> 8) & 0x7f);
    const uint8_t color = document_.visualColorIndex(note, perTrackColors_) & 0x0f;

    addVisualCount(pitch, color, 1);

    // VisualNote is source/start ordered, so a seek rebuild naturally leaves
    // the newest still-active note as the representative keyboard color.
    visualPitchColor_[pitch] = static_cast<int8_t>(color);
}

void MainWindow::addVisualCount(uint8_t pitch, uint8_t color, uint32_t count)
{
    if (pitch >= 128 || color >= 16 || count == 0)
        return;

    const auto addClamped = [](uint32_t current, uint32_t amount) {
        const uint64_t sum = uint64_t(current) + uint64_t(amount);
        return static_cast<uint32_t>(
            std::min<uint64_t>(sum, std::numeric_limits<uint32_t>::max()));
    };

    visualPitchCount_[pitch] = addClamped(visualPitchCount_[pitch], count);
    visualPitchColorCounts_[pitch][color] =
        addClamped(visualPitchColorCounts_[pitch][color], count);
    visualColorVoices_[color] = addClamped(visualColorVoices_[color], count);

    visualActiveVoices_ = static_cast<int>(
        std::min<int64_t>(
            int64_t(std::numeric_limits<int>::max()),
            int64_t(visualActiveVoices_) + int64_t(count)));

    visualPitchMask_[pitch] = 1;
}

void MainWindow::removeVisualCount(uint8_t pitch, uint8_t color, uint32_t count)
{
    if (pitch >= 128 || color >= 16 || count == 0)
        return;

    const uint32_t removed =
        std::min(count, visualPitchColorCounts_[pitch][color]);

    visualPitchColorCounts_[pitch][color] -= removed;
    visualPitchCount_[pitch] -=
        std::min(removed, visualPitchCount_[pitch]);
    visualColorVoices_[color] -=
        std::min(removed, visualColorVoices_[color]);

    visualActiveVoices_ = static_cast<int>(
        std::max<int64_t>(
            0,
            int64_t(visualActiveVoices_) - int64_t(removed)));

    if (visualPitchCount_[pitch] == 0) {
        visualPitchMask_[pitch] = 0;
        visualPitchColor_[pitch] = -1;
        return;
    }

    if (visualPitchColor_[pitch] == static_cast<int8_t>(color) &&
        visualPitchColorCounts_[pitch][color] == 0) {
        // This is the same deterministic fallback used by the old heap path.
        // Exact held-state is represented by counts; the owner stream restores
        // the newest color whenever another start for this pitch is crossed.
        visualPitchColor_[pitch] = -1;
        for (int candidate = 15; candidate >= 0; --candidate) {
            if (visualPitchColorCounts_[pitch][candidate] != 0) {
                visualPitchColor_[pitch] = static_cast<int8_t>(candidate);
                break;
            }
        }
    }
}

void MainWindow::applyVisualKeyEvent(
    const wasmidi::VisualKeyEvent& event,
    bool add)
{
    const uint8_t pitch = document_.visualKeyPitch(event.packedData);
    const uint8_t color = document_.visualKeyColor(event.packedData, perTrackColors_) & 0x0f;

    if (add)
        addVisualCount(pitch, color, event.count);
    else
        removeVisualCount(pitch, color, event.count);
}

bool MainWindow::restoreVisualStateFromPage(double targetTick)
{
    if (keyboardVisualPages_.empty())
        return false;

    const KeyboardVisualPage* best = nullptr;
    uint64_t bestTick = 0;

    for (const auto& page : keyboardVisualPages_) {
        if (page.generation != keyboardVisualGeneration_ || page.spanTicks == 0)
            continue;

        const uint64_t tick = page.startTick();
        if (double(tick) > targetTick)
            continue;

        if (!best || tick >= bestTick) {
            best = &page;
            bestTick = tick;
        }
    }

    if (!best)
        return false;

    visualPitchCount_.fill(0);
    visualPitchMask_.fill(0);
    visualPitchColor_.fill(-1);
    for (auto& counts : visualPitchColorCounts_)
        counts.fill(0);
    visualColorVoices_.fill(0);
    visualActiveVoices_ = 0;

    const auto& sourceCounts =
        perTrackColors_ ? best->trackCounts : best->globalCounts;

    for (std::size_t pitch = 0; pitch < 128; ++pitch) {
        uint64_t pitchTotal = 0;
        for (std::size_t color = 0; color < 16; ++color) {
            const uint32_t count = sourceCounts[pitch][color];
            visualPitchColorCounts_[pitch][color] = count;
            pitchTotal += count;
            visualColorVoices_[color] = static_cast<uint32_t>(
                std::min<uint64_t>(
                    uint64_t(std::numeric_limits<uint32_t>::max()),
                    uint64_t(visualColorVoices_[color]) + count));
        }

        visualPitchCount_[pitch] = static_cast<uint32_t>(
            std::min<uint64_t>(pitchTotal, std::numeric_limits<uint32_t>::max()));
        visualPitchMask_[pitch] = pitchTotal != 0 ? 1 : 0;

        if (pitchTotal == 0)
            continue;

        const uint32_t ownerWord = best->ownerColors[pitch];
        int owner = perTrackColors_
            ? int((ownerWord >> 8) & 0xffu)
            : int(ownerWord & 0xffu);

        if (owner < 0 || owner >= 16 ||
            visualPitchColorCounts_[pitch][std::size_t(owner)] == 0) {
            owner = -1;
            for (int candidate = 15; candidate >= 0; --candidate) {
                if (visualPitchColorCounts_[pitch][std::size_t(candidate)] != 0) {
                    owner = candidate;
                    break;
                }
            }
        }

        visualPitchColor_[pitch] = static_cast<int8_t>(owner);
        visualActiveVoices_ = static_cast<int>(
            std::min<int64_t>(
                int64_t(std::numeric_limits<int>::max()),
                int64_t(visualActiveVoices_) +
                    int64_t(std::min<uint64_t>(
                        pitchTotal,
                        uint64_t(std::numeric_limits<int>::max())))));
    }

    visualKeyStartCursor_ = std::min<std::size_t>(
        best->startCursor, document_.visualKeyStarts.size());
    visualKeyEndCursor_ = std::min<std::size_t>(
        best->endCursor, document_.visualKeyEnds.size());
    visualKeyOwnerCursor_ = std::min<std::size_t>(
        best->ownerCursor, document_.visualKeyOwners.size());
    visualTick_ = double(bestTick);
    ++keyboardFrames_;
    visualStateValid_ = true;

    advanceVisualStateTo(targetTick);
    return true;
}

void MainWindow::advanceVisualStateTo(double targetTick)
{
    // Forward playback advances compact counted streams built by the MIDI
    // parser worker. A million identical starts at one crashpoint can now be
    // one add instead of one million heap pushes on the UI thread.
    while (visualKeyStartCursor_ < document_.visualKeyStarts.size() &&
           double(document_.visualKeyStarts[visualKeyStartCursor_].tick) <= targetTick) {
        applyVisualKeyEvent(document_.visualKeyStarts[visualKeyStartCursor_], true);
        ++visualKeyStartCursor_;
    }

    while (visualKeyOwnerCursor_ < document_.visualKeyOwners.size() &&
           double(document_.visualKeyOwners[visualKeyOwnerCursor_].tick) <= targetTick) {
        const auto& owner = document_.visualKeyOwners[visualKeyOwnerCursor_];
        const uint8_t pitch = document_.visualKeyPitch(owner.packedData);
        if (pitch < 128 && visualPitchCount_[pitch] != 0) {
            visualPitchColor_[pitch] = static_cast<int8_t>(
                document_.visualKeyColor(owner.packedData, perTrackColors_) & 0x0f);
        }
        ++visualKeyOwnerCursor_;
    }

    while (visualKeyEndCursor_ < document_.visualKeyEnds.size() &&
           double(document_.visualKeyEnds[visualKeyEndCursor_].tick) < targetTick) {
        applyVisualKeyEvent(document_.visualKeyEnds[visualKeyEndCursor_], false);
        ++visualKeyEndCursor_;
    }

    visualTick_ = targetTick;
    ++keyboardFrames_;
}

void MainWindow::rebuildVisualStateAt(double targetTick)
{
    if (restoreVisualStateFromPage(targetTick))
        return;

    visualPitchCount_.fill(0);
    visualPitchMask_.fill(0);
    visualPitchColor_.fill(-1);
    for (auto& counts : visualPitchColorCounts_)
        counts.fill(0);
    visualColorVoices_.fill(0);
    visualActiveVoices_ = 0;

    // Arbitrary seeks use the exact source stream plus its block max-end index.
    // This scans only blocks that can still contain a held note at targetTick.
    const std::size_t hi = document_.upperBoundVisualStart(targetTick);
    const std::size_t blockSize = wasmidi::MidiDocument::VisualSeekBlockSize;
    const std::size_t blockCount = (hi + blockSize - 1) / blockSize;

    for (std::size_t block = 0; block < blockCount; ++block) {
        if (block < document_.visualBlockMaxEnd.size() &&
            double(document_.visualBlockMaxEnd[block]) < targetTick) {
            continue;
        }

        const std::size_t begin = block * blockSize;
        const std::size_t end = std::min(hi, begin + blockSize);
        for (std::size_t i = begin; i < end; ++i) {
            const auto& note = document_.visualNotes[i];
            if (double(note.endTick) >= targetTick)
                addVisualNote(i);
        }
    }

    const auto startIt = std::upper_bound(
        document_.visualKeyStarts.begin(),
        document_.visualKeyStarts.end(),
        targetTick,
        [](double tick, const wasmidi::VisualKeyEvent& event) {
            return tick < double(event.tick);
        });
    visualKeyStartCursor_ = static_cast<std::size_t>(
        startIt - document_.visualKeyStarts.begin());

    // A note is active at its exact end tick, so only end events strictly
    // before targetTick have already been consumed.
    const auto endIt = std::lower_bound(
        document_.visualKeyEnds.begin(),
        document_.visualKeyEnds.end(),
        targetTick,
        [](const wasmidi::VisualKeyEvent& event, double tick) {
            return double(event.tick) < tick;
        });
    visualKeyEndCursor_ = static_cast<std::size_t>(
        endIt - document_.visualKeyEnds.begin());

    const auto ownerIt = std::upper_bound(
        document_.visualKeyOwners.begin(),
        document_.visualKeyOwners.end(),
        targetTick,
        [](double tick, const wasmidi::VisualKeyOwner& owner) {
            return tick < double(owner.tick);
        });
    visualKeyOwnerCursor_ = static_cast<std::size_t>(
        ownerIt - document_.visualKeyOwners.begin());

    visualTick_ = targetTick;
    ++keyboardFrames_;
    visualStateValid_ = true;
}

void MainWindow::syncVisualState(double targetTick, bool forceRebuild)
{
    if (document_.remoteIndexed) {
#ifdef __EMSCRIPTEN__
        const bool needBaseline = forceRebuild || !visualStateValid_;
        if (forceRebuild)
            remoteLiveSnapshots_.clear();

        // Present a snapshot that was prepared ahead of time before asking the
        // Worker for another one.  Piano/stats therefore advance on the exact
        // same clock sample as the roll instead of on arbitrary Worker reply
        // latency.
        applyRemoteLiveSnapshot(targetTick);

        const double targetSeconds = std::clamp(
            document_.tickToSeconds(targetTick), 0.0, double(duration_));
        // One or two display frames of lead is enough to hide the normal Worker
        // round trip.  A 250 ms future cursor made live-state reconstruction
        // compete with renderer prefetch and increased the amount of state that
        // had to be buffered/published.
        const double requestSeconds = needBaseline
            ? targetSeconds
            : std::min<double>(duration_, targetSeconds + (isPlaying_ ? 0.034 : 0.0));
        const double requestTick = document_.secondsToTick(requestSeconds);
        const double npsStartTick = document_.secondsToTick(
            std::max(0.0, requestSeconds - 0.25));
        const double ccStartTick = document_.secondsToTick(
            std::max(0.0, requestSeconds - 1.0));
        if (!needBaseline || !remoteLiveBaselinePending_) {
            wasmidi_mapped_request_key_state(
                requestTick, npsStartTick, ccStartTick, needBaseline ? 1 : 0);
            if (needBaseline)
                remoteLiveBaselinePending_ = true;
        }
#else
        (void)targetTick;
        (void)forceRebuild;
#endif
        return;
    }

    if (document_.visualNotes.empty()) {
        clearVisualState();
        return;
    }

    const auto oldMask = visualPitchMask_;
    const auto oldColors = visualPitchColor_;

    if (forceRebuild || !visualStateValid_ || targetTick < visualTick_) {
        rebuildVisualStateAt(targetTick);
    } else {
        advanceVisualStateTo(targetTick);
    }

    if (oldMask != visualPitchMask_ || oldColors != visualPitchColor_)
        emit activePitchesChanged();
}

void MainWindow::updateDominantColor()
{
    int newColor = dominantColor_;

    if (!document_.visualNotes.empty()) {
        const double futureSeconds =
            std::min<double>(duration_, double(currentTime_) + 0.15);
        const double futureTick = document_.secondsToTick(futureSeconds);

        const std::size_t targetLo = document_.upperBoundVisualStart(
            document_.secondsToTick(currentTime_));
        const std::size_t targetHi = document_.upperBoundVisualStart(futureTick);

        const bool rebuild =
            !neuralWindowValid_ ||
            targetLo < neuralFutureLo_ ||
            targetHi < neuralFutureHi_ ||
            targetLo > targetHi;

        auto addNoteColor = [this](std::size_t index, int direction) {
            if (index >= document_.visualNotes.size())
                return;
            const uint8_t color =
                document_.visualColorIndex(document_.visualNotes[index], perTrackColors_) & 0x0f;
            if (direction > 0) {
                ++neuralFutureColors_[color];
            } else if (neuralFutureColors_[color] != 0) {
                --neuralFutureColors_[color];
            }
        };

        if (rebuild) {
            neuralFutureColors_.fill(0);
            for (std::size_t i = targetLo; i < targetHi; ++i)
                addNoteColor(i, +1);
        } else {
            while (neuralFutureLo_ < targetLo) {
                addNoteColor(neuralFutureLo_, -1);
                ++neuralFutureLo_;
            }
            while (neuralFutureHi_ < targetHi) {
                addNoteColor(neuralFutureHi_, +1);
                ++neuralFutureHi_;
            }
        }

        neuralFutureLo_ = targetLo;
        neuralFutureHi_ = targetHi;
        neuralWindowValid_ = true;

        uint64_t bestCount = 0;
        int bestColor = -1;
        for (int color = 0; color < 16; ++color) {
            const uint64_t combined =
                uint64_t(visualColorVoices_[color]) + neuralFutureColors_[color];
            if (combined > bestCount) {
                bestCount = combined;
                bestColor = color;
            }
        }

        if (bestColor >= 0 && bestColor < channelColors_.size())
            newColor = bestColor;
    }

    if (newColor != dominantColor_) {
        dominantColor_ = newColor;
        if (adaptColors_)
            emit accentColorChanged();
    }
}

QColor MainWindow::accentColor() const
{
    return adaptColors_ && dominantColor_ >= 0 && dominantColor_ < channelColors_.size()
        ? channelColors_[dominantColor_]
        : DefaultAccent;
}

void MainWindow::setAdaptColors(bool enable)
{
    if (adaptColors_ == enable)
        return;
    adaptColors_ = enable;
    WASMIDI_PERSIST_SETTING("adaptColors", adaptColors_ ? 1 : 0);
    emit accentColorChanged();
}

void MainWindow::setChannelColors(const QVariantList& colors)
{
    for (int i = 0; i < colors.size() && i < channelColors_.size(); ++i)
        setChannelColor(i, colors[i].value<QColor>());
}

void MainWindow::resetChannelColors()
{
    for (int i = 0; i < channelColors_.size(); ++i)
        setChannelColor(i, DefaultChannelColors[i]);
}

void MainWindow::applyImportedColors(const uint32_t* rgb, int count)
{
    for (int i = 0; i < count && i < channelColors_.size(); ++i)
        setChannelColor(i, QColor::fromRgb(static_cast<QRgb>(rgb[i] & 0x00ffffffu)));
}

void MainWindow::importColorConfig()
{
#ifdef __EMSCRIPTEN__
    wasmidi_browser_import_color_config();
#endif
}

void MainWindow::exportColorConfig()
{
    // The same layout as the config.xml it imports; programs that read that
    // file take its Visual/Colors list and ignore the root's name.
    QString xml = QStringLiteral(
        "<?xml version=\"1.0\" ?>\n<WebAssemblyMIDIPlayer>\n    <Visual>\n        <Colors>\n");
    for (const auto& color : channelColors_)
        xml += QStringLiteral("            <Color R=\"%1\" G=\"%2\" B=\"%3\" />\n")
                   .arg(color.red()).arg(color.green()).arg(color.blue());
    xml += QStringLiteral("        </Colors>\n    </Visual>\n</WebAssemblyMIDIPlayer>\n");
#ifdef __EMSCRIPTEN__
    const QByteArray bytes = xml.toUtf8();
    wasmidi_browser_download_text("config.xml", bytes.constData(), bytes.size());
#else
    Q_UNUSED(xml);
#endif
}

QVariantMap MainWindow::takeKeyboardSample()
{
    const double lag = hasMidi() && visualStateValid_
        ? std::max(0.0, double(currentTime_) - document_.tickToSeconds(visualTick_))
        : 0.0;
    QVariantMap sample;
    sample.insert(QStringLiteral("frames"), keyboardFrames_);
    sample.insert(QStringLiteral("lag"), lag);
    keyboardFrames_ = 0;
    return sample;
}

void MainWindow::updateLiveStats()
{
    if (!hasMidi()) {
        if (nps_ != 0) {
            nps_ = 0;
            emit npsChanged();
        }

        if (activeVoices_ != 0) {
            activeVoices_ = 0;
            emit activeVoicesChanged();
        }

        if (ccPerSecond_ != 0) {
            ccPerSecond_ = 0;
            emit ccPerSecondChanged();
        }

        return;
    }

    if (document_.remoteIndexed) {
        const double time = currentTime_;
        const double tick = document_.secondsToTick(time);
        syncVisualState(tick);

        // NPS, controller density, polyphony and piano state arrive together
        // from the mapped Worker's rolling live cursor. Do not approximate
        // these values from precomputed all-song histograms.

        float newBpm = 120.0f;
        if (!tempoTimes_.empty()) {
            auto it = std::upper_bound(tempoTimes_.begin(), tempoTimes_.end(), currentTime_);
            std::size_t index = it == tempoTimes_.begin() ? 0 :
                static_cast<std::size_t>((it - tempoTimes_.begin()) - 1);
            index = std::min(index, tempoBpms_.size() - 1);
            newBpm = tempoBpms_[index];
        }
        if (!qFuzzyCompare(bpm_, newBpm)) { bpm_ = newBpm; emit bpmChanged(); }
        updateDominantColor();
        return;
    }

    if (document_.tickGroups.empty())
        return;

    const double time =
        currentTime_;

    const uint32_t currentTick =
        static_cast<uint32_t>(
            std::min<double>(
                document_.maxTick,
                std::floor(
                    document_.secondsToTick(
                        time))));

    // MPWGL2 live NPS is exactly:
    // (upperBound(startArr,t) - lowerBound(startArr,t-0.25)) * 4.
    // visualNotes is the same stable start-ordered note stream, so perform the
    // identical binary searches in fractional tick space.
    const double currentVisualTick =
        document_.secondsToTick(time);

    const double npsStartVisualTick =
        document_.secondsToTick(
            std::max(
                0.0,
                time - 0.25));

    const std::size_t exactNpsLo =
        document_.lowerBoundVisualStart(
            npsStartVisualTick);

    const std::size_t exactNpsHi =
        document_.upperBoundVisualStart(
            currentVisualTick);

    const uint32_t ccStartTick =
        static_cast<uint32_t>(
            std::max(
                0.0,
                std::floor(
                    document_.secondsToTick(
                        std::max(
                            0.0,
                            time - 1.0)))));

    const std::size_t targetHi =
        document_.upperBoundGroup(
            currentTick);

    const std::size_t targetCcLo =
        document_.lowerBoundGroup(
            ccStartTick);

    const bool discontinuity =
        !liveWindowsValid_ ||
        time < liveLastTime_ ||
        time - liveLastTime_ > 0.5;

    if (discontinuity) {
        liveNpsCount_ = 0;
        liveCcCount_ = 0;

        for (std::size_t i = targetCcLo;
             i < targetHi;
             ++i) {
            liveCcCount_ +=
                document_.tickGroups[i]
                    .controlCount;
        }

        liveHi_ = targetHi;
        liveNpsLo_ = 0;
        liveCcLo_ = targetCcLo;
        liveWindowsValid_ = true;
    } else {
        while (liveHi_ < targetHi) {
            const auto& group =
                document_.tickGroups[liveHi_];

            liveCcCount_ +=
                group.controlCount;

            ++liveHi_;
        }
        while (liveCcLo_ < targetCcLo &&
               liveCcLo_ < liveHi_) {
            liveCcCount_ -=
                document_.tickGroups[
                    liveCcLo_]
                    .controlCount;

            ++liveCcLo_;
        }
    }

    liveLastTime_ =
        static_cast<float>(time);

    // Keyboard/polyphony state uses the fractional tick corresponding to the
    // exact playback time. Using floor(currentTick) kept NoteOff keys lit until
    // the next whole MIDI tick, which was visible at low PPQ / slow tempos.
    syncVisualState(
        currentVisualTick);

    const uint64_t exactNpsCount =
        exactNpsHi >= exactNpsLo
            ? uint64_t(
                exactNpsHi -
                exactNpsLo)
            : 0u;

    const int newNps =
        static_cast<int>(
            std::min<uint64_t>(
                exactNpsCount * 4u,
                uint64_t(
                    std::numeric_limits<int>::max())));

    const int newCc =
        static_cast<int>(
            std::min<uint64_t>(
                liveCcCount_,
                uint64_t(
                    std::numeric_limits<int>::max())));

    const int newActive =
        visualActiveVoices_;

    float newBpm = 120.0f;

    if (!tempoTimes_.empty()) {
        auto it =
            std::upper_bound(
                tempoTimes_.begin(),
                tempoTimes_.end(),
                currentTime_);

        std::size_t index = 0;

        if (it != tempoTimes_.begin()) {
            index =
                static_cast<std::size_t>(
                    (it -
                     tempoTimes_.begin()) -
                    1);
        }

        index =
            std::min(
                index,
                tempoBpms_.size() - 1);

        newBpm =
            tempoBpms_[index];
    }

    if (nps_ != newNps) {
        nps_ = newNps;
        emit npsChanged();
    }

    if (activeVoices_ != newActive) {
        activeVoices_ = newActive;
        emit activeVoicesChanged();
    }

    if (ccPerSecond_ != newCc) {
        ccPerSecond_ = newCc;
        emit ccPerSecondChanged();
    }

    bool peakChanged = false;
    if (newNps > peakNps_) {
        peakNps_ = newNps;
        peakNpsTime_ = currentTime_;
        emit peakNpsChanged();
        peakChanged = true;
    }
    if (newActive > peakPolyphony_) {
        peakPolyphony_ = newActive;
        peakPolyphonyTime_ = currentTime_;
        emit peakPolyphonyChanged();
        peakChanged = true;
    }
    if (peakChanged)
        emit timelineChanged();

    if (!qFuzzyCompare(
            bpm_,
            newBpm)) {
        bpm_ = newBpm;
        emit bpmChanged();
    }

    updateDominantColor();
}

void MainWindow::updateCurrentTime()
{
    if (!isPlaying_)
        return;

    // Match MPWGL2's transport model: playback time is derived directly from
    // a monotonic wall clock.  Rendering, keyboard snapshots and the 64-screen
    // visual cache observe this clock; none of them are allowed to throttle it.
    // This prevents a slow/late visual page from permanently putting the roll
    // behind the song and also keeps SnappySynth scheduling independent.
    const qint64 elapsedMs =
        playbackClock_.isValid()
            ? playbackClock_.elapsed()
            : 0;

    playbackLastElapsedMs_ = elapsedMs;

    float nextTime =
        std::clamp(
            playbackAnchorSeconds_ +
                static_cast<float>(elapsedMs) / 1000.0f,
            0.0f,
            duration_);

#ifdef __EMSCRIPTEN__
    if (soundfontLoaded_ && synthReady_) {
        // The roll, keyboard and graphs run on the wall clock; WasmiSynth can
        // only re-anchor it, never slow it down (HANDOFF sec. 48).
        const double audioClock = wasmidi_snappy_audio_clock();
        const double wallTime = double(nextTime);
        if (std::isfinite(audioClock) && audioClock >= 0.0) {
            const double gap = wallTime - audioClock;
            if (!presentationOffsetLocked_) {
                if (audioClock > double(playbackAnchorSeconds_) + 0.001 || elapsedMs > 1000) {
                    presentationOffsetSeconds_ = std::clamp(gap, 0.0, 1.0);
                    presentationOffsetLocked_ = true;
                } else {
                    nextTime = playbackAnchorSeconds_;
                }
            } else if (gap < presentationOffsetSeconds_) {
                presentationOffsetSeconds_ = std::max(0.0, gap);
            }
        }
        if (presentationOffsetLocked_)
            nextTime = static_cast<float>(std::clamp<double>(
                wallTime - presentationOffsetSeconds_, 0.0, double(duration_)));
    }
#endif

    currentTime_ = nextTime;

    if (currentTime_ >= duration_) {
        stop();
        return;
    }

    updateSynthSynchronization();

    emit currentTimeChanged();
    updateLiveStats();
}

void MainWindow::updateEffectiveVelocityFloor(double renderLoadRatio)
{
    // Fidelity mode: the supplied native SnappySynth never discards quiet notes
    // merely because the mixer is under pressure. Browser-side adaptive velocity
    // shedding changes the musical result and was a major source of audible
    // divergence. Keep only the explicit user-selected floor (default 0).
    (void)renderLoadRatio;
    synthVelocityLastRaiseElapsedMs_ = -1;
    synthEffectiveVelocityFloor_ = std::clamp(synthVelocityFloor_, 0, 127);

    if (skippedVelocity_ != synthEffectiveVelocityFloor_) {
        skippedVelocity_ = synthEffectiveVelocityFloor_;
        emit skippedVelocityChanged();
    }
}

void MainWindow::updateSynthSynchronization()
{
#ifdef __EMSCRIPTEN__
    // SnappySynthV2 is a continuous realtime renderer. Do not seek/reset it in
    // response to transient Worklet lag: flushing voices/ring state was turning
    // one short underrun into the repeatable "plays for a second, then dies"
    // failure on dense MIDIs. Audio is independent from the renderer; the
    // compact MIDI event-coverage scheduler owns the look-ahead horizon.
    updateEffectiveVelocityFloor(0.0);
    synthWasStarved_ = wasmidi_snappy_starved() != 0;
    if (!synthWasStarved_)
        synthStarvedSinceElapsedMs_ = -1;
#else
    updateEffectiveVelocityFloor(0.0);
#endif
}

uint32_t MainWindow::packSynthMessage(const wasmidi::CompactEvent& event) const
{
    const uint8_t command = event.status & 0xf0;
    if (command != 0x80 && command != 0x90 && command != 0xb0 &&
        command != 0xc0 && command != 0xe0) {
        return 0xffffffffu;
    }

    return uint32_t(event.status) |
           (uint32_t(event.data1) << 8) |
           (uint32_t(event.data2) << 16);
}

void MainWindow::resetSynthSchedule(float seconds)
{
    const double time = std::clamp<double>(seconds, 0.0, duration_);
    const uint32_t tick = static_cast<uint32_t>(
        std::max(0.0, std::floor(document_.secondsToTick(time))));

    if (document_.remoteIndexed) {
        synthGroupCursor_ = 0;
        synthSysExCursor_ = 0;
        synthScheduledUntil_ = time;
        synthMessages_.clear();
        synthTimes_.clear();
#ifdef __EMSCRIPTEN__
        if (soundfontLoaded_) {
            wasmidi_mapped_synth_reset(double(tick));
            // Do not post an empty synth schedule here. The audio worker now
            // waits for the mapped producer's first ACK after play(reset)/seek;
            // an empty main-thread batch would falsely satisfy that handshake
            // and let NumBuffers of silence get queued before the real MIDI.
        }
#endif
        return;
    }

    synthGroupCursor_ = document_.lowerBoundGroup(tick);
    synthSysExCursor_ = static_cast<std::size_t>(
        std::lower_bound(
            document_.sysEx.begin(), document_.sysEx.end(), tick,
            [](const wasmidi::SysExEvent& event, uint32_t value) {
                return event.tick < value;
            }) - document_.sysEx.begin());
    synthScheduledUntil_ = time;
    synthMessages_.clear();
    synthTimes_.clear();

#ifdef __EMSCRIPTEN__
    if (!soundfontLoaded_)
        return;

    // Restore SnappySynthV2's GM/GS/XG SysEx state exactly as the original
    // driver does. SysEx count is normally tiny, so replaying the historical
    // state at seek is cheap and preserves drum-part/channel remaps/tuning.
    for (std::size_t si = 0; si < synthSysExCursor_; ++si) {
        const auto& sx = document_.sysEx[si];
        if (!sx.data.empty()) {
            wasmidi_snappy_schedule_sysex(
                sx.data.data(), static_cast<int>(sx.data.size()), time);
        }
    }

    // Restore the last controller/program/bend state at a seek. Groups with no
    // control data are skipped, so this scans the sparse control history rather
    // than materializing or replaying every NoteOn in a Black MIDI.
    std::array<std::array<int16_t, 128>, 16> cc{};
    for (auto& row : cc)
        row.fill(-1);
    std::array<int16_t, 16> program{};
    std::array<int16_t, 16> bend{};
    program.fill(-1);
    bend.fill(-1);

    for (std::size_t gi = 0; gi < synthGroupCursor_; ++gi) {
        const auto& group = document_.tickGroups[gi];
        if (group.controlCount == 0)
            continue;
        const std::size_t end = group.eventOffset + group.eventCount;
        for (std::size_t ei = group.eventOffset; ei < end; ++ei) {
            const auto& event = document_.events[ei];
            const uint8_t cmd = event.status & 0xf0;
            const uint8_t ch = event.status & 0x0f;
            if (cmd == 0xb0)
                cc[ch][event.data1 & 0x7f] = event.data2 & 0x7f;
            else if (cmd == 0xc0)
                program[ch] = event.data1 & 0x7f;
            else if (cmd == 0xe0)
                bend[ch] = int16_t((event.data1 & 0x7f) | ((event.data2 & 0x7f) << 7));
        }
    }

    auto appendState = [this, time](uint32_t message) {
        synthMessages_.push_back(message);
        synthTimes_.push_back(time);
    };

    for (int ch = 0; ch < 16; ++ch) {
        if (cc[ch][0] >= 0)
            appendState(uint32_t(0xb0 | ch) | (0u << 8) | (uint32_t(cc[ch][0]) << 16));
        if (cc[ch][32] >= 0)
            appendState(uint32_t(0xb0 | ch) | (32u << 8) | (uint32_t(cc[ch][32]) << 16));
        if (program[ch] >= 0)
            appendState(uint32_t(0xc0 | ch) | (uint32_t(program[ch]) << 8));
        for (int controller = 1; controller < 128; ++controller) {
            if (controller == 32 || cc[ch][controller] < 0)
                continue;
            appendState(uint32_t(0xb0 | ch) |
                        (uint32_t(controller) << 8) |
                        (uint32_t(cc[ch][controller]) << 16));
        }
        if (bend[ch] >= 0) {
            const uint16_t value = static_cast<uint16_t>(bend[ch]);
            appendState(uint32_t(0xe0 | ch) |
                        (uint32_t(value & 0x7f) << 8) |
                        (uint32_t((value >> 7) & 0x7f) << 16));
        }
    }

    // Restore notes that genuinely span the seek point. VisualNote carries the
    // original channel in bits 24..27; future original NoteOff events will close
    // these voices at their correct MIDI time. Notes that start exactly on the
    // seek tick are NOT restored here because synthGroupCursor_ will schedule
    // their original NoteOns, preventing duplicate voices at the boundary.
    const std::size_t hi =
        tick == 0
            ? 0
            : document_.lowerBoundVisualStart(double(tick));
    const std::size_t blockSize = wasmidi::MidiDocument::VisualSeekBlockSize;
    const std::size_t blockCount = (hi + blockSize - 1) / blockSize;
    for (std::size_t block = 0; block < blockCount; ++block) {
        if (block < document_.visualBlockMaxEnd.size() &&
            document_.visualBlockMaxEnd[block] < tick)
            continue;
        const std::size_t begin = block * blockSize;
        const std::size_t end = std::min(hi, begin + blockSize);
        for (std::size_t i = begin; i < end; ++i) {
            const auto& note = document_.visualNotes[i];
            if (note.endTick < tick)
                continue;
            const uint8_t velocity = note.packedData & 0x7f;
            if (velocity < synthEffectiveVelocityFloor_)
                continue;
            const uint8_t pitch = (note.packedData >> 8) & 0x7f;
            const uint8_t channel = (note.packedData >> 24) & 0x0f;
            appendState(uint32_t(0x90 | channel) |
                        (uint32_t(pitch) << 8) |
                        (uint32_t(velocity) << 16));
        }
    }

    if (!synthMessages_.empty()) {
        wasmidi_snappy_schedule(synthMessages_.data(), synthTimes_.data(),
                                static_cast<int>(synthMessages_.size()), time);
    } else {
        wasmidi_snappy_schedule(nullptr, nullptr, 0, time);
    }
    synthMessages_.clear();
    synthTimes_.clear();
#endif
}

void MainWindow::scheduleSynthAhead()
{
#ifdef __EMSCRIPTEN__
    if (!isPlaying_ || !soundfontLoaded_)
        return;

    constexpr std::size_t BatchLimit = 65536;

    const double rate =
        synthSampleRate_ > 0 ? double(synthSampleRate_) : 44100.0;
    const double blockSeconds =
        double(std::max(1, synthBufferFrames_)) / rate;
    const double transportSeconds =
        blockSeconds * double(std::max(1, synthNumBuffers_));

    // MIDI EVENT look-ahead is not PCM pre-render.  The original SnappySynth
    // realtime contract keeps event admission ahead of the device timeline;
    // starving the worker at a one-second safeUntil boundary is what produced
    // the repeatable "one second then silence" failure.  Keep several seconds
    // of compact event coverage while the PCM ring remains only NumBuffers deep.
    const double requestedAhead =
        std::clamp(transportSeconds * 8.0 + blockSeconds * 4.0,
                   2.0,
                   4.0);
    const double audioClock = wasmidi_snappy_audio_clock();
    const double scheduleBase = std::max<double>(
        double(currentTime_),
        std::isfinite(audioClock) && audioClock >= 0.0
            ? audioClock
            : double(currentTime_));

    const double desiredTarget =
        std::min<double>(duration_, scheduleBase + requestedAhead);

    if (document_.remoteIndexed) {
        // Feed the mapped parser the complete realtime look-ahead in one logical
        // request.  pumpSynthWindow() already streams it in bounded batches and
        // advances safeUntil at exact tick boundaries, so slicing this again
        // into ~40 ms requests only increased latency and let the AudioWorklet
        // run out of MIDI between requests on dense files.
        // Do not stop asking for a farther horizon merely because the previous
        // dense batch is still in flight. The bridge coalesces this into one
        // furthest queued request, so coverage advances continuously instead
        // of waiting for the old one-second request to finish first.
        const double target = desiredTarget;
        if (target <= synthScheduledUntil_ + 0.015)
            return;

        const uint32_t targetTick = static_cast<uint32_t>(
            std::min<double>(document_.maxTick,
                             std::ceil(document_.secondsToTick(target))));
        double safeUntil = target;
        if (target >= double(duration_) - 1e-7)
            safeUntil += std::max(0.05, blockSeconds * 2.0);

        wasmidi_mapped_synth_pump(
            double(targetTick), safeUntil, synthEffectiveVelocityFloor_);
        synthScheduledUntil_ = target;
        return;
    }

    const double target = desiredTarget;
    if (target <= synthScheduledUntil_ + 0.015)
        return;

    if (document_.tickGroups.empty())
        return;

    const uint32_t targetTick = static_cast<uint32_t>(
        std::min<double>(document_.maxTick,
                         std::ceil(document_.secondsToTick(target))));
    const std::size_t endGroup = document_.upperBoundGroup(targetTick);

    auto flush = [this](double safeUntil) {
        if (synthMessages_.empty()) {
            wasmidi_snappy_schedule(nullptr, nullptr, 0, safeUntil);
            return;
        }
        wasmidi_snappy_schedule(synthMessages_.data(), synthTimes_.data(),
                                static_cast<int>(synthMessages_.size()), safeUntil);
        synthMessages_.clear();
        synthTimes_.clear();
    };

    synthMessages_.clear();
    synthTimes_.clear();
    synthMessages_.reserve(std::min<std::size_t>(BatchLimit, 8192));
    synthTimes_.reserve(std::min<std::size_t>(BatchLimit, 8192));

    for (; synthGroupCursor_ < endGroup; ++synthGroupCursor_) {
        const auto& group = document_.tickGroups[synthGroupCursor_];
        const double eventTime = document_.tickToSeconds(group.tick);
        const std::size_t end = group.eventOffset + group.eventCount;

        for (std::size_t i = group.eventOffset; i < end; ++i) {
            const auto& event = document_.events[i];
            uint32_t message = packSynthMessage(event);
            if (message == 0xffffffffu)
                continue;

            // Source SnappySynthV2 supports exact-overlap stack count in the
            // high byte. Pack only consecutive identical NoteOns so controller
            // ordering at the same tick can never be changed.
            if ((event.status & 0xf0) == 0x90 && event.data2 != 0) {
                std::size_t run = 1;
                while (i + run < end && run < 256) {
                    const auto& next = document_.events[i + run];
                    if (packSynthMessage(next) != message)
                        break;
                    ++run;
                }

                if (event.data2 < synthEffectiveVelocityFloor_) {
                    i += run - 1;
                    continue;
                }

                if (run > 1) {
                    message |= uint32_t(run - 1) << 24;
                    i += run - 1;
                }
            }

            synthMessages_.push_back(message);
            synthTimes_.push_back(eventTime);

            if (synthMessages_.size() >= BatchLimit)
                flush(synthScheduledUntil_);
        }
    }

    // Forward all original SysEx through the SnappySynthV2 SysEx dispatcher.
    while (synthSysExCursor_ < document_.sysEx.size() &&
           document_.sysEx[synthSysExCursor_].tick <= targetTick) {
        const auto& sx = document_.sysEx[synthSysExCursor_++];
        if (!sx.data.empty()) {
            wasmidi_snappy_schedule_sysex(
                sx.data.data(), static_cast<int>(sx.data.size()),
                document_.tickToSeconds(sx.tick));
        }
    }

    // Only advance safeUntil after every event through the target horizon has
    // been transferred; this prevents the audio worker from rendering past a
    // partially delivered ultra-dense tick.
    // Let the final audio block cross the exact MIDI duration by a tiny
    // bounded margin. Without this, a 128/256/512-frame AudioWorklet request
    // that straddles the last MIDI timestamp can be refused forever, leaving
    // the audio clock a few samples short of `duration_` and preventing the
    // normal end-of-song stop condition.
    double safeUntil = target;
    if (endGroup < document_.tickGroups.size()) {
        // All events before the next tick are already delivered, so the event
        // coverage boundary is that next event time rather than the arbitrary
        // request edge. This prevents silence during event-free gaps.
        safeUntil = std::max(
            safeUntil,
            document_.tickToSeconds(document_.tickGroups[endGroup].tick));
    }
    if (target >= double(duration_) - 1e-7) {
        const double rate = synthSampleRate_ > 0
            ? double(synthSampleRate_)
            : 48000.0;
        safeUntil += std::max(
            0.05,
            (double(synthBufferFrames_) / rate) * 2.0);
    }

    flush(safeUntil);
    synthScheduledUntil_ = target;
#endif
}

void MainWindow::dispatchScheduler()
{
    scheduleSynthAhead();
}

void MainWindow::pollSynthState()
{
#ifdef __EMSCRIPTEN__
    const bool ready = wasmidi_snappy_ready() != 0;
    const bool becameReady = ready && !synthReady_;
    const bool loaded = wasmidi_snappy_soundfont_loaded() != 0;
    const int sampleRate = wasmidi_snappy_sample_rate();
    const int active = wasmidi_snappy_active_voices();
    const int freeVoices = wasmidi_snappy_free_voices();
    const int steals = wasmidi_snappy_steals();
    const int rebalanced = wasmidi_snappy_rebalanced();
    const int droppedNotes = wasmidi_snappy_dropped_notes();
    const int layers = wasmidi_snappy_layers();
    const int regions = wasmidi_snappy_regions();
    const int workerCount = wasmidi_snappy_worker_count();
    const int underruns = wasmidi_snappy_underruns();

    char nameBuffer[512] = {};
    char statusBuffer[512] = {};
    wasmidi_snappy_copy_string(0, nameBuffer, int(sizeof(nameBuffer)));
    wasmidi_snappy_copy_string(1, statusBuffer, int(sizeof(statusBuffer)));
    const QString name = QString::fromUtf8(nameBuffer);
    const QString status = QString::fromUtf8(statusBuffer);

    const bool changed =
        ready != synthReady_ || loaded != soundfontLoaded_ ||
        sampleRate != synthSampleRate_ || active != synthActiveVoices_ ||
        freeVoices != synthFreeVoices_ || steals != synthSteals_ ||
        rebalanced != synthRebalanced_ || droppedNotes != synthDroppedNotes_ ||
        wasmidi_snappy_render_load_x10() / 10.0 != synthRenderLoad_ ||
        layers != synthLayers_ || regions != synthRegions_ ||
        workerCount != synthWorkerCount_ ||
        underruns != synthUnderruns_ || name != soundfontName_ || status != synthStatus_;

    synthReady_ = ready;
    soundfontLoaded_ = loaded;
    synthSampleRate_ = sampleRate;
    synthActiveVoices_ = active;
    synthFreeVoices_ = freeVoices;
    synthSteals_ = steals;
    synthRebalanced_ = rebalanced;
    synthDroppedNotes_ = droppedNotes;
    synthRenderLoad_ = wasmidi_snappy_render_load_x10() / 10.0;
    synthRenderLatencyMs_ = wasmidi_snappy_render_latency_x100() / 100.0;
    synthDispatchMs_ = wasmidi_snappy_dispatch_x100() / 100.0;
    synthSimdPercent_ = wasmidi_snappy_path_simd_percent();
    synthWorkersActive_ = wasmidi_snappy_workers_active();
    synthAllocMs_ = wasmidi_snappy_alloc_x10() / 10.0;
    synthFreeRatio_ = wasmidi_snappy_free_ratio();
    synthCcCollapsed_ = wasmidi_snappy_cc_collapsed();
    {
        const int packed = wasmidi_snappy_cc_hotspot();
        synthCcHotNumber_ = (packed >> 16) & 0x7f;
        synthCcHotPercent_ = packed & 0xffff;
        synthCcHotCount_ = wasmidi_snappy_cc_hotspot_count();
    }
    {
        const int packed = wasmidi_snappy_presample();
        synthPresampleSeen_ = wasmidi_snappy_presample_seen();
        synthPresampleResampled_ = (packed >> 16) & 0xffff;
        synthPresampleSkipped_ = packed & 0xffff;
    }
    {
        const int packed = wasmidi_snappy_miss_reason();
        synthMissReason_ = packed >> 8;
        synthMissPercent_ = packed & 0xff;
    }
    synthWorkerBusyMs_ = wasmidi_snappy_worker_busy_x10() / 10.0;
    synthPumpGapMs_ = wasmidi_snappy_pump_gap_ms();
    synthRingFill_ = wasmidi_snappy_ring_fill();
    synthLayers_ = layers;
    synthRegions_ = regions;
    synthWorkerCount_ = workerCount;
    synthUnderruns_ = underruns;
    soundfontName_ = name;
    synthStatus_ = status.isEmpty() ? QStringLiteral("SnappySynthV2 idle") : status;

    if (becameReady) {
        // The bridge may not have existed yet during MainWindow construction.
        // Re-apply the one persisted configuration atomically when it becomes
        // available instead of relying on JS defaults after a page reload.
        applySynthConfig();
        wasmidi_snappy_set_volume(volume_);
        wasmidi_snappy_set_overlap_gain(synthOverlapGain_ ? 1 : 0);
    wasmidi_snappy_set_gpu(synthGpu_ ? 1 : 0);
        publishSynthPrebufferConfig();
    }

    if (loaded && !synthWasSoundfontLoaded_) {
        synthPlaybackPrimed_ = false;
        if (isPlaying_) {
            wasmidi_snappy_local_floor(synthEffectiveVelocityFloor_);
            wasmidi_snappy_play(currentTime_, 1);
            resetSynthSchedule(currentTime_);
            synthPlaybackPrimed_ = true;
            scheduleSynthAhead();
        }
    }
    if (!loaded)
        synthPlaybackPrimed_ = false;
    synthWasSoundfontLoaded_ = loaded;

    if (changed)
        emit synthStateChanged();
#endif
}
