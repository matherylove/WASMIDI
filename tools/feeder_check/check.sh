#!/bin/sh
# Run from tools/feeder_check. See HANDOFF sec. 26.

# zstd for bpfa_midi_store.cpp on hosts without zstd headers (HANDOFF sec. 46)
mkdir -p /tmp/wasmidi_zstd_inc
cat > /tmp/wasmidi_zstd_inc/zstd.h <<'ZEOF'
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
size_t ZSTD_compress(void* dst, size_t dstCapacity, const void* src, size_t srcSize, int level);
size_t ZSTD_decompress(void* dst, size_t dstCapacity, const void* src, size_t compressedSize);
unsigned ZSTD_isError(size_t code);
size_t ZSTD_compressBound(size_t srcSize);
typedef struct ZSTD_CCtx_s ZSTD_CCtx;
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
ZSTD_CCtx* ZSTD_createCCtx(void);
size_t ZSTD_freeCCtx(ZSTD_CCtx* cctx);
size_t ZSTD_compressCCtx(ZSTD_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize, int level);
ZSTD_DCtx* ZSTD_createDCtx(void);
size_t ZSTD_freeDCtx(ZSTD_DCtx* dctx);
size_t ZSTD_decompressDCtx(ZSTD_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
#ifdef __cplusplus
}
#endif
ZEOF
ZSTD_FLAGS="-I/tmp/wasmidi_zstd_inc"
ZSTD_LIBS="-l:libzstd.so.1"
g++ -O2 -std=c++17 $ZSTD_FLAGS -I../../src -I../../src/midi -o ref ref.cpp ../../src/midi/midi_mapped_store.cpp ../../src/midi/midi_parser.cpp ../../src/midi/midi_document_codec.cpp || exit 1
python3 synth_mid.py /tmp/feeder_synth.mid
for w in "0 30" "5.5 20" "12.3 20" "60 5"; do
  set -- $w
  ./ref /tmp/feeder_synth.mid $1 $2 0 /tmp/ref.bin 2>/dev/null
  node jsref.mjs /tmp/feeder_synth.mid $1 $2 0 /tmp/js.bin 2>/dev/null
  cmp -s /tmp/ref.bin /tmp/js.bin && echo "start=$1 IDENTICAL" || echo "start=$1 DIFFER"
  node proto.mjs /tmp/feeder_synth.mid $1 $2 /tmp/proto.bin 2>/dev/null
  printf "start=$1 protocol: "; python3 cmp.py /tmp/ref.bin /tmp/proto.bin
done

# BPFA store vs current store, synth stream (HANDOFF sec. 43)
g++ -O2 -std=c++17 $ZSTD_FLAGS -I../../src -I../../src/midi -o cmp_old store_compare.cpp ../../src/midi/midi_mapped_store.cpp ../../src/midi/midi_parser.cpp ../../src/midi/midi_document_codec.cpp ../../src/midi/bpfa_midi_store.cpp $ZSTD_LIBS || exit 1
g++ -O2 -std=c++17 -DUSE_BPFA $ZSTD_FLAGS -I../../src -I../../src/midi -o cmp_bpfa store_compare.cpp ../../src/midi/midi_mapped_store.cpp ../../src/midi/midi_parser.cpp ../../src/midi/midi_document_codec.cpp ../../src/midi/bpfa_midi_store.cpp $ZSTD_LIBS || exit 1
for w in "0 30" "1.0 20" "5.5 20" "12.3 20" "25 30" "60 5"; do
  set -- $w
  ./cmp_old /tmp/feeder_synth.mid $1 $2 0 /tmp/a.bin 2>/dev/null
  ./cmp_bpfa /tmp/feeder_synth.mid $1 $2 0 /tmp/b.bin 2>/dev/null
  cmp -s /tmp/a.bin /tmp/b.bin && echo "bpfa store start=$1 IDENTICAL" || echo "bpfa store start=$1 DIFFER"
done
rm -f cmp_old cmp_bpfa

# BPFA store vs current store: renderer sweep and live/keyboard snapshots (HANDOFF sec. 44)
SRCS="../../src/midi/midi_mapped_store.cpp ../../src/midi/midi_parser.cpp ../../src/midi/midi_document_codec.cpp ../../src/midi/bpfa_midi_store.cpp"
for tool in render_compare live_compare; do
  g++ -O2 -std=c++17 $ZSTD_FLAGS -I../../src -I../../src/midi -o ${tool}_old $tool.cpp $SRCS $ZSTD_LIBS || exit 1
  g++ -O2 -std=c++17 -DUSE_BPFA $ZSTD_FLAGS -I../../src -I../../src/midi -o ${tool}_bpfa $tool.cpp $SRCS $ZSTD_LIBS || exit 1
