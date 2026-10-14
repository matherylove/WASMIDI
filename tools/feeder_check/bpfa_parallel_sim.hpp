// Host simulation of the parallel load (HANDOFF sec. 60): the same partition, partial
// indexes and in-order merge the browser does with Web Workers, run one after another.
#pragma once
#include "bpfa_midi_store.hpp"
#include <cstdlib>
#include <cstring>
#include <vector>

namespace wasmidi {

inline bool bpfaParallelIndex(BpfaMidiStore& store, uint64_t size, MidiReadAt readAt, void* user,
                              MidiDocument& meta, unsigned helpers, bool compressed)
{
    uint8_t h[14];
    if (!readAt(user, 0, h, 14)) return false;
    const uint16_t format = uint16_t((h[8] << 8) | h[9]);
    const uint16_t tracks = uint16_t((h[10] << 8) | h[11]);
    const uint16_t ppq = uint16_t((h[12] << 8) | h[13]);
    const uint32_t headerSize = (uint32_t(h[4]) << 24) | (uint32_t(h[5]) << 16) | (uint32_t(h[6]) << 8) | h[7];
    std::vector<uint64_t> chunk(tracks + 1);
    uint64_t pos = 8ull + headerSize;
    for (uint16_t t = 0; t < tracks; ++t) {
        uint8_t c[8];
        if (!readAt(user, pos, c, 8)) return false;
        chunk[t] = pos;
        pos += 8ull + ((uint32_t(c[4]) << 24) | (uint32_t(c[5]) << 16) | (uint32_t(c[6]) << 8) | c[7]);
    }
    chunk[tracks] = size;
    const uint32_t block = BpfaMidiStore::blockSizeForTracks(tracks);
    std::vector<uint16_t> cut{0};
    for (unsigned k = 1; k < helpers; ++k) {
        const uint64_t target = size * k / helpers;
        uint16_t t = cut.back();
        while (t < tracks && chunk[t] < target) ++t;
        if (t > cut.back() && t < tracks) cut.push_back(t);
    }
    cut.push_back(tracks);
    if (!store.beginMerge(size, format, tracks, ppq, block, compressed)) return false;
    if (!compressed && !store.mergeReadRaw(readAt, user)) return false;
    for (std::size_t k = 0; k + 1 < cut.size(); ++k) {
        BpfaMidiStore helper;
        BpfaMidiStore::PartialPlan plan;
        plan.format = format;
        plan.trackCount = tracks;
        plan.ticksPerBeat = ppq;
        plan.firstTrack = cut[k];
        plan.endTrack = cut[k + 1];
        plan.firstChunkPosition = chunk[cut[k]];
        plan.rangeBegin = k == 0 ? 0 : chunk[cut[k]];
        plan.rangeEnd = chunk[cut[k + 1]];
        plan.blockSize = block;
        plan.compress = compressed;
        plan.snapshotBudget = (16ull << 20) * (plan.rangeEnd - plan.rangeBegin) / size;
        if (!helper.indexPartial(size, readAt, user, plan)) { std::fprintf(stderr, "partial %zu: %s\n", k, helper.error()); return false; }
        if (!store.mergePartial(helper.partialBlob().data(), helper.partialBlob().size())) { std::fprintf(stderr, "merge: %s\n", store.error()); return false; }
    }
    return store.finishMerge(meta);
}

} // namespace wasmidi
