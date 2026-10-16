#include "midi_document_codec.hpp"
#include "midi_parser.hpp"
#include "midi_mapped_store.hpp"
#include "bpfa_midi_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <exception>
#include <new>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {

// BPFA loader by default (HANDOFF sec. 44); -DWASMIDI_PARSER_BPFA=0 restores the old store.
#ifndef WASMIDI_PARSER_BPFA
#define WASMIDI_PARSER_BPFA 1
#endif
#if WASMIDI_PARSER_BPFA
wasmidi::BpfaMidiStore g_mappedStore;
// Raw (uncompressed) source up to this size; set by the worker from the memory budget
// before each parse (HANDOFF sec. 54).
uint64_t g_rawSourceLimit = 512ull * 1024ull * 1024ull;
#else
wasmidi::MidiMappedStore g_mappedStore;
#endif
wasmidi::MidiDocument g_document;
std::vector<wasmidi::VisualNote> g_visualPage;
std::vector<wasmidi::MidiMappedStore::EventWord> g_eventBatch;
std::vector<wasmidi::MidiMappedStore::EventWord> g_historicalSelectorState;
std::vector<double> g_eventBatchTimes;
std::vector<wasmidi::MidiMappedStore::SysExBatchEvent> g_sysExBatchEvents;
std::vector<uint8_t> g_sysExBatchBytes;
std::vector<wasmidi::VisualNote> g_renderAppends;
std::vector<wasmidi::MidiMappedStore::RenderClose> g_renderCloses;
uint32_t g_renderAppendBase = 1;
bool g_renderBatchComplete = false;
uint32_t g_renderBatchNextTick = 0;
bool g_renderBatchHasNextTick = false;
std::vector<uint32_t> g_keySnapshotWords;
bool g_eventBatchComplete = false;
uint32_t g_eventBatchNextTick = 0;
bool g_eventBatchHasNextTick = false;
std::vector<uint8_t> g_result;
std::string g_error;

#ifdef __EMSCRIPTEN__
EM_JS(void, wmp_post_progress, (int percent, double stageAddress), {
    // Memory64 C/C++ pointers are i64 and enter JavaScript as BigInt. Do not
    // pass a raw const char* into UTF8ToString(): Emscripten 3.1.56 asserts
    // that UTF8ToString receives a Number. C++ legalizes the pointer to f64
    // first; the current <=16 GiB Memory64 address range is exactly representable.
    const stage = Number(stageAddress);
    const stageText = stage ? UTF8ToString(stage) : "Parsing MIDI";
    const absolutePercent = 15 + Math.floor(
        Math.max(0, Math.min(100, percent | 0)) * 0.79);

    // The production parser runs in a browser Worker, while CI executes the
    // exact generated module in Node. Never assume the Worker-only `self`
    // global exists just to report optional progress.
    const root = typeof globalThis !== "undefined"
        ? globalThis
        : (typeof self !== "undefined" ? self : null);

    if (root) {
        root.__wasmidiMidiParserStage = stageText;
        root.__wasmidiMidiParserPercent = absolutePercent;
    }

    if (typeof postMessage === "function") {
        postMessage({
            type: "progress",
            percent: absolutePercent,
            stage: stageText
        });
    }
});

EM_JS(void, wmp_post_absolute_progress, (int percent, double stageAddress), {
    const stage = Number(stageAddress);
    const stageText = stage ? UTF8ToString(stage) : "Loading MIDI";
    const absolutePercent = Math.max(0, Math.min(100, percent | 0));

    const root = typeof globalThis !== "undefined"
        ? globalThis
        : (typeof self !== "undefined" ? self : null);

    if (root) {
        root.__wasmidiMidiParserStage = stageText;
        root.__wasmidiMidiParserPercent = absolutePercent;
    }

    if (typeof postMessage === "function") {
        postMessage({
            type: "progress",
            percent: absolutePercent,
            stage: stageText
        });
    }
});

EM_JS(int, wmp_read_file_slice,
      (double sourceOffset, double byteCount, double destinationAddress), {
    const root = typeof globalThis !== "undefined"
        ? globalThis
        : (typeof self !== "undefined" ? self : null);

    const offset = Number(sourceOffset);
    const count = Number(byteCount);
    const destination = Number(destinationAddress);

    const fail = message => {
        if (root)
            root.__wasmidiMidiParserReadError = String(message || "MIDI source read failed");
        return 0;
    };

    if (!root ||
        !Number.isSafeInteger(offset) || offset < 0 ||
        !Number.isSafeInteger(count) || count < 0 ||
        !Number.isSafeInteger(destination) || destination < 0 ||
        !Number.isSafeInteger(destination + count) ||
        destination + count > HEAPU8.length) {
        return fail("Invalid bounded MIDI source read request");
    }

    if (count === 0)
        return 1;

    try {
        let bytes = null;

        // Fast source path used for files small enough to keep one contiguous
        // browser-side copy. It removes repeated Blob.slice/FileReaderSync
        // crossings while preserving exactly the same C++ mapped parser.
        const whole = root.__wasmidiMidiParserWholeFile;
        if (whole instanceof Uint8Array) {
            if (offset + count > whole.byteLength)
                return fail("Contiguous MIDI source read exceeds file bounds");
            bytes = whole.subarray(offset, offset + count);
        }
        // CI/diagnostic path: the exact generated Memory64 module can provide a
        // synchronous read-at hook without requiring browser FileReaderSync.
        else if (typeof root.__wasmidiMidiParserReadAt === "function") {
            const result = root.__wasmidiMidiParserReadAt(offset, count);
            if (result instanceof Uint8Array) {
                bytes = result;
            } else if (result instanceof ArrayBuffer) {
                bytes = new Uint8Array(result);
            } else if (ArrayBuffer.isView(result)) {
                bytes = new Uint8Array(
                    result.buffer,
                    result.byteOffset,
                    result.byteLength);
            }
        } else {
            const file = root.__wasmidiMidiParserFile;
            if (!file || typeof file.slice !== "function")
                return fail("Browser MIDI File source is unavailable");

            if (typeof FileReaderSync !== "function")
                return fail("FileReaderSync is unavailable in the MIDI parser Worker");

            let reader = root.__wasmidiMidiParserFileReader;
            if (!reader) {
                reader = new FileReaderSync();
                root.__wasmidiMidiParserFileReader = reader;
            }

            const buffer = reader.readAsArrayBuffer(
                file.slice(offset, offset + count));
            bytes = new Uint8Array(buffer);
        }

        if (!bytes || bytes.byteLength !== count) {
            return fail(
                "MIDI source returned " +
                (bytes ? bytes.byteLength : 0) +
                " bytes, expected " + count);
        }

        HEAPU8.set(bytes, destination);
        return 1;
    } catch (error) {
        return fail(
            error && error.message
                ? error.message
                : String(error || "MIDI source read failed"));
    }
});

