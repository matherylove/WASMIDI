// LZMA / LZMA2 streaming decoder for the archive Worker (HANDOFF sec. 110).
#include <stdlib.h>
#include <string.h>

#include "LzmaDec.h"
#include "Lzma2Dec.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define WA_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WA_EXPORT
#endif

enum { WA_INPUT_CAPACITY = 1 << 22 };

typedef struct WaDecoder {
    int lzma2;
    CLzmaDec lzma;
    CLzma2Dec lzma2Dec;
    Byte *dic;
    SizeT dicSize;
    SizeT outStart;
    SizeT outEnd;
    SizeT consumed;
    int status;
    Byte input[WA_INPUT_CAPACITY];
} WaDecoder;

static void *waAlloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void waFree(ISzAllocPtr p, void *address) { (void)p; free(address); }
static const ISzAlloc waAllocator = { waAlloc, waFree };

static CLzmaDec *waCore(WaDecoder *d) { return d->lzma2 ? &d->lzma2Dec.decoder : &d->lzma; }

WA_EXPORT void wa_dec_free(WaDecoder *d)
{
    if (!d)
        return;
    if (d->lzma2)
        Lzma2Dec_FreeProbs(&d->lzma2Dec, &waAllocator);
    else
        LzmaDec_FreeProbs(&d->lzma, &waAllocator);
    free(d->dic);
    free(d);
}

// props: 5 bytes for LZMA, 1 byte for LZMA2. unpackSize < 0 means unknown.
WA_EXPORT WaDecoder *wa_dec_new(int lzma2, const Byte *props, int propsLen, double unpackSize)
{
    WaDecoder *d = (WaDecoder *)calloc(1, sizeof(WaDecoder));
    if (!d)
        return NULL;
    d->lzma2 = lzma2 ? 1 : 0;
    UInt32 dictSize = 0;
    if (d->lzma2) {
        if (propsLen < 1 || props[0] > 40)
            goto fail;
        dictSize = props[0] == 40 ? 0xFFFFFFFFu : (UInt32)((2u | (props[0] & 1u)) << (props[0] / 2 + 11));
        Lzma2Dec_CONSTRUCT(&d->lzma2Dec);
        if (Lzma2Dec_AllocateProbs(&d->lzma2Dec, props[0], &waAllocator) != SZ_OK)
            goto fail;
    } else {
        if (propsLen < LZMA_PROPS_SIZE)
            goto fail;
        dictSize = (UInt32)props[1] | ((UInt32)props[2] << 8) | ((UInt32)props[3] << 16) | ((UInt32)props[4] << 24);
        LzmaDec_CONSTRUCT(&d->lzma);
        if (LzmaDec_AllocateProbs(&d->lzma, props, LZMA_PROPS_SIZE, &waAllocator) != SZ_OK)
            goto fail;
    }
    SizeT size = dictSize < (1u << 22) ? (1u << 22) : dictSize;
    if (unpackSize >= 0 && unpackSize < (double)size)
        size = unpackSize < 4096 ? 4096 : (SizeT)unpackSize;
    if (size > 0x7FFF0000u)
        goto fail;
    d->dic = (Byte *)malloc(size);
    if (!d->dic)
        goto fail;
    d->dicSize = size;
    CLzmaDec *core = waCore(d);
    core->dic = d->dic;
    core->dicBufSize = size;
    if (d->lzma2)
        Lzma2Dec_Init(&d->lzma2Dec);
    else
        LzmaDec_Init(&d->lzma);
    return d;
fail:
    wa_dec_free(d);
    return NULL;
}

WA_EXPORT Byte *wa_dec_input(WaDecoder *d) { return d->input; }
WA_EXPORT int wa_dec_input_capacity(void) { return WA_INPUT_CAPACITY; }

// Decodes from input[0, inLen) producing at most `limit` bytes. Returns the
// LZMA status (ELzmaStatus) or -1 on a data error. The output is
// dic[outStart, outEnd) and stays valid until the next call.
WA_EXPORT int wa_dec_run(WaDecoder *d, int inLen, int limit, int finishEnd)
{
    CLzmaDec *core = waCore(d);
    if (core->dicPos == d->dicSize)
        core->dicPos = 0;
    d->outStart = core->dicPos;
    SizeT room = d->dicSize - core->dicPos;
    if (limit >= 0 && (SizeT)limit <= room)
        room = (SizeT)limit;
    else
        finishEnd = 0;
    SizeT srcLen = inLen < 0 ? 0 : (SizeT)inLen;
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    ELzmaFinishMode mode = finishEnd ? LZMA_FINISH_END : LZMA_FINISH_ANY;
    SRes res = d->lzma2
        ? Lzma2Dec_DecodeToDic(&d->lzma2Dec, core->dicPos + room, d->input, &srcLen, mode, &status)
        : LzmaDec_DecodeToDic(&d->lzma, core->dicPos + room, d->input, &srcLen, mode, &status);
    d->consumed = srcLen;
    d->outEnd = core->dicPos;
    d->status = res == SZ_OK ? (int)status : -1;
    return d->status;
}

WA_EXPORT int wa_dec_consumed(WaDecoder *d) { return (int)d->consumed; }
WA_EXPORT Byte *wa_dec_out_ptr(WaDecoder *d) { return d->dic + d->outStart; }
WA_EXPORT int wa_dec_out_size(WaDecoder *d) { return (int)(d->outEnd - d->outStart); }
