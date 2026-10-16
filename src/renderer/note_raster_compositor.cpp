#include "note_raster_compositor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wasmidi {
namespace {

static_assert(sizeof(CompositorNote) == 12,
    "CompositorNote must match VisualNote in src/midi/midi_parser.hpp");

inline int PopCount64(std::uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(value);
#else
    value = value - ((value >> 1) & 0x5555555555555555ull);
    value = (value & 0x3333333333333333ull) +
            ((value >> 2) & 0x3333333333333333ull);
    value = (value + (value >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return static_cast<int>((value * 0x0101010101010101ull) >> 56);
#endif
}

}  // namespace

void CullViewport(
    const CompositorNote* notes,
    std::size_t noteCount,
    const CompositorSettings& settings,
    CompositorScratch& scratch,
    CompositorResult& out) {

    out.notes.clear();
    out.visitedNotes = noteCount;
    out.culledNotes = 0;
    out.rejectedByFullRow = 0;

    const int width = (std::max)(1, settings.rasterWidth);
    const int keyCount = (std::max)(0, settings.keyCount);
    if (keyCount == 0 || notes == nullptr || noteCount == 0)
        return;

    // One occupancy bit per cell. For 128 keys at 1920 columns this is 30 KB
    // and stays in cache, where the previous owner-per-cell array was 3.9 MB.
    const std::size_t wordsPerRow = (static_cast<std::size_t>(width) + 63u) / 64u;
    scratch.occupancy.assign(
        wordsPerRow * static_cast<std::size_t>(keyCount), 0ull);
    scratch.uncovered.assign(static_cast<std::size_t>(keyCount),
        static_cast<std::uint32_t>(width));
    scratch.partial.assign(wordsPerRow * static_cast<std::size_t>(keyCount), 0ull);
    // Only read where `partial` has the bit, so it is never cleared.
    scratch.partialRange.resize(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(keyCount) * 2u);

    const std::int64_t viewStart = static_cast<std::int64_t>(settings.startTick);
    const std::int64_t viewEnd = settings.viewEndTick != 0
        ? static_cast<std::int64_t>(settings.viewEndTick)
        : viewStart + static_cast<std::int64_t>(settings.spanTicks);
    const double span =
        static_cast<double>((std::max<std::uint32_t>)(1u, settings.spanTicks));

    std::vector<std::uint8_t> keep(noteCount, 0u);

    // Reverse index order is front-to-back under BPFA's layering, since the
    // array is sorted by startTick. No sort, and the first writer of a cell is
    // final.
    for (std::size_t step = 0; step < noteCount; ++step) {
        const std::size_t index = noteCount - 1u - step;
        const CompositorNote& note = notes[index];

        const int pitch = static_cast<int>((note.packedData >> 8) & 0xffu);
        const int row = pitch - settings.firstKey;
        if (row < 0 || row >= keyCount)
            continue;

        // Whole row already covered: reject without touching a cell.
        std::uint32_t& uncovered = scratch.uncovered[static_cast<std::size_t>(row)];
        if (uncovered == 0u) {
            ++out.rejectedByFullRow;
            continue;
        }

        const std::int64_t noteStart = static_cast<std::int64_t>(note.startTick);
        const std::int64_t noteEnd = note.endTick > 0u
            ? static_cast<std::int64_t>(note.endTick)
            : viewEnd;
        if (noteEnd <= viewStart || noteStart >= viewEnd)
            continue;

        // Tiles are culled on their grid but drawn shifted by any fraction of a pixel
        // (HANDOFF sec. 73): a column is hidden only when the notes in front cover all
        // of its ticks. Columns a note covers partially keep the covered tick range.
        const std::int64_t clipStart = (std::max)(noteStart, viewStart);
        const std::int64_t clipEnd = (std::min)(noteEnd, viewEnd);
        if (clipEnd <= clipStart)
            continue;
        const double x0 = static_cast<double>((clipStart - viewStart) * width) / span;
        const double x1 = static_cast<double>((clipEnd - viewStart) * width) / span;
        const int touchLeft = (std::max)(0, static_cast<int>(std::floor(x0)));
        const int touchRight = (std::min)(width, static_cast<int>(std::ceil(x1)));
        const int fullLeft = (std::min)(width, static_cast<int>(std::ceil(x0)));
        const int fullRight = (std::max)(fullLeft,
            (std::min)(width, static_cast<int>(std::floor(x1))));
        if (touchRight <= touchLeft)
            continue;

        int edges[2];
        int edgeCount = 0;
        if (touchLeft < fullLeft)
            edges[edgeCount++] = touchLeft;
        if (fullRight < touchRight && (edgeCount == 0 || edges[0] != fullRight))
            edges[edgeCount++] = fullRight;

        const std::size_t rowBase = static_cast<std::size_t>(row) * wordsPerRow;
        std::uint64_t* rowWords = scratch.occupancy.data() + rowBase;
        std::uint64_t* rowPartial = scratch.partial.data() + rowBase;
        std::uint32_t* rowRanges = scratch.partialRange.data() +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 2u;
        auto rangeMask = [](int word, int left, int right) {
            const int lo = (word == (left >> 6)) ? (left & 63) : 0;
            const int hi = (word == ((right - 1) >> 6)) ? ((right - 1) & 63) : 63;
            return (hi - lo) == 63 ? ~0ull : (((1ull << (hi - lo + 1)) - 1ull) << lo);
        };
        auto bit = [](const std::uint64_t* words, int column) {
            return (words[column >> 6] >> (column & 63)) & 1ull;
        };
        // Covered ticks of a partial column; 0 and ~0u stand for its two borders.
        auto edgeRange = [&](int column, std::uint32_t& lo, std::uint32_t& hi) {
            lo = x0 <= static_cast<double>(column) ? 0u
                                                   : static_cast<std::uint32_t>(clipStart);
            hi = x1 >= static_cast<double>(column + 1) ? ~0u
                                                       : static_cast<std::uint32_t>(clipEnd);
        };

        bool visible = false;
        for (int word = fullLeft >> 6; fullRight > fullLeft &&
             word <= ((fullRight - 1) >> 6) && !visible; ++word)
            visible = (~rowWords[word] & rangeMask(word, fullLeft, fullRight)) != 0;
        for (int e = 0; e < edgeCount && !visible; ++e) {
            const int column = edges[e];
            if (bit(rowWords, column))
                continue;
            std::uint32_t lo, hi;
            edgeRange(column, lo, hi);
            const std::uint32_t* range = rowRanges + static_cast<std::size_t>(column) * 2u;
            visible = !bit(rowPartial, column) || lo < range[0] || hi > range[1];
        }
        if (!visible)
            continue;

        int claimed = 0;
        for (int word = fullLeft >> 6; fullRight > fullLeft &&
             word <= ((fullRight - 1) >> 6); ++word) {
            const std::uint64_t mask = rangeMask(word, fullLeft, fullRight);
            claimed += PopCount64(~rowWords[word] & mask);
            rowWords[word] |= mask;
        }
        for (int e = 0; e < edgeCount; ++e) {
            const int column = edges[e];
            if (bit(rowWords, column))
                continue;
            std::uint32_t lo, hi;
            edgeRange(column, lo, hi);
            std::uint32_t* range = rowRanges + static_cast<std::size_t>(column) * 2u;
            const std::uint64_t columnBit = 1ull << (column & 63);
            if (!(rowPartial[column >> 6] & columnBit)) {
                rowPartial[column >> 6] |= columnBit;
                range[0] = lo;
                range[1] = hi;
            } else if (lo <= range[1] && range[0] <= hi) {
                range[0] = (std::min)(range[0], lo);
                range[1] = (std::max)(range[1], hi);
            } else if (static_cast<std::uint64_t>(hi) - lo >
                       static_cast<std::uint64_t>(range[1]) - range[0]) {
                range[0] = lo;
                range[1] = hi;
            }
            if (range[0] == 0u && range[1] == ~0u) {
                rowWords[column >> 6] |= columnBit;
                ++claimed;
            }
        }

        keep[index] = 1u;
        uncovered -= static_cast<std::uint32_t>(claimed);
    }

    out.notes.reserve(noteCount / 4u + 16u);
    for (std::size_t index = 0; index < noteCount; ++index)
        if (keep[index])
            out.notes.push_back(notes[index]);
    out.culledNotes = noteCount - out.notes.size();
}

}  // namespace wasmidi