#endif

void progressCallback(void*, int percent, const char* stage)
{
#ifdef __EMSCRIPTEN__
    const double stageAddress = stage
        ? static_cast<double>(reinterpret_cast<std::uintptr_t>(stage))
        : 0.0;
    wmp_post_progress(percent, stageAddress);
#else
    (void)percent;
    (void)stage;
#endif
}


bool browserReadAt(void*,
                   uint64_t offset,
                   uint8_t* destination,
                   std::size_t byteCount)
{
#ifdef __EMSCRIPTEN__
    constexpr uint64_t JsExactIntegerLimit = 9007199254740991ull;

    if ((!destination && byteCount != 0) ||
        offset > JsExactIntegerLimit ||
        uint64_t(byteCount) > JsExactIntegerLimit ||
        reinterpret_cast<std::uintptr_t>(destination) > JsExactIntegerLimit) {
        return false;
    }

    return wmp_read_file_slice(
        static_cast<double>(offset),
        static_cast<double>(byteCount),
        static_cast<double>(reinterpret_cast<std::uintptr_t>(destination))) != 0;
#else
    (void)offset;
    (void)destination;
    (void)byteCount;
    return false;
#endif
}

#if defined(__EMSCRIPTEN__) && WASMIDI_PARSER_BPFA
// Synth feed helper (HANDOFF sec. 70): compressed source slabs shared by the parser
// Worker as SharedArrayBuffers of sourceSlabBytes() each.
EM_JS(int, wmp_copy_feed_slab, (double physicalOffset, double byteCount, double destinationAddress), {
    const root = typeof globalThis !== "undefined" ? globalThis : self;
    const slabs = root.__wasmidiFeedSlabs;
    const slabBytes = root.__wasmidiFeedSlabBytes;
    const offset = Number(physicalOffset), count = Number(byteCount), destination = Number(destinationAddress);
    if (!slabs || !slabBytes) return 0;
    const index = Math.floor(offset / slabBytes), at = offset - index * slabBytes;
    const slab = slabs[index];
    if (!slab || at + count > slab.byteLength || destination + count > HEAPU8.length) return 0;
    HEAPU8.set(new Uint8Array(slab, at, count), destination);
    return 1;
});
#endif

bool feedSlabFetch(void*, uint64_t physicalOffset, uint8_t* destination, std::size_t byteCount)
{
#if defined(__EMSCRIPTEN__) && WASMIDI_PARSER_BPFA
    return wmp_copy_feed_slab(static_cast<double>(physicalOffset), static_cast<double>(byteCount),
                              static_cast<double>(reinterpret_cast<std::uintptr_t>(destination))) != 0;
#else
    (void)physicalOffset; (void)destination; (void)byteCount;
    return false;
#endif
}

} // namespace

namespace {

constexpr double kJsExactIntegerLimit = 9007199254740991.0; // 2^53 - 1

bool jsExactNonNegativeInteger(double value)
{
    return std::isfinite(value) &&
           value >= 0.0 &&
           value <= kJsExactIntegerLimit &&
           std::floor(value) == value;
}

bool jsSizeToNative(double value, std::size_t& size)
{
    if (!jsExactNonNegativeInteger(value) ||
        value > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        size = 0;
        return false;
    }

    size = static_cast<std::size_t>(value);
    return true;
}

double pointerToJsAddress(const void* pointer)
{
    return static_cast<double>(
        reinterpret_cast<std::uintptr_t>(pointer));
}

int parseFileDocument(std::size_t size)
{
    try {
        std::vector<uint8_t>().swap(g_result);
        std::vector<wasmidi::VisualNote>().swap(g_visualPage);
        std::vector<wasmidi::MidiMappedStore::EventWord>().swap(g_eventBatch);
        std::vector<uint32_t>().swap(g_keySnapshotWords);
        g_error.clear();
        g_document = wasmidi::MidiDocument{};
        g_mappedStore.clear();

        // Pass 13: browser File/Blob is the memory-mapped backing store. The
        // worker indexes every track and builds bounded state checkpoints, but
        // never materializes one CompactEvent/VisualNote per source event.
#if WASMIDI_PARSER_BPFA
        g_mappedStore.setRawSourceLimit(g_rawSourceLimit);
#endif
        if (!g_mappedStore.index(
                static_cast<uint64_t>(size),
                &browserReadAt,
                nullptr,
                g_document,
                &progressCallback,
                nullptr)) {
            g_error = g_mappedStore.error();
            g_document = wasmidi::MidiDocument{};
            return 0;
        }

        return 1;
    } catch (const std::bad_alloc&) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_mappedStore.clear();
        g_error =
            "Mapped MIDI index could not obtain more memory from the browser/OS";
        return 0;
    } catch (const std::exception& error) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_mappedStore.clear();
        g_error = std::string("Mapped MIDI exception: ") + error.what();
        return 0;
    } catch (...) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_mappedStore.clear();
        g_error = "Unknown mapped MIDI parser exception";
        return 0;
    }
}

