#include "gl_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __EMSCRIPTEN__
// ?tiles=0 disables the culled tile cache for A/B tests (HANDOFF sec. 42).
EM_JS(int, wasmidi_tiles_disabled, (), {
    try { return new URL(globalThis.location.href).searchParams.get("tiles") === "0" ? 1 : 0; }
    catch (_) { return 0; }
});
// ?tileslice=0 builds each cull tile in one frame, as before rev. 88.
EM_JS(int, wasmidi_tile_slice_disabled, (), {
    try { return new URL(globalThis.location.href).searchParams.get("tileslice") === "0" ? 1 : 0; }
    catch (_) { return 0; }
});
// ?rolldraw=0 skips the note draw calls (all CPU work stays): measures the roll's CPU
// side where the GPU is software, as in headless Chromium (HANDOFF sec. 81).
EM_JS(int, wasmidi_roll_draw_disabled, (), {
    try { return new URL(globalThis.location.href).searchParams.get("rolldraw") === "0" ? 1 : 0; }
    catch (_) { return 0; }
});
// Held roll frames (0) and roll catch-ups (1), read by ?visualdebug=1.
EM_JS(void, wasmidi_roll_debug_count, (int catchUp), {
    if (catchUp)
        globalThis.__wasmidiRollCatchUps = (globalThis.__wasmidiRollCatchUps | 0) + 1;
    else
        globalThis.__wasmidiRollHeld = (globalThis.__wasmidiRollHeld | 0) + 1;
});
EM_JS(void, wasmidi_roll_debug_drawn, (double instances, double raw, double resident), {
    const root = globalThis;
    root.__wasmidiRollDrawnMax = Math.max(root.__wasmidiRollDrawnMax || 0, instances);
    root.__wasmidiRollRawMax = Math.max(root.__wasmidiRollRawMax || 0, raw);
    root.__wasmidiRollResident = resident;
    root.__wasmidiAppMemory = HEAP8.length;
});
// Cull tile work on the page thread, read by ?visualdebug=1: kind 0 is a slice of a
// tile built over several frames, 1 finishes one, 2 is a visible tile built at once.
EM_JS(void, wasmidi_roll_debug_tile, (double ms, int kind), {
    const root = globalThis;
    if (kind) root.__wasmidiRollTiles = (root.__wasmidiRollTiles | 0) + 1;
    if (kind == 2) root.__wasmidiRollTilesVisible = (root.__wasmidiRollTilesVisible | 0) + 1;
    root.__wasmidiRollTileMs = (root.__wasmidiRollTileMs || 0) + ms;
    root.__wasmidiRollTileMax = Math.max(root.__wasmidiRollTileMax || 0, ms);
});
#else
static void wasmidi_roll_debug_tile(double, int) {}
static int wasmidi_tile_slice_disabled() { return 0; }
static int wasmidi_tiles_disabled() { return 0; }
static int wasmidi_roll_draw_disabled() { return 0; }
static void wasmidi_roll_debug_count(int) {}
static void wasmidi_roll_debug_drawn(double, double, double) {}
#endif

#ifdef __EMSCRIPTEN__
namespace {
wasmidi::GLRenderer* g_visualCacheRenderer = nullptr;
}

EM_JS(void, wasmidi_visual_cache_install,
      (const uint32_t* words, uint32_t noteCount,
       const uint32_t* keyStarts, uint32_t keyStartCount,
       const uint32_t* keyEnds, uint32_t keyEndCount,
       const uint32_t* keyOwners, uint32_t keyOwnerCount,
       uint32_t generation, uint32_t maxTick), {
    const root = globalThis;
    const previous = root.__wasmidiVisualCache;
    if (previous && previous.worker) {
        try { previous.worker.terminate(); } catch (_) {}
    }

    const state = {
        worker: null,
        generation: generation >>> 0
    };
    root.__wasmidiVisualCache = state;

    if (typeof Worker !== 'function' || !noteCount) return;

    try {
        const copyBytes = (ptr, byteLength) => {
            const copy = new Uint8Array(byteLength >>> 0);
            if (byteLength)
                copy.set(HEAPU8.subarray(ptr, ptr + byteLength));
            return copy;
        };

        const noteBytes = (noteCount >>> 0) * 12;
        const startBytes = (keyStartCount >>> 0) * 12;
        const endBytes = (keyEndCount >>> 0) * 12;
        const ownerBytes = (keyOwnerCount >>> 0) * 8;

        const copy = copyBytes(words, noteBytes);
        const startsCopy = copyBytes(keyStarts, startBytes);
        const endsCopy = copyBytes(keyEnds, endBytes);
        const ownersCopy = copyBytes(keyOwners, ownerBytes);

        const worker = new Worker('./visual-cache-worker.js');
        state.worker = worker;

        worker.onmessage = event => {
            const message = event.data || {};
            if ((message.generation >>> 0) !== state.generation)
                return;

            if (message.type === 'keyPage') {
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
                    _wasmidi_visual_key_page_ready(
                        message.generation >>> 0,
                        message.spanTicks >>> 0,
                        message.pageIndex >>> 0,
                        ptr,
                        wordCount >>> 0);
                } finally {
                    if (ptr) _free(ptr);
                }
                return;
            }

            if (message.type !== 'page')
                return;

            const payload = new Uint8Array(message.data || new ArrayBuffer(0));
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
                    message.sourceCount >>> 0,
                    Number(message.difficulty) || 0.0);
            } finally {
                if (ptr) _free(ptr);
            }
        };

        worker.onerror = event => {
            console.error('[WASMIDI visual cache worker]',
                          event && event.message ? event.message : event);
        };

        worker.postMessage({
            type: 'install',
            generation: state.generation,
            maxTick: maxTick >>> 0,
            notes: copy.buffer,
            keyStarts: startsCopy.buffer,
            keyEnds: endsCopy.buffer,
            keyOwners: ownersCopy.buffer
        }, [copy.buffer, startsCopy.buffer, endsCopy.buffer, ownersCopy.buffer]);
    } catch (error) {
        console.error('[WASMIDI visual cache]', error);
    }
});

EM_JS(void, wasmidi_visual_cache_shutdown, (), {
    const state = globalThis.__wasmidiVisualCache;
    if (state && state.worker) {
        try { state.worker.terminate(); } catch (_) {}
    }
    globalThis.__wasmidiVisualCache = null;
});

EM_JS(void, wasmidi_visual_cache_prime,
      (uint32_t generation, uint32_t spanTicks, uint32_t firstPage,
       uint32_t pageCount, uint32_t currentPage,
       uint32_t missingLo, uint32_t missingHi, uint32_t missingTop), {
    const mapped = globalThis.__wasmidiMappedMidi;
    if (mapped && mapped.worker && mapped.mappedStore) {
        // Remote-indexed playback uses the persistent Sharp ring exclusively.
        // Never run the legacy visual-page builder in parallel on the same
        // Memory64 Worker: it duplicates note traversal and competes directly
        // with synth admission and live keyboard/stat requests.
        return;
    }

    const state = globalThis.__wasmidiVisualCache;
    if (!state || !state.worker ||
        state.generation !== (generation >>> 0))
        return;

    state.worker.postMessage({
        type: 'prime',
        generation: generation >>> 0,
        spanTicks: spanTicks >>> 0,
        firstPage: firstPage >>> 0,
        count: pageCount >>> 0,
        currentPage: currentPage >>> 0,
        missingLo: missingLo >>> 0,
        missingHi: missingHi >>> 0,
        missingTop: missingTop >>> 0
    });
});

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_visual_page_ready(
    uint32_t generation,
    uint32_t spanTicks,
    uint32_t pageIndex,
    const uint32_t* words,
    uint32_t noteCount,
    uint32_t sourceCount,
    double difficulty)
{
    if (g_visualCacheRenderer) {
        g_visualCacheRenderer->receiveVisualPage(
            generation, spanTicks, pageIndex, words,
            noteCount, sourceCount, difficulty);
    }
}

EM_JS(void, wasmidi_sharp_render_request,
      (uint32_t generation, uint32_t startTick, uint32_t endTick,
       uint32_t urgentThrough, int perTrack, int reset), {
    const mapped = globalThis.__wasmidiMappedMidi;
    if (!mapped || !mapped.worker || !mapped.mappedStore)
        return;
    mapped.worker.postMessage({
        type: 'sharp-render',
        generation: generation >>> 0,
        startTick: startTick >>> 0,
        endTick: endTick >>> 0,
        urgentThrough: urgentThrough >>> 0,
        perTrack: !!perTrack,
        reset: !!reset
    });
});

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_sharp_render_reset_ready(uint32_t generation)
{
    if (g_visualCacheRenderer)
        g_visualCacheRenderer->receiveSharpRenderReset(generation);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void wasmidi_sharp_render_delta_ready(
    uint32_t generation,
    uint32_t appendBase,
    const wasmidi::VisualNote* appends,
    uint32_t appendCount,
    const uint32_t* closeWords,
    uint32_t closeCount,
    uint32_t safeThrough,
    int complete)
{
    if (g_visualCacheRenderer) {
        g_visualCacheRenderer->receiveSharpRenderDelta(
            generation, appendBase, appends, appendCount,
            closeWords, closeCount, safeThrough, complete != 0);
    }
}
#endif

namespace wasmidi {
namespace {

// Horizontal mapped-cache contract:
//   * one page ~= one complete visible screen at the span captured when the
//     cache is created;
//   * 64 complete future screens must already be resident before transport is
//     allowed to advance;
//   * one additional future screen is prefetched as a rollover spare, so moving
//     into the next page does not instantly drop below the 64-screen guarantee;
//   * two history pages remain resident so every page intersecting the current
//     viewport stays in memory until it has actually scrolled off-screen.
constexpr uint32_t VisualHistoryPages = 2;
constexpr uint32_t VisualRequiredAheadPages = 64;
constexpr uint32_t VisualPrefetchAheadPages = VisualRequiredAheadPages + 1;
constexpr std::size_t VisualPageCacheCapacity =
    std::size_t(VisualHistoryPages) + 1u +
    std::size_t(VisualPrefetchAheadPages);

GLuint compileShader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

    if (ok != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

GLuint linkProgram(const char* vertexSource,
                   const char* fragmentSource)
{
    const GLuint vertex =
        compileShader(GL_VERTEX_SHADER, vertexSource);

    const GLuint fragment =
        compileShader(GL_FRAGMENT_SHADER, fragmentSource);

    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);

    if (ok != GL_TRUE) {
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

constexpr std::size_t InitialRingCapacity =
    std::size_t(1) << 18;


float wrapHue(float value)
{
    while (value < 0.0f)
        value += 360.0f;

    while (value >= 360.0f)
        value -= 360.0f;

    return value;
}

} // namespace

GLRenderer::GLRenderer()
{
#ifdef __EMSCRIPTEN__
    g_visualCacheRenderer = this;
#endif

    static const uint8_t defaults[16][4] = {
        {129,140,248,255},{167,139,250,255},{196,181,253,255},{251,146, 60,255},
        { 74,222,128,255},{ 56,189,248,255},{244,114,182,255},{250,204, 21,255},
        {248,113,113,255},{ 52,211,153,255},{ 96,165,250,255},{232,121,249,255},
        {251,113,133,255},{163,230, 53,255},{ 34,211,238,255},{251,191, 36,255}
    };

    for (int i = 0; i < 16; ++i) {
        channelColors_[i] = {
            defaults[i][0],
            defaults[i][1],
            defaults[i][2],
            defaults[i][3]
        };
    }
}

GLRenderer::~GLRenderer()
{
    destroy();
}

bool GLRenderer::createPrograms()
{
    static const char* noteVertex = R"GLSL(#version 300 es
precision highp float;
precision highp int;

layout(location=0) in uint aStartTick;
layout(location=1) in uint aEndTick;
layout(location=2) in uint aPackedData;

uniform vec3 uMetrics;
uniform int uViewStart;
uniform int uViewEnd;
uniform int uCurrentTick;
uniform int uPerTrack;
uniform sampler2D uPalette;

flat out vec4 vColor;
flat out int vIsActive;
flat out float opacity;

void main() {
    int endTick = aEndTick > 0u ? int(aEndTick) : uViewEnd;
    uint isEnd = uint(gl_VertexID) & 1u;
    uint isTop = uint(gl_VertexID >> 1) & 1u;

    float startX = float(int(aStartTick) - uViewStart) * uMetrics.x - 1.0;
    float endX = float(endTick - uViewStart) * uMetrics.x - 1.0;
    float x = bool(isEnd) ? endX : startX;

    uint noteIndex = (aPackedData >> 16u) & 0xffu;
    uint colorIndex = noteIndex & 0x0fu;

    float y = uMetrics.y +
        float(((aPackedData >> 8u) & 0xffu) + isTop) * uMetrics.z;
    float z = float(endTick - int(aStartTick)) / 16777216.0;

    vColor = texelFetch(uPalette, ivec2(int(colorIndex), 0), 0);
    vIsActive =
        (uCurrentTick >= int(aStartTick) && uCurrentTick <= endTick) ? 1 : 0;
    opacity = float((aPackedData & 0xffu) + 1u) / 128.0;
    gl_Position = vec4(x, y, z, 1.0);
}
)GLSL";

    static const char* noteFragment = R"GLSL(#version 300 es
precision mediump float;

flat in vec4 vColor;
flat in int vIsActive;
flat in float opacity;
uniform int uGlowEnabled;
uniform int uTransparencyEnabled;
out vec4 fragColor;

void main() {
    float noteOpacity = uTransparencyEnabled == 1 ? opacity : 1.0;
    vec3 color = vColor.rgb;
    color = (uGlowEnabled == 1 && vIsActive == 1)
        ? min(color * 2.0 + vec3(0.1), vec3(1.0))
        : color;
    fragColor = vec4(color, noteOpacity);
}
)GLSL";

    static const char* backgroundVertex = R"GLSL(#version 300 es
precision highp float;

out vec2 vUv;

void main()
{
    vec2 pos =
        vec2(
            (gl_VertexID & 1) == 0
                ? -1.0
                : 1.0,
            (gl_VertexID & 2) == 0
                ? -1.0
                : 1.0);

    vUv = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.9999, 1.0);
}
)GLSL";

    static const char* backgroundFragment = R"GLSL(#version 300 es
precision mediump float;

in vec2 vUv;

uniform float uHue;
uniform float uActivity;
uniform float uAspect;
uniform float uAlpha;

out vec4 fragColor;

vec3 hsl2rgb(float h, float s, float l)
{
    h = fract(h);
    s = clamp(s, 0.0, 1.0);
    l = clamp(l, 0.0, 1.0);

    float c =
        (1.0 -
         abs(2.0 * l - 1.0)) *
        s;

    float hp = h * 6.0;
    float x =
        c *
        (1.0 -
         abs(
             mod(hp, 2.0) -
             1.0));

    vec3 rgb;

    if (hp < 1.0)
        rgb = vec3(c,x,0);
    else if (hp < 2.0)
        rgb = vec3(x,c,0);
    else if (hp < 3.0)
        rgb = vec3(0,c,x);
    else if (hp < 4.0)
        rgb = vec3(0,x,c);
    else if (hp < 5.0)
        rgb = vec3(x,0,c);
    else
        rgb = vec3(c,0,x);

    float m = l - c * 0.5;
    return rgb + vec3(m);
}

float blob(vec2 uv, vec2 center, float radius)
{
    vec2 d = uv - center;
    d.x *= uAspect;

    float dist =
        length(d);

    return
        max(
            0.0,
            1.0 -
            dist /
            radius);
}

void main()
{
    float hue =
        fract(uHue / 360.0);

    float activity =
        clamp(
            uActivity,
            0.0,
            1.0);

    vec3 c1 =
        hsl2rgb(
            hue,
            0.52,
            0.03 +
            activity * 0.04);

    vec3 c2 =
        hsl2rgb(
            fract(
                hue +
                55.0 / 360.0),
            0.48,
            0.05 +
            activity * 0.05);

    float mixValue =
        clamp(
            (vUv.x +
             (1.0 - vUv.y)) *
            0.5,
            0.0,
            1.0);

    vec3 color =
        mix(
            c1,
            c2,
            mixValue);

    float b1 =
        blob(
            vUv,
            vec2(.15,.80),
            .30);

    float b2 =
        blob(
            vUv,
            vec2(.82,.25),
            .28);

    vec3 blob1 =
        hsl2rgb(
            hue,
            .65,
            .28 +
            activity * .14);

    vec3 blob2 =
        hsl2rgb(
            fract(
                hue +
                50.0 / 360.0),
            .65,
            .28 +
            activity * .14);

    color +=
        blob1 *
        b1 * b1 *
        (.09 +
         activity * .07);

    color +=
        blob2 *
        b2 * b2 *
        (.09 +
         activity * .07);

    // Premultiplied: the page's neural background shows through (HANDOFF sec. 92).
    fragColor =
        vec4(
            clamp(
                color,
                0.0,
                1.0) *
            uAlpha,
            uAlpha);
}
)GLSL";


    noteProgram_ =
        linkProgram(
            noteVertex,
            noteFragment);

    backgroundProgram_ =
        linkProgram(
            backgroundVertex,
            backgroundFragment);

    // The horizontal note program is the only mandatory pipeline.
    // Background effects are optional accelerations and must never make
    // the piano roll disappear if a browser/driver rejects one of their shaders.
    if (!noteProgram_)
        return false;

    viewStartUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uViewStart");

    viewEndUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uViewEnd");

    currentTickUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uCurrentTick");

    metricsUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uMetrics");

    viewportWidthUniform_ = -1;

    perTrackUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uPerTrack");

    paletteUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uPalette");

    glowUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uGlowEnabled");

    transparencyUniform_ =
        glGetUniformLocation(
            noteProgram_,
            "uTransparencyEnabled");

    if (backgroundProgram_) {
        backgroundHueUniform_ =
            glGetUniformLocation(
                backgroundProgram_,
                "uHue");

        backgroundActivityUniform_ =
            glGetUniformLocation(
                backgroundProgram_,
                "uActivity");

        backgroundAspectUniform_ =
            glGetUniformLocation(
                backgroundProgram_,
                "uAspect");

        backgroundAlphaUniform_ =
            glGetUniformLocation(
                backgroundProgram_,
                "uAlpha");
    }

    return true;
}

