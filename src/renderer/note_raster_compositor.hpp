#pragma once

// Viewport occlusion culling for the horizontal piano roll, derived from BPFA's
// NoteMeshCache compositor (linux/NoteMeshCache.cpp).
//
// The piano roll submits one GPU instance per visible note, so cost scales with
// how many notes are inside the viewport. A crashpoint puts hundreds of
// thousands of notes on screen, nearly all of them narrower than a pixel and
// completely hidden behind others.
//
// BPFA composes the viewport into a raster so its work is bounded by visible
// keys times pixels rather than by note count. This takes that idea but stops
// short of BPFA's geometry emission, deliberately: BPFA bakes borders, velocity
// shading and sharp/natural key widths into vertices, while WASMIDI's shader
// draws a flat palette colour with no border, applies opacity of
// (velocity + 1) / 128, and brightens the sounding note. Emitting BPFA's
// geometry would change how WASMIDI looks.
//
// So the output is notes, not triangles: the same VisualNote layout, the same
// instanced draw, the same shader, palette, glow and opacity, with only the
// hidden notes removed.
//
// Since HANDOFF sec. 102 the renderer draws its tiles from RasterTile (below), which
// keeps per pixel only the note on top; the cull stays for the host checks.
//
// Free of Qt, GL and engine headers so it can be tested on the host.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wasmidi {

// Mirrors VisualNote in src/midi/midi_parser.hpp.
struct CompositorNote {
    std::uint32_t startTick = 0;
    std::uint32_t endTick = 0;
    std::uint32_t packedData = 0;
};

// Layering follows BPFA's rule, from NoteMeshCache's Composer::Add: the note
// that starts later is on top, and on an equal start the later source order
// wins. WASMIDI's own rule was the opposite (the shader writes
// z = (endTick - startTick) / 16777216 under GL_LESS, so a shorter note won),
// and it was dropped on request. BPFA's rule is also the faster one: reverse
// iteration is already front-to-back under it, so the first writer is final and
// a cell needs nothing but an occupied bit, with no duration sort.
//
// The renderer must draw the surviving notes in the same order for this to
// hold, so depth testing has to be off or z made constant for the note pass.

// Which nibble of VisualNote::packedData holds the palette slot.
//
// BPFA selects colour as tracks->channelColors[track * 16 + channel], falling
// back to primary[channel & 15]. WASMIDI makes the same distinction when it
// packs the note stream rather than in the shader: bits 16..19 carry the
// global/channel slot and bits 20..23 the per-track slot, and gl_renderer.cpp
// repacks the stream when perTrackColors_ changes. The shader's uPerTrack
// uniform is vestigial; it is set but never read, so the choice below only
// needs to agree with whichever stream the renderer built.
enum class ColorSource {
    GlobalChannel,  // bits 16..19, BPFA's primary[channel & 15]
    PerTrack        // bits 20..23, BPFA's channelColors[track * 16 + channel]
};

inline std::uint32_t SelectColorSlot(std::uint32_t packedData,
                                     ColorSource source) {
    return source == ColorSource::PerTrack
        ? ((packedData >> 20) & 0x0fu)
        : ((packedData >> 16) & 0x0fu);
}

struct CompositorSettings {
    std::uint32_t startTick = 0;
    std::uint32_t spanTicks = 1;
    // Raster resolution along the time axis. Matching the framebuffer width
    // makes a cell exactly one pixel column, the point at which discarding a
    // note cannot change a rendered pixel.
    int rasterWidth = 1920;
    int firstKey = 0;
    int keyCount = 128;
    // An open note (endTick == 0) is drawn to the right edge of the viewport,
    // matching the shader's `aEndTick > 0u ? aEndTick : uViewEnd`.
    std::uint32_t viewEndTick = 0;
};

struct CompositorResult {
    // Survivors, still sorted by startTick ascending, so the caller submits
    // them in ring order exactly as before.
    std::vector<CompositorNote> notes;
    std::uint64_t visitedNotes = 0;
    std::uint64_t culledNotes = 0;
    // Notes rejected by the O(1) full-row test rather than by scanning cells.
    std::uint64_t rejectedByFullRow = 0;
};

// Scratch buffers, reused across viewports so a per-frame or per-tile cull does
// not reallocate. Safe to keep one per worker thread.
struct CompositorScratch {
    std::vector<std::uint64_t> occupancy;
    std::vector<std::uint32_t> uncovered;
    std::vector<std::uint64_t> partial;
    std::vector<std::uint32_t> partialRange;
};

// Culls notes that cannot contribute a pixel. `notes` must be sorted by
// startTick ascending, which is how the parser emits them.
void CullViewport(
    const CompositorNote* notes,
    std::size_t noteCount,
    const CompositorSettings& settings,
    CompositorScratch& scratch,
    CompositorResult& out);

// The same cull split into slices, so a caller can spread one viewport over several
// frames. `notes` and `scratch` must stay untouched until CullFinish.
struct CompositorProgress {
    const CompositorNote* notes = nullptr;
    std::size_t noteCount = 0;
    std::size_t done = 0;
    CompositorSettings settings;
    std::vector<std::uint8_t> keep;
    std::uint64_t rejectedByFullRow = 0;
};
void CullBegin(
    const CompositorNote* notes,
    std::size_t noteCount,
    const CompositorSettings& settings,
    CompositorScratch& scratch,
    CompositorProgress& progress);
// Visits up to `maxNotes` more notes; true once every note was visited.
bool CullStep(CompositorScratch& scratch, CompositorProgress& progress, std::size_t maxNotes);
void CullFinish(CompositorProgress& progress, CompositorResult& out);

// Visible-only tile (HANDOFF sec. 102): every cell keeps the one note the GPU would
// show at that pixel centre, so a tile is drawn as one quad, each pixel once. Unlike
// the cull above it assumes the tile is drawn on whole pixels.
struct RasterTile {
    int width = 0;
    // keyCount rows of `width` cells: index into `table` plus one, 0 where empty.
    std::vector<std::uint32_t> cells;
    std::vector<CompositorNote> table;
    std::uint64_t rejectedByFullRow = 0;
};
void RasterBegin(const CompositorSettings& settings, CompositorScratch& scratch, RasterTile& out);
// Adds `count` 12-byte notes laid out like CompositorNote, from the last one to the
// first: front to back for an array in ring order.
void RasterAddReversed(const void* notes, std::size_t count, const CompositorSettings& settings,
    CompositorScratch& scratch, RasterTile& out);

}  // namespace wasmidi