int packDocument()
{
    try {
    #ifdef __EMSCRIPTEN__
        wmp_post_absolute_progress(
            94,
            static_cast<double>(reinterpret_cast<std::uintptr_t>(
                "Packing parsed MIDI")));
    #endif

        if (!wasmidi::serializeMidiDocument(
                g_document,
                g_result,
                g_error)) {
            return 0;
        }

        // The wire image is now authoritative in this Worker. Release all
        // parsed source vectors before JS copies/transfers the wire buffer.
        g_document = wasmidi::MidiDocument{};

    #ifdef __EMSCRIPTEN__
        wmp_post_absolute_progress(
            95,
            static_cast<double>(reinterpret_cast<std::uintptr_t>(
                "Parsed")));
    #endif
        return 1;
    } catch (const std::bad_alloc&) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_error =
            "MIDI parser could not obtain more memory from the browser/OS while packing the parsed document";
        return 0;
    } catch (const std::exception& error) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_error = std::string("MIDI pack exception: ") + error.what();
        return 0;
    } catch (...) {
        std::vector<uint8_t>().swap(g_result);
        g_document = wasmidi::MidiDocument{};
        g_error = "Unknown MIDI pack exception";
        return 0;
    }
}

} // namespace

extern "C" {

// MEMORY64 raw pointer exports use JS BigInt, but Emscripten's generated helper
// wrappers are not guaranteed to legalize every pointer/size export in the same
// way. Keep the JavaScript boundary deliberately Number-only instead. Every
// address used by Chromium's current <=16 GiB Memory64 implementation is far
// below 2^53 and is therefore exactly representable as a JavaScript Number.
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_parse_file_js(double byteCount)
{
    std::size_t size = 0;
    if (!jsSizeToNative(byteCount, size)) {
        g_error = "Invalid browser MIDI source size";
        return 0;
    }

    return parseFileDocument(size);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_pack()
{
    return packDocument();
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_result_ptr_js()
{
    return g_result.empty()
        ? 0.0
        : pointerToJsAddress(g_result.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_result_size_js()
{
    return static_cast<double>(g_result.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wmp_release_result()
{
    std::vector<uint8_t>().swap(g_result);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_build_visual_page_js(double startTick, double endTick)
{
    if (!jsExactNonNegativeInteger(startTick) ||
        !jsExactNonNegativeInteger(endTick) ||
        startTick > double(std::numeric_limits<uint32_t>::max()) ||
        endTick > double(std::numeric_limits<uint32_t>::max())) {
        g_error = "Invalid mapped visual-page tick range";
        return 0;
    }
    if (!g_mappedStore.buildVisualPage(
            static_cast<uint32_t>(startTick),
            static_cast<uint32_t>(endTick),
            g_visualPage)) {
        g_error = g_mappedStore.error();
        if (g_error.empty()) g_error = "Could not build mapped visual page";
        return 0;
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_visual_page_ptr_js()
{
    return g_visualPage.empty() ? 0.0 : pointerToJsAddress(g_visualPage.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_visual_page_count_js()
{
    return static_cast<double>(g_visualPage.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_build_key_snapshot_js(double tick)
{
    if (!jsExactNonNegativeInteger(tick) ||
        tick > double(std::numeric_limits<uint32_t>::max())) {
        g_error = "Invalid mapped keyboard tick";
        return 0;
    }
    wasmidi::MidiMappedStore::KeySnapshot snapshot;
    if (!g_mappedStore.buildKeySnapshot(static_cast<uint32_t>(tick), snapshot)) {
        g_error = g_mappedStore.error();
        if (g_error.empty()) g_error = "Could not build mapped keyboard state";
        return 0;
    }
    g_keySnapshotWords.resize(128u * 3u);
    for (std::size_t p = 0; p < 128; ++p) {
        g_keySnapshotWords[p] = snapshot.counts[p];
        g_keySnapshotWords[128u + p] = snapshot.globalColors[p];
        g_keySnapshotWords[256u + p] = snapshot.trackColors[p];
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_build_live_snapshot_js(
    double tick,
    double npsStartTick,
    double ccStartTick,
    int forceReset)
{
    wasmidi::MidiMappedStore::LiveSnapshot snapshot;
    if (!g_mappedStore.buildLiveSnapshot(
            tick, npsStartTick, ccStartTick, snapshot, forceReset != 0)) {
        g_error = g_mappedStore.error();
        if (g_error.empty()) g_error = "Could not build mapped live state";
        return 0;
    }

    constexpr std::size_t HeaderWords = 4u;
    g_keySnapshotWords.resize(HeaderWords + 128u * 3u);
    g_keySnapshotWords[0] = snapshot.activeVoices;
    g_keySnapshotWords[1] = snapshot.nps;
    g_keySnapshotWords[2] = snapshot.ccPerSecond;
    g_keySnapshotWords[3] = static_cast<uint32_t>(std::clamp<double>(
        std::floor(tick), 0.0, double(std::numeric_limits<uint32_t>::max())));
    for (std::size_t p = 0; p < 128; ++p) {
        g_keySnapshotWords[HeaderWords + p] = snapshot.keys.counts[p];
        g_keySnapshotWords[HeaderWords + 128u + p] = snapshot.keys.globalColors[p];
        g_keySnapshotWords[HeaderWords + 256u + p] = snapshot.keys.trackColors[p];
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_key_snapshot_ptr_js()
{
    return g_keySnapshotWords.empty() ? 0.0 : pointerToJsAddress(g_keySnapshotWords.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_key_snapshot_word_count_js()
{
    return static_cast<double>(g_keySnapshotWords.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wmp_reset_event_cursor_js(double startTick)
{
    const uint32_t tick =
        jsExactNonNegativeInteger(startTick)
            ? static_cast<uint32_t>(std::min<double>(
                  startTick, double(std::numeric_limits<uint32_t>::max())))
            : 0u;
    g_mappedStore.resetEventCursor(tick);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_build_event_batch_js(double endTick, double maxEvents)
{
    if (!jsExactNonNegativeInteger(endTick) ||
        !jsExactNonNegativeInteger(maxEvents) ||
        endTick > double(std::numeric_limits<uint32_t>::max())) {
        g_error = "Invalid mapped event-batch request";
        return 0;
    }
    const std::size_t limit = static_cast<std::size_t>(
        std::clamp<double>(maxEvents, 1.0, 262144.0));
    if (!g_mappedStore.buildEventBatch(
            static_cast<uint32_t>(endTick),
            limit,
            g_eventBatch,
            g_sysExBatchEvents,
            g_sysExBatchBytes,
            g_eventBatchComplete,
            g_eventBatchNextTick,
            g_eventBatchHasNextTick)) {
        g_error = g_mappedStore.error();
        if (g_error.empty()) g_error = "Could not build mapped event batch";
        return 0;
    }

    g_eventBatchTimes.resize(g_eventBatch.size());
    uint32_t lastTick = std::numeric_limits<uint32_t>::max();
    double lastSeconds = 0.0;
    for (std::size_t i = 0; i < g_eventBatch.size(); ++i) {
        const uint32_t tick = g_eventBatch[i].tick;
        if (tick != lastTick) {
            lastTick = tick;
            lastSeconds = g_mappedStore.metadata().tickToSeconds(tick);
        }
        g_eventBatchTimes[i] = lastSeconds;
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_build_historical_sysex_js(double startTick)
{
    if (!jsExactNonNegativeInteger(startTick) ||
        startTick > double(std::numeric_limits<uint32_t>::max())) {
        g_error = "Invalid historical SysEx request";
        return 0;
    }
    if (!g_mappedStore.buildHistoricalSysEx(
            static_cast<uint32_t>(startTick),
            g_sysExBatchEvents,
            g_sysExBatchBytes)) {
        g_error = g_mappedStore.error();
        if (g_error.empty()) g_error = "Could not build historical SysEx state";
        return 0;
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_build_historical_selector_state_js(double startTick)
{
    if (!jsExactNonNegativeInteger(startTick) ||
        startTick > double(std::numeric_limits<uint32_t>::max())) {
        g_error = "Invalid historical selector-state request";
        return 0;
    }
    if (!g_mappedStore.buildHistoricalSelectorState(
            static_cast<uint32_t>(startTick),
            g_historicalSelectorState)) {
        g_error = g_mappedStore.error();
        if (g_error.empty())
            g_error = "Could not build historical selector state";
        return 0;
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_historical_selector_state_ptr_js()
{
    return g_historicalSelectorState.empty()
        ? 0.0
        : pointerToJsAddress(g_historicalSelectorState.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_historical_selector_state_count_js()
{
    return static_cast<double>(g_historicalSelectorState.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_event_batch_ptr_js()
{
    return g_eventBatch.empty() ? 0.0 : pointerToJsAddress(g_eventBatch.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_event_batch_count_js()
{
    return static_cast<double>(g_eventBatch.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_event_batch_time_ptr_js()
{
    return g_eventBatchTimes.empty() ? 0.0
        : pointerToJsAddress(g_eventBatchTimes.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_event_batch_complete_js()
{
    return g_eventBatchComplete ? 1 : 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_event_batch_next_tick_js()
{
    return static_cast<double>(g_eventBatchNextTick);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_event_batch_has_next_tick_js()
{
    return g_eventBatchHasNextTick ? 1 : 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_sysex_batch_event_ptr_js()
{
    return g_sysExBatchEvents.empty() ? 0.0
        : pointerToJsAddress(g_sysExBatchEvents.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_sysex_batch_event_count_js()
{
    return static_cast<double>(g_sysExBatchEvents.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_sysex_batch_data_ptr_js()
{
    return g_sysExBatchBytes.empty() ? 0.0
        : pointerToJsAddress(g_sysExBatchBytes.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_sysex_batch_data_size_js()
{
    return static_cast<double>(g_sysExBatchBytes.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wmp_reset_render_cursor_js(double startTick, int perTrackColors)
{
    const uint32_t tick =
        jsExactNonNegativeInteger(startTick)
            ? static_cast<uint32_t>(std::min<double>(
                  startTick, double(std::numeric_limits<uint32_t>::max())))
            : 0u;
    g_mappedStore.resetRenderCursor(tick, perTrackColors != 0);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_build_render_sweep_js(double endTick, double maxSourceEvents)
{
    if (!jsExactNonNegativeInteger(endTick) ||
        !jsExactNonNegativeInteger(maxSourceEvents) ||
        endTick > double(std::numeric_limits<uint32_t>::max())) {
        g_error = "Invalid SharpMIDI render sweep request";
        return 0;
    }

    const std::size_t budget = static_cast<std::size_t>(
        std::clamp<double>(maxSourceEvents, 1.0, 1048576.0));
    if (!g_mappedStore.buildRenderSweep(
            static_cast<uint32_t>(endTick),
            budget,
            g_renderAppends,
            g_renderCloses,
            g_renderAppendBase,
            g_renderBatchComplete,
            g_renderBatchNextTick,
            g_renderBatchHasNextTick)) {
        g_error = g_mappedStore.error();
        if (g_error.empty())
            g_error = "Could not build SharpMIDI render sweep";
        return 0;
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_render_append_ptr_js()
{
    return g_renderAppends.empty()
        ? 0.0
        : pointerToJsAddress(g_renderAppends.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_render_append_count_js()
{
    return static_cast<double>(g_renderAppends.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_render_append_base_js()
{
    return static_cast<double>(g_renderAppendBase);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_render_close_ptr_js()
{
    return g_renderCloses.empty()
        ? 0.0
        : pointerToJsAddress(g_renderCloses.data());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_render_close_count_js()
{
    return static_cast<double>(g_renderCloses.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_render_batch_complete_js()
{
    return g_renderBatchComplete ? 1 : 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_render_batch_next_tick_js()
{
    return static_cast<double>(g_renderBatchNextTick);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_render_batch_has_next_tick_js()
{
    return g_renderBatchHasNextTick ? 1 : 0;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_tick_to_seconds_js(double tick)
{
    if (!g_mappedStore.valid() || !std::isfinite(tick))
        return 0.0;
    return g_mappedStore.metadata().tickToSeconds(
        static_cast<uint32_t>(std::clamp<double>(
            tick, 0.0, double(g_mappedStore.metadata().maxTick))));
}

// WASMIDI Audio (HANDOFF sec. 64) drives the synth without the Qt player, so it asks
// the parser for the timeline it would otherwise read from the packed document.
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_seconds_to_tick_js(double seconds)
{
    if (!g_mappedStore.valid() || !std::isfinite(seconds))
        return 0.0;
    const auto& md = g_mappedStore.metadata();
    return std::clamp<double>(md.secondsToTick(std::max(0.0, seconds)), 0.0, double(md.maxTick));
}

double g_documentInfo[6];
std::vector<uint8_t> g_textBlob;
std::vector<double> g_densityBlob;

// Lyrics / markers / text: u32 count, then per event f64 seconds, u8 type, u8 0,
// u16 length, bytes (HANDOFF sec. 67).
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_text_events_js()
{
#if WASMIDI_PARSER_BPFA
    try {
        g_textBlob.clear();
        if (!g_mappedStore.valid()) return 0.0;
        const auto& md = g_mappedStore.metadata();
        const auto& events = g_mappedStore.textEvents();
        auto put = [](const void* p, std::size_t n) {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            g_textBlob.insert(g_textBlob.end(), b, b + n);
        };
        const uint32_t count = uint32_t(events.size());
        put(&count, 4);
        for (const auto& e : events) {
            const double seconds = md.tickToSeconds(e.tick);
            const uint8_t type = e.type, zero = 0;
            const uint16_t length = uint16_t(e.text.size());
            put(&seconds, 8);
            put(&type, 1);
            put(&zero, 1);
            put(&length, 2);
            put(e.text.data(), e.text.size());
        }
        return pointerToJsAddress(g_textBlob.data());
    } catch (...) {
        g_textBlob.clear();
        return 0.0;
    }
#else
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_text_events_size_js() { return double(g_textBlob.size()); }

// Note-on density for the NPS graph: [n, start seconds of bucket 0..n, counts 0..n-1].
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_density_js()
{
#if WASMIDI_PARSER_BPFA
    try {
        g_densityBlob.clear();
        if (!g_mappedStore.valid()) return 0.0;
        const auto& md = g_mappedStore.metadata();
        const auto& density = g_mappedStore.noteDensity();
        const uint64_t bucketTicks = g_mappedStore.densityBucketTicks();
        const std::size_t factor = std::max<std::size_t>(1, (density.size() + 262143) / 262144);
        const std::size_t n = (density.size() + factor - 1) / factor;
        g_densityBlob.resize(1 + (n + 1) + n);
        g_densityBlob[0] = double(n);
        for (std::size_t i = 0; i <= n; ++i)
            g_densityBlob[1 + i] = md.tickToSeconds(uint32_t(std::min<uint64_t>(uint64_t(i) * factor * bucketTicks, md.maxTick)));
        for (std::size_t i = 0; i < n; ++i) {
            uint64_t c = 0;
            for (std::size_t j = i * factor; j < std::min(density.size(), (i + 1) * factor); ++j) c += density[j];
            g_densityBlob[2 + n + i] = double(c);
        }
        return pointerToJsAddress(g_densityBlob.data());
    } catch (...) {
        g_densityBlob.clear();
        return 0.0;
    }
#else
    return 0.0;
#endif
}

// [maxTick, durationSeconds, ticksPerBeat, noteCount, trackCount, format]
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_document_info_js()
{
    if (!g_mappedStore.valid())
        return 0.0;
    const auto& md = g_mappedStore.metadata();
    g_documentInfo[0] = double(md.maxTick);
    g_documentInfo[1] = double(md.durationSeconds);
    g_documentInfo[2] = double(md.ticksPerBeat);
    g_documentInfo[3] = double(md.noteCount);
    g_documentInfo[4] = double(md.trackCount);
    g_documentInfo[5] = double(md.format);
    return pointerToJsAddress(g_documentInfo);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_error_ptr_js()
{
    return pointerToJsAddress(g_error.c_str());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_error_size_js()
{
    return static_cast<double>(g_error.size());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wmp_set_raw_source_limit_js(double bytes)
{
#if WASMIDI_PARSER_BPFA
    g_rawSourceLimit = bytes > 0.0 ? static_cast<uint64_t>(bytes) : 0u;
#else
    (void)bytes;
#endif
}

// ---- Parallel load (HANDOFF sec. 60) ----------------------------------------
#if WASMIDI_PARSER_BPFA
std::vector<double> g_trackTable;
std::vector<uint8_t> g_mergeBuffer;
#endif

// [format, trackCount, ticksPerBeat, blockSize, chunk position of every track..., file size]
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_track_table_js(double byteCount)
{
#if WASMIDI_PARSER_BPFA
    try {
        g_trackTable.clear();
        std::size_t size = 0;
        if (!jsSizeToNative(byteCount, size) || size < 14) return 0;
        uint8_t h[14];
        if (!browserReadAt(nullptr, 0, h, 14) || std::memcmp(h, "MThd", 4) != 0) return 0;
        const uint32_t headerSize = (uint32_t(h[4]) << 24) | (uint32_t(h[5]) << 16) | (uint32_t(h[6]) << 8) | h[7];
        const uint16_t tracks = uint16_t((h[10] << 8) | h[11]);
        g_trackTable.push_back(double((h[8] << 8) | h[9]));
        g_trackTable.push_back(double(tracks));
        g_trackTable.push_back(double((h[12] << 8) | h[13]));
        g_trackTable.push_back(double(wasmidi::BpfaMidiStore::blockSizeForTracks(tracks)));
        uint64_t position = 8ull + headerSize;
        for (uint16_t t = 0; t < tracks; ++t) {
            uint8_t c[8];
            if (position + 8 > size || !browserReadAt(nullptr, position, c, 8) || std::memcmp(c, "MTrk", 4) != 0) {
                g_trackTable.clear();
                return 0;
            }
            g_trackTable.push_back(double(position));
            position += 8ull + ((uint32_t(c[4]) << 24) | (uint32_t(c[5]) << 16) | (uint32_t(c[6]) << 8) | c[7]);
        }
        g_trackTable.push_back(double(size));
        return int(g_trackTable.size());
    } catch (...) {
        g_trackTable.clear();
        return 0;
    }
#else
    (void)byteCount;
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_track_table_ptr_js()
{
#if WASMIDI_PARSER_BPFA
    return g_trackTable.empty() ? 0.0 : pointerToJsAddress(g_trackTable.data());
#else
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_partial_index_js(double byteCount, int format, int trackCount, int ticksPerBeat, int firstTrack,
                         int endTrack, double firstChunkPosition, double rangeBegin, double rangeEnd,
                         int blockSize, int compress, double snapshotBudget)
{
#if WASMIDI_PARSER_BPFA
    try {
        g_error.clear();
        std::size_t size = 0;
        if (!jsSizeToNative(byteCount, size)) { g_error = "Invalid browser MIDI source size"; return 0; }
        wasmidi::BpfaMidiStore::PartialPlan plan;
        plan.format = uint16_t(format);
        plan.trackCount = uint16_t(trackCount);
        plan.ticksPerBeat = uint16_t(ticksPerBeat);
        plan.firstTrack = uint16_t(firstTrack);
        plan.endTrack = uint16_t(endTrack);
        plan.firstChunkPosition = uint64_t(firstChunkPosition);
        plan.rangeBegin = uint64_t(rangeBegin);
        plan.rangeEnd = uint64_t(rangeEnd);
        plan.blockSize = uint32_t(blockSize);
        plan.compress = compress != 0;
        plan.snapshotBudget = uint64_t(snapshotBudget);
        if (!g_mappedStore.indexPartial(uint64_t(size), &browserReadAt, nullptr, plan, &progressCallback, nullptr)) {
            g_error = g_mappedStore.error();
            return 0;
        }
        return 1;
    } catch (...) {
        g_error = "Partial MIDI index could not obtain more memory from the browser/OS";
        return 0;
    }
#else
    (void)byteCount; (void)format; (void)trackCount; (void)ticksPerBeat; (void)firstTrack; (void)endTrack;
    (void)firstChunkPosition; (void)rangeBegin; (void)rangeEnd; (void)blockSize; (void)compress; (void)snapshotBudget;
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_partial_blob_ptr_js()
{
#if WASMIDI_PARSER_BPFA
    return g_mappedStore.partialBlob().empty() ? 0.0 : pointerToJsAddress(g_mappedStore.partialBlob().data());
#else
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_partial_blob_size_js()
{
#if WASMIDI_PARSER_BPFA
    return double(g_mappedStore.partialBlob().size());
#else
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wmp_partial_release_js()
{
#if WASMIDI_PARSER_BPFA
    g_mappedStore.releasePartialBlob();
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_merge_begin_js(double byteCount, int format, int trackCount, int ticksPerBeat, int blockSize,
                       int compressed)
{
#if WASMIDI_PARSER_BPFA
    std::vector<uint8_t>().swap(g_result);
    std::vector<wasmidi::VisualNote>().swap(g_visualPage);
    std::vector<uint32_t>().swap(g_keySnapshotWords);
    g_error.clear();
    g_document = wasmidi::MidiDocument{};
    std::size_t size = 0;
    if (!jsSizeToNative(byteCount, size)) { g_error = "Invalid browser MIDI source size"; return 0; }
    if (!g_mappedStore.beginMerge(uint64_t(size), uint16_t(format), uint16_t(trackCount), uint16_t(ticksPerBeat),
                                  uint32_t(blockSize), compressed != 0)) {
        g_error = g_mappedStore.error();
        return 0;
    }
    return 1;
#else
    (void)byteCount; (void)format; (void)trackCount; (void)ticksPerBeat; (void)blockSize; (void)compressed;
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_merge_read_raw_js()
{
#if WASMIDI_PARSER_BPFA
    if (!g_mappedStore.mergeReadRaw(&browserReadAt, nullptr, &progressCallback, nullptr)) {
        g_error = g_mappedStore.error();
        return 0;
    }
    return 1;
#else
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_merge_buffer_js(double byteCount)
{
#if WASMIDI_PARSER_BPFA
    try {
        std::size_t size = 0;
        if (!jsSizeToNative(byteCount, size) || size == 0) return 0.0;
        g_mergeBuffer.resize(size);
        return pointerToJsAddress(g_mergeBuffer.data());
    } catch (...) {
        g_error = "Parallel MIDI load could not obtain more memory from the browser/OS";
        return 0.0;
    }
#else
    (void)byteCount;
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_merge_part_js()
{
#if WASMIDI_PARSER_BPFA
    const bool ok = g_mappedStore.mergePartial(g_mergeBuffer.data(), g_mergeBuffer.size());
    std::vector<uint8_t>().swap(g_mergeBuffer);
    if (!ok) g_error = g_mappedStore.error();
    return ok ? 1 : 0;
#else
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_merge_finish_js()
{
#if WASMIDI_PARSER_BPFA
    if (!g_mappedStore.finishMerge(g_document)) {
        g_error = g_mappedStore.error();
        g_document = wasmidi::MidiDocument{};
        return 0;
    }
    std::vector<double>().swap(g_trackTable);
    return 1;
#else
    return 0;
#endif
}

// CI probe for Memory64 heap growth, now that the BPFA store keeps the heap small
// (HANDOFF sec. 49).
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_memory_growth_probe_js(double megabytes)
{
    const std::size_t bytes = static_cast<std::size_t>(std::max(1.0, megabytes)) * 1024u * 1024u;
    uint8_t* block = new (std::nothrow) uint8_t[bytes];
    if (!block) return 0;
    std::memset(block, 1, bytes);
    const int ok = block[bytes - 1] == 1;
    delete[] block;
    return ok;
}


// ---- Synth feed (HANDOFF sec. 70) ----
// Coordinator side (the parser Worker): config, reset, sequential build, helper requests
// and in-order stitch. The output reuses the event-batch buffers (g_eventBatch, times,
// SysEx). Helper side: a second instance of this module imports the feed image and
// decodes requests, reading compressed source slabs that the page shares with it.

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wmp_feed_config_js(double maxVoices, double admissionPercent, double sampleRate, double blockFrames,
                        double velocityFloor, double windowBlocks)
{
#if WASMIDI_PARSER_BPFA
    auto clampU32 = [](double v, double lo, double hi) {
        return static_cast<uint32_t>(std::clamp(std::isfinite(v) ? v : lo, lo, hi));
    };
    wasmidi::BpfaMidiStore::SynthFeedConfig cfg;
    cfg.maxVoices = clampU32(maxVoices, 1.0, 1048576.0);
    cfg.admissionPercent = clampU32(admissionPercent, 0.0, 100.0);
    cfg.sampleRate = clampU32(sampleRate, 8000.0, 384000.0);
    cfg.blockFrames = clampU32(blockFrames, 16.0, 16384.0);
    cfg.velocityFloor = clampU32(velocityFloor, 0.0, 127.0);
    cfg.windowBlocks = clampU32(windowBlocks, 1.0, 64.0);
    g_mappedStore.setSynthFeedConfig(cfg);
#else
    (void)maxVoices; (void)admissionPercent; (void)sampleRate; (void)blockFrames; (void)velocityFloor; (void)windowBlocks;
#endif
}

#if WASMIDI_PARSER_BPFA
double g_feedSafeSeconds = 0.0;
bool g_feedComplete = false;
std::vector<uint8_t> g_feedBuffer;
#endif

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wmp_feed_reset_js(double startTick)
{
#if WASMIDI_PARSER_BPFA
    const uint32_t tick = jsExactNonNegativeInteger(startTick)
        ? static_cast<uint32_t>(std::min<double>(startTick, double(std::numeric_limits<uint32_t>::max()))) : 0u;
    g_mappedStore.resetSynthFeed(tick);
    g_feedSafeSeconds = 0.0;
    g_feedComplete = false;
#else
    (void)startTick;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_feed_build_js(double endSeconds, double maxEvents)
{
#if WASMIDI_PARSER_BPFA
    try {
        const std::size_t limit = static_cast<std::size_t>(std::clamp(std::isfinite(maxEvents) ? maxEvents : 1.0, 1.0, 1048576.0));
        if (!g_mappedStore.buildSynthFeed(std::isfinite(endSeconds) ? endSeconds : 0.0, limit, g_eventBatch,
                                          g_eventBatchTimes, g_sysExBatchEvents, g_sysExBatchBytes,
                                          g_feedSafeSeconds, g_feedComplete)) {
            g_error = g_mappedStore.error();
            if (g_error.empty()) g_error = "Could not build the synth feed";
            return 0;
        }
        return 1;
    } catch (...) {
        g_error = "Synth feed could not obtain more memory from the browser/OS";
        return 0;
    }
#else
    (void)endSeconds; (void)maxEvents;
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_feed_safe_seconds_js()
{
#if WASMIDI_PARSER_BPFA
    return g_feedSafeSeconds;
#else
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_feed_complete_js()
{
#if WASMIDI_PARSER_BPFA
    return g_feedComplete ? 1 : 0;
#else
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_feed_block_js()
{
#if WASMIDI_PARSER_BPFA
    return static_cast<double>(g_mappedStore.synthFeedBlock());
#else
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_feed_block_seconds_js()
{
#if WASMIDI_PARSER_BPFA
    return g_mappedStore.synthFeedBlockSeconds();
#else
    return 0.0;
#endif
}

// Shared scratch for images, requests and results crossing to/from JavaScript.
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_feed_buffer_js(double byteCount)
{
#if WASMIDI_PARSER_BPFA
    try {
        std::size_t size = 0;
        if (!jsSizeToNative(byteCount, size)) return 0.0;
        g_feedBuffer.resize(std::max<std::size_t>(size, 1u));
        return pointerToJsAddress(g_feedBuffer.data());
    } catch (...) {
        g_error = "Synth feed could not obtain more memory from the browser/OS";
        return 0.0;
    }
#else
    (void)byteCount;
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_feed_buffer_size_js()
{
#if WASMIDI_PARSER_BPFA
    return static_cast<double>(g_feedBuffer.size());
#else
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_feed_buffer_ptr_js()
{
#if WASMIDI_PARSER_BPFA
    return g_feedBuffer.empty() ? 0.0 : pointerToJsAddress(g_feedBuffer.data());
#else
    return 0.0;
#endif
}

// Coordinator: the helper image into the feed buffer (0 when the source is not the
// compressed slabs, e.g. a raw in-memory source).
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_feed_image_js()
{
#if WASMIDI_PARSER_BPFA
    try { return g_mappedStore.exportFeedImage(g_feedBuffer) ? 1 : 0; }
    catch (...) { g_error = "Synth feed image ran out of memory"; return 0; }
#else
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_source_slab_count_js()
{
#if WASMIDI_PARSER_BPFA
    return static_cast<double>(g_mappedStore.sourceSlabCount());
#else
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_source_slab_ptr_js(double index)
{
#if WASMIDI_PARSER_BPFA
    if (!jsExactNonNegativeInteger(index) || index >= double(g_mappedStore.sourceSlabCount())) return 0.0;
    return pointerToJsAddress(g_mappedStore.sourceSlab(static_cast<std::size_t>(index)));
#else
    (void)index;
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_source_slab_bytes_js()
{
#if WASMIDI_PARSER_BPFA
    return static_cast<double>(wasmidi::BpfaMidiStore::sourceSlabBytes());
#else
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_feed_request_js(double firstBlock, double blocks, double chunks)
{
#if WASMIDI_PARSER_BPFA
    if (!jsExactNonNegativeInteger(firstBlock) || !jsExactNonNegativeInteger(blocks) || !jsExactNonNegativeInteger(chunks)) {
        g_error = "Invalid synth feed request";
        return 0;
    }
    try {
        if (!g_mappedStore.buildFeedRequest(static_cast<uint64_t>(firstBlock), static_cast<uint32_t>(std::min(blocks, 64.0)),
                                            static_cast<uint32_t>(std::min(chunks, 4096.0)), g_feedBuffer)) {
            g_error = "Could not plan the synth feed request";
            return 0;
        }
        return 1;
    } catch (...) {
        g_error = "Synth feed request ran out of memory";
        return 0;
    }
#else
    (void)firstBlock; (void)blocks; (void)chunks;
    return 0;
#endif
}

// Coordinator: stitches the result the page copied into the feed buffer.
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_feed_stitch_js(double byteCount)
{
#if WASMIDI_PARSER_BPFA
    std::size_t size = 0;
    if (!jsSizeToNative(byteCount, size) || size > g_feedBuffer.size()) { g_error = "Invalid synth feed result"; return 0; }
    try {
        if (!g_mappedStore.stitchFeedResult(g_feedBuffer.data(), size, g_eventBatch, g_eventBatchTimes,
                                            g_sysExBatchEvents, g_sysExBatchBytes, g_feedSafeSeconds, g_feedComplete)) {
            g_error = g_mappedStore.error();
            return 0;
        }
        return 1;
    } catch (...) {
        g_error = "Synth feed stitch ran out of memory";
        return 0;
    }
#else
    (void)byteCount;
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_feed_helper_init_js(double byteCount)
{
#if WASMIDI_PARSER_BPFA
    std::size_t size = 0;
    if (!jsSizeToNative(byteCount, size) || size > g_feedBuffer.size()) { g_error = "Invalid synth feed image"; return 0; }
    try {
        if (!g_mappedStore.importFeedImage(g_feedBuffer.data(), size, feedSlabFetch, nullptr)) {
            g_error = g_mappedStore.error();
            return 0;
        }
        return 1;
    } catch (...) {
        g_error = "Synth feed helper ran out of memory";
        return 0;
    }
#else
    (void)byteCount;
    return 0;
#endif
}

// Helper: decodes the request in the feed buffer; the result replaces the buffer.
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_feed_decode_js(double byteCount)
{
#if WASMIDI_PARSER_BPFA
    std::size_t size = 0;
    if (!jsSizeToNative(byteCount, size) || size > g_feedBuffer.size()) { g_error = "Invalid synth feed request"; return 0; }
    try {
        std::vector<uint8_t> request(g_feedBuffer.begin(), g_feedBuffer.begin() + std::ptrdiff_t(size));
        if (!g_mappedStore.decodeFeedRequest(request.data(), request.size(), g_feedBuffer)) {
            g_error = g_mappedStore.error();
            return 0;
        }
        return 1;
    } catch (...) {
        g_error = "Synth feed helper ran out of memory";
        return 0;
    }
#else
    (void)byteCount;
    return 0;
#endif
}

// ---- Visual stream (HANDOFF sec. 71): the roll sweep and the live state read helper spans ----

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void wmp_visual_config_js(int enabled, int autoDecode)
{
#if WASMIDI_PARSER_BPFA
    g_mappedStore.setVisualStreamEnabled(enabled != 0);
    g_mappedStore.setVisualAutoDecode(autoDecode != 0);
#else
    (void)enabled; (void)autoDecode;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_visual_head_js()
{
#if WASMIDI_PARSER_BPFA
    return static_cast<double>(g_mappedStore.visualStreamHead());
#else
    return 0.0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_visual_done_js()
{
#if WASMIDI_PARSER_BPFA
    return g_mappedStore.visualStreamDone() ? 1 : 0;
#else
    return 1;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double wmp_visual_span_end_js(double startTick, double sourceEvents)
{
#if WASMIDI_PARSER_BPFA
    if (!jsExactNonNegativeInteger(startTick) || startTick > 4294967295.0 || !(sourceEvents >= 1.0)) return 0.0;
    return static_cast<double>(g_mappedStore.visualSpanEnd(static_cast<uint32_t>(startTick),
                                                           static_cast<uint64_t>(std::min(sourceEvents, 1.0e12))));
#else
    (void)startTick; (void)sourceEvents;
    return 0.0;
#endif
}

// Coordinator: the request for [startTick, endTick) into the feed buffer.
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_visual_request_js(double startTick, double endTick)
{
#if WASMIDI_PARSER_BPFA
    if (!jsExactNonNegativeInteger(startTick) || !jsExactNonNegativeInteger(endTick) || endTick > 4294967296.0) {
        g_error = "Invalid visual span";
        return 0;
    }
    try {
        if (!g_mappedStore.buildVisualRequest(static_cast<uint32_t>(startTick),
                                              static_cast<uint32_t>(std::min(endTick, 4294967295.0)), g_feedBuffer)) {
            g_error = "Could not plan the visual span";
            return 0;
        }
        return 1;
    } catch (...) {
        g_error = "Visual span request ran out of memory";
        return 0;
    }
#else
    (void)startTick; (void)endTick;
    return 0;
#endif
}

// Helper: decodes the visual request in the feed buffer; the result replaces the buffer.
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_visual_decode_js(double byteCount)
{
#if WASMIDI_PARSER_BPFA
    std::size_t size = 0;
    if (!jsSizeToNative(byteCount, size) || size > g_feedBuffer.size()) { g_error = "Invalid visual request"; return 0; }
    try {
        std::vector<uint8_t> request(g_feedBuffer.begin(), g_feedBuffer.begin() + std::ptrdiff_t(size));
        if (!g_mappedStore.decodeVisualRequest(request.data(), request.size(), g_feedBuffer)) {
            g_error = g_mappedStore.error();
            return 0;
        }
        return 1;
    } catch (...) {
        g_error = "Visual helper ran out of memory";
        return 0;
    }
#else
    (void)byteCount;
    return 0;
#endif
}

// Coordinator: appends the helper result the page copied into the feed buffer.
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_visual_append_js(double byteCount)
{
#if WASMIDI_PARSER_BPFA
    std::size_t size = 0;
    if (!jsSizeToNative(byteCount, size) || size > g_feedBuffer.size()) { g_error = "Invalid visual result"; return 0; }
    try {
        if (!g_mappedStore.appendVisualResult(g_feedBuffer.data(), size)) {
            g_error = g_mappedStore.error();
            return 0;
        }
        return 1;
    } catch (...) {
        g_error = "Visual stream ran out of memory";
        return 0;
    }
#else
    (void)byteCount;
    return 0;
#endif
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int wmp_pointer_bits()
{
    return static_cast<int>(sizeof(void*) * 8u);
}

} // extern "C"

// Keep a real inert entry point even for Emscripten. The Worker asks the
// modularized runtime not to execute it, and NO_EXIT_RUNTIME=1 makes the module
// remain callable even if a generated-glue/runtime variant does invoke main().
int main()
{
    return 0;
}