bool GLRenderer::initialize()
{
    if (initialized_)
        return true;

    if (!createPrograms())
        return false;

    glGenVertexArrays(
        1,
        &noteVao_);

    glGenBuffers(
        1,
        &noteVbo_);

    glBindVertexArray(
        noteVao_);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        noteVbo_);

    glEnableVertexAttribArray(0);

    glVertexAttribIPointer(
        0, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            offsetof(
                VisualNote,
                startTick)));

    glVertexAttribDivisor(0, 1);

    glEnableVertexAttribArray(1);

    glVertexAttribIPointer(
        1, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            offsetof(
                VisualNote,
                endTick)));

    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);

    glVertexAttribIPointer(
        2, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            offsetof(
                VisualNote,
                packedData)));

    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenVertexArrays(1, &carryVao_);
    glGenBuffers(1, &carryVbo_);
    glBindVertexArray(carryVao_);
    glBindBuffer(GL_ARRAY_BUFFER, carryVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(VisualNote), nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, startTick)));
    glVertexAttribDivisor(0, 1);

    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, endTick)));
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, packedData)));
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenVertexArrays(1, &cullVao_);
    glBindVertexArray(cullVao_);
    for (GLuint attribute = 0; attribute < 3; ++attribute) {
        glEnableVertexAttribArray(attribute);
        glVertexAttribDivisor(attribute, 1);
    }
    glBindVertexArray(0);

    glGenVertexArrays(1, &denseVao_);
    glGenBuffers(1, &denseVbo_);
    glBindVertexArray(denseVao_);
    glBindBuffer(GL_ARRAY_BUFFER, denseVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(VisualNote), nullptr, GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, startTick)));
    glVertexAttribDivisor(0, 1);

    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, endTick)));
    glVertexAttribDivisor(1, 1);

    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
        reinterpret_cast<void*>(offsetof(VisualNote, packedData)));
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenVertexArrays(
        1,
        &backgroundVao_);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenTextures(1, &sharpPaletteTexture_);
    regenerateSharpPalette();

    // SharpMIDI-raylib starts with a 2^23-note persistent ring. Reserving the
    // same capacity avoids CPU-vector + VBO realloc/copy stalls exactly when a
    // Black MIDI enters its densest sections.
    allocateRing(
        std::size_t(1) << 23);

    initialized_ = true;
    return true;
}

void GLRenderer::destroy()
{
    const GLuint buffers[] = {
        noteVbo_,
        carryVbo_,
        denseVbo_
    };

    for (GLuint buffer : buffers) {
        if (buffer)
            glDeleteBuffers(1, &buffer);
    }

    const GLuint vaos[] = {
        noteVao_,
        carryVao_,
        denseVao_,
        backgroundVao_
    };

    for (GLuint vao : vaos) {
        if (vao)
            glDeleteVertexArrays(1, &vao);
    }

    const GLuint programs[] = {
        noteProgram_,
        backgroundProgram_
    };

    for (GLuint program : programs) {
        if (program)
            glDeleteProgram(program);
    }

    if (sharpPaletteTexture_)
        glDeleteTextures(1, &sharpPaletteTexture_);
    sharpPaletteTexture_ = 0;

    noteVbo_ = 0;
    carryVbo_ = 0;
    denseVbo_ = 0;

    noteVao_ = 0;
    carryVao_ = 0;
    denseVao_ = 0;
    releaseCullTiles();
    if (cullVao_) {
        glDeleteVertexArrays(1, &cullVao_);
        cullVao_ = 0;
    }
    backgroundVao_ = 0;

    noteProgram_ = 0;
    backgroundProgram_ = 0;

    ring_.clear();
    carryNotes_.clear();
    denseNotes_.clear();
    denseSourceScratch_.clear();
    denseCoverage_.clear();
    visualPages_.clear();
    ringCapacity_ = 0;
    ringMask_ = 0;
    sourceBegin_ = 0;
    sourceEnd_ = 0;

#ifdef __EMSCRIPTEN__
    if (g_visualCacheRenderer == this) {
        wasmidi_visual_cache_shutdown();
        g_visualCacheRenderer = nullptr;
    }
#endif

    initialized_ = false;
}

void GLRenderer::resize(int width, int height)
{
    width_ = std::max(1, width);
    height_ = std::max(1, height);
}

void GLRenderer::setDocument(const MidiDocument* document)
{
    document_ = document;
    sourceBegin_ = 0;
    sourceEnd_ = 0;
    carryNotes_.clear();
    forceCacheReset_ = true;
    sharpForceReset_ = true;
    rollFrameValid_ = false;
    ++sharpRemoteGeneration_;
    if (sharpRemoteGeneration_ == 0)
        ++sharpRemoteGeneration_;
    resetSharpRenderer(false);
    regenerateSharpPalette();

#ifdef __EMSCRIPTEN__
    // The faithful renderer does not install or build visual pages. Tear down
    // any legacy cache worker left from an earlier document/version.
    wasmidi_visual_cache_shutdown();
#endif
}

void GLRenderer::setTransportRevision(uint64_t revision)
{
    if (!transportRevisionValid_) {
        transportRevision_ = revision;
        transportRevisionValid_ = true;
        return;
    }
    if (transportRevision_ == revision)
        return;

    transportRevision_ = revision;
    forceCacheReset_ = true;
    sharpForceReset_ = true;
}

void GLRenderer::setCurrentTime(float seconds)
{
    const float value = std::max(0.0f, seconds);
    // Do not infer a seek from clock correction. The AudioWorklet clock may
    // legitimately jump by a fraction of a second after an underrun/report
    // correction; treating that as a seek destroys the prefetched ring and is
    // what caused several blank frames before it rebuilt. MainWindow exposes an
    // explicit transport revision for actual seek/stop operations.
    currentTime_ = value;
}

void GLRenderer::setNoteSpeed(float secondsPerWindow)
{
    const float value =
        std::clamp(
            secondsPerWindow,
            0.1f,
            60.0f);

    if (std::abs(noteSpeed_ - value) < 0.00001f)
        return;

    noteSpeed_ = value;
    forceCacheReset_ = true;
    sharpForceReset_ = true;
}

void GLRenderer::setPostBuffer(float seconds)
{
    const float value =
        std::clamp(
            seconds,
            0.0f,
            10.0f);

    if (std::abs(postBuffer_ - value) < 0.00001f)
        return;

    postBuffer_ = value;
    forceCacheReset_ = true;
}

void GLRenderer::setPerTrackColors(bool enabled)
{
    if (perTrackColors_ == enabled)
        return;
    perTrackColors_ = enabled;
    sharpForceReset_ = true;
}

void GLRenderer::setChannelColor(
    uint8_t channel,
    uint8_t r,
    uint8_t g,
    uint8_t b)
{
    if (channel >= channelColors_.size())
        return;

    const std::array<uint8_t,4> value =
        {r,g,b,255};

    if (channelColors_[channel] == value)
        return;

    channelColors_[channel] = value;
    paletteDirty_ = true;
    regenerateSharpPalette();
}

void GLRenderer::setNeuralVisual(float hue, float activity)
{
    neuralTargetHue_ = wrapHue(hue);
    neuralActivity_ =
        std::clamp(activity, 0.0f, 1.0f);
}

void GLRenderer::updateBackgroundHue()
{
    const auto now =
        std::chrono::steady_clock::now();

    float frameScale =
        std::chrono::duration<float>(
            now -
            neuralClock_).count() *
        60.0f;

    neuralClock_ = now;

    frameScale =
        std::clamp(
            frameScale,
            0.0f,
            3.0f);

    float hueDiff =
        neuralTargetHue_ -
        neuralHue_;

    if (hueDiff > 180.0f)
        hueDiff -= 360.0f;

    if (hueDiff < -180.0f)
        hueDiff += 360.0f;

    neuralHue_ =
        wrapHue(
            neuralHue_ +
            hueDiff *
            .02f *
            frameScale);
}

void GLRenderer::setBackgroundAlpha(float alpha)
{
    backgroundAlpha_ =
        std::clamp(alpha, 0.0f, 1.0f);
}

void GLRenderer::renderBackground()
{
    updateBackgroundHue();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    if (backgroundProgram_) {
        glUseProgram(
            backgroundProgram_);

        glUniform1f(
            backgroundHueUniform_,
            neuralHue_);

        glUniform1f(
            backgroundActivityUniform_,
            neuralActivity_);

        glUniform1f(
            backgroundAspectUniform_,
            float(width_) /
            float(std::max(1, height_)));

        glUniform1f(
            backgroundAlphaUniform_,
            backgroundAlpha_);

        glBindVertexArray(
            backgroundVao_);

        glDrawArrays(
            GL_TRIANGLE_STRIP,
            0,
            4);

        glBindVertexArray(0);
        glUseProgram(0);
    } else {
        // Safe visual fallback: notes remain usable even if the optional
        // background shader is rejected by a WebGL implementation.
        glClearColor(
            .010f * backgroundAlpha_,
            .010f * backgroundAlpha_,
            .026f * backgroundAlpha_,
            backgroundAlpha_);

        glClear(
            GL_COLOR_BUFFER_BIT);
    }
}