done
for pt in 0 1; do for w in "0 40" "5.5 20"; do
  set -- $w
  ./render_compare_old /tmp/feeder_synth.mid $1 $2 $pt /tmp/ra.bin 2>/dev/null
  ./render_compare_bpfa /tmp/feeder_synth.mid $1 $2 $pt /tmp/rb.bin 2>/dev/null
  cmp -s /tmp/ra.bin /tmp/rb.bin && echo "render perTrack=$pt start=$1 IDENTICAL" || echo "render perTrack=$pt start=$1 DIFFER"
done; done
# Colors per track with the fold (HANDOFF sec. 108): BPFA's folded sweep = old sweep filtered by pixel centres.
for fold in 4000:640 40000:64 100000:32; do
  FOLD=$fold ./render_compare_old /tmp/feeder_synth.mid 0 40 1 /tmp/ra.bin 2>/dev/null
  FOLD=$fold ./render_compare_bpfa /tmp/feeder_synth.mid 0 40 1 /tmp/rb.bin 2>/dev/null
  [ -s /tmp/ra.bin ] && cmp -s /tmp/ra.bin /tmp/rb.bin && echo "render fold $fold IDENTICAL" || echo "render fold $fold DIFFER"
done
./live_compare_old /tmp/feeder_synth.mid /tmp/la.bin 0 5 2.5 30 1 2>/dev/null
./live_compare_bpfa /tmp/feeder_synth.mid /tmp/lb.bin 0 5 2.5 30 1 2>/dev/null
cmp -s /tmp/la.bin /tmp/lb.bin && echo "live snapshots IDENTICAL" || echo "live snapshots DIFFER"
rm -f render_compare_old render_compare_bpfa live_compare_old live_compare_bpfa

# BPFA store vs current store: visual pages (HANDOFF sec. 45)
g++ -O2 -std=c++17 $ZSTD_FLAGS -I../../src -I../../src/midi -o page_compare_old page_compare.cpp $SRCS $ZSTD_LIBS || exit 1
g++ -O2 -std=c++17 -DUSE_BPFA $ZSTD_FLAGS -I../../src -I../../src/midi -o page_compare_bpfa page_compare.cpp $SRCS $ZSTD_LIBS || exit 1
./page_compare_old /tmp/feeder_synth.mid /tmp/pa.bin 0 3 1.5 10 2>/dev/null
./page_compare_bpfa /tmp/feeder_synth.mid /tmp/pb.bin 0 3 1.5 10 2>/dev/null
cmp -s /tmp/pa.bin /tmp/pb.bin && echo "visual pages IDENTICAL" || echo "visual pages DIFFER"
rm -f page_compare_old page_compare_bpfa

# Parallel load simulated on the host (HANDOFF sec. 60): raw and zstd, 3 helpers
for t in store_compare render_compare live_compare page_compare; do
  g++ -O2 -std=c++17 -DUSE_BPFA $ZSTD_FLAGS -I../../src -I../../src/midi -I. -o ${t}_par $t.cpp $SRCS $ZSTD_LIBS || exit 1
  g++ -O2 -std=c++17 $ZSTD_FLAGS -I../../src -I../../src/midi -I. -o ${t}_ref $t.cpp $SRCS $ZSTD_LIBS || exit 1
done
for mode in raw zstd; do
  if [ $mode = zstd ]; then export BPFA_ZSTD=1; else unset BPFA_ZSTD; fi
  export BPFA_PARALLEL=3
  ./store_compare_ref /tmp/feeder_synth.mid 0 30 0 /tmp/a.bin 2>/dev/null; ./store_compare_par /tmp/feeder_synth.mid 0 30 0 /tmp/b.bin 2>/dev/null
  cmp -s /tmp/a.bin /tmp/b.bin && echo "parallel $mode synth IDENTICAL" || echo "parallel $mode synth DIFFER"
  ./render_compare_ref /tmp/feeder_synth.mid 0 40 1 /tmp/a.bin 2>/dev/null; ./render_compare_par /tmp/feeder_synth.mid 0 40 1 /tmp/b.bin 2>/dev/null
  cmp -s /tmp/a.bin /tmp/b.bin && echo "parallel $mode render IDENTICAL" || echo "parallel $mode render DIFFER"
  ./live_compare_ref /tmp/feeder_synth.mid /tmp/a.bin 0 5 2.5 30 1 2>/dev/null; ./live_compare_par /tmp/feeder_synth.mid /tmp/b.bin 0 5 2.5 30 1 2>/dev/null
  cmp -s /tmp/a.bin /tmp/b.bin && echo "parallel $mode live IDENTICAL" || echo "parallel $mode live DIFFER"
  ./page_compare_ref /tmp/feeder_synth.mid /tmp/a.bin 0 3 1.5 10 2>/dev/null; ./page_compare_par /tmp/feeder_synth.mid /tmp/b.bin 0 3 1.5 10 2>/dev/null
  cmp -s /tmp/a.bin /tmp/b.bin && echo "parallel $mode pages IDENTICAL" || echo "parallel $mode pages DIFFER"
  unset BPFA_PARALLEL
done
unset BPFA_ZSTD
rm -f *_par *_ref