void GLRenderer::resetVisualPageCache(bool reinstallDocument)
{
    visualPages_.clear();
    visualPendingPages_.clear();
    visualPageSpanTicks_ = 0;
    visualWantedFirstPage_ = 0;
    visualWantedPageCount_ = 0;
    visualCurrentPage_ = 0;
    visualPrimeWidth_ = 0;
    ++visualPageRevision_;
    if (visualPageRevision_ == 0)
        ++visualPageRevision_;
    remoteDrawRevision_ = 0;
    remoteDrawSpan_ = 0;
    remoteDrawFirstPage_ = std::numeric_limits<uint32_t>::max();
    remoteDrawLastPage_ = std::numeric_limits<uint32_t>::max();
    remoteWantedDrawFirstPage_ = std::numeric_limits<uint32_t>::max();
    remoteWantedDrawLastPage_ = std::numeric_limits<uint32_t>::max();
    denseNotes_.clear();
    denseSourceScratch_.clear();

    // Every reset starts a new asynchronous page transaction. This is required
    // even for a seek-only reset where the document itself stays installed.
    ++visualCacheGeneration_;
    if (visualCacheGeneration_ == 0)
        ++visualCacheGeneration_;

    if (!reinstallDocument)
        return;

#ifdef __EMSCRIPTEN__
    // Remote-indexed documents keep source events in the persistent Memory64
    // parser Worker. There is intentionally nothing to copy into the legacy
    // visual-cache Worker here. visual_cache_prime() routes page requests to
    // the mapped Worker instead.
    if (document_ && document_->remoteIndexed) {
        wasmidi_visual_cache_shutdown();
    } else if (document_ && !document_->visualNotes.empty()) {
        const std::size_t count = document_->visualNotes.size();
        const uint32_t safeCount =
            static_cast<uint32_t>(
                std::min<std::size_t>(
                    count,
                    std::numeric_limits<uint32_t>::max()));

        wasmidi_visual_cache_install(
            reinterpret_cast<const uint32_t*>(
                document_->visualNotes.data()),
            safeCount,
            reinterpret_cast<const uint32_t*>(
                document_->visualKeyStarts.data()),
            static_cast<uint32_t>(
                std::min<std::size_t>(
                    document_->visualKeyStarts.size(),
                    std::numeric_limits<uint32_t>::max())),
            reinterpret_cast<const uint32_t*>(
                document_->visualKeyEnds.data()),
            static_cast<uint32_t>(
                std::min<std::size_t>(
                    document_->visualKeyEnds.size(),
                    std::numeric_limits<uint32_t>::max())),
            reinterpret_cast<const uint32_t*>(
                document_->visualKeyOwners.data()),
            static_cast<uint32_t>(
                std::min<std::size_t>(
                    document_->visualKeyOwners.size(),
                    std::numeric_limits<uint32_t>::max())),
            visualCacheGeneration_,
            document_->maxTick);
    }
#endif
}

void GLRenderer::receiveVisualPage(
    uint32_t generation,
    uint32_t spanTicks,
    uint32_t pageIndex,
    const uint32_t* words,
    uint32_t noteCount,
    uint32_t sourceCount,
    double difficulty)
{
    if (generation != visualCacheGeneration_ ||
        spanTicks == 0 ||
        spanTicks != visualPageSpanTicks_ ||
        visualWantedPageCount_ == 0 ||
        pageIndex < visualWantedFirstPage_ ||
        pageIndex >= visualWantedFirstPage_ + visualWantedPageCount_) {
        return;
    }

    visualPendingPages_.erase(pageIndex);

    VisualPage page;
    page.spanTicks = spanTicks;
    page.pageIndex = pageIndex;
    page.sourceCount = sourceCount;
    page.difficulty = difficulty;
    page.notes.resize(noteCount);

    if (noteCount && words) {
        std::memcpy(
            page.notes.data(),
            words,
            std::size_t(noteCount) * sizeof(VisualNote));
    }

    if (visualPages_.size() >= VisualPageCacheCapacity &&
        visualPages_.find(pageIndex) == visualPages_.end()) {
        auto victim = visualPages_.end();
        uint32_t farthest = 0;
        for (auto it = visualPages_.begin(); it != visualPages_.end(); ++it) {
            // A page that still intersects the current viewport is pinned. It is
            // never legal to reclaim visible geometry just to make room for a
            // speculative future page.
            if (remoteWantedDrawFirstPage_ !=
                    std::numeric_limits<uint32_t>::max() &&
                it->first >= remoteWantedDrawFirstPage_ &&
                it->first <= remoteWantedDrawLastPage_) {
                continue;
            }

            const uint32_t distance =
                it->first > visualCurrentPage_
                    ? it->first - visualCurrentPage_
                    : visualCurrentPage_ - it->first;
            if (victim == visualPages_.end() || distance > farthest) {
                victim = it;
                farthest = distance;
            }
        }
        if (victim != visualPages_.end())
            visualPages_.erase(victim);
    }

    visualPages_[pageIndex] = std::move(page);

    // Far-ahead pages must not force the current VBO to be concatenated and
    // re-uploaded. Only pages the current viewport is waiting for invalidate
    // the assembled remote draw list.
    if (remoteWantedDrawFirstPage_ == std::numeric_limits<uint32_t>::max() ||
        (pageIndex >= remoteWantedDrawFirstPage_ &&
         pageIndex <= remoteWantedDrawLastPage_)) {
        ++visualPageRevision_;
        if (visualPageRevision_ == 0)
            ++visualPageRevision_;
    }
}

void GLRenderer::primeVisualPageCache(
    uint32_t viewStart,
    uint32_t viewEnd)
{
    if (!document_ ||
        (!document_->remoteIndexed && document_->visualNotes.empty()))
        return;

    const uint32_t actualSpan =
        std::max<uint32_t>(1, viewEnd - viewStart);

    if (visualPageSpanTicks_ == 0) {
        // A page is a cache tile, not a promise that every later viewport must
        // equal exactly one page. Keep this span stable across tempo changes.
        // Rebuilding all 64 pages whenever ticks-per-screen changes was far
        // slower than rendering directly and caused visible cache holes.
        visualPageSpanTicks_ = actualSpan;
        visualPages_.clear();
    }

    const uint32_t span = std::max<uint32_t>(1, visualPageSpanTicks_);
    const uint32_t currentPage = viewStart / span;
    const uint32_t firstPage =
        currentPage > VisualHistoryPages
            ? currentPage - VisualHistoryPages
            : 0;
    const uint32_t maxPage = document_->maxTick / span;
    const uint64_t wantedLast64 =
        uint64_t(currentPage) + uint64_t(VisualPrefetchAheadPages);
    const uint32_t wantedLast = static_cast<uint32_t>(
        std::min<uint64_t>(uint64_t(maxPage), wantedLast64));
    const uint32_t pageCount =
        wantedLast >= firstPage
            ? wantedLast - firstPage + 1
            : 1;

    visualWantedFirstPage_ = firstPage;
    visualWantedPageCount_ = pageCount;
    visualCurrentPage_ = currentPage;

    // Normal forward motion must NOT cancel pages already being built. The old
    // requestChanged path cleared the in-flight set every time currentPage moved,
    // so a dense MIDI continually restarted its 64-screen prefetch and produced
    // each page only at the last possible moment. Seeking still changes the cache
    // generation in resetVisualCache(), which is the correct cancellation point.

    for (auto it = visualPages_.begin(); it != visualPages_.end();) {
        if (it->second.spanTicks != span ||
            it->first < firstPage ||
            it->first >= firstPage + pageCount) {
            it = visualPages_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = visualPendingPages_.begin();
         it != visualPendingPages_.end();) {
        if (*it < firstPage || *it >= firstPage + pageCount)
            it = visualPendingPages_.erase(it);
        else
            ++it;
    }

    // Tell the Worker exactly which rolling tiles are absent AND not already
    // in flight. Unlike Pass 13.1, this is not gated solely on requestChanged:
    // if a page reply was dropped/cancelled, the renderer can recover it.
    uint32_t missingLo = 0;
    uint32_t missingHi = 0;
    uint32_t missingTop = 0;
    for (uint32_t i = 0; i < pageCount; ++i) {
        const uint32_t pageIndex = firstPage + i;
        const auto cached = visualPages_.find(pageIndex);
        const bool missing =
            cached == visualPages_.end() || cached->second.spanTicks != span;
        if (!missing || visualPendingPages_.find(pageIndex) != visualPendingPages_.end())
            continue;
        if (i < 32)
            missingLo |= uint32_t(1) << i;
        else if (i < 64)
            missingHi |= uint32_t(1) << (i - 32);
        else
            missingTop |= uint32_t(1) << (i - 64);
    }

#ifdef __EMSCRIPTEN__
    if (missingLo != 0 || missingHi != 0 || missingTop != 0) {
        for (uint32_t i = 0; i < pageCount; ++i) {
            const bool requested =
                i < 32
                    ? ((missingLo >> i) & 1u) != 0
                    : i < 64
                        ? ((missingHi >> (i - 32)) & 1u) != 0
                        : ((missingTop >> (i - 64)) & 1u) != 0;
            if (requested)
                visualPendingPages_.insert(firstPage + i);
        }

        wasmidi_visual_cache_prime(
            visualCacheGeneration_,
            span,
            firstPage,
            pageCount,
            currentPage,
            missingLo,
            missingHi,
            missingTop);
    }
#else
    (void)missingLo;
    (void)missingHi;
    (void)missingTop;
#endif
}

bool GLRenderer::collectCachedPageNotes(
    uint32_t searchStart,
    uint32_t viewEnd,
    std::vector<VisualNote>& output) const
{
    output.clear();

    if (visualPageSpanTicks_ == 0 || visualPages_.empty())
        return false;

    const uint32_t span = visualPageSpanTicks_;
    const uint32_t firstPage = searchStart / span;
    const uint32_t lastPage = viewEnd / span;

    // A normal viewport touches at most three screen pages (one history page
    // plus the visible/future range). Refuse pathological stale-scale cases.
    if (lastPage < firstPage || lastPage - firstPage > 63)
        return false;

    std::size_t reserveCount = 0;
    for (uint32_t pageIndex = firstPage;
         pageIndex <= lastPage;
         ++pageIndex) {
        const auto it = visualPages_.find(pageIndex);
        if (it == visualPages_.end() ||
            it->second.spanTicks != span) {
            return false;
        }
        reserveCount += it->second.notes.size();
    }

    output.reserve(reserveCount);

    // Each tile is independently seekable, so it repeats notes that began in
    // an earlier tile and are still alive at this tile's left edge. Never draw
    // those carries twice: doing so changes MPWGL2's stable-start overwrite
    // order. Instead reconcile a later carry with the already-emitted open
    // instance. If the later tile contains its NoteOff, it also supplies the
    // real endTick so a long note stops at exactly the correct position.
    std::unordered_map<uint64_t, std::vector<std::size_t>> openByVisualKey;
    openByVisualKey.reserve(1024);

    const auto visualKey = [](const VisualNote& note) -> uint64_t {
        return (uint64_t(note.startTick) << 32) |
               uint64_t(note.packedData);
    };

    for (uint32_t pageIndex = firstPage;
         pageIndex <= lastPage;
         ++pageIndex) {
        const auto& page = visualPages_.at(pageIndex);
        const uint64_t pageStart64 = uint64_t(pageIndex) * uint64_t(span);
        const uint32_t pageStart = static_cast<uint32_t>(
            std::min<uint64_t>(
                pageStart64,
                std::numeric_limits<uint32_t>::max()));

        // For identical-looking overlapping notes, map carries to prior open
        // instances in FIFO order. MPWGL2 also pairs repeated NoteOns FIFO.
        std::unordered_map<uint64_t, std::size_t> carryCursor;

        for (const VisualNote& note : page.notes) {
            if (document_ && document_->remoteIndexed) {
                if (note.startTick > viewEnd ||
                    (note.endTick != 0 && note.endTick < searchStart)) {
                    continue;
                }

                const bool carry =
                    pageIndex != firstPage &&
                    note.startTick < pageStart;

                bool reconciledCarry = false;
                if (carry) {
                    const uint64_t key = visualKey(note);
                    const auto found = openByVisualKey.find(key);
                    if (found != openByVisualKey.end()) {
                        std::size_t& cursor = carryCursor[key];
                        auto& candidates = found->second;

                        while (cursor < candidates.size()) {
                            const std::size_t outputIndex = candidates[cursor++];
                            if (outputIndex >= output.size())
                                continue;

                            VisualNote& existing = output[outputIndex];
                            if (existing.endTick != 0 &&
                                existing.endTick < pageStart) {
                                continue;
                            }

                            if (note.endTick != 0)
                                existing.endTick = note.endTick;

                            reconciledCarry = true;
                            break;
                        }
                    }
                    // No earlier copy is resident (normally only possible when
                    // the first requested tile changed during recovery). Keep
                    // this carry so the screen remains complete.
                }

                if (!reconciledCarry) {
                    output.push_back(note);
                    if (note.endTick == 0) {
                        openByVisualKey[visualKey(note)].push_back(
                            output.size() - 1);
                    }
                }
                continue;
            }

            if (note.startTick < searchStart || note.startTick > viewEnd)
                continue;
            output.push_back(note);
        }
    }

    // Pages are already emitted in MPWGL2 stable-start order. Because the
    // cache uses half-open tick tiles, every non-carry note belongs to exactly
    // one page; concatenating pages in ascending page order therefore preserves
    // that global order. Re-sorting here used incomplete carry end-times and
    // could undo the parser's exact closure ordering after a seek.

    return true;
}

bool GLRenderer::buildDenseDrawList(
    uint32_t viewStart,
    uint32_t viewEnd,
    std::size_t desiredBegin,
    std::size_t desiredEnd)
{
    if (!document_ || viewEnd <= viewStart)
        return false;

    constexpr std::size_t DenseThreshold = 8192;
    constexpr std::size_t MaxRawRecoveryNotes = 1000000;

    const std::size_t rawVisibleCount =
        carryNotes_.size() +
        (desiredEnd > desiredBegin ? desiredEnd - desiredBegin : 0);

    if (rawVisibleCount < DenseThreshold)
        return false;

    const uint32_t span = viewEnd - viewStart;
    const uint32_t searchStart = viewStart > span ? viewStart - span : 0;

    std::vector<VisualNote> pageNotes;
    const bool pagesReady =
        collectCachedPageNotes(searchStart, viewEnd, pageNotes);

    if (!pagesReady && rawVisibleCount > MaxRawRecoveryNotes) {
        // Recovery path: do not replace a GPU-heavy frame with a million-note
        // CPU copy while the background page Worker is still finishing it.
        return false;
    }

    denseSourceScratch_.clear();
    denseSourceScratch_.reserve(
        carryNotes_.size() +
        (pagesReady
            ? pageNotes.size()
            : (desiredEnd > desiredBegin ? desiredEnd - desiredBegin : 0)));

    denseSourceScratch_.insert(
        denseSourceScratch_.end(),
        carryNotes_.begin(),
        carryNotes_.end());

    if (pagesReady) {
        denseSourceScratch_.insert(
            denseSourceScratch_.end(),
            pageNotes.begin(),
            pageNotes.end());
    } else {
        const auto& notes = document_->visualNotes;
        const std::size_t begin = std::min(desiredBegin, notes.size());
        const std::size_t end = std::min(desiredEnd, notes.size());
        denseSourceScratch_.insert(
            denseSourceScratch_.end(),
            notes.begin() + static_cast<std::ptrdiff_t>(begin),
            notes.begin() + static_cast<std::ptrdiff_t>(end));
    }

    if (denseSourceScratch_.empty())
        return false;

    const int columns = std::max(1, width_ - 1);
    const std::size_t wordsPerPitch =
        (std::size_t(columns) + 63u) / 64u;
    denseCoverage_.assign(
        std::size_t(128) * wordsPerPitch,
        uint64_t(0));
    denseNotes_.clear();
    denseNotes_.reserve(
        std::min<std::size_t>(
            denseSourceScratch_.size(),
            std::size_t(columns) * 128u));

    const double tickSpan = double(viewEnd - viewStart);

    auto snappedColumn = [&](uint32_t tick) -> int {
        const double normalized =
            (double(tick) - double(viewStart)) / tickSpan;
        return static_cast<int>(
            std::floor(normalized * double(columns) + 0.5));
    };

    // Reverse source order: later notes are opaque and therefore define which
    // pixels an earlier note could still possibly contribute. We never split a
    // partially visible note; only fully hidden/sub-pixel-equivalent notes are
    // removed, preserving stacking, clipping and any per-note shading.
    for (auto it = denseSourceScratch_.rbegin();
         it != denseSourceScratch_.rend();
         ++it) {
        const VisualNote& note = *it;
        const int pitch = int((note.packedData >> 8) & 0x7f);

        int left = snappedColumn(note.startTick);
        int right = snappedColumn(note.endTick);
        if (right < left)
            std::swap(left, right);

        left = std::clamp(left, 0, columns);
        right = std::clamp(right, 0, columns);

        // Equal snapped endpoints create a zero-area triangle strip in the
        // current shader, so dropping them is exactly raster-equivalent.
        if (right <= left)
            continue;

        const std::size_t rowBase = std::size_t(pitch) * wordsPerPitch;
        const std::size_t firstWord = std::size_t(left) >> 6u;
        const std::size_t lastWord = std::size_t(right - 1) >> 6u;
        bool contributes = false;

        for (std::size_t word = firstWord; word <= lastWord; ++word) {
            const unsigned lo =
                word == firstWord
                    ? unsigned(std::size_t(left) & 63u)
                    : 0u;
            const unsigned hi =
                word == lastWord
                    ? unsigned((std::size_t(right - 1) & 63u) + 1u)
                    : 64u;

            const uint64_t lowMask =
                lo == 0u ? ~uint64_t(0) : (~uint64_t(0) << lo);
            const uint64_t highMask =
                hi == 64u ? ~uint64_t(0) : ((uint64_t(1) << hi) - 1u);
            const uint64_t mask = lowMask & highMask;

            if ((~denseCoverage_[rowBase + word] & mask) != 0u) {
                contributes = true;
                break;
            }
        }

        if (!contributes)
            continue;

        denseNotes_.push_back(note);

        for (std::size_t word = firstWord; word <= lastWord; ++word) {
            const unsigned lo =
                word == firstWord
                    ? unsigned(std::size_t(left) & 63u)
                    : 0u;
            const unsigned hi =
                word == lastWord
                    ? unsigned((std::size_t(right - 1) & 63u) + 1u)
                    : 64u;
            const uint64_t lowMask =
                lo == 0u ? ~uint64_t(0) : (~uint64_t(0) << lo);
            const uint64_t highMask =
                hi == 64u ? ~uint64_t(0) : ((uint64_t(1) << hi) - 1u);
            denseCoverage_[rowBase + word] |= lowMask & highMask;
        }
    }

    std::reverse(denseNotes_.begin(), denseNotes_.end());

    // If a sparse-looking dense range cannot be reduced meaningfully, retain
    // the persistent source ring instead of uploading a nearly identical VBO.
    if (denseNotes_.size() * 10 >= denseSourceScratch_.size() * 9)
        return false;

    uploadDenseDrawList();
    return !denseNotes_.empty();
}

void GLRenderer::uploadDenseDrawList()
{
    if (!initialized_ || !denseVbo_)
        return;

    glBindBuffer(GL_ARRAY_BUFFER, denseVbo_);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(denseNotes_.size() * sizeof(VisualNote)),
        denseNotes_.empty() ? nullptr : denseNotes_.data(),
        GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLRenderer::drawDenseNotes()
{
    if (denseNotes_.empty())
        return;

    glBindVertexArray(denseVao_);
    glDrawArraysInstanced(
        GL_TRIANGLE_STRIP,
        0,
        4,
        static_cast<GLsizei>(denseNotes_.size()));
}

void GLRenderer::allocateRing(std::size_t capacity)
{
    std::size_t rounded = 1;
    while (rounded < std::max<std::size_t>(capacity, 2))
        rounded <<= 1;
#ifdef __EMSCRIPTEN__
    if (ringCapacity_ != 0 && rounded > ringCapacity_) {
        EM_ASM({ console.warn("WASMIDI renderer ring grows", $0, "->", $1, "notes (HANDOFF sec. 34)"); },
               double(ringCapacity_), double(rounded));
    }
#endif

    const std::size_t oldCapacity = ringCapacity_;
    const std::size_t oldMask = ringMask_;
    std::vector<VisualNote> oldRing = std::move(ring_);

    ringCapacity_ = rounded;
    ringMask_ = rounded - 1;
    ring_.assign(ringCapacity_, VisualNote{});

    const uint32_t copyEnd = std::max(sharpHead_, sharpRemotePreparedHead_);
    if (oldCapacity != 0 && copyEnd > sharpTail_) {
        for (uint32_t id = sharpTail_; id < copyEnd; ++id)
            ring_[std::size_t(id) & ringMask_] =
                oldRing[std::size_t(id) & oldMask];
    }

    glBindBuffer(GL_ARRAY_BUFFER, noteVbo_);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(ringCapacity_ * sizeof(VisualNote)),
        nullptr,
        GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (copyEnd > sharpTail_)
        uploadSharpRange(sharpTail_, copyEnd);
}

void GLRenderer::ensureRingCapacity(std::size_t required)
{
    if (required <= ringCapacity_)
        return;

    std::size_t target = std::max<std::size_t>(
        ringCapacity_, std::size_t(1) << 23);
    while (target < required)
        target <<= 1;
    allocateRing(target);
}

void GLRenderer::uploadSourceRange(
    std::size_t begin,
    std::size_t end)
{
    if (!document_ ||
        begin >= end ||
        ringCapacity_ == 0) {
        return;
    }

    const auto& notes =
        document_->visualNotes;

    if (end > notes.size())
        end = notes.size();

    glBindBuffer(
        GL_ARRAY_BUFFER,
        noteVbo_);

    std::size_t cursor = begin;

    while (cursor < end) {
        const std::size_t physical =
            cursor & ringMask_;

        const std::size_t count =
            std::min(
                end - cursor,
                ringCapacity_ - physical);

        // Copy only the newly entering contiguous source segment into the CPU
        // mirror and issue one matching contiguous WebGL transfer.
        for (std::size_t i = 0;
             i < count;
             ++i) {
            ring_[physical + i] =
                notes[cursor + i];
        }

        glBufferSubData(
            GL_ARRAY_BUFFER,
            static_cast<GLintptr>(
                physical *
                sizeof(VisualNote)),
            static_cast<GLsizeiptr>(
                count *
                sizeof(VisualNote)),
            ring_.data() + physical);

        cursor += count;
    }

    glBindBuffer(
        GL_ARRAY_BUFFER,
        0);
}

void GLRenderer::rebuildVisualCache(
    std::size_t begin,
    std::size_t end)
{
    sourceBegin_ = begin;
    sourceEnd_ = end;

    ensureRingCapacity(
        std::max<std::size_t>(
            1,
            sourceEnd_ -
            sourceBegin_));

    uploadSourceRange(
        sourceBegin_,
        sourceEnd_);

    forceCacheReset_ = false;
}

void GLRenderer::uploadCarryCache()
{
    if (!initialized_ || carryVbo_ == 0)
        return;

    glBindBuffer(
        GL_ARRAY_BUFFER,
        carryVbo_);

    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(
            std::max<std::size_t>(
                1,
                carryNotes_.size()) *
            sizeof(VisualNote)),
        carryNotes_.empty()
            ? nullptr
            : carryNotes_.data(),
        GL_DYNAMIC_DRAW);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        0);
}

void GLRenderer::rebuildCarryCache(
    uint32_t viewStart,
    std::size_t desiredBegin)
{
    carryNotes_.clear();

    if (!document_ || desiredBegin == 0) {
        uploadCarryCache();
        return;
    }

    const auto& notes =
        document_->visualNotes;

    const auto& blockMaxEnd =
        document_->visualBlockMaxEnd;

    const std::size_t blockSize =
        MidiDocument::VisualSeekBlockSize;

    const std::size_t lastBlock =
        (desiredBegin + blockSize - 1) /
        blockSize;

    // A seek can land in the middle of a very long note. Search only source
    // blocks whose maximum end tick can still intersect the visible history;
    // this avoids rescanning every old note while preserving exact source order.
    for (std::size_t block = 0;
         block < lastBlock;
         ++block) {
        if (block < blockMaxEnd.size() &&
            blockMaxEnd[block] < viewStart) {
            continue;
        }

        const std::size_t begin =
            block * blockSize;

        const std::size_t end =
            std::min(
                desiredBegin,
                begin + blockSize);

        for (std::size_t i = begin;
             i < end;
             ++i) {
            if (notes[i].endTick >= viewStart)
                carryNotes_.push_back(notes[i]);
        }
    }

    uploadCarryCache();
}

void GLRenderer::advanceCarryCache(
    uint32_t viewStart,
    std::size_t oldBegin,
    std::size_t desiredBegin)
{
    if (!document_) {
        carryNotes_.clear();
        uploadCarryCache();
        return;
    }

    if (desiredBegin < oldBegin) {
        rebuildCarryCache(
            viewStart,
            desiredBegin);
        return;
    }

    bool changed = false;

    const auto oldSize =
        carryNotes_.size();

    carryNotes_.erase(
        std::remove_if(
            carryNotes_.begin(),
            carryNotes_.end(),
            [viewStart](const VisualNote& note) {
                return note.endTick < viewStart;
            }),
        carryNotes_.end());

    changed =
        carryNotes_.size() != oldSize;

    const auto& notes =
        document_->visualNotes;

    const std::size_t appendEnd =
        std::min(
            desiredBegin,
            notes.size());

    for (std::size_t i = oldBegin;
         i < appendEnd;
         ++i) {
        if (notes[i].endTick >= viewStart) {
            carryNotes_.push_back(notes[i]);
            changed = true;
        }
    }

    if (changed)
        uploadCarryCache();
}

void GLRenderer::syncVisualCache(
    uint32_t viewStart,
    uint32_t viewEnd)
{
    if (document_ && document_->remoteIndexed) {
        sourceBegin_ = 0;
        sourceEnd_ = 0;
        if (!carryNotes_.empty()) {
            carryNotes_.clear();
            uploadCarryCache();
        }
        return;
    }

    if (!document_ ||
        document_->visualNotes.empty()) {
        sourceBegin_ = 0;
        sourceEnd_ = 0;
        if (!carryNotes_.empty()) {
            carryNotes_.clear();
            uploadCarryCache();
        }
        return;
    }

    const uint32_t span =
        std::max<uint32_t>(
            1,
            viewEnd - viewStart);

    // This reproduces MPWGL2's _writeStrip search range for a full roll:
    // lowerBound(stripStart - windowTicks - postTicks). With viewStart equal
    // to stripStart and span=(window+post), this is viewStart-span.
    const uint32_t searchStart =
        viewStart > span
            ? viewStart - span
            : 0;

    const std::size_t desiredBegin =
        document_->
            lowerBoundVisualStart(
                double(searchStart));

    const std::size_t desiredEnd =
        document_->
            upperBoundVisualStart(
                double(viewEnd));

    const bool forwardCompatible =
        !forceCacheReset_ &&
        desiredBegin >= sourceBegin_ &&
        desiredEnd >= sourceEnd_ &&
        desiredBegin <= sourceEnd_;

    if (!forwardCompatible) {
        rebuildVisualCache(
            desiredBegin,
            desiredEnd);
        rebuildCarryCache(
            viewStart,
            desiredBegin);
        return;
    }

    advanceCarryCache(
        viewStart,
        sourceBegin_,
        desiredBegin);

    const std::size_t required =
        desiredEnd - desiredBegin;

    if (required > ringCapacity_) {
        sourceBegin_ = desiredBegin;
        sourceEnd_ = desiredEnd;
        ensureRingCapacity(required);
        // allocateRing() already re-uploaded the new source range.
        forceCacheReset_ = false;
        return;
    }

    if (desiredEnd > sourceEnd_) {
        uploadSourceRange(
            sourceEnd_,
            desiredEnd);
    }

    sourceBegin_ = desiredBegin;
    sourceEnd_ = desiredEnd;
}

void GLRenderer::setInstanceBase(
    std::size_t physicalIndex)
{
    const std::size_t base =
        physicalIndex *
        sizeof(VisualNote);

    glBindBuffer(
        GL_ARRAY_BUFFER,
        noteVbo_);

    glVertexAttribIPointer(
        0, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            base +
            offsetof(
                VisualNote,
                startTick)));

    glVertexAttribIPointer(
        1, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            base +
            offsetof(
                VisualNote,
                endTick)));

    glVertexAttribIPointer(
        2, 1,
        GL_UNSIGNED_INT,
        sizeof(VisualNote),
        reinterpret_cast<void*>(
            base +
            offsetof(
                VisualNote,
                packedData)));
}

void GLRenderer::resetSharpRenderer(bool requestRemote)
{
    sharpHead_ = 1;
    sharpTail_ = 1;
    ++sharpRingEpoch_;
    sharpLastSweepEnd_ = -1;
    sharpLastWindowTicks_ = 0;
    sharpStableWindowTicks_ = 0;
    sharpStableNoteSpeed_ = -1.0f;
    sharpActiveCount_.fill(0);
    sharpActiveColor_.fill(0);
    sharpActiveId_.fill(0);
    sharpRemoteRequestedEnd_ = 0;
    sharpRemoteSafeThrough_ = 0;
    sharpRemoteUrgentThrough_ = 0;
    sharpRemoteStartTick_ = 0;
    sharpRemoteWaitingReset_ = false;
    sharpRemoteBatches_.clear();
    sharpRemoteRebuilding_ = false;
    sharpRemoteBuildTarget_ = 0;
    sharpRemoteBuildWindowTicks_ = 0;
    sharpRemoteBuildPerTrack_ = false;
    sharpRemotePendingBase_ = 0;
    sharpRemotePreparedHead_ = 1;
    sharpRemotePendingAppends_.clear();
    sharpRemotePendingCloseWords_.clear();
    sharpRemotePendingCloseOffset_ = 0;
    sharpCatchUpFloor_ = 0;
    sharpCatchUpLead_ = 0;
    sharpHeldSince_ = {};
    sharpReadySince_ = {};

    // Head/tail/generation delimit the valid ring contents. Clearing the full
    // 2^23-note CPU mirror here writes roughly 96 MiB on every seek/reset and
    // can steal several display/audio quanta for data that will never be read.
    // Remote/local sweep code overwrites every record before publishing it, so
    // leave old bytes untouched and invalidate them logically instead.

#ifdef __EMSCRIPTEN__
    if (requestRemote && document_ && document_->remoteIndexed) {
        ++sharpRemoteGeneration_;
        if (sharpRemoteGeneration_ == 0)
            ++sharpRemoteGeneration_;
    }
#else
    (void)requestRemote;
#endif
}

void GLRenderer::calculateSharpView(
    uint32_t& currentTick,
    uint32_t& viewStart,
    uint32_t& viewEnd,
    uint32_t& windowTicks)
{
    if (!document_) {
        currentTick = viewStart = viewEnd = 0;
        windowTicks = 1;
        return;
    }

    currentTick = static_cast<uint32_t>(std::clamp<double>(
        std::floor(document_->secondsToTick(currentTime_)),
        0.0,
        double(document_->maxTick)));

    // SharpMIDI's WindowTicks is a stable renderer parameter.  The previous
    // port recomputed it at every tempo change, which made an otherwise
    // forward playback look like a seek and repeatedly destroyed/rebuilt the
    // ring.  Map WASMIDI's seconds control once at the song's initial tempo
    // (the same fixed mapping MPWGL2 uses) and keep the tick window unchanged
    // until the user actually changes Note Scale/Speed or loads another MIDI.
    if (sharpStableWindowTicks_ == 0 ||
        std::abs(sharpStableNoteSpeed_ - noteSpeed_) > 0.00001f) {
        const double referenceSeconds = std::min<double>(
            document_->durationSeconds,
            std::max(0.001, double(noteSpeed_)));
        const double referenceTick = document_->secondsToTick(referenceSeconds);
        const double zeroTick = document_->secondsToTick(0.0);
        const uint32_t half = std::max<uint32_t>(
            1u,
            static_cast<uint32_t>(std::ceil(std::max(1.0, referenceTick - zeroTick))));
        sharpStableWindowTicks_ = std::max<uint32_t>(2u, half * 2u);
        sharpStableNoteSpeed_ = noteSpeed_;
    }

    windowTicks = sharpStableWindowTicks_;
    const uint32_t half = std::max<uint32_t>(1u, windowTicks >> 1u);
    viewStart = currentTick > half ? currentTick - half : 0u;
    viewEnd = std::min<uint32_t>(document_->maxTick, currentTick + half);
    if (viewEnd <= viewStart)
        viewEnd = std::min<uint32_t>(document_->maxTick, viewStart + 1u);

    // Keep several *complete screens* resident ahead of the playhead instead
    // of generating the visible transition just-in-time.  The horizon is
    // density-aware so sparse MIDIs can be prepared very far ahead while a
    // Black MIDI stays inside a conservative fraction of the 2^23-note ring.
    const double notesPerTick = document_->maxTick > 0
        ? double(document_->noteCount) / double(document_->maxTick)
        : 0.0;
    const double estimatedPerScreen =
        std::max(1.0, notesPerTick * double(windowTicks));
    const double residentBudget =
        double(std::max<std::size_t>(ringCapacity_, std::size_t(1) << 23)) * 0.55;
    // Keep a healthy multi-screen reserve, but do not let speculative
    // preprocessing consume the same CPU/Worker budget needed by audio and
    // live-state updates.  Pass 13.8 raised this to 12..96 screens and then
    // also forced a 20-second horizon; on dense MIDIs that became continuous
    // background work and reduced performance everywhere.
    const uint32_t screensAhead = static_cast<uint32_t>(std::clamp<double>(
        std::floor(residentBudget / estimatedPerScreen), 4.0, 32.0));
    const uint64_t ahead = uint64_t(windowTicks) * uint64_t(screensAhead);
    sharpLookaheadTicks_ = static_cast<uint32_t>(std::min<uint64_t>(
        ahead, std::numeric_limits<uint32_t>::max()));
}

void GLRenderer::appendSharpNote(
    uint32_t startTick,
    uint8_t key,
    uint8_t velocity,
    uint8_t noteIndex)
{
    const std::size_t active = std::size_t(sharpHead_ - sharpTail_);
    ensureRingCapacity(active + 2u);

    const uint32_t id = sharpHead_++;
    ring_[std::size_t(id) & ringMask_] = {
        startTick,
        0u,
        uint32_t(velocity) |
            (uint32_t(key) << 8) |
            (uint32_t(noteIndex) << 16)
    };
}

void GLRenderer::closeSharpNote(
    uint32_t noteId,
    uint32_t endTick,
    std::vector<uint32_t>* closeIds)
{
    if (noteId < sharpTail_ || noteId >= sharpHead_ || ringCapacity_ == 0)
        return;

    ring_[std::size_t(noteId) & ringMask_].endTick = endTick;
    if (closeIds)
        closeIds->push_back(noteId);
}

void GLRenderer::processLocalSharpEvent(
    uint32_t tick,
    const CompactEvent& event,
    std::vector<uint32_t>& closeIds)
{
    const uint8_t command = event.status & 0xf0u;
    if (command != 0x90u && command != 0x80u)
        return;

    const uint8_t channel = event.status & 0x0fu;
    const uint8_t key = event.data1 & 0x7fu;
    const uint8_t velocity = event.data2 & 0x7fu;
    const bool noteOn = command == 0x90u && velocity != 0;

    // Small/non-mapped files do not retain the original track byte after parse.
    // Their high color nibble is the parser's per-track slot; it has the same
    // 4-bit owner domain SharpMIDI gets after its byte-sized (track << 4) wrap.
    const uint8_t ownerIndex = sharpModePerTrack_
        ? static_cast<uint8_t>(((event.color >> 4) << 4) | channel)
        : channel;
    const uint8_t colorSlot = sharpModePerTrack_
        ? static_cast<uint8_t>((event.color >> 4) & 0x0fu)
        : static_cast<uint8_t>(event.color & 0x0fu);

    const std::size_t headerIndex =
        (std::size_t(channel) << 7) | std::size_t(key);
    uint16_t count = sharpActiveCount_[headerIndex];
    const uint8_t activeOwner = sharpActiveColor_[headerIndex];

    if (noteOn) {
        if (count != 0 && activeOwner != ownerIndex) {
            closeSharpNote(sharpActiveId_[headerIndex], tick, &closeIds);
            count = 0;
        }

        if (count == 0) {
            const uint32_t id = sharpHead_;
            appendSharpNote(tick, key, velocity, colorSlot);
            sharpActiveId_[headerIndex] = id;
            sharpActiveColor_[headerIndex] = ownerIndex;
        }

        count = static_cast<uint16_t>(count + 1u);
    } else if (count > 0 && activeOwner == ownerIndex) {
        count = static_cast<uint16_t>(count - 1u);
        if (count == 0)
            closeSharpNote(sharpActiveId_[headerIndex], tick, &closeIds);
    }

    sharpActiveCount_[headerIndex] = count;
}

void GLRenderer::sweepLocalSharpRange(uint32_t fromTick, uint32_t toTick)
{
    if (!document_ || document_->events.empty() || fromTick > toTick)
        return;

    const uint32_t appendBegin = sharpHead_;
    std::vector<uint32_t> closeIds;
    closeIds.reserve(4096);

    std::size_t groupIndex = document_->lowerBoundGroup(fromTick);
    while (groupIndex < document_->tickGroups.size()) {
        const TickGroup& group = document_->tickGroups[groupIndex];
        if (group.tick > toTick)
            break;

        const std::size_t begin = group.eventOffset;
        const std::size_t end = std::min<std::size_t>(
            document_->events.size(), begin + group.eventCount);
        for (std::size_t i = begin; i < end; ++i)
            processLocalSharpEvent(group.tick, document_->events[i], closeIds);
        ++groupIndex;
    }

    if (sharpHead_ > appendBegin)
        uploadSharpRange(appendBegin, sharpHead_);
    uploadSharpCloseIds(closeIds);
}

void GLRenderer::uploadSharpRange(uint32_t beginId, uint32_t endId)
{
    if (beginId >= endId || ringCapacity_ == 0 || noteVbo_ == 0)
        return;

    glBindBuffer(GL_ARRAY_BUFFER, noteVbo_);
    uint32_t cursor = beginId;
    while (cursor < endId) {
        const std::size_t physical = std::size_t(cursor) & ringMask_;
        const std::size_t count = std::min<std::size_t>(
            std::size_t(endId - cursor), ringCapacity_ - physical);
        glBufferSubData(
            GL_ARRAY_BUFFER,
            static_cast<GLintptr>(physical * sizeof(VisualNote)),
            static_cast<GLsizeiptr>(count * sizeof(VisualNote)),
            ring_.data() + physical);
        cursor += static_cast<uint32_t>(count);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLRenderer::uploadSharpCloseIds(std::vector<uint32_t>& closeIds)
{
    if (closeIds.empty() || ringCapacity_ == 0 || noteVbo_ == 0)
        return;

    // Never refresh the entire live ring for a dense NoteOff transition. At
    // millions of resident notes that turns a close burst into a 50-100+ MB
    // WebGL upload and presents exactly as a frozen frame. Mark 256-note
    // physical blocks instead, then upload only contiguous dirty block runs.
    if (closeIds.size() >= 32768u) {
        constexpr std::size_t BlockNotes = 256u;
        const std::size_t blockCount =
            (ringCapacity_ + BlockNotes - 1u) / BlockNotes;
        std::vector<uint8_t> dirty(blockCount, 0u);
        for (uint32_t id : closeIds) {
            if (id < sharpTail_ || id >= sharpHead_)
                continue;
            const std::size_t physical = std::size_t(id) & ringMask_;
            dirty[physical / BlockNotes] = 1u;
        }

        glBindBuffer(GL_ARRAY_BUFFER, noteVbo_);
        std::size_t block = 0;
        while (block < blockCount) {
            while (block < blockCount && !dirty[block]) ++block;
            if (block >= blockCount) break;
            const std::size_t firstBlock = block;
            while (block < blockCount && dirty[block]) ++block;
            const std::size_t firstNote = firstBlock * BlockNotes;
            const std::size_t endNote = std::min(ringCapacity_, block * BlockNotes);
            glBufferSubData(
                GL_ARRAY_BUFFER,
                static_cast<GLintptr>(firstNote * sizeof(VisualNote)),
                static_cast<GLsizeiptr>((endNote - firstNote) * sizeof(VisualNote)),
                ring_.data() + firstNote);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        closeIds.clear();
        return;
    }

    std::sort(closeIds.begin(), closeIds.end());
    closeIds.erase(std::unique(closeIds.begin(), closeIds.end()), closeIds.end());

    glBindBuffer(GL_ARRAY_BUFFER, noteVbo_);
    std::size_t i = 0;
    while (i < closeIds.size()) {
        const uint32_t firstId = closeIds[i];
        if (firstId < sharpTail_ || firstId >= sharpHead_) {
            ++i;
            continue;
        }

        const std::size_t firstPhysical = std::size_t(firstId) & ringMask_;
        std::size_t run = 1;
        while (i + run < closeIds.size()) {
            const uint32_t prev = closeIds[i + run - 1];
            const uint32_t next = closeIds[i + run];
            if (next != prev + 1u)
                break;
            const std::size_t nextPhysical = std::size_t(next) & ringMask_;
            if (nextPhysical != firstPhysical + run)
                break;
            ++run;
        }

        glBufferSubData(
            GL_ARRAY_BUFFER,
            static_cast<GLintptr>(firstPhysical * sizeof(VisualNote)),
            static_cast<GLsizeiptr>(run * sizeof(VisualNote)),
            ring_.data() + firstPhysical);
        i += run;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLRenderer::advanceSharpTail(uint32_t viewStart)
{
    if (ringCapacity_ == 0)
        return;

    const uint32_t safeTail = sharpHead_ > ringCapacity_
        ? sharpHead_ - static_cast<uint32_t>(ringCapacity_)
        : 1u;
    if (sharpTail_ < safeTail)
        sharpTail_ = safeTail;

    while (sharpTail_ < sharpHead_) {
        const VisualNote& note = ring_[std::size_t(sharpTail_) & ringMask_];
        const bool open = note.endTick == 0;
        if (!open && note.endTick < viewStart)
            ++sharpTail_;
        else
            break;
    }
}

void GLRenderer::requestRemoteSharpSweep(
    uint32_t startTick,
    uint32_t endTick,
    bool reset)
{
#ifdef __EMSCRIPTEN__
    const bool requestPerTrack =
        sharpRemoteRebuilding_ ? sharpRemoteBuildPerTrack_ : sharpModePerTrack_;
    wasmidi_sharp_render_request(
        sharpRemoteGeneration_, startTick, endTick,
        sharpRemoteUrgentThrough_,
        requestPerTrack ? 1 : 0, reset ? 1 : 0);
#else
    (void)startTick;
    (void)endTick;
    (void)reset;
#endif
}

void GLRenderer::receiveSharpRenderReset(uint32_t generation)
{
    if (generation != sharpRemoteGeneration_)
        return;
    // The worker cursor is now ordered for the new generation. Start publishing
    // its ring progressively; keeping the previous frame visible until a
    // million-note replacement completed was the literal source of the
    // apparent one-frame freeze during dense transitions.
    sharpRemoteWaitingReset_ = false;
    sharpHead_ = 1u;
    sharpTail_ = 1u;
    ++sharpRingEpoch_;
    sharpRemotePreparedHead_ = 1u;
    sharpRemotePendingBase_ = 1u;
    sharpRemotePendingAppends_.clear();
    sharpRemotePendingCloseWords_.clear();
    sharpRemotePendingCloseOffset_ = 0;
    sharpModePerTrack_ = sharpRemoteBuildPerTrack_;
}

void GLRenderer::receiveSharpRenderDelta(
    uint32_t generation,
    uint32_t appendBase,
    const VisualNote* appends,
    uint32_t appendCount,
    const uint32_t* closeWords,
    uint32_t closeCount,
    uint32_t safeThrough,
    bool complete)
{
    if (generation != sharpRemoteGeneration_)
        return;

    // Worker replies arrive outside QQuickFramebufferObject::render(). Never
    // issue GL calls here; queue compact deltas and apply them when the FBO's
    // WebGL2 context is current on the next render pass.
    SharpRemoteBatch batch;
    batch.generation = generation;
    batch.appendBase = appendBase;
    batch.safeThrough = safeThrough;
    batch.complete = complete;
    if (appendCount && appends)
        batch.appends.assign(appends, appends + appendCount);
    if (closeCount && closeWords)
        batch.closeWords.assign(closeWords, closeWords + closeCount * 2u);
    sharpRemoteBatches_.push_back(std::move(batch));
}

void GLRenderer::flushRemoteSharpBatches(
    uint32_t requiredThrough,
    uint32_t desiredThrough)
{
    if (sharpRemoteBatches_.empty())
        return;

    std::vector<uint32_t> closeIds;

    // Bound WebGL upload work per presented frame. The Worker may finish many
    // chunks between two rAF/FBO callbacks; draining the whole backlog here
    // simply converts asynchronous preprocessing into a single visible stall.
    const bool visibleFrameMissing = sharpRemoteSafeThrough_ < requiredThrough;
    const bool prefetchLow = sharpRemoteSafeThrough_ < desiredThrough;

    // Keep VBO installation bounded per presented frame.  The 13.8 catch-up
    // budgets could push ~2M note/close operations through glBufferSubData in
    // one FBO callback, which fixed some blank frames by simply moving the
    // stall onto the UI/render thread.  A small proactive low-watermark budget
    // keeps several complete screens installed without stealing whole frames.
    const std::size_t maxBatches = visibleFrameMissing
        ? 4u
        : (prefetchLow ? 3u : 2u);
    const std::size_t maxWork = visibleFrameMissing
        ? 524288u
        : (prefetchLow ? 393216u : 262144u);
    std::size_t processCount = 0;
    std::size_t processWork = 0;
    while (processCount < sharpRemoteBatches_.size()) {
        const SharpRemoteBatch& candidate = sharpRemoteBatches_[processCount];
        const std::size_t work = candidate.appends.size() + candidate.closeWords.size() / 2u;
        if (processCount != 0 &&
            (processCount >= maxBatches || processWork + work > maxWork))
            break;
        processWork += work;
        ++processCount;
    }
    if (processCount == 0)
        processCount = 1;

    for (std::size_t batchIndex = 0; batchIndex < processCount; ++batchIndex) {
        SharpRemoteBatch& batch = sharpRemoteBatches_[batchIndex];
        if (batch.generation != sharpRemoteGeneration_)
            continue;

        // Rebuilds and normal forward sweeps use the same progressive hidden
        // ring staging path below.  A reset starts with head=1; complete ticks
        // are published as their chunks arrive, so there is no giant final
        // CPU copy/VBO upload and the transport can keep drawing every frame.

        // Incremental worker batches can split an enormous same-tick event
        // group. Stage append geometry immediately into the hidden portion of
        // the persistent ring/VBO, but do not advance sharpHead_ until the tick
        // is complete. Dense ticks therefore cost small incremental transfers
        // instead of one multi-million-note upload at the last second.
        if (!batch.appends.empty()) {
            uint32_t appendBase = batch.appendBase;
            uint32_t appendCount = static_cast<uint32_t>(batch.appends.size());
            uint32_t skip = 0;
            if (appendBase < sharpRemotePreparedHead_) {
                skip = std::min<uint32_t>(
                    appendCount, sharpRemotePreparedHead_ - appendBase);
            } else if (appendBase > sharpRemotePreparedHead_) {
                sharpForceReset_ = true;
                continue;
            }

            const uint32_t actualBase = appendBase + skip;
            const uint32_t actualCount = appendCount - skip;
            if (actualCount != 0) {
                const uint32_t required = actualBase + actualCount;
                const std::size_t resident = required > sharpTail_
                    ? std::size_t(required - sharpTail_) + 1u
                    : 2u;
                ensureRingCapacity(resident);
                // WebGL2 cannot expose SharpMIDI's persistently mapped VBO,
                // but the CPU ring can still receive each Worker delta as at
                // most two contiguous memcpy operations instead of one C++
                // assignment per note. This matters at million-note ticks.
                const std::size_t physical = std::size_t(actualBase) & ringMask_;
                const std::size_t first = std::min<std::size_t>(
                    actualCount, ringCapacity_ - physical);
                std::memcpy(
                    ring_.data() + physical,
                    batch.appends.data() + skip,
                    first * sizeof(VisualNote));
                const std::size_t second = std::size_t(actualCount) - first;
                if (second != 0) {
                    std::memcpy(
                        ring_.data(),
                        batch.appends.data() + skip + first,
                        second * sizeof(VisualNote));
                }
                // Upload while hidden. drawSharpRing() can only instance IDs
                // below sharpHead_, so a partial tick never becomes visible.
                uploadSharpRange(actualBase, required);
                sharpRemotePreparedHead_ = required;
                sharpRemotePendingBase_ = required;
            }
        }

        sharpRemotePendingCloseWords_.insert(
            sharpRemotePendingCloseWords_.end(),
            batch.closeWords.begin(), batch.closeWords.end());

        if (batch.complete) {
            sharpRemoteSafeThrough_ = std::max(
                sharpRemoteSafeThrough_, batch.safeThrough);
        } else if (batch.safeThrough > 0) {
            sharpRemoteSafeThrough_ = std::max(
                sharpRemoteSafeThrough_, batch.safeThrough - 1u);
        }

        const auto tickIsSafe = [&batch](uint32_t tick) {
            return batch.complete
                ? tick <= batch.safeThrough
                : tick < batch.safeThrough;
        };

        // Publishing a completed tick is now only a cheap head advance; all
        // corresponding note records were already copied/uploaded above.
        while (sharpHead_ < sharpRemotePreparedHead_ &&
               tickIsSafe(ring_[std::size_t(sharpHead_) & ringMask_].startTick)) {
            ++sharpHead_;
        }

        // Render closes are emitted in source tick order. Consume only the safe
        // chronological prefix instead of rescanning/copying the entire pending
        // list after every partial batch (the old path became O(n^2)).
        std::size_t closeWordsConsumed = 0;
        const std::size_t closeWord = sharpRemotePendingCloseOffset_;
        while (closeWord + closeWordsConsumed + 1 <
               sharpRemotePendingCloseWords_.size()) {
            const uint32_t id =
                sharpRemotePendingCloseWords_[closeWord + closeWordsConsumed];
            const uint32_t endTick =
                sharpRemotePendingCloseWords_[closeWord + closeWordsConsumed + 1];
            if (!tickIsSafe(endTick) || id >= sharpHead_)
                break;
            closeSharpNote(id, endTick, &closeIds);
            closeWordsConsumed += 2;
        }
        if (closeWordsConsumed != 0) {
            sharpRemotePendingCloseOffset_ += closeWordsConsumed;
            if (sharpRemotePendingCloseOffset_ ==
                sharpRemotePendingCloseWords_.size()) {
                sharpRemotePendingCloseWords_.clear();
                sharpRemotePendingCloseOffset_ = 0;
            } else if (sharpRemotePendingCloseOffset_ >= 65536u &&
                       sharpRemotePendingCloseOffset_ * 2u >=
                           sharpRemotePendingCloseWords_.size()) {
                sharpRemotePendingCloseWords_.erase(
                    sharpRemotePendingCloseWords_.begin(),
                    sharpRemotePendingCloseWords_.begin() +
                        static_cast<std::ptrdiff_t>(sharpRemotePendingCloseOffset_));
                sharpRemotePendingCloseOffset_ = 0;
            }
        }

        if (batch.complete) {
            sharpLastSweepEnd_ = static_cast<int64_t>(batch.safeThrough);
            sharpLastWindowTicks_ = sharpStableWindowTicks_;
            if (sharpRemoteRebuilding_ &&
                batch.safeThrough >= sharpRemoteBuildTarget_) {
                sharpRemoteSafeThrough_ = batch.safeThrough;
                sharpModePerTrack_ = sharpRemoteBuildPerTrack_;
                sharpRemoteRequestedEnd_ = sharpRemoteBuildTarget_;
                sharpRemoteRebuilding_ = false;
                                    }
        }
    }

    uploadSharpCloseIds(closeIds);
    if (processCount >= sharpRemoteBatches_.size()) {
        sharpRemoteBatches_.clear();
    } else {
        sharpRemoteBatches_.erase(
            sharpRemoteBatches_.begin(),
            sharpRemoteBatches_.begin() + static_cast<std::ptrdiff_t>(processCount));
    }
}

void GLRenderer::regenerateSharpPalette()
{
    // The piano keyboard and horizontal renderer share the exact same 16
    // configurable palette slots. Repeat those slots across the 256 texels so
    // legacy/high-byte note indices remain harmless while new Sharp batches
    // carry only the resolved 4-bit display color.
    for (uint32_t i = 0; i < 256u; ++i) {
        const auto& color = channelColors_[i & 0x0fu];
        sharpPaletteData_[i * 4u + 0u] = color[0];
        sharpPaletteData_[i * 4u + 1u] = color[1];
        sharpPaletteData_[i * 4u + 2u] = color[2];
        sharpPaletteData_[i * 4u + 3u] = color[3];
    }
    sharpPaletteUploadPending_ = true;
}

static void drawNoteInstances(GLsizei count)
{
    static const bool skip = wasmidi_roll_draw_disabled() != 0;
    if (!skip)
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);
}

uint32_t GLRenderer::drawSharpRingRange(uint32_t viewEnd, bool notesReady)
{
    // Future geometry may already be resident in the ring/VBO. Do not submit
    // those instances to the GPU until their StartTick reaches this viewport;
    // prefetch therefore buys CPU/transfer headroom without increasing the
    // visible frame's vertex workload.
    uint32_t drawEnd = sharpTail_;
    if (notesReady && sharpHead_ > sharpTail_) {
        uint32_t lo = sharpTail_;
        uint32_t hi = sharpHead_;
        while (lo < hi) {
            const uint32_t mid = lo + ((hi - lo) >> 1u);
            const VisualNote& note = ring_[std::size_t(mid) & ringMask_];
            if (note.startTick <= viewEnd)
                lo = mid + 1u;
            else
                hi = mid;
        }
        drawEnd = lo;
    }

    const uint32_t count = drawEnd - sharpTail_;
    if (count != 0 && ringCapacity_ != 0) {
        const std::size_t start = std::size_t(sharpTail_) & ringMask_;
        const std::size_t first = std::min<std::size_t>(
            count, ringCapacity_ - start);
        const std::size_t second = std::size_t(count) - first;

        glBindVertexArray(noteVao_);
        if (first != 0) {
            setInstanceBase(start);
            drawNoteInstances(static_cast<GLsizei>(first));
        }
        if (second != 0) {
            setInstanceBase(0);
            drawNoteInstances(static_cast<GLsizei>(second));
        }
    }
    return count;
}

void GLRenderer::drawSharpRing(
    uint32_t currentTick,
    uint32_t viewStart,
    uint32_t viewEnd,
    uint32_t windowTicks,
    bool notesReady)
{
    updateCullTiles(viewStart, viewEnd, windowTicks, notesReady);

    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    // BPFA layering (HANDOFF sec. 37): ring order is merged note-on order, so with
    // depth off a later start, or a later note at the same start, ends on top.
    glDisable(GL_DEPTH_TEST);

    glUseProgram(noteProgram_);
    glUniform3f(
        metricsUniform_,
        2.0f / float(std::max<uint32_t>(1u, windowTicks)),
        -1.0f,
        2.0f / 128.0f);
    glUniform1i(viewStartUniform_, static_cast<GLint>(viewStart));
    glUniform1i(viewEndUniform_, static_cast<GLint>(viewEnd));
    glUniform1i(currentTickUniform_, static_cast<GLint>(currentTick));
    glUniform1i(perTrackUniform_, sharpModePerTrack_ ? 1 : 0);
    glUniform1i(glowUniform_, 1);
    glUniform1i(transparencyUniform_, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sharpPaletteTexture_);
    if (sharpPaletteUploadPending_) {
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, sharpPaletteData_.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        sharpPaletteUploadPending_ = false;
    }
    glUniform1i(paletteUniform_, 0);

    bool drewTiles = false;
    double drawn = 0.0, raw = 0.0;
    if (!cullTiles_.empty() && windowTicks != 0 && width_ > 0) {
        const uint32_t firstTile = viewStart / windowTicks;
        const uint32_t lastTile = viewEnd / windowTicks;
        // A tile is drawn from its own buffer once it holds every note of its visible part.
        const auto usable = [&](const CullTile* tile) {
            return tile && tile->coverage >= std::min<uint64_t>(
                viewEnd, (uint64_t(tile->index) + 1u) * windowTicks);
        };
        bool anyUsable = false;
        for (uint32_t k = firstTile; k <= lastTile; ++k)
            if (usable(findCullTile(k))) anyUsable = true;
        if (anyUsable) {
            const double pixelsPerTick = double(width_) / double(windowTicks);
            glEnable(GL_SCISSOR_TEST);
            for (uint32_t k = firstTile; k <= lastTile; ++k) {
                const double tileStart = double(uint64_t(k) * windowTicks);
                // A pixel column belongs to the tile its centre falls in.
                const int px0 = std::clamp(static_cast<int>(std::ceil(
                    (tileStart - double(viewStart)) * pixelsPerTick - 0.5)), 0, width_);
                const int px1 = std::clamp(static_cast<int>(std::ceil(
                    (tileStart + double(windowTicks) - double(viewStart)) * pixelsPerTick - 0.5)), 0, width_);
                if (px1 <= px0) continue;
                glScissor(px0, 0, px1 - px0, height_);
                const CullTile* tile = findCullTile(k);
                if (usable(tile)) {
                    if (tile->count == 0) continue;
                    glBindVertexArray(cullVao_);
                    glBindBuffer(GL_ARRAY_BUFFER, tile->vbo);
                    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
                        reinterpret_cast<void*>(offsetof(VisualNote, startTick)));
                    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
                        reinterpret_cast<void*>(offsetof(VisualNote, endTick)));
                    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(VisualNote),
                        reinterpret_cast<void*>(offsetof(VisualNote, packedData)));
                    drawNoteInstances(static_cast<GLsizei>(tile->count));
                    drawn += double(tile->count);
                } else {
                    raw += double(drawSharpRingRange(viewEnd, notesReady));
                }
            }
            glDisable(GL_SCISSOR_TEST);
            drewTiles = true;
        }
    }
    if (!drewTiles)
        raw += double(drawSharpRingRange(viewEnd, notesReady));
    wasmidi_roll_debug_drawn(drawn + raw, raw, double(sharpHead_ - sharpTail_));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDisable(GL_DEPTH_TEST);

    // Exact SharpMIDI playhead behavior: it moves from the left edge to the
    // center during the first half-window and then remains centered.
    const float pixelsPerTick = 2.0f / float(std::max<uint32_t>(1u, windowTicks));
    const int lineX = std::clamp(
        static_cast<int>(std::min(
            float(currentTick) * pixelsPerTick * float(width_) * 0.5f,
            float(width_) * 0.5f)),
        0,
        std::max(0, width_ - 1));

    glEnable(GL_SCISSOR_TEST);
    glScissor(lineX, 0, 1, height_);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

void GLRenderer::calculateView(
    uint32_t& currentTick,
    uint32_t& viewStart,
    uint32_t& viewEnd) const
{
    if (!document_) {
        currentTick = viewStart = viewEnd = 0;
        return;
    }

    currentTick =
        static_cast<uint32_t>(
            std::clamp<double>(
                std::floor(
                    document_->
                        secondsToTick(
                            currentTime_)),
                0.0,
                document_->maxTick));

    const double rightSeconds =
        std::min<double>(
            document_->durationSeconds,
            double(currentTime_) +
            double(noteSpeed_));

    const uint32_t rightTick =
        static_cast<uint32_t>(
            std::clamp<double>(
                std::ceil(
                    document_->
                        secondsToTick(
                            rightSeconds)),
                currentTick + 1.0,
                double(
                    std::max<uint32_t>(
                        currentTick + 1,
                        document_->maxTick))));

    const double historySeconds =
        postBuffer_ > 0.0f
            ? double(postBuffer_)
            : double(noteSpeed_) *
              (0.18 / 0.82);

    const double leftSeconds =
        std::max(
            0.0,
            double(currentTime_) -
            historySeconds);

    viewStart =
        static_cast<uint32_t>(
            std::max(
                0.0,
                std::floor(
                    document_->
                        secondsToTick(
                            leftSeconds))));

    viewEnd =
        std::max(
            viewStart + 1,
            rightTick);
}
bool GLRenderer::renderRoll()
{
    if (!initialized_ && !initialize())
        return false;

    glViewport(0, 0, width_, height_);

    if (!document_) {
        renderBackground();
        rollFrameValid_ = false;
        return true;
    }

    uint32_t currentTick = 0;
    uint32_t viewStart = 0;
    uint32_t viewEnd = 0;
    uint32_t windowTicks = 1;
    calculateSharpView(currentTick, viewStart, viewEnd, windowTicks);

    uint32_t sweepEnd = std::min<uint32_t>(
        document_->maxTick,
        viewEnd > std::numeric_limits<uint32_t>::max() - sharpLookaheadTicks_
            ? document_->maxTick
            : viewEnd + sharpLookaheadTicks_);

    // Do not force an additional wall-clock horizon here.  The adaptive
    // screen reserve already grows with noteSpeed and density.  A hard 20 s
    // target made the mapped Worker permanently busy on dense files, starving
    // synth event admission and live keyboard/stat snapshots.
    // Quantize horizon growth to one viewport. This avoids posting a Worker
    // extension request every display frame while preserving the same amount
    // of prepared future geometry.
    if (windowTicks > 1u && sweepEnd < document_->maxTick) {
        const uint64_t rounded =
            ((uint64_t(sweepEnd) + windowTicks - 1u) / windowTicks) * windowTicks;
        sweepEnd = static_cast<uint32_t>(std::min<uint64_t>(
            rounded, document_->maxTick));
    }
    // A catch-up's build reaches past the normal horizon; until the horizon passes
    // it, that is still the horizon (a lower one would read as a backward seek).
    if (sharpCatchUpFloor_ != 0u) {
        if (currentTick < sharpCatchUpFrom_ || sweepEnd >= sharpCatchUpFloor_)
            sharpCatchUpFloor_ = 0u;
        else
            sweepEnd = sharpCatchUpFloor_;
    }

    // Keep at least two complete screens beyond the visible edge in the
    // Worker *and* applied to the VBO. This is the emergency watermark sent to
    // the parser Worker and the proactive drain target for already-completed
    // batches. The normal horizon remains much farther ahead.
    const uint64_t urgent64 = uint64_t(viewEnd) + uint64_t(windowTicks) * 2u;
    sharpRemoteUrgentThrough_ = static_cast<uint32_t>(std::min<uint64_t>(
        urgent64, document_->maxTick));

    // Drain completed background work after both the visible and future
    // endpoints are known. Do this *before* issuing another extension request
    // so already-generated frames are installed first instead of allowing a
    // JS->C++ upload backlog to grow invisibly until the playhead catches it.
    if (document_->remoteIndexed) {
        const uint64_t desired64 = uint64_t(viewEnd) + uint64_t(windowTicks) * 4u;
        const uint32_t desiredThrough = static_cast<uint32_t>(std::min<uint64_t>(
            desired64, sweepEnd));
        flushRemoteSharpBatches(viewEnd, desiredThrough);
    }

    const bool activeCompatible =
        !sharpForceReset_ &&
        sharpLastSweepEnd_ >= 0 &&
        sharpLastWindowTicks_ == windowTicks &&
        sharpModePerTrack_ == perTrackColors_;
    const bool forwardIncremental =
        activeCompatible &&
        sweepEnd >= static_cast<uint32_t>(sharpLastSweepEnd_);

    if (document_->remoteIndexed) {
        const bool buildCompatible =
            sharpRemoteRebuilding_ &&
            sharpRemoteBuildWindowTicks_ == windowTicks &&
            sharpRemoteBuildPerTrack_ == perTrackColors_;

        bool needRebuild = !forwardIncremental;
        if (sharpRemoteRebuilding_ && buildCompatible && !sharpForceReset_) {
            // The in-flight target, not the partial worker safe point, defines
            // whether normal playback is still the same transaction. Dense
            // same-tick groups can leave safeThrough unchanged for many yields;
            // restarting on that value was an accidental endless rebuild loop.
            const uint32_t distance = sweepEnd > sharpRemoteBuildTarget_
                ? sweepEnd - sharpRemoteBuildTarget_
                : sharpRemoteBuildTarget_ - sweepEnd;
            if (distance <= windowTicks) {
                needRebuild = false;
                if (sweepEnd > sharpRemoteBuildTarget_ &&
                    sweepEnd - sharpRemoteBuildTarget_ >= windowTicks &&
                    sharpRemoteBatches_.size() < 8u) {
                    // Do not let the Worker run arbitrarily farther ahead while
                    // completed geometry is already waiting to be installed in
                    // WebGL.  Backpressure keeps CPU/memory bandwidth available
                    // for SnappySynth and live-state work without shrinking the
                    // actual prepared-VBO watermark.
                    sharpRemoteBuildTarget_ = sweepEnd;
                    sharpRemoteRequestedEnd_ = sweepEnd;
                    requestRemoteSharpSweep(sharpRemoteStartTick_, sweepEnd, false);
                }
            }
        }

        // Catch up (HANDOFF sec. 80, 81): when everything built so far is already
        // left of the screen, or no complete frame came for a second of playback,
        // restart ahead of the playhead instead of finishing it. The jump grows
        // while the notes keep arriving late.
        bool catchUp = false;
        if (!needRebuild && !sharpRemoteWaitingReset_) {
            uint32_t builtThrough = sharpRemoteSafeThrough_;
            for (const SharpRemoteBatch& batch : sharpRemoteBatches_)
                if (batch.generation == sharpRemoteGeneration_)
                    builtThrough = std::max(builtThrough, batch.safeThrough);
            if (sharpRemoteRebuilding_)
                builtThrough = std::max(builtThrough, sharpRemoteStartTick_);
            const auto now = std::chrono::steady_clock::now();
            const bool heldTooLong = sharpHeldSince_ != std::chrono::steady_clock::time_point{} &&
                now - sharpHeldSince_ >= std::chrono::seconds(1) &&
                uint64_t(currentTick) >= uint64_t(sharpHeldTick_) + windowTicks / 4u;
            if ((builtThrough < viewStart || heldTooLong) &&
                now - sharpCatchUpAt_ >= std::chrono::seconds(1)) {
                sharpCatchUpAt_ = now;
                sharpHeldTick_ = currentTick;
                sharpCatchUpLead_ = sharpCatchUpLead_ == 0u ? 1u : std::min(sharpCatchUpLead_ * 2u, 8u);
                needRebuild = true;
                catchUp = true;
                wasmidi_roll_debug_count(1);
            }
        }

        if (needRebuild) {
            ++sharpRemoteGeneration_;
            if (sharpRemoteGeneration_ == 0)
                ++sharpRemoteGeneration_;
            sharpRemoteBatches_.clear();
            sharpRemotePendingAppends_.clear();
            sharpRemotePendingCloseWords_.clear();
            sharpRemotePendingCloseOffset_ = 0;
            sharpRemotePendingBase_ = 0;
            sharpRemotePreparedHead_ = 1;

            sharpRemoteRebuilding_ = true;
            sharpRemoteSafeThrough_ = 0u;
                    // Drop the stale generation immediately. The background/playhead
            // keeps animating while complete ticks from the replacement arrive.
            sharpHead_ = 1u;
            sharpTail_ = 1u;
            ++sharpRingEpoch_;
            sharpRemotePreparedHead_ = 1u;
            sharpRemotePendingBase_ = 1u;
                    sharpRemoteBuildTarget_ = sweepEnd;
                    sharpRemoteBuildWindowTicks_ = windowTicks;
            sharpRemoteBuildPerTrack_ = perTrackColors_;
            const uint64_t restart = uint64_t(viewStart) +
                (catchUp ? uint64_t(sharpCatchUpLead_) * windowTicks : 0u);
            sharpRemoteStartTick_ = static_cast<uint32_t>(std::min<uint64_t>(
                restart > windowTicks ? restart - windowTicks : 0u, document_->maxTick));
            if (catchUp) {
                sweepEnd = static_cast<uint32_t>(std::min<uint64_t>(document_->maxTick,
                    std::max<uint64_t>(sweepEnd, uint64_t(sharpRemoteStartTick_) + uint64_t(windowTicks) * 3u)));
                sharpRemoteBuildTarget_ = sweepEnd;
                sharpCatchUpFloor_ = sweepEnd;
                sharpCatchUpFrom_ = currentTick;
            }
            sharpRemoteRequestedEnd_ = sweepEnd;
            sharpRemoteWaitingReset_ = true;
            sharpForceReset_ = false;
            requestRemoteSharpSweep(sharpRemoteStartTick_, sweepEnd, true);
        } else if (!sharpRemoteRebuilding_ &&
                   sweepEnd > sharpRemoteRequestedEnd_ &&
                   sweepEnd - sharpRemoteRequestedEnd_ >= windowTicks &&
                   sharpRemoteBatches_.size() < 8u) {
            sharpRemoteRequestedEnd_ = sweepEnd;
            requestRemoteSharpSweep(sharpRemoteStartTick_, sweepEnd, false);
        }
    } else {
        sharpModePerTrack_ = perTrackColors_;
        if (!forwardIncremental) {
            resetSharpRenderer(false);
            sharpStableWindowTicks_ = windowTicks;
            sharpStableNoteSpeed_ = noteSpeed_;
            sharpModePerTrack_ = perTrackColors_;
            const uint32_t resetStart = viewStart > windowTicks
                ? viewStart - windowTicks
                : 0u;
            sweepLocalSharpRange(resetStart, sweepEnd);
            sharpLastSweepEnd_ = sweepEnd;
            sharpLastWindowTicks_ = windowTicks;
            sharpForceReset_ = false;
        } else if (sweepEnd > static_cast<uint32_t>(sharpLastSweepEnd_)) {
            sweepLocalSharpRange(
                static_cast<uint32_t>(sharpLastSweepEnd_) + 1u,
                sweepEnd);
            sharpLastSweepEnd_ = sweepEnd;
        }
    }

    // Never age/cull the old ring against a seek destination while a remote
    // replacement is still being built. That was the direct cause of notes
    // disappearing before their EndTick and of the all-black seek state.
    if (!document_->remoteIndexed || !sharpRemoteRebuilding_)
        advanceSharpTail(viewStart);

    // Never expose a half-generated visible window. This is the contingency
    // path only; the primary strategy is the multi-screen prefetch above.
    const bool notesReady = !document_->remoteIndexed ||
        (!sharpRemoteWaitingReset_ && sharpRemoteSafeThrough_ >= viewEnd &&
         sharpRemoteStartTick_ <= viewStart);
    const auto frameNow = std::chrono::steady_clock::now();
    if (notesReady) {
        sharpHeldSince_ = {};
        if (sharpReadySince_ == std::chrono::steady_clock::time_point{})
            sharpReadySince_ = frameNow;
        else if (frameNow - sharpReadySince_ >= std::chrono::seconds(5))
            sharpCatchUpLead_ = 0u;
    } else {
        sharpReadySince_ = {};
        // Waiting for the playhead to reach a catch-up's jump is not running late.
        const bool waitingForPlayhead = document_->remoteIndexed && sharpRemoteStartTick_ > viewStart;
        if (waitingForPlayhead || sharpHeldSince_ == std::chrono::steady_clock::time_point{}) {
            sharpHeldSince_ = frameNow;
            sharpHeldTick_ = currentTick;
        }
    }

    // Keep the last complete frame in the FBO until the notes for this one
    // arrive (HANDOFF sec. 80).
    if (!notesReady && rollFrameValid_ &&
        rollFrameWidth_ == width_ && rollFrameHeight_ == height_) {
        wasmidi_roll_debug_count(0);
        return false;
    }

    renderBackground();
    drawSharpRing(currentTick, viewStart, viewEnd, windowTicks, notesReady);
    rollFrameValid_ = notesReady;
    rollFrameWidth_ = width_;
    rollFrameHeight_ = height_;

    return true;
}



const GLRenderer::CullTile* GLRenderer::findCullTile(uint32_t index) const
{
    for (const CullTile& tile : cullTiles_)
        if (tile.index == index) return &tile;
    return nullptr;
}

GLRenderer::CullTile* GLRenderer::findCullTileMutable(uint32_t index)
{
    for (CullTile& tile : cullTiles_)
        if (tile.index == index) return &tile;
    return nullptr;
}

void GLRenderer::releaseCullTiles()
{
    for (CullTile& tile : cullTiles_)
        if (tile.vbo) glDeleteBuffers(1, &tile.vbo);
    cullTiles_.clear();
    cullJob_.active = false;
}

void GLRenderer::updateCullTiles(uint32_t viewStart, uint32_t viewEnd, uint32_t windowTicks, bool notesReady)
{
    static const bool tilesDisabled = wasmidi_tiles_disabled() != 0;
    if (tilesDisabled || !document_ || !document_->remoteIndexed || windowTicks == 0 || width_ <= 0 ||
        ringCapacity_ == 0) {
        releaseCullTiles();
        return;
    }
    uint64_t signature = 1469598103934665603ull;
    const auto mix = [&signature](uint64_t value) {
        signature ^= value;
        signature *= 1099511628211ull;
    };
    mix(windowTicks);
    mix(uint64_t(width_));
    mix(sharpModePerTrack_ ? 1u : 0u);
    mix(sharpRingEpoch_);
    mix(uint64_t(reinterpret_cast<uintptr_t>(document_)));
    if (signature != cullSignature_) {
        releaseCullTiles();
        cullSignature_ = signature;
    }

    const uint32_t firstTile = viewStart / windowTicks;
    const uint32_t lastTile = viewEnd / windowTicks;
    for (std::size_t i = 0; i < cullTiles_.size();) {
        if (cullTiles_[i].index < firstTile || cullTiles_[i].index >= firstTile + 64u) {
            if (cullTiles_[i].vbo) glDeleteBuffers(1, &cullTiles_[i].vbo);
            cullTiles_[i] = cullTiles_.back();
            cullTiles_.pop_back();
        } else {
            ++i;
        }
    }
    if (cullJob_.active && (cullJob_.index < firstTile || cullJob_.index >= firstTile + 64u))
        cullJob_.active = false;

    // The visible tiles are brought up to date now, even before the notes for their
    // whole span have arrived: drawing them raw costs far more (HANDOFF sec. 81). A
    // slice-built tile that already covers enough is finished instead (sec. 84).
    if (notesReady) {
        for (uint32_t k = firstTile; k <= lastTile && k < firstTile + 64u; ++k) {
            const uint64_t tileEnd = (uint64_t(k) + 1u) * windowTicks;
            const uint64_t needed = std::min<uint64_t>(viewEnd, tileEnd);
            const CullTile* tile = findCullTile(k);
            if (tile && tile->coverage >= needed) continue;
            if (sharpRemoteSafeThrough_ < needed) continue;
            if (cullJob_.active && cullJob_.index == k && cullJob_.coverage >= needed) {
                const auto started = std::chrono::steady_clock::now();
                stepCullJob(1.0e9);
                wasmidi_roll_debug_tile(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count(), 2);
                continue;
            }
            if (cullJob_.active && cullJob_.index == k)
                cullJob_.active = false;
            const auto started = std::chrono::steady_clock::now();
            buildCullTile(k, windowTicks);
            wasmidi_roll_debug_tile(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count(), 2);
        }
    }

    // Everything else is built a slice per frame: a dense tile is a million notes, tens
    // of milliseconds in one piece.
    if (!cullJob_.active) {
        for (uint32_t k = firstTile; k < firstTile + 64u; ++k) {
            const uint64_t tileStart = uint64_t(k) * windowTicks;
            const uint64_t tileEnd = tileStart + windowTicks;
            const uint64_t target = std::min<uint64_t>(tileEnd, sharpRemoteSafeThrough_);
            if (target <= tileStart) break;
            const CullTile* tile = findCullTile(k);
            if (tile && tile->coverage >= target) continue;
            // A tile still filling is built only if it may come on screen before it is
            // complete, and redone once enough new notes arrived for it.
            if (target < tileEnd && k > lastTile + 1u) break;
            if (target < tileEnd && tile && target - tile->coverage < windowTicks / 16u) break;
            startCullJob(k, windowTicks, static_cast<uint32_t>(target));
            break;
        }
    }
    if (cullJob_.active) {
        const auto started = std::chrono::steady_clock::now();
        static const bool sliceDisabled = wasmidi_tile_slice_disabled() != 0;
        const bool visible = cullJob_.index <= lastTile;
        const bool finished = stepCullJob(sliceDisabled ? 1.0e9 : (visible ? 6.0 : 3.0));
        wasmidi_roll_debug_tile(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count(), finished ? 1 : 0);
    }
}

void GLRenderer::uploadCullTile(uint32_t index, uint32_t coverage, const CompositorNote* notes, std::size_t count)
{
    CullTile* existing = findCullTileMutable(index);
    if (!existing) {
        cullTiles_.push_back(CullTile{});
        existing = &cullTiles_.back();
        existing->index = index;
    }
    CullTile& tile = *existing;
    tile.coverage = coverage;
    tile.count = static_cast<uint32_t>(count);
    if (count == 0) return;
    if (!tile.vbo) glGenBuffers(1, &tile.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, tile.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count * sizeof(CompositorNote)),
        notes, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLRenderer::buildCullTile(uint32_t index, uint32_t windowTicks)
{
    constexpr std::size_t kCullMinNotes = 50000;
    const uint32_t tileStart = index * windowTicks;
    const uint32_t tileEnd = tileStart + windowTicks;
    // Every note that starts, or closes, before the coverage tick is in the ring.
    const uint32_t coverage = std::min<uint32_t>(tileEnd, sharpRemoteSafeThrough_);

    cullInput_.clear();
    const uint64_t collectEnd = coverage == tileEnd ? uint64_t(tileEnd) : uint64_t(coverage) + 1u;
    const uint32_t head = std::max(sharpHead_, sharpRemotePreparedHead_);
    for (uint32_t id = sharpTail_; id < head; ++id) {
        const VisualNote& note = ring_[std::size_t(id) & ringMask_];
        if (note.startTick >= collectEnd) break;
        if (note.endTick != 0 && note.endTick <= tileStart) continue;
        cullInput_.push_back({ note.startTick, note.endTick, note.packedData });
    }
    if (cullInput_.size() < kCullMinNotes) {
        uploadCullTile(index, coverage, cullInput_.data(), cullInput_.size());
        return;
    }

    CompositorSettings settings;
    settings.startTick = tileStart;
    settings.spanTicks = windowTicks;
    settings.rasterWidth = width_;
    settings.firstKey = 0;
    settings.keyCount = 128;
    settings.viewEndTick = coverage;
    CullViewport(cullInput_.data(), cullInput_.size(), settings, cullScratch_, cullResult_);
    uploadCullTile(index, coverage, cullResult_.notes.data(), cullResult_.notes.size());
}

void GLRenderer::startCullJob(uint32_t index, uint32_t windowTicks, uint32_t coverage)
{
    cullJob_.active = true;
    cullJob_.collecting = true;
    cullJob_.index = index;
    cullJob_.coverage = coverage;
    cullJob_.windowTicks = windowTicks;
    cullJob_.nextId = sharpTail_;
    cullJob_.input.clear();
}

bool GLRenderer::stepCullJob(double budgetMs)
{
    constexpr std::size_t kCullMinNotes = 50000;
    constexpr uint32_t kCollectSlice = 16384;
    constexpr std::size_t kCullSlice = 4096;
    CullJob& job = cullJob_;
    const auto started = std::chrono::steady_clock::now();
    const auto spent = [&started]() {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    };
    const uint32_t tileStart = job.index * job.windowTicks;
    const uint32_t tileEnd = tileStart + job.windowTicks;

    if (job.collecting) {
        // The notes before the coverage tick no longer change, but the tail may pass
        // the ones that ended left of the screen (their slots can be reused).
        const uint64_t collectEnd = job.coverage == tileEnd ? uint64_t(tileEnd) : uint64_t(job.coverage) + 1u;
        for (;;) {
            const uint32_t head = std::max(sharpHead_, sharpRemotePreparedHead_);
            uint32_t id = std::max(job.nextId, sharpTail_);
            const uint32_t sliceEnd = head - id > kCollectSlice ? id + kCollectSlice : head;
            bool reached = sliceEnd == head;
            for (; id < sliceEnd; ++id) {
                const VisualNote& note = ring_[std::size_t(id) & ringMask_];
                if (note.startTick >= collectEnd) { reached = true; break; }
                if (note.endTick != 0 && note.endTick <= tileStart) continue;
                job.input.push_back({ note.startTick, note.endTick, note.packedData });
            }
            job.nextId = id;
            if (reached) break;
            if (spent() >= budgetMs) return false;
        }
        job.collecting = false;
        if (job.input.size() < kCullMinNotes) {
            job.active = false;
            const CullTile* tile = findCullTile(job.index);
            if (!tile || tile->coverage < job.coverage)
                uploadCullTile(job.index, job.coverage, job.input.data(), job.input.size());
            return true;
        }
        CompositorSettings settings;
        settings.startTick = tileStart;
        settings.spanTicks = job.windowTicks;
        settings.rasterWidth = width_;
        settings.firstKey = 0;
        settings.keyCount = 128;
        settings.viewEndTick = job.coverage;
        CullBegin(job.input.data(), job.input.size(), settings, job.scratch, job.progress);
        if (spent() >= budgetMs) return false;
    }

    while (!CullStep(job.scratch, job.progress, kCullSlice))
        if (spent() >= budgetMs) return false;
    CullFinish(job.progress, job.result);
    job.active = false;
    const CullTile* tile = findCullTile(job.index);
    if (!tile || tile->coverage < job.coverage)
        uploadCullTile(job.index, job.coverage, job.result.notes.data(), job.result.notes.size());
    return true;
}


} // namespace wasmidi
